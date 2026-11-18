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

#include <cstdio>
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
#include "nimbus/harness/turn_contract.h"   // kEmptyAnswer
#include "nimbus/orch/journal.h"

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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_correct_reply_is_delivered_once_unchanged_on_every_path);
  RUN_TEST(test_consecutive_turns_deliver_one_message_each);
  RUN_TEST(test_the_seam_never_loses_a_turn);
  return UNITY_END();
}
