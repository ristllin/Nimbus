#include <unity.h>

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/conversation.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/history_rows.h"

// The history's invariant, as a class (lane H of the history fix, plan v3): on every head
// wire (Anthropic, OpenAI, Mistral, the Z.ai and custom single shots), loop on and off, an
// owner's turn and a routine's, with or without a summary, a clock, a photo, recall and a
// large input, over days of history before today and a long today:
//   1. every live row after the summary's last row (thru) is in the request, or named with
//      a memory_episodic call whose cursor reads it back (the [LEFT OUT] note or the
//      coverage line); rows at or before thru are the summary's;
//   2. no live row leaves the request by the give-up order unless the real body was refused
//      for its size first (v5.1.1's wire guard cut on 121 of 123 owner turns from an
//      estimate, day_context.cpp:140-167 at v5.1.1, and said nothing).
// Requests go through the device's own provider code to a scripted transport.

namespace {

namespace orch = nimbus::orch;
namespace providers = agent::providers;
using agent::TurnEngine;
using harness_test::Exchange;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::FakeProviderDeps;
using harness_test::LogCapture;

constexpr uint32_t kDay = 1792972800u;   // 2026-10-26 00:00 UTC (Mon)

std::string jsonEscaped(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// The head's final answer on its own wire.
std::string finalBody(const std::string& wire) {
  const std::string args = "{\"reply\":\"done\"}";
  if (wire == "anthropic")
    return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\",\"input\":" + args +
           "}],\"stop_reason\":\"tool_use\"}";
  if (wire == "openai")
    return "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"c\","
           "\"name\":\"orch_turn\",\"arguments\":\"" + jsonEscaped(args) + "\"}]}";
  if (wire == "custom")
    return "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"" + jsonEscaped(args) + "\"}}]}";
  return "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":"
         "\"abcdefghi\",\"function\":{\"name\":\"orch_turn\",\"arguments\":\"" + jsonEscaped(args) + "\"}}]}}]}";
}

struct Head {
  const char* name;
  const char* host;    // the engine's head host
  const char* model;
  const char* wire;    // the reply shape its requests get
};
const Head kHeads[] = {
    {"anthropic/sonnet", "anthropic", "claude-sonnet-5-5", "anthropic"},
    {"anthropic/haiku", "anthropic", "claude-haiku-5-5", "anthropic"},
    {"openai/luna", "openai", "gpt-5.6-luna", "openai"},
    {"mistral/large-4", "mistral", "mistral-large-4-0", "mistral"},
    {"mistral/medium", "mistral", "mistral-medium-latest", "mistral"},
    {"zai/glm-5.3", "zai", "glm-5.3", "custom"},
    {"custom", "custom", "gpt-5.6-luna", "custom"},
};

struct Shape {
  int before = 0;      // turns on the days before today
  int today = 0;       // turns today
  size_t inputKB = 1;  // the message's size
  int recalled = 0;    // recalled memories
  bool loop = true;
  bool owner = true;   // false: a routine's turn (nobody's message)
  bool photo = false;  // a photo exchange in today's history
  int clock = 0;       // 0 = set (18:00), 1 = not set, 2 = 00:30 (the hour after midnight)
  bool summary = false;   // a fold covers the oldest half
  bool longDay = false;   // today's turns are long (a day over keep on the smaller heads)
};

struct Rig {
  FakePlatform plat;
  FakeConfig cfg;
  FakeProviderDeps wire;
  providers::ProviderDeps pd;
  std::vector<orch::EpisodicMessage> rows;   // oldest first
  agent::HistoryMarks marks;
  std::string summary;
  std::vector<std::string> recalled;
  uint32_t seq = 0;
  std::unique_ptr<TurnEngine> eng;

  std::string add(uint32_t at, const std::string& role, orch::MsgKind kind, const std::string& text,
                  const std::string& tags) {
    orch::EpisodicMessage m;
    char id[16];
    snprintf(id, sizeof id, "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = "1001";
    m.tsHours = at / 3600u;
    m.role = role;
    m.kind = kind;
    m.text = text + " tok" + m.id.substr(1);   // a token the request must carry, or name
    m.tags = tags + (at >= 1000000000u ? ",at:" + std::to_string(at) : "");
    rows.push_back(m);
    return m.id;
  }
  void turn(uint32_t at, size_t bytes) {
    const std::string u = add(at, "user", orch::MsgKind::Message, "question " + std::string(bytes / 3, 'q'), "from:Roy");
    add(at + 60, "assistant", orch::MsgKind::Message, "answer " + std::string(bytes - bytes / 3, 'a'),
        "fx:none,out:reply,turn:" + u);
  }

  Rig(const Head& h, const Shape& s) {
    providers::clearHostParks();
    cfg.toolLoop = s.loop;
    cfg.keyed = {h.host};
    cfg.keys[h.host] = "fake";
    cfg.priority = h.host;
    cfg.orchHost = h.host;
    cfg.orchModels[h.host] = h.model;
    plat.epoch = s.clock == 1 ? 0 : s.clock == 2 ? kDay + 1800 : kDay + 18 * 3600;
    pd = wire.contract();
    pd.nowMs = [this] { return plat.ms; };
    pd.key = [](const char*) { return std::string("fake"); };
    if (std::string(h.wire) == "custom") {
      wire.custBase = std::string(h.host) == "zai" ? "https://api.z.ai" : "https://llm.example.lan";
      wire.custPathPrefix = std::string(h.host) == "zai" ? "/api/paas/v4" : "";
      wire.custKey = "fake";
      wire.custModel = h.model;
    }
    for (int i = 0; i < 12; i++) wire.http.script.push_back(Exchange{"", "", 200, finalBody(h.wire)});
    build(h, s);
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [](const std::string&, const std::string&) {};
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& c) { return orch::principalForRole(c, orch::Role::Admin); };
    d.composeInputs = [this](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      in.chatSummary = summary;
      return in;
    };
    d.toolSpecs = [](const orch::Principal&) {
      std::vector<orch::ToolRegistry::Spec> specs;   // a device-sized tool list (56 tools)
      for (int i = 0; i < 56; i++)
        specs.push_back({"tool_" + std::to_string(i), std::string(560, 'd'),
                         "{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}}}"});
      return specs;
    };
    d.mcpDispatch = [](const std::string&, const orch::Principal&) {
      return std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}}");
    };
    d.recall = [this](const std::string&, const orch::Principal&) {
      orch::RecallOut out;
      out.hits = recalled;
      return out;
    };
    d.hosts.fabric = [this](const std::vector<std::string>& hl, const std::string& ins, const std::string& inp,
                            std::string& out, std::string& err, const agent::HeadTools& ht, orch::TurnSpend* spend,
                            const std::function<void(const std::string&, const std::string&)>& notify) {
      return providers::runFabricLoop(pd, hl, ins, inp, out, err, ht, spend, notify);
    };
    const std::string host = h.host;
    d.hosts.add(host, [this, host](std::string& conv, const std::string& ins, const std::string& inp, std::string& out,
                                   std::string& err, const agent::HeadTools* tools, orch::TokenUsage* use) {
      if (host == "mistral") return providers::orchTurnMistral(pd, conv, ins, inp, out, err, tools, use);
      if (host == "anthropic") return providers::orchTurnAnthropic(pd, conv, ins, inp, out, err, tools, use);
      if (host == "openai") return providers::orchTurnOpenAI(pd, conv, ins, inp, out, err, tools, use);
      return providers::orchTurnCustom(pd, conv, ins, inp, out, err, tools, use);
    });
    d.hosts.fabricSupports = [](const std::string& x) { return providers::fabricSupports(x); };
    d.histV2 = [] { return true; };
    d.historyRows = [this](const std::string&, const agent::HistoryQuery& q) { return read(q); };
    d.captureUserRow = [this](const std::string&, const std::string& t, const std::string& tags) {
      return add(plat.epoch ? (uint32_t)plat.epoch : 0, "user", orch::MsgKind::Message, t, tags);
    };
    d.captureTrigger = [](const std::string&, const std::string&, const std::string&) {};
    d.historyMarks = [this](const std::string&) { return marks; };
    d.noteHistoryShift = [this](const std::string&, const std::string& soft) { marks.softStart = soft; };
    eng.reset(new TurnEngine(std::move(d)));
  }
  ~Rig() { providers::clearHostParks(); }

  void build(const Head&, const Shape& s) {
    const uint32_t now = plat.epoch ? (uint32_t)plat.epoch : 0;
    const uint32_t dayStart = now ? (now - now % 86400u) : 0;
    for (int i = 0; i < s.before; i++)   // the days before, a few turns an hour
      turn(dayStart ? dayStart - 7 * 86400u + (uint32_t)i * 2400u : 0, 900);
    if (s.summary && !rows.empty()) {
      marks.thru = rows[rows.size() / 2 - (rows.size() / 2) % 2 + 1].id;   // an assistant row: a turn's end
      summary = "1. Decisions\n- the owner logs meals here.\n" + std::string(3000, 's');
    }
    for (int i = 0; i < s.today; i++) {
      const uint32_t at = dayStart ? dayStart + (uint32_t)(s.clock == 2 ? 60 : 8 * 3600) + (uint32_t)i * 300u : 0;
      if (s.photo && i == s.today / 2) {
        add(at, "user", orch::MsgKind::Image, "[image 1/1 ref=ab12cd34ef56ab78] a bowl of yogurt with blueberries",
            "sent:" + std::to_string(at));
        add(at, "user", orch::MsgKind::Message, "[image 1/1 ref=ab12cd34ef56ab78]\n\nThey wrote: estimate it",
            "from:Roy,att:ab12cd34ef56ab78");
        continue;
      }
      turn(at, s.longDay ? 4000 : 1200);
    }
    for (int i = 0; i < s.recalled; i++) recalled.push_back("[80%] a remembered fact " + std::string(280, 'r'));
  }

  std::vector<orch::EpisodicMessage> read(const agent::HistoryQuery& q) const {
    std::vector<orch::EpisodicMessage> out;
    if (!q.after.empty()) {
      for (auto it = rows.begin(); it != rows.end() && out.size() < q.limit; ++it)
        if (orch::rowIdAfter(it->id, q.after)) out.push_back(*it);
      return out;
    }
    for (auto it = rows.rbegin(); it != rows.rend() && out.size() < q.limit; ++it)
      if (q.before.empty() || orch::rowIdAfter(q.before, it->id)) out.push_back(*it);
    return out;
  }
};

bool isRowId(const std::string& s, size_t at) {
  if (at + 9 > s.size() || s[at] != 'm') return false;
  for (size_t k = at + 1; k < at + 9; k++)
    if (!((s[k] >= '0' && s[k] <= '9') || (s[k] >= 'a' && s[k] <= 'f'))) return false;
  return true;
}

// Every "before" cursor a request names in a memory_episodic call: a row id within a few
// bytes of the key (the call sits JSON-escaped inside the request).
std::vector<std::string> namedBefores(const std::string& body) {
  std::vector<std::string> out;
  for (size_t at = body.find("memory_episodic"); at != std::string::npos; at = body.find("memory_episodic", at + 1)) {
    const size_t key = body.find("before", at);
    if (key == std::string::npos || key > at + 80) continue;
    for (size_t m = key + 6; m < key + 16 && m < body.size(); m++)
      if (isRowId(body, m)) {
        out.push_back(body.substr(m, 9));
        break;
      }
  }
  return out;
}

struct Verdict {
  int cases = 0, refusals = 0, named = 0, violations = 0;
  std::string first;
};

void check(const Head& h, const Shape& s, Verdict& v) {
  LogCapture::install();
  Rig r(h, s);
  const std::string input = std::string(s.inputKB * 1024, 'i');
  if (s.owner)
    r.eng->handleMessage("about my day: " + input, "Roy", "1001");
  else
    r.eng->injectScheduledTurn("1001", "Review the day: " + input, "review", "L1");
  v.cases++;
  char what[200];
  snprintf(what, sizeof what, "%s before=%d today=%d in=%zuKB recall=%d loop=%d owner=%d photo=%d clock=%d sum=%d long=%d",
           h.name, s.before, s.today, s.inputKB, s.recalled, s.loop, s.owner, s.photo, s.clock, s.summary, s.longDay);
  if (r.wire.http.seen.empty()) {   // nothing was sent: the turn failed before any request
    v.violations++;
    if (v.first.empty()) v.first = std::string(what) + ": no request was sent";
    return;
  }
  const std::string& body = r.wire.http.seen[0].body;   // round 0 as it was sent
  const std::vector<std::string> befores = namedBefores(body);
  const bool refused = LogCapture::contains(" B budget") || LogCapture::contains("refused the request for its size");
  const bool leftOut = body.find("LEFT OUT TO FIT THIS REQUEST") != std::string::npos;
  v.refusals += refused ? 1 : 0;
  // 2. a cut by the give-up order follows a real refusal.
  if (leftOut && !refused) {
    v.violations++;
    if (v.first.empty()) v.first = std::string(what) + ": history left out with no refusal";
  }
  // 1. every live row after thru is in the request or named.
  const std::string inFlight = s.owner && !r.rows.empty() ? r.rows.back().id : std::string();   // arrives as the input
  for (const auto& m : r.rows) {
    if (!r.marks.thru.empty() && !orch::rowIdAfter(m.id, r.marks.thru)) continue;   // the summary's
    if (m.id == inFlight) continue;
    if (body.find("tok" + m.id.substr(1)) != std::string::npos) continue;
    bool named = false;
    for (const auto& b : befores) named = named || orch::rowIdAfter(b, m.id);
    if (named) {
      v.named++;
      continue;
    }
    v.violations++;
    if (v.first.empty()) v.first = std::string(what) + ": row " + m.id + " neither sent nor named";
    break;
  }
}

// A deterministic spread of shapes (the matrix's corners and a sample of its inside).
std::vector<Shape> shapes() {
  std::vector<Shape> out;
  const int befores[] = {0, 60, 200};
  const int todays[] = {0, 12, 40};
  const size_t inputs[] = {1, 12, 30};
  uint32_t lcg = 12345;
  const auto next = [&lcg](uint32_t mod) {
    lcg = lcg * 1103515245u + 12345u;
    return (lcg >> 16) % mod;
  };
  for (int b : befores)
    for (int t : todays)
      for (size_t in : inputs) {
        Shape s;
        s.before = b;
        s.today = t;
        s.inputKB = in;
        s.recalled = next(2) ? 10 : 0;
        s.loop = next(3) != 0;
        s.owner = next(4) != 0;
        s.photo = t > 0 && next(3) == 0;
        s.clock = (int)next(3);
        s.summary = b > 0 && next(2) == 0;
        s.longDay = t >= 12 && next(3) == 0;
        out.push_back(s);
      }
  return out;
}

}  // namespace

// ---- the 15 Nov complaint, replayed ------------------------------------------------------
// Fixture rows shaped like the owner's 15 Nov chat (its row ids, times and sizes; the facts the
// complaint turns on, never the owner's words): the summary stopped on 2 Nov (thru m000017d9),
// a shift hid everything up to m00001c96, and on 15 Nov a blinded photo estimate at 15:55
// (m00001cce: blueberries 50 g), the truth and the comparison at 15:57 (m00001cd9), a
// follow-up at 15:58, then at 16:00 "Do you understand why?". v5.1.1 sent that request with 4
// history rows: the estimate and the comparison were gone.
namespace {
constexpr uint32_t k15Nov = 1794700800u;   // 2026-11-15 00:00 UTC (Sat)

struct ReplayRow {
  uint32_t idSfx;
  uint32_t at;   // seconds after 15 Nov 00:00 UTC
  const char* role;
  orch::MsgKind kind;
  const char* text;
};
const ReplayRow kComplaint[] = {
    {0x1c97, 8 * 3600, "user", orch::MsgKind::Message, "Breakfast: skyr with oats and a banana. Log it."},
    {0x1c9a, 8 * 3600 + 90, "assistant", orch::MsgKind::Message,
     "Logged as breakfast-0805: about 520 kcal / 38 g protein. Today stands at 520 kcal."},
    {0x1cbd, 11 * 3600, "system", orch::MsgKind::Message, "[routine \"weekend-checkin\" ran]"},
    {0x1cbe, 11 * 3600 + 30, "assistant", orch::MsgKind::Message,
     "Weekend check-in: today's focus is learning. One quick idea for the afternoon, if you want it."},
    {0x1cc0, 12 * 3600, "user", orch::MsgKind::Image,
     "[image 1/1 ref=f0813d04e9a8de6d] (seen; note: a plate of sliced chicken breast, pasta and vegetables)"},
    {0x1cc1, 12 * 3600 + 5, "user", orch::MsgKind::Message,
     "[image 1/1 ref=f0813d04e9a8de6d]\n\nThey wrote: 200g chicken breast, 400g pasta, you estimate the veg"},
    {0x1cc5, 12 * 3600 + 90, "assistant", orch::MsgKind::Message,
     "Logged as lunch-1204: about 1,212 kcal / 97 g protein (chicken 330 kcal, pasta 620 kcal, vegetables 90 kcal)."},
    {0x1cc7, 15 * 3600 + 3290, "user", orch::MsgKind::Image,
     "[image 1/1 ref=02501ff2da0b04f8] (seen; note: a bowl of yogurt with a granola sprinkle and blueberries around the rim)"},
    {0x1cc8, 15 * 3600 + 3300, "user", orch::MsgKind::Message,
     "[image 1/1 ref=02501ff2da0b04f8]\n\nThey wrote: Don't log yet. Estimate the quantities with your skill, I will confirm"},
    {0x1cce, 15 * 3600 + 3354, "assistant", orch::MsgKind::Message,
     "Photo estimate (blinded, locked before you confirm):\n| component | grams (range) | kcal | protein |\n"
     "| Yogurt (deep layer) | 200 g (180-220) | ~120 | ~22 g |\n| Granola (sprinkle) | 40 g (30-50) | ~180 | ~4 g |\n"
     "| Blueberries (around the rim) | 50 g (40-60) | ~29 | ~0.4 g |\n| Total | ~290 g | ~330 kcal | ~26 g |\n"
     "Confidence: moderate. Top uncertainty: the yogurt depth. Say the word and I will adjust, then log it."},
    {0x1cd0, 15 * 3600 + 3380, "user", orch::MsgKind::Image,
     "[image 1/1 ref=fdea4118cc038323] (seen; note: a granola label, 415 kcal and 21 g protein per 100 g)"},
    {0x1cd1, 15 * 3600 + 3392, "user", orch::MsgKind::Message,
     "[image 1/1 ref=fdea4118cc038323]\n\nThey wrote: Here is the truth: 150g blueberries, 200g skyr, 50g granola"},
    {0x1cd9, 15 * 3600 + 3456, "assistant", orch::MsgKind::Message,
     "Logged and calibrated. Truth vs locked estimate: mass 400 g vs ~290 g (under 27%), kcal 491 vs ~330. "
     "Granola label stored for reuse. Snack saved: 491 kcal / 29.6 g protein."},
    {0x1cdb, 15 * 3600 + 3514, "user", orch::MsgKind::Message, "The yogurt was spot on, you missed the blueberries"},
    {0x1cdd, 15 * 3600 + 3539, "assistant", orch::MsgKind::Message,
     "Noted: the miss was the blueberry volume, not the yogurt depth. I will weight fruit piles more next time."},
};

struct ReplayHead {
  const char* name;
  Head head;
};
const ReplayHead kReplayHeads[] = {
    {"Mistral Large 4", {"mistral/large-4", "mistral", "mistral-large-4-0", "mistral"}},
    {"Claude Sonnet 5.5", {"anthropic/sonnet", "anthropic", "claude-sonnet-5-5", "anthropic"}},
    {"gpt-5.6-luna", {"openai/luna", "openai", "gpt-5.6-luna", "openai"}},
    {"glm-5.3", {"zai/glm-5.3", "zai", "glm-5.3", "custom"}},
};

// The chat as it stood at 16:00: thirteen days of turns after the 2 Nov summary, hidden by a
// shift up to m00001c96, then 15 Nov.
void complaintChat(Rig& r) {
  r.rows.clear();
  r.seq = 0x17d9;
  r.marks.thru = "m000017d9";
  r.marks.softStart = "m00001c96";
  r.summary = "1. Decisions & preferences\n- the owner logs meals by photo and weight.\n" + std::string(3800, 's');
  const uint32_t from = k15Nov - 13 * 86400u;
  for (uint32_t i = 0; r.seq < 0x1c95; i++) r.turn(from + i * 1800u, 600);
  for (const ReplayRow& row : kComplaint) {
    r.seq = row.idSfx - 1;
    const std::string tags = std::string(row.role) == "assistant" ? "fx:read,out:reply" : "from:R";
    r.add(k15Nov + row.at, row.role, row.kind, row.text, tags);
  }
}
}  // namespace

// Plan v3 lane H DoD: the 16:00 request keeps the 15:55 estimate and the 15:57 comparison at
// the Mistral, Sonnet, luna and glm budgets, and anything it leaves out it names.
static void test_the_15_nov_complaint_keeps_the_estimate_and_the_comparison() {
  for (const ReplayHead& rh : kReplayHeads) {
    LogCapture::install();
    Shape shape;
    shape.clock = 0;
    Rig r(rh.head, shape);
    r.plat.epoch = k15Nov + 16 * 3600;
    complaintChat(r);
    r.recalled.assign(10, "[70%] a remembered food fact " + std::string(260, 'r'));
    r.eng->handleMessage("Do you understand why? It is easy to compare the grams you estimated with my weights. "
                         "How did you miss that?",
                         "R", "1001");
    TEST_ASSERT_FALSE_MESSAGE(r.wire.http.seen.empty(), rh.name);
    const std::string& body = r.wire.http.seen[0].body;
    const bool estimate = body.find("50 g (40-60)") != std::string::npos;
    const bool comparison = body.find("400 g vs ~290 g") != std::string::npos;
    printf("complaint replay %s: body %zu B, estimate %s, comparison %s\n", rh.name, body.size(),
           estimate ? "sent" : "MISSING", comparison ? "sent" : "MISSING");
    TEST_ASSERT_TRUE_MESSAGE(estimate, rh.name);
    TEST_ASSERT_TRUE_MESSAGE(comparison, rh.name);
    TEST_ASSERT_TRUE_MESSAGE(body.find("CONVERSATION COVERAGE") != std::string::npos, rh.name);   // the hidden 13 days, named
  }
}

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

static void test_every_live_row_is_sent_or_named_on_every_head_wire() {
  Verdict v;
  for (const Head& h : kHeads)
    for (const Shape& s : shapes()) check(h, s, v);
  printf("wire fit class: %d cases, %d refused at round 0, %d rows named, %d violations\n", v.cases, v.refusals,
         v.named, v.violations);
  TEST_ASSERT_EQUAL_MESSAGE(0, v.violations, v.first.c_str());
  TEST_ASSERT_TRUE(v.cases >= 150);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_live_row_is_sent_or_named_on_every_head_wire);
  RUN_TEST(test_the_15_nov_complaint_keeps_the_estimate_and_the_comparison);
  return UNITY_END();
}
