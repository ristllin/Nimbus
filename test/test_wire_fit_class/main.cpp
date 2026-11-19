#include <algorithm>
#include <unity.h>

#include <cstdio>
#include <map>
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
#include "nimbus/orch/budget.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/history_rows.h"
#include "nimbus/orch/model_window.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/router_route.h"

// The history's invariant, as a class (lane H of the history fix, plan v3): on every head
// wire (Anthropic, OpenAI, Mistral, Z.ai, the custom single shot, and the Cumulo head through
// each of its four upstreams), loop on and off, an
// owner's turn and a routine's, with or without a summary, a clock, a photo, recall and a
// large input, over days of history before today and a long today:
//   1. every live row after the summary's last row (thru) is in the request, or named with
//      a memory_episodic call whose cursor reads it back (the [LEFT OUT] note or the
//      coverage line); rows at or before thru are the summary's;
//   2. no live row leaves the request by the give-up order unless the real body was refused
//      for its size first (v5.1.1's wire guard cut on 121 of 123 owner turns from an
//      estimate, day_context.cpp:140-167 at v5.1.1, and said nothing);
//   3. the request goes out in the form its budget was read in (lane L): one flat prompt
//      string, or a split prompt with the history as messages (request_budget.h
//      turnBudgetFor), and at its head's own budget a turn with the tool loop off is never
//      refused at round 0 on a direct head: its history is sized for the form it goes out in
//      (sized from the body budget, a loop-off Large 4, Medium or glm-5.3 head got that
//      budget's history for one 55,904 B string: 15 of these shapes were refused, and recall
//      and the playbook left first).
// Requests go through the device's own provider code to a scripted transport, each wire at
// its head's own request budget (the wire reads the head's model; the Z.ai head is pointed
// as the device points it), and again with every wire at the 85,000 B floor, the budget of
// a routed or custom head, where nearly every shape is refused at round 0.

namespace {

namespace orch = nimbus::orch;
namespace providers = agent::providers;

namespace {
// Read-class registry names (tool_groups.h) for the filler tools: an unattended turn (a
// routine, a synthesis) is offered only what its class may call (unattended_policy.h, lane
// RB-Q2a), and a read is what every class may call, so the tool bytes reach every shape.
const char* const kReadToolNames[] = {"read_file", "list_files", "search", "web_search", "view", "calc", "device_status", "ota_status", "selftest", "system_health", "device_log", "agent_list", "file_stat", "read_pdf", "skill_list", "files.read", "files.list", "files.search", "memory.search", "memory.episodic", "web.search", "results.get", "image.look", "loop.list", "device.status", "device.log", "device.selftest", "system.health", "ota.status", "files.stat", "media.list", "tenant.list", "models.list", "docs.list", "docs.read", "docs.search", "skill.list", "skill.get", "results.list"};
constexpr size_t kReadToolCount = sizeof(kReadToolNames) / sizeof(kReadToolNames[0]);
}  // namespace

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
    // The Cumulo head through each upstream (the device's cumuloAttempt): the upstream's own
    // wire over the router, at the routed floor.
    {"cumulo/openai", "cumulo", "openai/gpt-5.6", "openai"},
    {"cumulo/anthropic", "cumulo", "anthropic/claude-sonnet-5-5", "anthropic"},
    {"cumulo/mistral", "cumulo", "mistral/mistral-large-4-0", "mistral"},
    {"cumulo/zai", "cumulo", "zai/glm-5.3", "custom"},
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
    pd.orchModel = [this](const char* host) { return cfg.orchModels[host]; };   // the wire budgets its own head
    const bool zai = std::string(h.host) == "zai";
    if (std::string(h.wire) == "custom") {
      wire.custBase = zai ? "https://api.z.ai" : "https://llm.example.lan";
      wire.custPathPrefix = zai ? "/api/paas/v4" : "";
      wire.custKey = "fake";
      wire.custModel = h.model;
    }
    if (zai) pointAtZai(h.model);
    // A Z.ai turn under the loop answers with a tool call (chat completions), a flat one in content.
    const std::string final = finalBody(zai && s.loop ? "mistral" : h.wire);
    for (int i = 0; i < 12; i++) wire.http.script.push_back(Exchange{"", "", 200, final});
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
      // A device-sized tool list (the bytes of 56 tools of 560 B) under read-class names,
      // which every unattended class is offered (lane RB-Q2a).
      std::vector<orch::ToolRegistry::Spec> specs;
      const size_t per = 56 * (560 + 100) / kReadToolCount - 100;
      for (size_t i = 0; i < kReadToolCount; i++)
        specs.push_back({kReadToolNames[i], std::string(per, 'd'),
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
      return hostTurn(host, HostCall{conv, ins, inp, out, err, tools, use});
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

  // One head request, as the engine hands it to a host.
  struct HostCall {
    std::string& conv;
    const std::string& ins;
    const std::string& inp;
    std::string& out;
    std::string& err;
    const agent::HeadTools* tools;
    orch::TokenUsage* use;
  };
  // The upstream's own wire for `upstream` over `deps` (the custom single shot for the rest).
  static bool wireTurn(const std::string& upstream, const providers::ProviderDeps& deps, const HostCall& c) {
    if (upstream == "mistral") return providers::orchTurnMistral(deps, c.conv, c.ins, c.inp, c.out, c.err, c.tools, c.use);
    if (upstream == "anthropic")
      return providers::orchTurnAnthropic(deps, c.conv, c.ins, c.inp, c.out, c.err, c.tools, c.use);
    if (upstream == "openai") return providers::orchTurnOpenAI(deps, c.conv, c.ins, c.inp, c.out, c.err, c.tools, c.use);
    return providers::orchTurnCustom(deps, c.conv, c.ins, c.inp, c.out, c.err, c.tools, c.use);
  }
  // A direct head on its own wire; the Cumulo head as the device runs it (orchestrator.cpp
  // cumuloAttempt): the selector's upstream on its own wire through the router, on the model
  // the selector names; Z.ai and any other upstream as a single shot over the router's chat
  // completions.
  bool hostTurn(const std::string& host, const HostCall& c) {
    if (host != "cumulo") return wireTurn(host, pd, c);
    const orch::RouterRoute rr = orch::resolveRouterRoute(cfg.orchModels["cumulo"]);
    providers::ProviderDeps r = pd;
    r.routerBase = [] { return std::string("https://app.cumulo-nimbus.ai"); };
    r.routerKey = [] { return std::string("cumulo_sk_test"); };
    const std::string model = rr.model;
    r.orchModel = [model](const char*) { return model; };
    const bool native = rr.upstream == "anthropic" || rr.upstream == "openai" || rr.upstream == "mistral";
    if (rr.upstream == "mistral") c.conv.clear();   // the router admits a new conversation only
    if (!native) providers::configureRouterCompatHead(r, rr.upstream, model);
    return wireTurn(native ? rr.upstream : "custom", r, c);
  }

  // The Z.ai head as the device points it (transport_tls.cpp zaiHead): its own budget and
  // the tool loop, for its head requests and its fabric rounds.
  void pointAtZai(const std::string& model) {
    pd.zaiHead = [model](providers::ProviderDeps& z) {
      z.customBase = [] { return std::string("https://api.z.ai"); };
      z.customPathPrefix = [] { return std::string("/api/paas/v4"); };
      z.customKey = [] { return std::string("fake"); };
      z.customConv = [] { return std::string("openai"); };
      z.customModel = [model] { return model; };
      providers::applyZaiHeadPolicy(z);
    };
    pd.zaiHead(pd);
  }

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
  int loopOff = 0, loopOffRefusals = 0;   // the shapes with the tool loop off, and those refused at round 0
  uint64_t loopOnDigest = 1469598103934665603ull;   // FNV-1a over every request a loop-on shape sent
  std::string first;
};

// The flat form's own history header (prompt_layout.cpp historyText): a split prompt carries
// the history as messages and never this line.
const char kFlatHistoryHeader[] = "## RECENT CONVERSATION (this chat, oldest first)";

void digest(uint64_t& h, const std::string& s) {
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  h = (h ^ 0xffu) * 1099511628211ull;   // a boundary between requests
}

void violation(Verdict& v, const std::string& what) {
  v.violations++;
  if (v.first.empty()) v.first = what;
}

// 1. every live row after thru is in the request or named (the owner's in-flight row arrives
// as the input).
void checkRows(const Rig& r, const Shape& s, const std::string& body, const std::string& what, Verdict& v) {
  const std::vector<std::string> befores = namedBefores(body);
  const std::string inFlight = s.owner && !r.rows.empty() ? r.rows.back().id : std::string();
  for (const auto& m : r.rows) {
    const bool summarized = !r.marks.thru.empty() && !orch::rowIdAfter(m.id, r.marks.thru);
    if (summarized || m.id == inFlight || body.find("tok" + m.id.substr(1)) != std::string::npos) continue;
    const bool named = std::any_of(befores.begin(), befores.end(),
                                   [&m](const std::string& b) { return orch::rowIdAfter(b, m.id); });
    if (!named) return violation(v, what + ": row " + m.id + " neither sent nor named");
    v.named++;
  }
}

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
  if (r.wire.http.seen.empty())   // nothing was sent: the turn failed before any request
    return violation(v, std::string(what) + ": no request was sent");
  const std::string& body = r.wire.http.seen[0].body;   // round 0 as it was sent
  const bool refused = LogCapture::contains(" B budget") || LogCapture::contains("refused the request for its size");
  const bool leftOut = body.find("LEFT OUT TO FIT THIS REQUEST") != std::string::npos;
  v.refusals += refused ? 1 : 0;
  v.loopOff += s.loop ? 0 : 1;
  v.loopOffRefusals += !s.loop && refused ? 1 : 0;
  if (s.loop)
    for (const auto& sent : r.wire.http.seen) digest(v.loopOnDigest, sent.body);
  // 2. a cut by the give-up order follows a real refusal.
  if (leftOut && !refused) violation(v, std::string(what) + ": history left out with no refusal");
  // 3. the form the budget was read in is the form on the wire (a request with history shows it).
  const bool flatSent = body.find(kFlatHistoryHeader) != std::string::npos;
  if (s.before + s.today > 0 && flatSent != orch::turnBudgetFor(h.host, h.model, s.loop).flatTurn)
    violation(v, std::string(what) + (flatSent ? ": sent flat, budgeted split" : ": sent split, budgeted flat"));
  checkRows(r, s, body, what, v);
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
// The 16:00 request of one head: what it carried.
static void replayOn(const ReplayHead& rh, const char* budget) {
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
  printf("complaint replay %s (%s budget): body %zu B, estimate %s, comparison %s\n", rh.name, budget, body.size(),
         estimate ? "sent" : "MISSING", comparison ? "sent" : "MISSING");
  TEST_ASSERT_TRUE_MESSAGE(estimate, rh.name);
  TEST_ASSERT_TRUE_MESSAGE(comparison, rh.name);
  TEST_ASSERT_TRUE_MESSAGE(body.find("CONVERSATION COVERAGE") != std::string::npos, rh.name);   // the hidden 13 days, named
}

static void test_the_15_nov_complaint_keeps_the_estimate_and_the_comparison() {
  for (const ReplayHead& rh : kReplayHeads) replayOn(rh, "its own");
  providers::setTestBudgetOverride(85000);   // and where v5.1.1 stood: every wire at the floor
  for (const ReplayHead& rh : kReplayHeads) replayOn(rh, "the 85,000 B");
  providers::setTestBudgetOverride(0);
}

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

// Every head over every shape; one line per head, and each head's own verdict in `perHead`.
static Verdict everyHead(const char* budget, std::map<std::string, Verdict>* perHead = nullptr) {
  Verdict all;
  for (const Head& h : kHeads) {
    Verdict one;
    for (const Shape& s : shapes()) check(h, s, one);
    printf("wire fit class, %s budget, %s: %d cases, %d refused at round 0, %d rows named, %d violations\n", budget,
           h.name, one.cases, one.refusals, one.named, one.violations);
    printf("wire fit class, %s budget, %s: loop off %d of %d refused at round 0; loop on %d cases, requests %016llx\n",
           budget, h.name, one.loopOffRefusals, one.loopOff, one.cases - one.loopOff, (unsigned long long)one.loopOnDigest);
    all.cases += one.cases;
    all.refusals += one.refusals;
    all.named += one.named;
    all.violations += one.violations;
    all.loopOff += one.loopOff;
    all.loopOffRefusals += one.loopOffRefusals;
    if (all.first.empty()) all.first = one.first;
    if (perHead) (*perHead)[h.name] = one;
  }
  return all;
}

// The pass at each head's own budget, run once for the tests that read it.
static std::map<std::string, Verdict>& ownPerHead() {
  static std::map<std::string, Verdict> per;
  return per;
}
static const Verdict& ownBudgets() {
  static const Verdict own = everyHead("its own", &ownPerHead());
  return own;
}

static void test_every_live_row_is_sent_or_named_on_every_head_wire() {
  const Verdict own = ownBudgets();
  providers::setTestBudgetOverride(85000);
  const Verdict floor = everyHead("the 85,000 B");
  providers::setTestBudgetOverride(0);
  printf("wire fit class: own budgets %d cases, %d refused at round 0, %d rows named, %d violations; at the floor "
         "%d cases, %d refused, %d named, %d violations\n",
         own.cases, own.refusals, own.named, own.violations, floor.cases, floor.refusals, floor.named, floor.violations);
  TEST_ASSERT_EQUAL_MESSAGE(0, own.violations, own.first.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(0, floor.violations, floor.first.c_str());
  TEST_ASSERT_TRUE(own.cases >= 150 && floor.cases >= 150);
  // The floor pass is the stress: most shapes are over 85,000 B and go through the give-up order.
  TEST_ASSERT_TRUE_MESSAGE(floor.refusals > own.refusals && floor.refusals * 2 > floor.cases, "the floor refuses most shapes");
}

// Lane L, the class's third rule at each head's own budget: with the tool loop off no direct
// head's turn is refused at round 0, whatever the history (a routed head's own budget is the
// 85,000 B floor, where a split prompt with a long history is refused: the floor pass's case).
static void test_a_loop_off_turn_is_never_refused_at_round_0_at_its_own_budget() {
  ownBudgets();
  int loopOff = 0;
  for (const Head& h : kHeads) {
    if (std::string(h.host) == "cumulo") continue;
    const Verdict& v = ownPerHead().at(h.name);
    TEST_ASSERT_TRUE_MESSAGE(v.loopOff >= 5, h.name);   // the spread holds loop-off shapes for every head
    TEST_ASSERT_EQUAL_MESSAGE(0, v.loopOffRefusals, h.name);
    loopOff += v.loopOff;
  }
  TEST_ASSERT_TRUE(loopOff >= 35);
}

// ---- v5.1.1 parity with the tool loop off (lane L) ----------------------------------------
// The regression's three heads, the tool loop off, a history well past what one prompt string
// carries (200 exchanges on the days before, 40 today: about 230 KB), ten recalled memories and
// a food message, which attached the food playbook until v5.2.0 (RB-Q3a: no playbook rides any
// turn now). v5.1.1 sent that turn whole: its window (the newest 5 KB of history, the owner's
// day up to its cap) and the ten memories. Sized from the body budget the turn was refused at
// round 0 and gave up recall and the playbook before any history.
namespace {
const Head kLoopOffHeads[] = {
    {"mistral/large-4", "mistral", "mistral-large-4-0", "mistral"},
    {"mistral/medium", "mistral", "mistral-medium-latest", "mistral"},
    {"zai/glm-5.3", "zai", "glm-5.3", "custom"},
};

int countOf(const std::string& hay, const std::string& needle) {
  int n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++;
  return n;
}

void loopOffParityOn(const Head& h) {
  LogCapture::install();
  Shape s;
  s.before = 200;
  s.today = 40;
  s.recalled = 10;
  s.loop = false;
  Rig r(h, s);
  r.eng->handleMessage("Lunch was 200 g of chicken breast with rice and a salad. Log it.", "Roy", "1001");
  TEST_ASSERT_FALSE_MESSAGE(r.wire.http.seen.empty(), h.name);
  const std::string& body = r.wire.http.seen[0].body;
  const bool refused = LogCapture::contains(" B budget") || LogCapture::contains("refused the request for its size");
  const int memories = countOf(body, "a remembered fact");
  const bool playbook = body.find("[FOOD PLAYBOOK") != std::string::npos;
  // v5.1.1's window for this head: its high water, and the owner's day up to its cap.
  const uint32_t v511Ctx = orch::legacyCtxTokens(h.host, h.model);
  const size_t high = orch::deriveBudget(v511Ctx, {}).recentConvBytes;
  const size_t window = std::max(high, agent::historyDayBytes(v511Ctx));
  const agent::HistoryWindow& w = r.eng->lastHistoryWindow();
  printf("loop-off parity %s: round 0 body %zu B, refused %d, memories %d of 10, playbook %d, history %zu B in %zu "
         "turns (v5.1.1's window %zu B)\n",
         h.name, body.size(), refused, memories, playbook, w.bytes, w.kept.size(), window);
  TEST_ASSERT_FALSE_MESSAGE(refused, h.name);   // round 0 went out as composed: no refusal, no refit
  TEST_ASSERT_EQUAL_MESSAGE(10, memories, h.name);
  TEST_ASSERT_FALSE_MESSAGE(playbook, h.name);   // RB-Q3a
  TEST_ASSERT_TRUE_MESSAGE(body.find("LEFT OUT TO FIT THIS REQUEST") == std::string::npos, h.name);
  // The history is v5.1.1's window (the newest turn in it), in the flat form, and the rest is
  // named: the shift's hidden rows by the coverage line.
  TEST_ASSERT_TRUE_MESSAGE(w.bytes > 0 && w.bytes <= window, h.name);
  TEST_ASSERT_TRUE_MESSAGE(body.find("tok" + r.rows[r.rows.size() - 2].id.substr(1)) != std::string::npos, h.name);
  TEST_ASSERT_TRUE_MESSAGE(body.find(kFlatHistoryHeader) != std::string::npos, h.name);
  TEST_ASSERT_TRUE_MESSAGE(body.find("CONVERSATION COVERAGE") != std::string::npos, h.name);
  TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains(" hist 5/2/2 KB") || LogCapture::contains(" hist 4/2/2 KB"), h.name);
}
}  // namespace

static void test_a_loop_off_large_4_head_keeps_recall_as_v5_1_1_did() { loopOffParityOn(kLoopOffHeads[0]); }
static void test_a_loop_off_medium_head_keeps_recall_as_v5_1_1_did() { loopOffParityOn(kLoopOffHeads[1]); }
static void test_a_loop_off_glm_head_keeps_recall_as_v5_1_1_did() { loopOffParityOn(kLoopOffHeads[2]); }

// The loop state the budget reads is the owner's setting, never one turn's heap gate. A turn
// the gate sends as a single shot (entry heap in the 2 KB between the turn's floor and the
// loop's) on a head with the setting on keeps the head's standing marks: nothing shifts and
// the cache keeps its bound, so the turns after it carry the conversation as before. The
// price is this one request: its history is over the string, so it is refused locally and
// fitted by the give-up order (recall first), and it names what it left out. The known limit
// of sizing by the setting, pinned so a change to it is a decision.
static void test_a_heap_gated_single_shot_keeps_the_head_s_standing_marks() {
  LogCapture::install();
  Shape s;
  s.before = 60;
  s.today = 12;
  s.recalled = 10;
  Rig r(kLoopOffHeads[0], s);   // Large 4, the tool loop on
  r.plat.heap = 13000;          // over the turn floor (12,000), under the loop's (14,000)
  r.eng->handleMessage("about my day", "Roy", "1001");
  TEST_ASSERT_FALSE(r.wire.http.seen.empty());
  const std::string& body = r.wire.http.seen[0].body;
  TEST_ASSERT_TRUE(LogCapture::contains("tool-loop DEFERRED"));
  TEST_ASSERT_TRUE(LogCapture::contains(" hist 73/58/29 KB"));      // the head's marks (RB-BG: the standard profile)
  TEST_ASSERT_FALSE(LogCapture::contains("overflow shift"));
  TEST_ASSERT_TRUE(r.marks.softStart.empty());
  TEST_ASSERT_TRUE(body.find(kFlatHistoryHeader) != std::string::npos);   // it did go out flat
  TEST_ASSERT_TRUE(LogCapture::contains("refused the request for its size"));   // locally, before any send
  TEST_ASSERT_TRUE(body.find("LEFT OUT TO FIT THIS REQUEST") != std::string::npos);
  TEST_ASSERT_EQUAL(0, countOf(body, "a remembered fact"));               // recall is the first rung to go
  Verdict v;
  checkRows(r, s, body, "heap-gated single shot", v);                     // every live row sent or named
  TEST_ASSERT_EQUAL_MESSAGE(0, v.violations, v.first.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_live_row_is_sent_or_named_on_every_head_wire);
  RUN_TEST(test_a_loop_off_turn_is_never_refused_at_round_0_at_its_own_budget);
  RUN_TEST(test_the_15_nov_complaint_keeps_the_estimate_and_the_comparison);
  RUN_TEST(test_a_loop_off_large_4_head_keeps_recall_as_v5_1_1_did);
  RUN_TEST(test_a_loop_off_medium_head_keeps_recall_as_v5_1_1_did);
  RUN_TEST(test_a_loop_off_glm_head_keeps_recall_as_v5_1_1_did);
  RUN_TEST(test_a_heap_gated_single_shot_keeps_the_head_s_standing_marks);
  return UNITY_END();
}
