// test_delivery_e1 - the old engine's reply seam delivers what the model wrote, once.
//
// The claim scanner, the claim check, the spawn-claim and owner-voice checks are gone
// (lane RB-Q1b, v5.2.0). They read one turn and matched words, so they sent correct
// answers back for a correction round or appended "(Correction: ...)" and "(Note: ...)"
// lines to them: 39 of 702 replies in the owner's month carried a harness line, 21
// incidents (rebuild/evidence/DIAGNOSIS.md, RC-09). The classes below are the
// diagnosis's: a true statement about an earlier turn (the owner said it at 13h, the
// device denied it at 20h), a figure credited to a label or a plan the chat already
// holds, a check done in an earlier turn, a sub-agent started in an earlier turn, the
// owner's own earlier words quoted back, a plain acknowledgement.
//
// What holds for every class, on every wire (Anthropic Messages, OpenAI Responses, the
// chat dialect) and on every path the guards used to sit on (a tool-less reply, a reply
// after a tool error, a group chat, the voice channel, the ask field, a scheduled routine):
//   - the owner gets exactly one message, and it is the model's reply byte for byte;
//   - no "(Correction" or "(Note" text reaches the owner or the history row;
//   - the turn costs the rounds the model asked for and no correction round;
//   - the history row holds what the owner got.
// The engine is bound like the device: the real fabric loop, the real wire parsers and
// apply, over a scripted transport.
#include <unity.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_fabric.h"
#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/turn_contract.h"   // kEmptyAnswer, salvageForOwner
#include "nimbus/orch/deliver_text.h"         // the send seam (lane RB-Q1c)
#include "nimbus/orch/journal.h"
#include "nimbus/tg_html.h"

namespace {

namespace orch = nimbus::orch;
namespace providers = agent::providers;
using agent::TurnEngine;
using harness_test::Exchange;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::FakeProviderDeps;
using harness_test::LogCapture;

const char* const kWires[] = {"anthropic", "openai", "mistral"};

const char* apiHostOf(const std::string& host) {
  if (host == "anthropic") return "api.anthropic.com";
  if (host == "openai") return "api.openai.com";
  return "api.mistral.ai";
}

std::string jsonEscaped(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out += c;
  }
  return out;
}

// The model's final answer on each wire (the old engine's envelope, as the device sends it).
std::string finalBody(const std::string& host, const std::string& reply) {
  const std::string args = "{\"reply\":\"" + jsonEscaped(reply) + "\"}";
  if (host == "anthropic")
    return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\",\"input\":" + args +
           "}],\"stop_reason\":\"tool_use\"}";
  if (host == "openai")
    return "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"c\","
           "\"name\":\"orch_turn\",\"arguments\":\"" + jsonEscaped(args) + "\"}]}";
  return "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":"
         "\"abcdefghi\",\"function\":{\"name\":\"orch_turn\",\"arguments\":\"" + jsonEscaped(args) + "\"}}]}}]}";
}

// The same answer with the text in the ask field instead (it reaches the owner as the message).
std::string askBody(const std::string& host, const std::string& ask) {
  std::string b = finalBody(host, "");
  const std::string from = host == "anthropic" ? "{\"reply\":\"\"}" : jsonEscaped("{\"reply\":\"\"}");
  const std::string to = host == "anthropic" ? "{\"reply\":\"\",\"ask\":\"" + jsonEscaped(ask) + "\"}"
                                             : jsonEscaped("{\"reply\":\"\",\"ask\":\"" + jsonEscaped(ask) + "\"}");
  return b.replace(b.find(from), from.size(), to);
}

// One tool round: the model calls memory_search.
std::string toolBody(const std::string& host) {
  if (host == "anthropic")
    return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"toolcall1\",\"name\":\"memory_search\",\"input\":{\"query\":"
           "\"x\"}}],\"stop_reason\":\"tool_use\"}";
  if (host == "openai")
    return "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"toolcall1\","
           "\"name\":\"memory_search\",\"arguments\":\"{\\\"query\\\":\\\"x\\\"}\"}]}";
  return "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":"
         "\"toolcall1\",\"function\":{\"name\":\"memory_search\",\"arguments\":\"{\\\"query\\\":\\\"x\\\"}\"}}]}}]}";
}

Exchange ok(const std::string& host, const std::string& body) {
  Exchange e;
  e.expectHost = apiHostOf(host);
  e.body = body;
  return e;
}

// The paths the guards sat on.
enum class Path { ToolLess, AfterToolError, GroupChat, Voice, AskField, Scheduled };
const Path kPaths[] = {Path::ToolLess, Path::AfterToolError, Path::GroupChat, Path::Voice, Path::AskField,
                       Path::Scheduled};
const char* pathName(Path p) {
  switch (p) {
    case Path::ToolLess: return "tool-less";
    case Path::AfterToolError: return "after a tool error";
    case Path::GroupChat: return "group chat";
    case Path::Voice: return "voice";
    case Path::AskField: return "the ask field";
    case Path::Scheduled: return "a scheduled routine";
  }
  return "?";
}
const char* chatOf(Path p) {
  if (p == Path::GroupChat) return "-1001234567890";
  if (p == Path::Voice) return "voice";
  return "1001";
}

// One diagnosis class: the earlier turn the chat holds (owner, then assistant; may be
// empty), this turn's owner message, and the model's correct reply.
struct Case {
  const char* cls;
  const char* earlierOwner;
  const char* earlierReply;
  const char* owner;
  const char* reply;
};
const Case kCases[] = {
    {"a save in an earlier turn (17 Nov: the owner said it at 13h, the device denied it at 20h)",
     "log porridge with blueberries for breakfast", "Logged: porridge with blueberries, 13:02.",
     "did you log my breakfast?", "Yes, I logged it at 13:02: porridge with blueberries."},
    {"the owner's own earlier words quoted back", "I had porridge for breakfast and skipped lunch",
     "Got it.", "what did I eat today?", "You said you had porridge for breakfast and skipped lunch."},
    {"a figure credited to the label the owner sent earlier",
     "the label says 120 kcal per 100 g, I ate 200 g", "That is 240 kcal.", "how many calories was that?",
     "Per the label, 200 g is 240 kcal."},
    {"a daily target the chat already holds", "my daily target is 2,400 kcal", "Noted: 2,400 kcal a day.",
     "how much is left?", "You have 400 kcal left of your 2,400 kcal daily target."},
    {"a check done in an earlier turn", "when does the 7:10 train leave?",
     "I checked National Rail: the 7:10 runs on time.", "is it still on time?",
     "I checked it live earlier: the 7:10 was on time then. Ask me to look again if you want a fresh read."},
    {"a sub-agent started in an earlier turn", "research flights to Lisbon",
     "I've started a sub-agent on the Lisbon flights.", "any news?",
     "The sub-agent I started on the Lisbon flights is still running; its result will arrive here."},
    {"a plain acknowledgement", "", "", "remember that I prefer window seats", "Noted."},
    {"a cause the owner reported earlier", "my phone's Telegram app keeps crashing",
     "That sounds frustrating.", "why did my photo not arrive?",
     "Your phone's Telegram app was crashing earlier, so the photo may not have been sent."},
    // The reply guard's classes (lane RB-Q1c): each was a correct reply the guard cut or sent back.
    {"how to undo, in the owner's words (the owner-mechanics drop)", "", "", "log 2 eggs for breakfast",
     "Logged 2 eggs, 180 kcal. To undo, set the kcal field to 0 and send the row again."},
    {"advice that reads like a rule (the echo drop)", "", "", "any tips for my conference bio?",
     "Three tips. Concise. No em dashes. Lead with what you build."},
    {"a file the owner keeps, by its name (the plumbing check)", "make me a packing list", "Done.",
     "where did you put my packing list?", "It is in travel/packing.md, with 14 items."},
    {"a promise of later work with a routine behind it", "remind me about the train every morning",
     "Set for 06:30 every day.", "will you check the train tomorrow?",
     "Yes. I'll check the 7:10 tomorrow at 06:30 and tell you here."},
    {"numbers and their arithmetic", "the label says 120 kcal per 100 g", "Noted.", "I ate 360 g, what is that?",
     "3.6 x 120 kcal = 432 kcal. The day is now at 1,968 of 2,400 kcal, 432 kcal to go."},
};

std::vector<orch::EpisodicMessage> readRows(const std::vector<orch::EpisodicMessage>& rows,
                                            const agent::HistoryQuery& hq) {
  std::vector<orch::EpisodicMessage> out;
  if (!hq.after.empty()) {
    for (auto it = rows.begin(); it != rows.end() && out.size() < hq.limit; ++it)
      if (it->id > hq.after) out.push_back(*it);
    return out;
  }
  for (auto it = rows.rbegin(); it != rows.rend() && out.size() < hq.limit; ++it)
    if (hq.before.empty() || it->id < hq.before) out.push_back(*it);
  return out;
}

// The engine bound like the device: the real fabric loop on one scripted transport,
// history rows from a fake episodic store (histV2), the real apply.
struct Rig {
  FakePlatform plat;
  FakeConfig cfg;
  FakeProviderDeps wire;
  providers::ProviderDeps pd;
  std::vector<std::pair<std::string, std::string>> delivered;   // chat, text
  std::vector<std::pair<std::string, std::string>> captured;    // the assistant rows apply captured
  std::vector<orch::EpisodicMessage> rows;
  std::vector<orch::ToolRegistry::Spec> specs;
  std::unique_ptr<TurnEngine> eng;
  bool toolFails = false;
  int dispatched = 0;
  int seq = 0;

  explicit Rig(const std::string& host) {
    providers::clearHostParks();
    cfg.keyed = {host};
    cfg.priority = host;
    cfg.orchHost = host;
    specs.push_back({"memory_search", "search the memory",
                     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}}}"});
    pd = wire.contract();
    pd.orchModel = [](const char* h) {
      return std::string(h) == "mistral" ? std::string("mistral-medium-latest") : "model-" + std::string(h);
    };
    pd.nowMs = [this] { return plat.ms; };
    pd.key = [this](const char* h) {
      auto it = cfg.keys.find(h);
      return it == cfg.keys.end() ? std::string() : it->second;
    };
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [this](const std::string& chat, const std::string& t) { delivered.push_back({chat, t}); };
    d.apply.deliver = d.deliver;
    d.apply.captureAssistant = [this](const std::string& chat, const std::string& t) { captured.push_back({chat, t}); };
    d.apply.principalFor = [](const std::string& chat) {
      return orch::principalForRole(chat, chat.rfind("-100", 0) == 0 ? orch::Role::User : orch::Role::Admin);
    };
    d.toolSpecs = [this](const orch::Principal&) { return specs; };
    d.mcpDispatch = [this](const std::string&, const orch::Principal&) -> std::string {
      dispatched++;
      if (toolFails)
        return "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32000,\"message\":\"the search index is "
               "unavailable\"}}";
      return "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}}";
    };
    d.hosts.fabric = [this](const std::vector<std::string>& hl, const std::string& ins, const std::string& inp,
                            std::string& out, std::string& err, const agent::HeadTools& ht, orch::TurnSpend* spend,
                            const std::function<void(const std::string&, const std::string&)>& notify) {
      return providers::runFabricLoop(pd, hl, ins, inp, out, err, ht, spend, notify);
    };
    d.hosts.add(host, [this, host](std::string& conv, const std::string& ins, const std::string& inp,
                                   std::string& out, std::string& err, const agent::HeadTools* tools,
                                   orch::TokenUsage* use) {
      if (host == "mistral") return providers::orchTurnMistral(pd, conv, ins, inp, out, err, tools, use);
      if (host == "anthropic") return providers::orchTurnAnthropic(pd, conv, ins, inp, out, err, tools, use);
      return providers::orchTurnOpenAI(pd, conv, ins, inp, out, err, tools, use);
    });
    d.hosts.fabricSupports = [](const std::string& h) { return providers::fabricSupports(h); };
    d.hosts.parked = [this](const std::string& h, providers::HostPark* why) { return providers::hostParked(pd, h, why); };
    d.hosts.failed = [this](const std::string& h, orch::StepErr k, int status) {
      providers::noteHostFailed(pd, h, k, status);
    };
    d.hosts.answered = [](const std::string& h) { providers::unparkHost(h); };
    d.histV2 = [] { return true; };
    d.historyRows = [this](const std::string&, const agent::HistoryQuery& hq) { return readRows(rows, hq); };
    d.captureUserRow = [this](const std::string& chat, const std::string& t, const std::string&) {
      return addRow(chat, "user", t);
    };
    d.captureTrigger = [](const std::string&, const std::string&, const std::string&) {};
    d.historyMarks = [](const std::string&) { return agent::HistoryMarks(); };
    eng.reset(new TurnEngine(std::move(d)));
  }
  ~Rig() { providers::clearHostParks(); }

  std::string addRow(const std::string& chat, const std::string& role, const std::string& text) {
    orch::EpisodicMessage m;
    char id[16];
    snprintf(id, sizeof id, "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = chat;
    m.tsHours = 490000;
    m.role = role;
    m.kind = orch::MsgKind::Message;
    m.text = text;
    m.tags = role == "user" ? "from:Roy" : "";
    rows.push_back(m);
    return m.id;
  }
  void script(const Exchange& e) { wire.http.script.push_back(e); }
  size_t requests() const { return wire.http.seen.size(); }
};

std::string describe(const Case& c, const char* host, Path p, const std::string& got) {
  std::string m = std::string(c.cls) + " | " + host + " | " + pathName(p) + " | delivered: " + got;
  if (m.size() > 700) m.resize(700);
  return m;
}

// What the owner got and what was sent, against the seam's contract: "" when it held.
std::string contractBroken(const Rig& r, const Case& c, const std::string& chat, size_t rounds, Path p,
                           const std::string& where) {
  // Exactly one message, to the chat that asked, byte for byte the model's reply.
  if (r.delivered.size() != 1) return "delivered " + std::to_string(r.delivered.size()) + " messages: " + where;
  if (r.delivered[0].first != chat) return "delivered to another chat: " + where;
  if (r.delivered[0].second != c.reply) return "the reply changed: " + where;
  // The rounds the model asked for, and no correction round.
  if (r.requests() != rounds) return std::to_string(r.requests()) + " requests: " + where;
  if (p == Path::AfterToolError &&
      (r.dispatched != 1 || r.wire.http.seen.back().body.find("the search index is unavailable") == std::string::npos))
    return "the tool error did not reach the model: " + where;
  for (const auto& req : r.wire.http.seen)
    if (req.body.find("CLAIM CHECK") != std::string::npos || req.body.find("TURN CHECK") != std::string::npos)
      return "a check text in a request: " + where;
  // History holds what the owner got: one assistant row, the reply as delivered.
  if (r.captured.size() != 1 || r.captured[0].second != c.reply) return "the history row differs: " + where;
  return std::string();
}

// One turn of one class on one wire and one path: "" when the seam kept its contract,
// else what broke it. (A harness line changes the reply, so "the reply changed" covers it.)
std::string runCase(const Case& c, const char* host, Path p) {
  Rig r(host);
  const std::string chat = chatOf(p);
  if (c.earlierOwner[0]) {
    r.addRow(chat, "user", c.earlierOwner);
    r.addRow(chat, "assistant", c.earlierReply);
  }
  size_t rounds = 1;
  if (p == Path::AfterToolError) {
    r.toolFails = true;
    r.script(ok(host, toolBody(host)));
    rounds = 2;
  }
  r.script(ok(host, p == Path::AskField ? askBody(host, c.reply) : finalBody(host, c.reply)));
  if (p == Path::Scheduled)
    r.eng->injectScheduledTurn(chat, c.owner, "check");
  else
    r.eng->handleMessage(c.owner, "Roy", chat);
  std::string all;
  for (const auto& d : r.delivered) all += d.second + "\n---\n";
  return contractBroken(r, c, chat, rounds, p, describe(c, host, p, all));
}

}  // namespace

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

// The class sweep: every diagnosis class, every wire, every path.
static void test_a_correct_reply_is_delivered_once_unchanged_on_every_path() {
  std::string failures;
  int failed = 0, cases = 0;
  for (const Case& c : kCases)
    for (const char* host : kWires)
      for (Path p : kPaths) {
        cases++;
        const std::string why = runCase(c, host, p);
        if (why.empty()) continue;
        failed++;
        printf("FAIL %s\n", why.c_str());
        if (failures.size() < 1500) failures += why + "\n";
      }
  printf("delivery_e1 sweep: %d of %d cases kept the contract\n", cases - failed, cases);
  TEST_ASSERT_EQUAL_MESSAGE(0, failed, failures.c_str());
}

// Two turns in a row: the second turn's reply is delivered once and the first is not
// sent again (nothing replays or duplicates a reply across turns).
static void test_consecutive_turns_deliver_one_message_each() {
  for (const char* host : kWires) {
    Rig r(host);
    r.script(ok(host, finalBody(host, kCases[0].reply)));
    r.script(ok(host, finalBody(host, kCases[1].reply)));
    r.eng->handleMessage(kCases[0].owner, "Roy", "1001");
    r.eng->handleMessage(kCases[1].owner, "Roy", "1001");
    TEST_ASSERT_EQUAL_MESSAGE(2, (int)r.delivered.size(), host);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(kCases[0].reply, r.delivered[0].second.c_str(), host);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(kCases[1].reply, r.delivered[1].second.c_str(), host);
    TEST_ASSERT_EQUAL_MESSAGE(2, (int)r.requests(), host);
    TEST_ASSERT_EQUAL_MESSAGE(2, (int)r.captured.size(), host);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(kCases[1].reply, r.captured[1].second.c_str(), host);
  }
}

// Counter-test: the seam still sends what the model really says about its own failure
// (an honest "not saved" reply is delivered as written), and an empty answer is never
// lost: after its one retry the owner hears that the model returned nothing (kEmptyAnswer),
// once, and history holds that line.
static void test_the_seam_never_loses_a_turn() {
  for (const char* host : kWires) {
    Rig r(host);
    r.script(ok(host, finalBody(host, "That could not be saved: the card is full.")));
    r.eng->handleMessage("save my notes", "Roy", "1001");
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), host);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("That could not be saved: the card is full.", r.delivered[0].second.c_str(), host);
    Rig e(host);
    e.script(ok(host, finalBody(host, "")));
    e.script(ok(host, finalBody(host, "")));
    e.eng->handleMessage("hello?", "Roy", "1001");
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)e.delivered.size(), host);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(agent::kEmptyAnswer, e.delivered[0].second.c_str(), host);
    TEST_ASSERT_EQUAL_MESSAGE(2, (int)e.requests(), host);
  }
}

// The plain-chat cap of 2 tool rounds is gone (lane RB-Q1c, RC-14): a chat line takes the
// rounds it needs within the turn's round budget, like any other turn. The 1 Nov hosted case
// needed three; five shows there is no smaller cap left. Every wire.
static void test_a_plain_chat_turn_takes_the_rounds_it_needs() {
  for (const char* host : kWires)
    for (int tools : {3, 5}) {
      Rig r(host);
      for (int i = 0; i < tools; i++) r.script(ok(host, toolBody(host)));
      r.script(ok(host, finalBody(host, "We are at step four of six; the oven is preheating.")));
      r.eng->handleMessage("so where are we at?", "Roy", "1001");
      char what[96];
      snprintf(what, sizeof what, "%s, %d tool rounds", host, tools);
      TEST_ASSERT_EQUAL_MESSAGE(tools + 1, (int)r.requests(), what);
      TEST_ASSERT_EQUAL_MESSAGE(tools, r.dispatched, what);
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), what);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("We are at step four of six; the oven is preheating.",
                                       r.delivered[0].second.c_str(), what);
      TEST_ASSERT_FALSE_MESSAGE(LogCapture::contains("plain conversational turn"), what);
    }
}

// Counter-test: the turn's round budget still holds for a chat line. A model that never stops
// calling tools gets the configured rounds and then the tool-less final round, and the owner
// still gets one message.
static void test_the_round_budget_still_holds_for_a_chat_line() {
  for (const char* host : kWires) {
    Rig r(host);
    r.cfg.rounds = 3;
    for (int i = 0; i < 3; i++) r.script(ok(host, toolBody(host)));
    r.script(ok(host, finalBody(host, "Here is what I have so far.")));
    r.eng->handleMessage("so where are we at?", "Roy", "1001");
    TEST_ASSERT_EQUAL_MESSAGE(4, (int)r.requests(), host);
    TEST_ASSERT_EQUAL_MESSAGE(3, r.dispatched, host);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), host);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Here is what I have so far.", r.delivered[0].second.c_str(), host);
  }
}

// The placeholder check the old engine keeps (turn_contract.h, RB-Q1a): a forced final round
// that writes filler is sent back once, and filler twice ends in the failure line, never the
// filler.
static void test_a_forced_final_round_of_filler_is_still_caught() {
  for (const char* host : kWires) {
    Rig r(host);
    r.cfg.rounds = 2;
    for (int i = 0; i < 2; i++) r.script(ok(host, toolBody(host)));
    r.script(ok(host, finalBody(host, "placeholder")));
    r.script(ok(host, finalBody(host, "placeholder")));
    r.eng->handleMessage("so where are we at?", "Roy", "1001");
    TEST_ASSERT_EQUAL_MESSAGE(4, (int)r.requests(), host);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), host);
    TEST_ASSERT_TRUE_MESSAGE(r.delivered[0].second.find("didn't finish") != std::string::npos,
                             r.delivered[0].second.c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.delivered[0].second.find("placeholder") == std::string::npos,
                             r.delivered[0].second.c_str());
  }
}

// The salvage path (an answer cut short or unreadable, delivered without apply) keeps every
// sentence the model wrote: no later-promise line, no sentence dropped. Only the cut marker,
// exactly when the answer was cut.
static void test_a_salvaged_answer_keeps_every_sentence() {
  const char* text = "I'll finish the rest next turn. To undo, re-send the row with 0 kcal.";
  const agent::SalvageOutcome cut =
      agent::salvageForOwner(std::string("{\"reply\":\"") + text + "\"", "truncated", true);
  TEST_ASSERT_EQUAL_STRING((std::string(text) + "\n\n" + orch::kSalvageCutMarker).c_str(), cut.text.c_str());
  const agent::SalvageOutcome whole =
      agent::salvageForOwner(std::string("{\"reply\":\"") + text + "\",\"memory\":", "unreadable", false);
  TEST_ASSERT_EQUAL_STRING(text, whole.text.c_str());
}

// ---- the send seam: the one deliver function the device and nimbusd call (lane RB-Q1c) ----

// Every word of `want` (runs of letters and digits), in order, inside `got`.
static bool wordsInOrder(const std::string& want, const std::string& got) {
  size_t at = 0;
  for (size_t i = 0; i < want.size();) {
    if (!isalnum((unsigned char)want[i])) {
      i++;
      continue;
    }
    size_t j = i;
    while (j < want.size() && isalnum((unsigned char)want[j])) j++;
    at = got.find(want.substr(i, j - i), at);
    if (at == std::string::npos) return false;
    at += j - i;
    i = j;
  }
  return true;
}

// What the chat shows for a Telegram HTML body: tags out, the four entities back.
static std::string shown(const std::string& html) {
  std::string out;
  for (size_t i = 0; i < html.size(); i++) {
    if (html[i] == '<') {
      const size_t e = html.find('>', i);
      if (e == std::string::npos) break;
      i = e;
      continue;
    }
    if (html[i] == '&') {
      static const char* const kEnt[][2] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}};
      bool hit = false;
      for (const auto& en : kEnt)
        if (html.compare(i, strlen(en[0]), en[0]) == 0) {
          out += en[1];
          i += strlen(en[0]) - 1;
          hit = true;
          break;
        }
      if (hit) continue;
    }
    out += html[i];
  }
  return out;
}

// "" when Telegram would accept `html` as parse_mode=HTML: every '<' opens or closes one of the
// tags tgHtml writes, closes nest, nothing stays open, and every '&' starts an entity.
static std::string htmlRefusal(const std::string& html) {
  std::vector<std::string> open;
  for (size_t i = 0; i < html.size(); i++) {
    if (html[i] == '&') {
      const size_t semi = html.find(';', i);
      if (semi == std::string::npos || semi - i > 6) return "a bare &";
      continue;
    }
    if (html[i] == '>') return "a bare >";
    if (html[i] != '<') continue;
    const size_t e = html.find('>', i);
    if (e == std::string::npos) return "an unclosed <";
    std::string tag = html.substr(i + 1, e - i - 1);
    const bool close = !tag.empty() && tag[0] == '/';
    if (close) tag = tag.substr(1);
    const std::string name = tag.substr(0, tag.find(' '));
    if (name != "b" && name != "i" && name != "code" && name != "pre" && name != "a") return "a <" + name + "> tag";
    if (close) {
      if (open.empty() || open.back() != name) return "a stray </" + name + ">";
      open.pop_back();
    } else {
      open.push_back(name);
    }
    i = e;
  }
  return open.empty() ? std::string() : "an unclosed <" + open.back() + ">";
}

// The reply guard deleted sentences at this seam (dropEchoSentences, dropOwnerMechanics): every
// class reply now reaches Telegram whole, as one part, its plain text the reply byte for byte.
static void test_the_send_seam_sends_every_sentence() {
  for (const Case& c : kCases) {
    const std::vector<orch::TelegramPart> parts = orch::deliverText(c.reply);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)parts.size(), c.cls);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reply, parts[0].plain.c_str(), c.cls);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(nimbus::tgHtml(c.reply).c_str(), parts[0].html.c_str(), c.cls);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reply, shown(parts[0].html).c_str(), c.cls);
  }
}

// Markup Telegram would refuse is made safe (escaped, or converted only when it pairs), never
// dropped with its sentence: every word the model wrote is shown, and the HTML is accepted.
static void test_markup_telegram_would_refuse_is_made_safe() {
  const char* const kTexts[] = {
      "Use <b>bold here and a < b & c > d. The rest stays.",
      "**half open and `tick. Then a second sentence.",
      "<script>alert(1)</script> is shown as text. So is </i> a stray close.",
      "[click](javascript:alert(1)) stays text. Only a web link converts.",
      "A <provider> tag, List<String> and <3 stay. Fish & chips: 850 kcal.",
      "```\nif (a < b) { return \"<x>\"; }\n```\nThe code above compares.",
      "*almost italic\nacross lines* stays literal. _snake_case_ too.",
      "Caf\xC3\xA9 at 3 \xE2\x82\xAC, soup \xF0\x9F\x8D\xB2 and 120 kcal/100 g.",
  };
  for (const char* t : kTexts) {
    const std::vector<orch::TelegramPart> parts = orch::deliverText(t);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)parts.size(), t);
    const std::string why = htmlRefusal(parts[0].html);
    TEST_ASSERT_TRUE_MESSAGE(why.empty(), (why + ": " + parts[0].html).c_str());
    TEST_ASSERT_TRUE_MESSAGE(wordsInOrder(t, shown(parts[0].html)), parts[0].html.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(t, parts[0].plain.c_str(), t);   // the plain resend is the text
  }
  // A web link converts, its address escaped inside the tag.
  const std::vector<orch::TelegramPart> link = orch::deliverText("See [docs](https://example.com/a?b=1&c=2) now.");
  TEST_ASSERT_TRUE_MESSAGE(htmlRefusal(link[0].html).empty(), link[0].html.c_str());
  TEST_ASSERT_EQUAL_STRING("See docs now.", shown(link[0].html).c_str());
  // The sanitiser is the one content transform: tool-call markup and reasoning go, the
  // sentences around them stay whole.
  const std::vector<orch::TelegramPart> m =
      orch::deliverText("<think>plan the reply</think>Logged 2 eggs. <tool_call>{\"name\":\"x\"}</tool_call>Anything else?");
  TEST_ASSERT_EQUAL(1, (int)m.size());
  TEST_ASSERT_TRUE_MESSAGE(wordsInOrder("Logged 2 eggs. Anything else?", m[0].plain), m[0].plain.c_str());
  TEST_ASSERT_TRUE_MESSAGE(m[0].plain.find("plan the reply") == std::string::npos, m[0].plain.c_str());
  TEST_ASSERT_TRUE_MESSAGE(m[0].plain.find("tool_call") == std::string::npos, m[0].plain.c_str());
  TEST_ASSERT_TRUE(htmlRefusal(m[0].html).empty());
}

// A long reply is split into parts Telegram takes, none lost or duplicated: the parts are the
// text byte for byte, each at most kTelegramPartBytes, cut at a line where one is near, never
// inside a UTF-8 character, and nothing (no "(k/N)" header) is added to them.
static void test_a_long_reply_is_split_without_a_byte_lost_or_added() {
  std::string lines;
  for (int i = 0; i < 400; ++i) lines += "Line " + std::to_string(i) + ": the boat is the Nereid II.\n";
  std::string accents;
  for (int i = 0; i < 3000; ++i) accents += i % 3 ? "\xC3\xA9" : "\xE2\x82\xAC";   // no space to cut at
  std::string paras;
  for (int i = 0; i < 60; ++i) paras += "Paragraph " + std::to_string(i) + std::string(120, 'x') + ".\n\n";
  for (const std::string* t : {&lines, &accents, &paras}) {
    const std::vector<std::string> parts = orch::splitDeliverable(*t);
    TEST_ASSERT_TRUE(parts.size() > 1);
    std::string joined;
    for (const std::string& p : parts) {
      TEST_ASSERT_TRUE(p.size() <= orch::kTelegramPartBytes);
      TEST_ASSERT_FALSE_MESSAGE(((unsigned char)p[0] & 0xC0) == 0x80, "a part starts inside a character");
      const unsigned char last = (unsigned char)p.back();
      TEST_ASSERT_FALSE_MESSAGE(last >= 0xC0, "a part ends with a character's lead byte");
      joined += p;
    }
    TEST_ASSERT_TRUE_MESSAGE(joined == *t, "the parts are not the text");
  }
  for (const std::string& p : orch::splitDeliverable(lines)) TEST_ASSERT_EQUAL('\n', p.back());
  for (const std::string& p : orch::splitDeliverable(paras))
    TEST_ASSERT_TRUE(p.size() >= 2 && p.compare(p.size() - 2, 2, "\n\n") == 0);
  // The whole pipeline: one Telegram part per split part, each plain text that part.
  const std::vector<orch::TelegramPart> tp = orch::deliverText(lines);
  const std::vector<std::string> sp = orch::splitDeliverable(lines);
  TEST_ASSERT_EQUAL(sp.size(), tp.size());
  for (size_t i = 0; i < sp.size(); i++) TEST_ASSERT_EQUAL_STRING(sp[i].c_str(), tp[i].plain.c_str());
  // Nothing to send is no message; a run of blanks alone is never a message of its own.
  TEST_ASSERT_EQUAL(0, (int)orch::deliverText("").size());
  TEST_ASSERT_EQUAL(0, (int)orch::deliverText("  \n ").size());
  const std::vector<std::string> gap = orch::splitDeliverable("a" + std::string(9000, ' ') + "b", 4000);
  std::string gapText;
  for (const std::string& p : gap) {
    TEST_ASSERT_TRUE(p.find_first_not_of(' ') != std::string::npos);
    gapText += p;
  }
  TEST_ASSERT_EQUAL('a', gapText.front());
  TEST_ASSERT_EQUAL('b', gapText.back());
}

// A part is sent as it is, not sanitised again: a word the whole text kept (a literal
// "<reply>" in the middle) stays when a cut puts it at the start of a part.
static void test_a_part_is_not_sanitised_again() {
  const std::string text = std::string(3995, 'x') + " <reply> is the tag the old model wrote.";
  const std::vector<orch::TelegramPart> parts = orch::deliverText(text);
  TEST_ASSERT_EQUAL(2, (int)parts.size());
  TEST_ASSERT_EQUAL_STRING("<reply> is the tag the old model wrote.", parts[1].plain.c_str());
  TEST_ASSERT_TRUE(parts[1].html.find("&lt;reply&gt;") == 0);
}

static std::string slurp(const char* path) {
  std::string src;
  FILE* f = std::fopen(path, "rb");
  if (!f) return src;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) src.append(buf, n);
  std::fclose(f);
  return src;
}

// The body of the function that `sig` opens: from the signature to the first line that is "}".
static std::string bodyOf(const std::string& src, const char* sig) {
  const size_t at = src.find(sig);
  if (at == std::string::npos) return std::string();
  const size_t end = src.find("\n}\n", at);
  return src.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// The device's seam and the daemon's are this deliver function (a pin: both are Arduino or
// socket code that cannot run here): the device splits the deliverable text when it queues a
// reply and sends each part's HTML with the part as the plain resend; the daemon sends what
// deliverText makes. No sentence filter is left on any send path.
static void test_the_device_and_the_daemon_send_through_the_deliver_function() {
  const std::string dev = slurp("src/agent/telegram.cpp");
  TEST_ASSERT_FALSE_MESSAGE(dev.empty(), "src/agent/telegram.cpp not readable");
  const std::string send = bodyOf(dev, "bool send(const String& chatId, const String& text, bool block)");
  TEST_ASSERT_TRUE_MESSAGE(send.find("nimbus::orch::splitDeliverable(nimbus::orch::deliverableText(") !=
                               std::string::npos,
                           "send() must queue the deliver function's parts");
  const std::string impl = bodyOf(dev, "static bool doSendMessageImpl(");
  TEST_ASSERT_TRUE_MESSAGE(impl.find("nimbus::orch::telegramPart(") != std::string::npos,
                           "the drain must send the deliver function's part");
  TEST_ASSERT_TRUE_MESSAGE(impl.find("tgHtml(") == std::string::npos && impl.find("cleanReplyMarkup(") == std::string::npos,
                           "the send seam converts only through the deliver function");
  const std::string hosted = slurp("nimbusd/src/telegram.h");
  TEST_ASSERT_FALSE_MESSAGE(hosted.empty(), "nimbusd/src/telegram.h not readable");
  TEST_ASSERT_TRUE_MESSAGE(hosted.find("nimbus::orch::deliverText(") != std::string::npos,
                           "nimbusd must send the deliver function's parts");
  for (const char* path : {"src/agent/telegram.cpp", "nimbusd/src/telegram.h", "lib/harness/src/engine.cpp",
                           "lib/harness/src/apply.cpp", "lib/harness/src/jobs.cpp", "lib/harness/src/turn_contract.cpp"}) {
    const std::string src = slurp(path);
    TEST_ASSERT_FALSE_MESSAGE(src.empty(), path);
    for (const char* gone : {"dropEchoSentences", "dropOwnerMechanics", "reply_guard.h", "laterPromiseSpan"})
      TEST_ASSERT_TRUE_MESSAGE(src.find(gone) == std::string::npos, (std::string(path) + " holds " + gone).c_str());
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_correct_reply_is_delivered_once_unchanged_on_every_path);
  RUN_TEST(test_consecutive_turns_deliver_one_message_each);
  RUN_TEST(test_the_seam_never_loses_a_turn);
  RUN_TEST(test_a_plain_chat_turn_takes_the_rounds_it_needs);
  RUN_TEST(test_the_round_budget_still_holds_for_a_chat_line);
  RUN_TEST(test_a_forced_final_round_of_filler_is_still_caught);
  RUN_TEST(test_a_salvaged_answer_keeps_every_sentence);
  RUN_TEST(test_the_send_seam_sends_every_sentence);
  RUN_TEST(test_markup_telegram_would_refuse_is_made_safe);
  RUN_TEST(test_a_long_reply_is_split_without_a_byte_lost_or_added);
  RUN_TEST(test_a_part_is_not_sanitised_again);
  RUN_TEST(test_the_device_and_the_daemon_send_through_the_deliver_function);
  return UNITY_END();
}
