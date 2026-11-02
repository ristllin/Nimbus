#include <unity.h>

#include <algorithm>
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
#include "../support/image_turn_fixture.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/jobs.h"
#include "nimbus/harness/providers.h"
#include "nimbus/harness/turn_contract.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/provider_park.h"
#include "nimbus/orch/transcript.h"

// A turn never dies of its own size, and never ends in silence. The rig is the turn
// engine over the REAL fabric loop and the real provider wires on a fake transport,
// fed with what Lumi's turns really looked like:
//   1 Nov 18:51Z "headloop: round 0 body 90519 B over the 85000 B budget - folded 0 B
//   ... nothing left to fold": an auto-synthesis died before any request, and the owner
//   got "That didn't finish" plus the raw results.
//   31 Oct 20:09Z "fabric: mistral step failed (cut: cut at the output limit (6050
//   tokens)) - no same-host retry": a 6-photo album failed after 3m56s.
//   31 Oct "I'll rewrite the whole file to correct it next turn": nothing behind it.
//   1 Nov VN: a forced final answer delivered the literal "placeholder".
// The refusal bodies are the providers' real answers, recorded 1 Nov from each API
// (lanes/OVF/evidence/overflow-refusals.jsonl).

namespace {

namespace orch = nimbus::orch;
namespace providers = agent::providers;
using agent::TurnEngine;
using harness_test::Exchange;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::FakeProviderDeps;
using harness_test::LogCapture;

const char* apiHostOf(const std::string& host) {
  if (host == "anthropic") return "api.anthropic.com";
  if (host == "openai") return "api.openai.com";
  return "api.mistral.ai";
}

std::string jsonEscaped(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

std::string finalBody(const std::string& host, const std::string& reply = "done") {
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

// One tool round: the model asks for memory_search.
std::string toolBody(const std::string& host, int n = 1) {
  const std::string id = "toolcall" + std::to_string(n % 10);
  if (host == "anthropic")
    return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"" + id +
           "\",\"name\":\"memory_search\",\"input\":{\"query\":\"x\"}}],\"stop_reason\":\"tool_use\"}";
  if (host == "openai")
    return "{\"id\":\"r\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"" + id +
           "\",\"name\":\"memory_search\",\"arguments\":\"{\\\"query\\\":\\\"x\\\"}\"}]}";
  return "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"" +
         id + "\",\"function\":{\"name\":\"memory_search\",\"arguments\":\"{\\\"query\\\":\\\"x\\\"}\"}}]}}]}";
}

// A round cut at the output limit, in each provider's words.
std::string cutBody(const std::string& host, const std::string& partial = "Here is the first photo") {
  const std::string args = "{\"reply\":\"" + jsonEscaped(partial);   // no closing: cut
  if (host == "anthropic")
    return "{\"content\":[{\"type\":\"text\",\"text\":\"" + jsonEscaped(partial) + "\"}],\"stop_reason\":\"max_tokens\"}";
  if (host == "openai")
    return "{\"id\":\"r\",\"status\":\"incomplete\",\"incomplete_details\":{\"reason\":\"max_output_tokens\"},"
           "\"output\":[{\"type\":\"function_call\",\"call_id\":\"c\",\"name\":\"orch_turn\",\"arguments\":\"" +
           jsonEscaped(args) + "\"}]}";
  return "{\"choices\":[{\"finish_reason\":\"length\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":"
         "\"abcdefghi\",\"function\":{\"name\":\"orch_turn\",\"arguments\":\"" + jsonEscaped(args) + "\"}}]}}]}";
}

Exchange ok(const std::string& host, const std::string& body) {
  Exchange e;
  e.expectHost = apiHostOf(host);
  e.body = body;
  return e;
}
Exchange refused(const std::string& host, int status, const std::string& body) {
  Exchange e = ok(host, body);
  e.status = status;
  return e;
}

// The recorded refusals (the body is what each provider answered on 1 Nov).
struct Refusal {
  const char* name;
  const char* host;
  int status;
  const char* body;
};
const Refusal kOverflow[] = {
    {"anthropic", "anthropic", 400,
     "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"prompt is too long: "
     "1500057 tokens > 1000000 maximum\"},\"request_id\":\"req_x\"}"},
    {"openai", "openai", 400,
     "{\"error\":{\"message\":\"Your input exceeds the context window of this model. Please adjust your input "
     "and try again.\",\"type\":\"invalid_request_error\",\"param\":\"input\",\"code\":\"context_length_exceeded\"}}"},
    {"mistral", "mistral", 400,
     "{\"object\":\"error\",\"message\":\"Prompt 1500016 > 1048576 maximum context length\",\"type\":"
     "\"invalid_request_prompt_too_long\",\"param\":null,\"code\":\"3059\",\"raw_status_code\":400}"},
};
// Output-cap refusals: a 400 that names a limit and is NOT a context overflow.
const Refusal kNearMiss[] = {
    {"anthropic", "anthropic", 400,
     "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"max_tokens: 90000000 > 128000, "
     "which is the maximum allowed number of output tokens for claude-haiku-5-5\"}}"},
    {"mistral", "mistral", 400,
     "{\"object\":\"error\",\"message\":\"max_tokens is too large: 90000000. This model supports at most 262144 "
     "completion tokens, whereas you provided 90000000.\",\"type\":\"invalid_request_max_tokens\",\"param\":null,"
     "\"code\":\"3058\",\"raw_status_code\":400}"},
};

std::vector<orch::ToolRegistry::Spec> sizedSpecs(size_t totalBytes) {
  std::vector<orch::ToolRegistry::Spec> specs;
  specs.push_back({"memory_search", "search the memory", "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}}}"});
  size_t have = 0;
  for (int i = 0; have < totalBytes; i++) {
    const std::string desc(700, 'd');
    specs.push_back({"tool_" + std::to_string(i), desc, "{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}}}"});
    have += desc.size() + 100;
  }
  return specs;
}

// The engine bound like the device: the real fabric loop and park on one transport,
// history rows from a fake episodic store (histV2), a real job engine.
struct Rig {
  FakePlatform plat;
  FakeConfig cfg;
  FakeProviderDeps wire;
  providers::ProviderDeps pd;
  std::vector<std::string> delivered;
  std::vector<orch::EpisodicMessage> rows;   // oldest first
  std::vector<orch::ToolRegistry::Spec> specs;
  struct MemJournalStore : orch::JournalStore {
    std::map<int, std::string> slots;
    std::string get(int slot) override {
      auto it = slots.find(slot);
      return it == slots.end() ? std::string() : it->second;
    }
    bool put(int slot, const std::string& v) override { slots[slot] = v; return true; }
    bool remove(int slot) override { slots.erase(slot); return true; }
    void clearNs() override { slots.clear(); }
  };
  harness_test::FakeAdapter subAdapter;
  agent::HeavyFabric subFabric;
  MemJournalStore journalStore;
  orch::Journal journal;
  std::unique_ptr<agent::JobEngine> jobs;
  std::unique_ptr<TurnEngine> eng;
  int dispatched = 0;
  std::string toolOutput = "ok";
  bool standing = false;   // a routine or wakeup stands for this chat
  std::vector<bool> trimmedForSize;   // one per turn end
  std::string failedPath;             // the last turn end's failure chain, host by host
  std::vector<std::string> recalledHits;
  int seq = 0;
  std::string turnTag;

  explicit Rig(const std::vector<std::string>& keyed, size_t toolBytes = 0, bool fabric = true) {
    providers::clearHostParks();
    cfg.keyed = std::set<std::string>(keyed.begin(), keyed.end());
    cfg.midFail = fabric;
    std::string prio;
    for (const auto& k : keyed) prio += (prio.empty() ? "" : ",") + k;
    cfg.priority = prio;
    cfg.orchHost = keyed.front();
    specs = sizedSpecs(toolBytes);
    pd = wire.contract();
    pd.nowMs = [this] { return plat.ms; };
    pd.key = [this](const char* h) {
      auto it = cfg.keys.find(h);
      return it == cfg.keys.end() ? std::string() : it->second;
    };
    agent::JobEngine::Deps jd;
    jd.platform = plat.contract();
    jd.deliver = [this](const std::string&, const std::string& t) { delivered.push_back(t); };
    jd.principalFor = [](const std::string& chat) { return orch::principalForRole(chat, orch::Role::Admin); };
    subAdapter.backend = keyed.front().c_str();
    subAdapter.pollScript = {agent::JobState::Running};
    subFabric.registerAdapter(&subAdapter);
    journal.begin(&journalStore);
    jd.fabric = &subFabric;
    jd.journal = &journal;
    jobs.reset(new agent::JobEngine(std::move(jd)));
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.jobs = jobs.get();
    d.deliver = [this](const std::string&, const std::string& t) { delivered.push_back(t); };
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& chat) { return orch::principalForRole(chat, orch::Role::Admin); };
    d.apply.enqueueSpawn = [this](const orch::Spawn& sp, const std::string& chat, bool quiet) {
      return jobs->enqueueSpawn(sp, chat, quiet);
    };
    d.apply.noteSpawned = [this] { jobs->noteSpawned(); };
    d.toolSpecs = [this](const orch::Principal&) { return specs; };
    d.mcpDispatch = [this](const std::string&, const orch::Principal&) {
      dispatched++;
      return "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + toolOutput + "\"}]}}";
    };
    d.hosts.fabric = [this](const std::vector<std::string>& hl, const std::string& ins, const std::string& inp,
                            std::string& out, std::string& err, const agent::HeadTools& ht, orch::TurnSpend* spend,
                            const std::function<void(const std::string&, const std::string&)>& notify) {
      return providers::runFabricLoop(pd, hl, ins, inp, out, err, ht, spend, notify);
    };
    for (const auto& h : keyed) {
      if (h == "zai") {   // the single-shot custom head (a head the fabric cannot drive)
        wire.custBase = "https://api.z.ai";
        wire.custKey = "fake";
        wire.custModel = "glm-5.3-flash";
        wire.custPathPrefix = "/api/paas/v4";
        cfg.keys["zai"] = "fake";
        d.hosts.add(h, [this](std::string& conv, const std::string& ins, const std::string& inp, std::string& out,
                              std::string& err, const agent::HeadTools* tools, orch::TokenUsage* use) {
          return providers::orchTurnCustom(pd, conv, ins, inp, out, err, tools, use);
        });
        continue;
      }
      d.hosts.add(h, [this, h](std::string& conv, const std::string& ins, const std::string& inp, std::string& out,
                               std::string& err, const agent::HeadTools* tools, orch::TokenUsage* use) {
        if (h == "mistral") return providers::orchTurnMistral(pd, conv, ins, inp, out, err, tools, use);
        if (h == "anthropic") return providers::orchTurnAnthropic(pd, conv, ins, inp, out, err, tools, use);
        return providers::orchTurnOpenAI(pd, conv, ins, inp, out, err, tools, use);
      });
    }
    d.hosts.fabricSupports = [](const std::string& h) { return providers::fabricSupports(h); };
    d.hosts.parked = [this](const std::string& h, providers::HostPark* why) { return providers::hostParked(pd, h, why); };
    d.hosts.failed = [this](const std::string& h, orch::StepErr k, int status) { providers::noteHostFailed(pd, h, k, status); };
    d.hosts.answered = [](const std::string& h) { providers::unparkHost(h); };
    d.histV2 = [] { return true; };
    d.historyRows = [this](const std::string&, size_t limit, const std::string& before) {
      std::vector<orch::EpisodicMessage> out;
      for (auto it = rows.rbegin(); it != rows.rend() && out.size() < limit; ++it) {
        if (!before.empty() && it->id >= before) continue;
        out.push_back(*it);
      }
      return out;
    };
    d.captureUserRow = [this](const std::string&, const std::string& t, const std::string&) {
      turnTag = addRow("user", t);
      return turnTag;
    };
    d.captureTrigger = [](const std::string&, const std::string&, const std::string&) {};
    d.historyMarks = [](const std::string&) { return agent::HistoryMarks(); };
    d.recall = [this](const std::string&, const orch::Principal&) {
      orch::RecallOut out;
      out.hits = recalledHits;
      return out;
    };
    d.standingWork = [this](const std::string&) { return standing; };
    d.hooks.onTurnEnd = [this](const agent::TurnEndEv& ev) {
      trimmedForSize.push_back(ev.trimmedForSize);
      failedPath = ev.chain ? ev.chain->path() : std::string();
    };
    eng.reset(new TurnEngine(std::move(d)));
  }
  ~Rig() { providers::clearHostParks(); }

  std::string addRow(const std::string& role, const std::string& text) {
    orch::EpisodicMessage m;
    char id[16];
    snprintf(id, sizeof id, "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = "1001";
    m.tsHours = 490000;
    m.role = role;
    m.kind = orch::MsgKind::Message;
    m.text = text;
    m.tags = role == "user" ? "from:Roy" : "";
    rows.push_back(m);
    return m.id;
  }
  // `pairs` past turns of `bytes` each, all quotes and line breaks: the wire writes
  // them twice their size, which is what the request's own size estimate cannot see.
  void quotedHistory(int pairs, size_t bytes) {
    std::string heavy;
    while (heavy.size() < bytes) heavy += "\"x\"\n";
    for (int i = 0; i < pairs; i++) {
      addRow("user", "old question " + std::to_string(i) + " " + heavy);
      addRow("assistant", "old answer " + std::to_string(i) + " " + heavy);
    }
  }
  void script(const Exchange& e) { wire.http.script.push_back(e); }
  size_t requests() const { return wire.http.seen.size(); }
  size_t requestsTo(const std::string& host) const {
    size_t n = 0;
    for (const auto& r : wire.http.seen) n += r.host == apiHostOf(host) ? 1 : 0;
    return n;
  }
  std::string all() const {
    std::string s;
    for (const auto& d : delivered) s += d + "\n---\n";
    return s;
  }
  bool saidAny(const char* what) const { return all().find(what) != std::string::npos; }
  void turn(const char* text = "hi") { eng->handleMessage(text, "Roy", "1001"); }
};

void assertAnswered(const Rig& r, const char* what, const char* reply = "done") {
  std::string msg = std::string(what) + " | delivered: " + r.all();
  if (msg.size() > 600) msg.resize(600);
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("didn't finish"), msg.c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("raw findings"), msg.c_str());
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny(reply), msg.c_str());
}

// The oldest past turn a request shows (-1 = none): the fixture's turns are numbered.
int oldestShown(const std::string& body) {
  for (int i = 0; i < 64; i++)
    if (body.find("old question " + std::to_string(i) + " ") != std::string::npos) return i;
  return -1;
}

// A provider's window refusal only just over the limit: the first trim cannot use up the
// whole ladder, so what bounds the retries is the guard itself.
const char* const kNearOverflowMistral =
    "{\"object\":\"error\",\"message\":\"Prompt 1100000 > 1048576 maximum context length\",\"type\":"
    "\"invalid_request_prompt_too_long\",\"param\":null,\"code\":\"3059\",\"raw_status_code\":400}";
const char* const kNearOverflowAnthropic =
    "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"prompt is too long: "
    "1100000 tokens > 1000000 maximum\"}}";
// A refusal so far over that everything on the ladder goes.
const char* const kFarOverflowMistral =
    "{\"object\":\"error\",\"message\":\"Prompt 1000000 > 10 maximum context length\",\"type\":"
    "\"invalid_request_prompt_too_long\",\"param\":null,\"code\":\"3059\",\"raw_status_code\":400}";

Exchange overloaded(const std::string& host) { return refused(host, 503, "{\"message\":\"Service unavailable\"}"); }

// An Anthropic round that thought, then called a tool. The signature is validated, so it
// must come back byte for byte.
std::string thinkingToolBody(size_t thoughtBytes = 3) {
  return "{\"content\":[{\"type\":\"thinking\",\"thinking\":\"" + std::string(thoughtBytes, 'w') +
         "\",\"signature\":\"SIGNED-ROUND-0\"},"
         "{\"type\":\"tool_use\",\"id\":\"toolcall1\",\"name\":\"memory_search\",\"input\":{\"query\":\"x\"}}],"
         "\"stop_reason\":\"tool_use\"}";
}

const char* const kWorkTurn = "Use a web lookup and analyze these six photos in detail";

void assertBodiesFit(const Rig& r, const char* what) {
  for (const auto& req : r.wire.http.seen)
    TEST_ASSERT_TRUE_MESSAGE(req.body.size() <= providers::kWireBodyMax, what);
}

}  // namespace

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

// ---- 1. A synthesis turn never fails on size ---------------------------------------
// The class: every tool size around the budget, on every wire, a synthesis turn with
// quote-heavy history gets its one request out and the owner gets the synthesis.
static void test_synthesis_never_fails_on_size() {
  int crossed = 0, cases = 0;
  for (const char* host : {"mistral", "anthropic", "openai"})
    for (size_t tools : {30000u, 40000u, 45000u, 50000u, 55000u, 60000u, 70000u, 90000u, 120000u}) {
      Rig r({host}, tools);
      r.quotedHistory(8, 1500);
      r.jobs->addFreshResult("job0008", "openai/gpt-5.2", std::string(7000, 'r') + " the findings");
      r.script(ok(host, finalBody(host, "synthesis done")));
      r.eng->maybeConsolidate("1001");
      char what[120];
      snprintf(what, sizeof what, "synthesis %s tools=%zu", host, tools);
      assertAnswered(r, what, "synthesis done");
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.requests(), what);
      assertBodiesFit(r, what);
      crossed += LogCapture::contains("B budget") ? 1 : 0;
      cases++;
      LogCapture::install();
    }
  printf("property: %d synthesis cases answered, %d crossed the budget at round 0\n", cases, crossed);
  TEST_ASSERT_TRUE(crossed > 3);   // the matrix really crosses the budget
}

// The results are what a synthesis is for: at Lumi's size they go out whole, the past turns
// pay. The owner hears nothing about it, since nothing they asked about was cut.
static void test_synthesis_at_lumis_size_keeps_its_results_whole() {
  int crossed = 0;
  for (size_t tools = 44000; tools <= 58000; tools += 2000) {
    Rig r({"mistral"}, tools);
    r.quotedHistory(8, 1500);
    const std::string result = "START " + std::string(5000, 'r') + " END";
    r.jobs->addFreshResult("job0008", "research-sub", result);
    r.script(ok("mistral", finalBody("mistral", "synthesis done")));
    r.eng->maybeConsolidate("1001");
    assertAnswered(r, "lumi size", "synthesis done");
    crossed += r.wire.http.seen[0].body.find("LEFT OUT") != std::string::npos ? 1 : 0;
    const bool whole = r.wire.http.seen[0].body.find("cut to fit") == std::string::npos;
    if (tools > 56000) continue;   // past this the past turns are not enough, and the results do pay
    TEST_ASSERT_TRUE_MESSAGE(whole, "the result is whole");
    TEST_ASSERT_FALSE_MESSAGE(r.saidAny("did not fit"), r.all().c_str());
  }
  TEST_ASSERT_TRUE(crossed > 0);
}

// The same on an owner turn, and what the turn leaves out is said: the model reads it in the
// request, the log names it, and the owner hears when it was the latest messages. The sweep must
// really cross the wire budget: with the request's own room held back, what crosses at round 0
// is a prompt and tools with little history left to give.
static void test_owner_turn_over_budget_answers_and_says_so() {
  const char* host = "mistral";
  int crossed = 0;
  for (size_t tools = 44000; tools <= 72000; tools += 2000) {
    LogCapture::install();
    Rig r({host}, tools);
    r.quotedHistory(8, 1500);
    r.script(ok(host, finalBody(host)));
    r.turn("so where are we at?");
    assertAnswered(r, "owner turn");
    TEST_ASSERT_EQUAL(1, (int)r.requests());
    assertBodiesFit(r, "owner turn");
    if (!LogCapture::contains("B budget")) continue;
    crossed++;
    const std::string& body = r.wire.http.seen[0].body;
    TEST_ASSERT_TRUE_MESSAGE(body.find("LEFT OUT") != std::string::npos, "the model is told what was left out");
    TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains("turn fit: left out"), "the log names what was left out");
    if (LogCapture::contains("of the newest conversation"))
      TEST_ASSERT_TRUE_MESSAGE(r.saidAny("written without the latest messages"), r.all().c_str());
  }
  TEST_ASSERT_TRUE_MESSAGE(crossed > 0, "the sweep never crossed the wire budget on an owner turn");
}

// What touches the owner's question is told to the owner, in the delivered text.
static void test_owner_is_told_what_was_left_out() {
  Rig r({"mistral"}, 2000);
  r.quotedHistory(8, 1200);
  r.script(refused("mistral", 400, kFarOverflowMistral));
  r.script(ok("mistral", finalBody("mistral")));
  r.turn("so where are we at?");
  TEST_ASSERT_EQUAL(2, (int)r.requests());
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("done"), r.all().c_str());
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("written without"), r.all().c_str());
}

// A chat's fold is a paid call: it is queued only when conversation history went, and the
// engine says so on the turn's end.
static void test_turn_end_says_when_history_went() {
  Rig plain({"mistral"}, 2000);
  plain.quotedHistory(2, 200);
  plain.script(ok("mistral", finalBody("mistral")));
  plain.turn("so where are we at?");
  TEST_ASSERT_EQUAL(1, (int)plain.trimmedForSize.size());
  TEST_ASSERT_FALSE_MESSAGE(plain.trimmedForSize[0], "nothing was trimmed");

  Rig went({"mistral"}, 2000);
  went.quotedHistory(8, 1200);
  went.script(refused("mistral", 400, kFarOverflowMistral));
  went.script(ok("mistral", finalBody("mistral")));
  went.turn("so where are we at?");
  TEST_ASSERT_TRUE_MESSAGE(went.trimmedForSize.back(), "history went");

  // The results were clipped and no history was there to go: a fold would not help.
  Rig clipped({"mistral"}, 2000);
  clipped.jobs->addFreshResult("job0008", "research-sub", std::string(20000, 'r'));
  clipped.script(refused("mistral", 400, kFarOverflowMistral));
  clipped.script(ok("mistral", finalBody("mistral", "synthesis done")));
  clipped.eng->maybeConsolidate("1001");
  TEST_ASSERT_TRUE_MESSAGE(clipped.saidAny("synthesis done"), clipped.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(clipped.trimmedForSize.back(), "only results were clipped");
}

// A tool round the model has already read gives way before the owner's earlier messages:
// the order provider-wire.md and trimForOverflow state, on the wire budget's path too.
static void test_budget_refit_folds_read_rounds_before_the_conversation() {
  int late = 0;
  for (const char* host : {"mistral", "anthropic", "openai"})
    for (size_t tools = 26000; tools <= 72000; tools += 2000) {
      LogCapture::install();
      Rig r({host}, tools);
      r.toolOutput = std::string(5000, 't');
      r.quotedHistory(8, 1500);
      for (int i = 1; i <= 3; i++) r.script(ok(host, toolBody(host, i)));
      r.script(ok(host, finalBody(host)));
      r.turn(kWorkTurn);
      assertBodiesFit(r, "read rounds");
      for (const std::string& line : LogCapture::lines()) {
        if (line.find("B budget") == std::string::npos) continue;
        if (line.rfind("headloop: round 2", 0) != 0 && line.rfind("headloop: round 3", 0) != 0) break;
        late++;
        TEST_ASSERT_TRUE_MESSAGE(line.find("replayed tool rounds") != std::string::npos, line.c_str());
        break;
      }
    }
  TEST_ASSERT_TRUE_MESSAGE(late > 0, "no case crossed the budget with older rounds to fold");
}

// The signed thinking of a round that ran rides every later request, whatever a refit did
// to the prompt (Anthropic refuses a replayed tool call without it).
static void test_a_refit_keeps_the_signed_thinking() {
  for (bool fabric : {true, false}) {
    Rig r({"anthropic"}, 2000, fabric);
    r.quotedHistory(8, 1200);
    r.script(ok("anthropic", thinkingToolBody()));
    r.script(refused("anthropic", 400, kNearOverflowAnthropic));
    r.script(ok("anthropic", finalBody("anthropic")));
    r.turn(kWorkTurn);
    const char* what = fabric ? "fabric" : "host loop";
    assertAnswered(r, what);
    TEST_ASSERT_EQUAL_MESSAGE(3, (int)r.requests(), what);
    TEST_ASSERT_EQUAL_MESSAGE(1, r.dispatched, what);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[1].body.find("SIGNED-ROUND-0") != std::string::npos, what);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[2].body.find("LEFT OUT") != std::string::npos, what);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[2].body.find("SIGNED-ROUND-0") != std::string::npos, what);
  }
  // The wire budget's prompt rung at round 1 rebuilds the step the same way.
  int crossed = 0;
  for (bool fabric : {true, false})
    for (size_t tools = 50000; tools <= 60000; tools += 200) {
      LogCapture::install();
      Rig r({"anthropic"}, tools, fabric);
      r.quotedHistory(8, 1500);
      r.script(ok("anthropic", thinkingToolBody(4000)));   // the replay itself takes the body over
      r.script(ok("anthropic", finalBody("anthropic")));
      r.turn(kWorkTurn);
      bool refitted = false;
      for (const std::string& line : LogCapture::lines())
        refitted = refitted || (line.rfind("headloop: round 1", 0) == 0 &&
                                (line.find("of the older history") != std::string::npos ||
                                 line.find("of the prompt and input") != std::string::npos));
      if (!refitted) continue;
      crossed++;
      TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[1].body.find("SIGNED-ROUND-0") != std::string::npos,
                               fabric ? "budget fabric" : "budget host loop");
    }
  TEST_ASSERT_TRUE_MESSAGE(crossed > 0, "no case refitted the prompt at round 1");
}

// The tool loop without the fabric (mid-turn failover off): the same request fits.
static void test_round0_over_budget_without_the_fabric() {
  int crossed = 0;
  for (size_t tools : {45000u, 50000u, 55000u, 60000u}) {
    Rig r({"mistral"}, tools, /*fabric=*/false);
    r.quotedHistory(8, 1500);
    r.jobs->addFreshResult("job0008", "openai/gpt-5.2", std::string(7000, 'r') + " the findings");
    r.script(ok("mistral", finalBody("mistral", "synthesis done")));
    r.eng->maybeConsolidate("1001");
    assertAnswered(r, "no fabric", "synthesis done");
    assertBodiesFit(r, "no fabric");
    crossed += LogCapture::contains("turn fit:") ? 1 : 0;
    LogCapture::install();
  }
  TEST_ASSERT_TRUE(crossed > 0);
}

// A failed synthesis sends the raw results alone: no message saying "Your message was not
// answered" to nobody (CUM-886's second half).
static void test_failed_synthesis_sends_only_the_raw_results() {
  Rig r({"mistral"}, 2000);
  r.jobs->addFreshResult("job0008", "openai/gpt-5.2", "the findings");
  Exchange down;
  down.status = 0;
  down.err = "no response";
  r.script(down);
  r.script(down);
  r.eng->maybeConsolidate("1001");
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("the findings"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("didn't finish"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("not answered"), r.all().c_str());
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), r.all().c_str());
  // The next owner turn's failure still speaks: the flag does not outlive the synthesis.
  r.script(down);
  r.script(down);
  r.turn("so where are we at?");
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("didn't finish"), r.all().c_str());
}

// ---- 2. Each provider's overflow refusal: trim and retry once ----------------------
static void test_real_overflow_refusals_trim_and_retry_once() {
  for (const Refusal& ref : kOverflow) {
    Rig r({ref.host}, 2000);
    r.quotedHistory(8, 1200);
    r.script(refused(ref.host, ref.status, ref.body));
    r.script(ok(ref.host, finalBody(ref.host)));
    r.turn("so where are we at?");
    assertAnswered(r, ref.name);
    TEST_ASSERT_EQUAL_MESSAGE(2, (int)r.requests(), ref.name);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[1].body.size() < r.wire.http.seen[0].body.size(), ref.name);
    const int shown = oldestShown(r.wire.http.seen[0].body);
    TEST_ASSERT_TRUE_MESSAGE(shown >= 0, ref.name);
    TEST_ASSERT_TRUE_MESSAGE(oldestShown(r.wire.http.seen[1].body) != shown, ref.name);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[1].body.find("LEFT OUT") != std::string::npos, ref.name);
  }
}

// Z.ai's head runs single-shot: the refusal the old matcher did not know (code 1261).
static void test_zai_overflow_on_the_single_shot_path() {
  Rig r({"zai"}, 2000);
  r.quotedHistory(8, 1200);
  Exchange refuse;
  refuse.status = 400;
  refuse.body = "{\"error\":{\"code\":\"1261\",\"message\":\"Prompt exceeds max length\"}}";
  r.script(refuse);
  r.script(refuse);   // the head's own schema-less retry of a 400, on the same prompt
  Exchange serve;
  serve.body = "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"{\\\"reply\\\":\\\"done\\\"}\"}}]}";
  r.script(serve);
  r.turn("so where are we at?");
  assertAnswered(r, "zai");
  TEST_ASSERT_EQUAL(3, (int)r.requests());
  TEST_ASSERT_TRUE(r.wire.http.seen[2].body.size() < r.wire.http.seen[0].body.size());
  TEST_ASSERT_TRUE(r.wire.http.seen[2].body.find("LEFT OUT") != std::string::npos);
}

// A trim and a fold together must not lose the folded lines when a round moves host,
// and a fold's lines survive a later refit (the seed is rewritten).
static void test_fold_lines_survive_a_host_switch() {
  nimbus::orch::Transcript tr;
  tr.addUser("LEAD one\nseed");
  orch::HeadToolCall c;
  for (int round = 0; round < 3; round++) {
    c.id = "c" + std::to_string(round);
    c.name = "memory_search";
    c.argsJson = "{\"query\":\"q\"}";
    tr.addToolCall(c, round, "");
    orch::HeadToolResult res;
    res.id = c.id;
    res.name = c.name;
    res.output = std::string(4000, 'r');
    tr.addToolResult(res, round);
  }
  tr.foldOlderRounds(1000);
  const std::string before = tr.entries()[0].text;
  TEST_ASSERT_TRUE(before.find(orch::Transcript::kFoldNote) != std::string::npos);
  tr.setSeedText("LEAD two\nseed");
  const std::string after = tr.entries()[0].text;
  TEST_ASSERT_TRUE(after.rfind("LEAD two", 0) == 0);
  TEST_ASSERT_TRUE(after.find(orch::Transcript::kFoldNote) != std::string::npos);
  TEST_ASSERT_TRUE(after.find("round 0") != std::string::npos);
}

// Photos count against a window but not against the byte budget: an overflow the history
// cannot pay for reads them as their transcriptions, on this host, on the retry.
static void test_overflow_gives_up_the_photos_after_older_history() {
  const Refusal& ref = kOverflow[0];
  Rig r({ref.host}, 200);
  r.cfg.orchModels[ref.host] = "claude-haiku-5-5";   // a model that takes images
  r.pd.orchModel = [](const char*) { return std::string("claude-haiku-5-5"); };
  harness_test::PhotoTurn photos;
  const std::string turn = photos.text;
  orch::offerTurnImages("1001", turn, std::move(photos.set));
  r.script(refused(ref.host, ref.status, ref.body));
  r.script(ok(ref.host, finalBody(ref.host)));
  r.turn(turn.c_str());
  assertAnswered(r, "photos");
  TEST_ASSERT_EQUAL(2, (int)r.requests());
  TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[0].body.find("\"type\":\"image\"") != std::string::npos, "the first shows them");
  TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[1].body.find("\"type\":\"image\"") == std::string::npos, "the retry does not");
  TEST_ASSERT_TRUE(r.wire.http.seen[1].body.find("machine transcription") != std::string::npos);
}

// Once: a second refusal moves to the next host, which runs the trimmed request.
static void test_overflow_twice_fails_over_with_the_trim() {
  const Refusal& ref = kOverflow[2];
  Rig r({"mistral", "anthropic"}, 2000);
  r.quotedHistory(8, 1200);
  r.script(refused("mistral", ref.status, ref.body));
  r.script(refused("mistral", ref.status, ref.body));
  r.script(ok("anthropic", finalBody("anthropic")));
  r.turn("so where are we at?");
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("done"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("didn't finish"), r.all().c_str());
  TEST_ASSERT_EQUAL(2, (int)r.requestsTo("mistral"));
  TEST_ASSERT_EQUAL(1, (int)r.requestsTo("anthropic"));
}

// A second overflow with nowhere to go ends honestly, without a raw provider line.
static void test_overflow_with_nowhere_to_go_is_honest() {
  const Refusal& ref = kOverflow[2];
  Rig r({"mistral"}, 2000);
  r.quotedHistory(8, 1200);
  r.script(refused("mistral", ref.status, ref.body));
  r.script(refused("mistral", ref.status, ref.body));
  r.turn("so where are we at?");
  TEST_ASSERT_EQUAL(2, (int)r.requests());
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("didn't finish"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("HTTP 400"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("1048576"), r.all().c_str());
}

// The dimension the matcher must not swallow: a refused max_tokens is not an overflow,
// so no history is thrown away for it.
static void test_output_cap_refusal_is_not_an_overflow() {
  for (const Refusal& ref : kNearMiss) {
    Rig r({ref.host, "openai"}, 2000);
    r.quotedHistory(8, 1200);
    r.script(refused(ref.host, ref.status, ref.body));
    r.script(ok("openai", finalBody("openai")));
    r.turn("so where are we at?");
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.requestsTo(ref.host), ref.name);   // not retried on that host
    TEST_ASSERT_EQUAL_MESSAGE(oldestShown(r.wire.http.seen[0].body), oldestShown(r.wire.http.seen[1].body), ref.name);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[1].body.find("LEFT OUT") == std::string::npos, ref.name);
  }
}

// Once means once: a refusal only just over the limit leaves rungs to give up, so nothing but
// the guard stops the loop from going back for each of them (Z.ai takes 27 s per refusal).
static void test_overflow_retries_once_only() {
  Rig r({"mistral"}, 2000);
  r.quotedHistory(8, 1200);
  for (int i = 0; i < 12; i++) r.script(refused("mistral", 400, kNearOverflowMistral));
  r.turn("so where are we at?");
  TEST_ASSERT_EQUAL(2, (int)r.requests());
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("didn't finish"), r.all().c_str());
}

// A loop is never run twice: re-sending the turn would repeat what its tools did. The refusal
// is only just over and older rounds pay for the trim, so the prompt was never touched and
// only the engine's guard stands between the second refusal and a re-run of the whole loop.
static void test_no_rerun_after_tools_ran_without_the_fabric() {
  for (const char* host : {"mistral", "anthropic", "openai"}) {
    Rig r({host}, 200, /*fabric=*/false);
    r.toolOutput = std::string(4000, 't');
    for (int i = 1; i <= 3; i++) r.script(ok(host, toolBody(host, i)));
    const char* body = std::string(host) == "anthropic"
                           ? kNearOverflowAnthropic
                           : std::string(host) == "mistral"
                                 ? kNearOverflowMistral
                                 : "{\"error\":{\"message\":\"Your input exceeds the context window of this model.\","
                                   "\"type\":\"invalid_request_error\",\"code\":\"context_length_exceeded\"}}";
    r.script(refused(host, 400, body));
    r.script(refused(host, 400, body));
    for (int i = 0; i < 6; i++) r.script(ok(host, toolBody(host, 4)));   // what a re-run would be served
    r.turn(kWorkTurn);
    TEST_ASSERT_EQUAL_MESSAGE(3, r.dispatched, host);
    TEST_ASSERT_EQUAL_MESSAGE(5, (int)r.requests(), host);
    TEST_ASSERT_TRUE_MESSAGE(r.saidAny("didn't finish"), r.all().c_str());
  }
}

// A round that recovered from its size is served: a later failure is told as itself.
static void test_a_recovered_overflow_is_not_the_cause() {
  Rig r({"mistral"}, 2000);
  r.quotedHistory(8, 1200);
  r.script(ok("mistral", toolBody("mistral", 1)));
  r.script(refused("mistral", 400, kNearOverflowMistral));
  r.script(ok("mistral", toolBody("mistral", 2)));
  r.script(overloaded("mistral"));
  r.script(overloaded("mistral"));
  r.turn(kWorkTurn);
  TEST_ASSERT_EQUAL(2, r.dispatched);
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("didn't finish"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("too long"), r.all().c_str());
}

// One trim per run of failures, not one per turn: a retry served by the same-host resend ends
// the run too, so a later round that outgrows the window gets its own.
static void test_a_later_overflow_gets_its_own_trim() {
  Rig r({"mistral"}, 2000);
  r.quotedHistory(8, 1200);
  r.script(refused("mistral", 400, kNearOverflowMistral));
  r.script(overloaded("mistral"));
  r.script(ok("mistral", toolBody("mistral", 1)));   // the same-host resend serves it
  r.script(refused("mistral", 400, kNearOverflowMistral));
  r.script(ok("mistral", finalBody("mistral")));
  r.turn(kWorkTurn);
  assertAnswered(r, "later overflow");
  TEST_ASSERT_EQUAL(5, (int)r.requests());
  TEST_ASSERT_EQUAL(1, r.dispatched);
}

// A tool loop with no fabric is what the hosted Virtual Nimbus and every router head run: the
// same recovery, on each wire, after a tool ran.
static void test_host_loop_recovers_mid_turn() {
  for (const char* host : {"mistral", "anthropic", "openai"}) {
    // Overflow after a tool ran: trimmed and answered on the same turn.
    Rig o({host}, 2000, /*fabric=*/false);
    o.quotedHistory(8, 1200);
    o.script(ok(host, toolBody(host, 1)));
    const Refusal* ref = nullptr;
    for (const Refusal& k : kOverflow)
      if (std::string(k.host) == host) ref = &k;
    o.script(refused(host, ref->status, ref->body));
    o.script(ok(host, finalBody(host)));
    o.turn(kWorkTurn);
    assertAnswered(o, host);
    TEST_ASSERT_EQUAL_MESSAGE(1, o.dispatched, host);
    TEST_ASSERT_EQUAL_MESSAGE(3, (int)o.requests(), host);
    TEST_ASSERT_TRUE_MESSAGE(o.wire.http.seen[2].body.find("LEFT OUT") != std::string::npos, host);
    TEST_ASSERT_TRUE_MESSAGE(o.wire.http.seen[2].body.find("THIS TURN RUNS ON") != std::string::npos, host);
    // Cut after a tool ran: the round runs once more with the note, and the note goes when
    // the round is served.
    Rig c({host}, 2000, /*fabric=*/false);
    c.script(ok(host, toolBody(host, 1)));
    c.script(ok(host, cutBody(host)));
    const bool headroomRetry = std::string(host) != "openai";   // the Responses wire has none in its step
    if (headroomRetry) c.script(ok(host, cutBody(host)));
    c.script(ok(host, toolBody(host, 2)));
    c.script(ok(host, finalBody(host)));
    c.turn(kWorkTurn);
    assertAnswered(c, host);
    TEST_ASSERT_EQUAL_MESSAGE(2, c.dispatched, host);
    const size_t n = c.requests();
    TEST_ASSERT_EQUAL_MESSAGE(headroomRetry ? 5 : 4, (int)n, host);
    TEST_ASSERT_TRUE_MESSAGE(c.wire.http.seen[n - 2].body.find("RAN OUT OF ROOM") != std::string::npos, host);
    TEST_ASSERT_TRUE_MESSAGE(c.wire.http.seen[n - 1].body.find("RAN OUT OF ROOM") == std::string::npos, host);
    // Over the wire budget after a tool ran, with only the prompt left to give.
    int crossed = 0;
    for (size_t tools = 40000; tools <= 80000; tools += 2000) {
      LogCapture::install();
      Rig b({host}, tools, /*fabric=*/false);
      b.toolOutput = std::string(4000, 't');
      b.quotedHistory(8, 400);
      b.script(ok(host, toolBody(host, 1)));
      b.script(ok(host, finalBody(host)));
      b.turn(kWorkTurn);
      if (b.dispatched != 1 || !LogCapture::contains("headloop: round 1")) continue;
      crossed++;
      assertAnswered(b, host);
      assertBodiesFit(b, host);
      TEST_ASSERT_EQUAL_MESSAGE(1, b.dispatched, host);
    }
    TEST_ASSERT_TRUE_MESSAGE(crossed > 0, host);
  }
}

// ---- 3. A cut step continues, then fails over; on multi-tool turns too -------------
// Lumi 31 Oct: the only open host cut twice. The round runs once more with the note.
static void test_cut_on_the_only_host_continues() {
  for (const char* host : {"mistral", "anthropic"}) {
    Rig r({host}, 2000);
    r.script(ok(host, cutBody(host)));
    r.script(ok(host, cutBody(host)));
    r.script(ok(host, finalBody(host)));
    r.turn("Use a web lookup and analyze these six photos in detail");
    assertAnswered(r, host);
    TEST_ASSERT_EQUAL_MESSAGE(3, (int)r.requests(), host);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[2].body.find("RAN OUT OF ROOM") != std::string::npos, host);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[2].body.find("Here is the first") != std::string::npos, host);
    TEST_ASSERT_FALSE_MESSAGE(r.saidAny("cut off"), host);
  }
}

// Lumi's shape: four tools ran, then the cut. The tools never run twice, and the next
// host gets their results and the note.
static void test_cut_after_tools_fails_over_with_the_work() {
  Rig r({"mistral", "anthropic"}, 2000);
  r.script(ok("mistral", toolBody("mistral", 1)));
  r.script(ok("mistral", cutBody("mistral")));
  r.script(ok("mistral", cutBody("mistral")));
  r.script(ok("anthropic", finalBody("anthropic")));
  r.turn("Use a web lookup and analyze these six photos in detail");
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("done"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("cut off"), r.all().c_str());
  TEST_ASSERT_EQUAL(1, r.dispatched);
  TEST_ASSERT_EQUAL(1, (int)r.requestsTo("anthropic"));
  TEST_ASSERT_TRUE(r.wire.http.seen.back().body.find("RAN OUT OF ROOM") != std::string::npos);
  TEST_ASSERT_TRUE(r.wire.http.seen.back().body.find("toolcall1") != std::string::npos);
}

// A round that recovered from its cut is served: a later round's failure is about itself,
// not a replay of that old partial.
static void test_a_recovered_cut_is_forgotten() {
  Rig r({"mistral"}, 2000);
  r.script(ok("mistral", cutBody("mistral")));
  r.script(ok("mistral", cutBody("mistral")));
  r.script(ok("mistral", toolBody("mistral", 1)));   // the note worked: a tool round
  r.script(refused("mistral", 401, "{\"message\":\"Unauthorized\"}"));
  r.turn("Use a web lookup and analyze these six photos in detail");
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("Here is the first"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("cut off"), r.all().c_str());
  TEST_ASSERT_EQUAL(1, r.dispatched);
}

// Every way out still cut: the honest marker with what was written, no promise, and a
// bounded number of requests.
static void test_cut_everywhere_ends_honestly_and_bounded() {
  Rig r({"mistral"}, 2000);
  r.script(ok("mistral", toolBody("mistral", 1)));
  for (int i = 0; i < 4; i++) r.script(ok("mistral", cutBody("mistral")));
  r.turn("Use a web lookup and analyze these six photos in detail");
  TEST_ASSERT_EQUAL(5, (int)r.requests());
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("Here is the first"), r.all().c_str());
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("cut off"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("next turn"), r.all().c_str());
  // With nothing written, what did run is still told.
  Rig n({"anthropic"}, 2000);
  n.script(ok("anthropic", toolBody("anthropic", 1)));
  const std::string empty =
      "{\"content\":[{\"type\":\"thinking\",\"thinking\":\"\"}],\"stop_reason\":\"max_tokens\"}";
  for (int i = 0; i < 4; i++) n.script(ok("anthropic", empty));
  n.turn("Use a web lookup and analyze these six photos in detail");
  TEST_ASSERT_TRUE_MESSAGE(n.saidAny("Already done this turn"), n.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(n.saidAny("next turn"), n.all().c_str());
}

// The note is about the round that was cut. Once it is served, later rounds (the final answer
// to a long album above all) are not told to answer in 300 words.
static void test_the_cut_note_goes_when_the_round_is_served() {
  Rig r({"mistral"}, 2000);
  r.script(ok("mistral", cutBody("mistral")));
  r.script(ok("mistral", cutBody("mistral")));
  r.script(ok("mistral", toolBody("mistral", 1)));
  r.script(ok("mistral", finalBody("mistral")));
  r.turn(kWorkTurn);
  assertAnswered(r, "note");
  TEST_ASSERT_EQUAL(4, (int)r.requests());
  TEST_ASSERT_TRUE(r.wire.http.seen[2].body.find("RAN OUT OF ROOM") != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[3].body.find("RAN OUT OF ROOM") == std::string::npos, "a served round's note stayed");
  TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[3].body.find("300 words") == std::string::npos, "a served round's note stayed");
}

// A cut the next host recovered, served only by the same-host resend: the round is served all
// the same. The later failure is about itself: no stale partial, no stale cause.
static void test_a_round_served_by_the_resend_forgets_its_cut() {
  Rig r({"mistral", "anthropic"}, 2000);
  r.script(ok("mistral", cutBody("mistral")));
  r.script(ok("mistral", cutBody("mistral")));
  r.script(overloaded("anthropic"));
  r.script(ok("anthropic", toolBody("anthropic", 1)));   // the same-host resend serves the round
  r.script(overloaded("anthropic"));
  r.script(overloaded("anthropic"));
  r.turn(kWorkTurn);
  TEST_ASSERT_EQUAL(1, r.dispatched);
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("Here is the first"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("cut off"), r.all().c_str());
  TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[4].body.find("RAN OUT OF ROOM") == std::string::npos, "the note outlived the round");
}

// A recovered cut is not why a later round failed, on one host or after a switch.
static void test_a_recovered_cut_is_not_the_cause() {
  Rig one({"mistral"}, 2000);
  one.script(ok("mistral", cutBody("mistral")));
  one.script(ok("mistral", cutBody("mistral")));
  one.script(ok("mistral", toolBody("mistral", 1)));
  one.script(overloaded("mistral"));
  one.script(overloaded("mistral"));
  one.turn(kWorkTurn);
  TEST_ASSERT_TRUE_MESSAGE(one.saidAny("didn't finish"), one.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(one.saidAny("ran out of room"), one.all().c_str());

  Rig two({"mistral", "anthropic"}, 2000);
  two.script(ok("mistral", cutBody("mistral")));
  two.script(ok("mistral", cutBody("mistral")));
  two.script(ok("anthropic", toolBody("anthropic", 1)));
  two.script(overloaded("anthropic"));
  two.script(overloaded("anthropic"));
  two.turn(kWorkTurn);
  TEST_ASSERT_TRUE_MESSAGE(two.saidAny("didn't finish"), two.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(two.saidAny("ran out of room"), two.all().c_str());
}

// A cut nothing recovered is still the turn's cause.
static void test_an_unrecovered_cut_stays_the_cause() {
  Rig r({"mistral", "anthropic"}, 2000);
  r.script(ok("mistral", cutBody("mistral")));
  r.script(ok("mistral", cutBody("mistral")));
  r.script(refused("anthropic", 400, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"nope\"}}"));
  r.turn(kWorkTurn);
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("Here is the first"), r.all().c_str());
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("cut off"), r.all().c_str());
  TEST_ASSERT_EQUAL_STRING("mistral -> anthropic", r.failedPath.c_str());
  // The cut is the last thing that happened: the chain still has it, whatever failed first.
  Rig last({"mistral", "anthropic"}, 2000);
  last.script(overloaded("mistral"));
  last.script(overloaded("mistral"));
  for (int i = 0; i < 4; i++) last.script(ok("anthropic", cutBody("anthropic")));
  last.turn(kWorkTurn);
  TEST_ASSERT_TRUE_MESSAGE(last.saidAny("cut off"), last.all().c_str());
  TEST_ASSERT_EQUAL_STRING("mistral -> anthropic", last.failedPath.c_str());
}

// A round that runs again inside one step only has the time the turn has left: the first
// attempts of a slow album took most of it.
static void test_a_rerun_gets_the_time_that_is_left() {
  for (bool fabric : {true, false}) {
    Rig r({"anthropic"}, 2000, fabric);
    r.pd.nowMs = [&r] { return r.wire.now; };
    r.cfg.deadlineS = 600;
    Exchange slow = ok("anthropic", cutBody("anthropic"));
    slow.takesMs = 270000;
    r.script(slow);
    r.script(slow);
    r.script(ok("anthropic", finalBody("anthropic")));
    r.turn(kWorkTurn);
    assertAnswered(r, "slow");
    TEST_ASSERT_EQUAL(3, (int)r.requests());
    const uint32_t last = r.wire.http.seen[2].timeoutMs;
    char what[64];
    snprintf(what, sizeof what, "fabric=%d rerun timeout %u ms", (int)fabric, (unsigned)last);
    TEST_ASSERT_TRUE_MESSAGE(last > 0 && last <= 60000, what);
  }
}

// ---- 4. Never a placeholder; a real task keeps its rounds --------------------------
// The VN reply, on the forced round: sent back once, the real answer delivered.
static void test_forced_placeholder_is_sent_back_once() {
  Rig r({"mistral"}, 2000);
  r.cfg.rounds = 1;
  r.script(ok("mistral", toolBody("mistral", 1)));
  r.script(ok("mistral", finalBody("mistral", "placeholder")));
  r.script(ok("mistral", finalBody("mistral", "the real answer")));
  r.turn("Use a web lookup to find the latest stable Python 3 release");
  assertAnswered(r, "forced placeholder", "the real answer");
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny("placeholder"), r.all().c_str());
  TEST_ASSERT_EQUAL(3, (int)r.requests());
}

// A model that keeps writing filler: the class of filler never reaches the owner.
static void test_placeholder_never_reaches_the_owner() {
  for (const char* filler : {"placeholder", "Placeholder.", "<placeholder>", "[reply]", "your reply here", "...", "TODO"}) {
    Rig r({"mistral"}, 2000);
    r.cfg.rounds = 1;
    r.script(ok("mistral", toolBody("mistral", 1)));
    r.script(ok("mistral", finalBody("mistral", filler)));
    r.script(ok("mistral", finalBody("mistral", filler)));
    r.turn("Use a web lookup to find the latest stable Python 3 release");
    TEST_ASSERT_TRUE_MESSAGE(r.saidAny("didn't finish"), filler);
    std::string all = r.all();
    TEST_ASSERT_TRUE_MESSAGE(all.find(filler) == std::string::npos, filler);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), filler);
    TEST_ASSERT_EQUAL_MESSAGE(3, (int)r.requests(), filler);
  }
  // The counter-test: a word the owner asked for is an answer, whatever the word.
  Rig e({"mistral"}, 2000);
  e.script(ok("mistral", finalBody("mistral", "placeholder")));
  e.turn("Say the word placeholder and nothing else.");
  TEST_ASSERT_TRUE_MESSAGE(e.saidAny("placeholder"), e.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(e.saidAny("didn't finish"), e.all().c_str());
}

// The cap: a real task runs its full budget; plain chat stays at 2 (EFF's savings).
static void test_a_real_task_keeps_its_rounds_and_chat_stays_capped() {
  static const char* const kTasks[] = {
      "Use a web lookup to find the latest stable Python 3 release listed on python.org/downloads. Tell me the "
      "version number and the URL you used.",
      "search the web for the current price of a Raspberry Pi 5",
      "please read my notes file and summarize it",
      "can you check the weather in London tomorrow",
      "look up who won the 2022 world cup",
      "compare the three plans and write it to a file",
      "find my last message about the dentist",
      "https://example.com/a what does this page say?",
  };
  for (const char* t : kTasks) {
    Rig r({"mistral"}, 2000);
    r.cfg.rounds = 5;
    r.script(ok("mistral", finalBody("mistral")));
    r.turn(t);
    std::string msg = std::string("a task keeps its rounds: ") + t;
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[0].body.find("several rounds (at most 2)") == std::string::npos, msg.c_str());
  }
  static const char* const kChat[] = {
      "hi", "so where are we at?", "thanks", "Did you teach me about draft models?", "why do you think so?",
      "what's today's food total?", "How are you", "ok great", "Its pumpkin soup.",
  };
  for (const char* t : kChat) {
    Rig r({"mistral"}, 2000);
    r.cfg.rounds = 5;
    r.script(ok("mistral", finalBody("mistral")));
    r.turn(t);
    std::string msg = std::string("chat stays capped: ") + t;
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[0].body.find("several rounds (at most 2)") != std::string::npos, msg.c_str());
  }
}

// ---- 5. A promise with nothing behind it, across the whole reply path --------------
// Lumi's own words (31 Oct), and the three the head-loop comment recorded.
static const char* const kPromises[] = {
    "The earlier total-row update failed because the header lacks a unique key. I'll rewrite the whole file to correct it next turn.",
    "The memory queue is full, so I'll confirm the protein target and log the dinner in the next turn.",
    "Searching the archive project for files... (this will complete next turn)",
    "I'll check the archive project files in the background and report back",
    "I've queued a memory query and will have the file-browsing tools next turn",
};
static const char* const kNotPromises[] = {
    "Tell me the weight and I'll log it next turn.",
    "If you want, I can finish that once you send the photo.",
    "Nothing is still running in the background.",
    "I can't finish this next turn or later: there is no job for it.",
    "Your next message starts a new turn.",
    "364 kcal left today.",
    "I'm seeing fries in the background, so the burger is the main item.",
    "I'm looking at the toast; the coffee in the background is not counted.",
    "I'm glad you're still working on your protein target, it will pay off.",
    "I'm proud that you keep working on your goals.",
};
static const char* const kLaterLine = "will not continue on its own";

// The served path: one correction round, and the model's corrected answer goes out.
static void test_promise_gets_one_correction_round() {
  for (const char* p : kPromises) {
    Rig r({"mistral"}, 2000);
    r.script(ok("mistral", finalBody("mistral", p)));
    r.script(ok("mistral", finalBody("mistral", "That is not done. Ask me to continue.")));
    r.turn("so where are we at?");
    TEST_ASSERT_TRUE_MESSAGE(r.saidAny("That is not done"), p);
    TEST_ASSERT_FALSE_MESSAGE(r.saidAny(p), p);
    TEST_ASSERT_EQUAL_MESSAGE(2, (int)r.requests(), p);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[1].body.find("nothing continues") != std::string::npos, p);
  }
}

// A model that repeats it: the owner gets the plain line with it. And the forced
// round, where no correction can run.
static void test_stubborn_or_forced_promise_gets_the_line() {
  for (const char* p : kPromises) {
    Rig r({"mistral"}, 2000);
    r.script(ok("mistral", finalBody("mistral", p)));
    r.script(ok("mistral", finalBody("mistral", p)));
    r.turn("so where are we at?");
    TEST_ASSERT_TRUE_MESSAGE(r.saidAny(kLaterLine), r.all().c_str());
    Rig f({"mistral"}, 2000);
    f.cfg.rounds = 1;
    f.script(ok("mistral", toolBody("mistral", 1)));
    f.script(ok("mistral", finalBody("mistral", p)));
    f.turn("Use a web lookup to find the latest stable Python 3 release");
    TEST_ASSERT_TRUE_MESSAGE(f.saidAny(kLaterLine), f.all().c_str());
  }
}

// The cut path and the unreadable path deliver the model's own text without apply:
// the same line rides there.
static void test_salvaged_promise_gets_the_line() {
  Rig r({"mistral"}, 2000);
  for (int i = 0; i < 4; i++) r.script(ok("mistral", cutBody("mistral", "I'll finish the rest next turn.")));
  r.turn("Use a web lookup and analyze these six photos in detail");
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("I'll finish the rest next turn."), r.all().c_str());
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny(kLaterLine), r.all().c_str());
}

// Prose (a head that ignored the schema) goes the same way.
static void test_prose_promise_gets_the_line() {
  Rig r({"anthropic"}, 2000);
  const std::string prose =
      "{\"content\":[{\"type\":\"text\",\"text\":\"I'll finish that next turn.\"}],\"stop_reason\":\"end_turn\"}";
  for (int i = 0; i < 3; i++) r.script(ok("anthropic", prose));
  r.turn("so where are we at?");
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny(kLaterLine), r.all().c_str());
}

// A synthesis turn and a scheduled routine: nobody waits on them, and the line still rides.
static void test_unattended_promise_gets_the_line() {
  const char* p = "I'll finish the rest of the summary next turn.";
  Rig r({"mistral"}, 2000);
  r.jobs->addFreshResult("job0008", "openai/gpt-5.2", "the findings");
  r.script(ok("mistral", finalBody("mistral", p)));
  r.script(ok("mistral", finalBody("mistral", p)));
  r.eng->maybeConsolidate("1001");
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny(kLaterLine), r.all().c_str());
  Rig s({"mistral"}, 2000);
  s.script(ok("mistral", finalBody("mistral", p)));
  s.script(ok("mistral", finalBody("mistral", p)));
  s.eng->injectScheduledTurn("1001", "Write the morning brief.", "brief");
  TEST_ASSERT_TRUE_MESSAGE(s.saidAny(kLaterLine), s.all().c_str());
}

// The single-shot head and an answer the device cannot parse as a turn: the same line.
static void test_unreadable_promise_gets_the_line() {
  Rig r({"zai"}, 2000);
  Exchange serve;
  serve.body =
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"{\\\"reply\\\":\\\"I'll finish that next "
      "turn.\\\",\\\"memory\\\":\"}}]}";
  r.script(serve);
  r.turn("so where are we at?");
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("I'll finish that next turn."), r.all().c_str());
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny(kLaterLine), r.all().c_str());
}

// A job behind the words: the same sentence is true, so it stands.
static void test_a_job_behind_the_promise_stands() {
  Rig r({"mistral"}, 2000);
  const std::string spawnArgs =
      "{\"reply\":\"Started a researcher, I'll report back when it finishes.\",\"session_ops\":[{\"op\":\"spawn\","
      "\"provider\":\"mistral\",\"name\":\"e2e\",\"task\":\"Reply NIMBUS-1\"}]}";
  r.script(ok("mistral",
              "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":"
              "\"abcdefghi\",\"function\":{\"name\":\"orch_turn\",\"arguments\":\"" + jsonEscaped(spawnArgs) + "\"}}]}}]}"));
  r.turn("start a researcher on this");
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("I'll report back"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny(kLaterLine), r.all().c_str());
  TEST_ASSERT_EQUAL(1, (int)r.requests());
  // The next turn, with the job running: the same words are still true.
  r.jobs->pump();
  r.script(ok("mistral", finalBody("mistral", "Still running, I'll report back when it finishes.")));
  r.turn("so where are we at?");
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("Still running"), r.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(r.saidAny(kLaterLine), r.all().c_str());
  TEST_ASSERT_EQUAL(2, (int)r.requests());
}

// A routine or a wakeup set in an earlier turn watches for the owner: "I'll let you know when
// it arrives" is true, on the routine's own run and when the owner asks. It does not make a
// promise of more work on the next turn true.
static void test_a_routine_behind_the_promise_stands() {
  const char* watch = "Not arrived yet. I'll let you know when it does.";
  Rig asked({"mistral"}, 2000);
  asked.standing = true;
  asked.script(ok("mistral", finalBody("mistral", watch)));
  asked.turn("any news?");
  TEST_ASSERT_EQUAL(1, (int)asked.requests());
  TEST_ASSERT_TRUE_MESSAGE(asked.saidAny("let you know"), asked.all().c_str());
  TEST_ASSERT_FALSE_MESSAGE(asked.saidAny(kLaterLine), asked.all().c_str());

  Rig run({"mistral"}, 2000);
  run.script(ok("mistral", finalBody("mistral", watch)));
  run.eng->injectScheduledTurn("1001", "Check the parcel and tell me when it arrives.", "parcel", "lp123456");
  TEST_ASSERT_EQUAL(1, (int)run.requests());
  TEST_ASSERT_FALSE_MESSAGE(run.saidAny(kLaterLine), run.all().c_str());

  // A wakeup that has fired has nothing left behind it.
  Rig once({"mistral"}, 2000);
  once.script(ok("mistral", finalBody("mistral", watch)));
  once.script(ok("mistral", finalBody("mistral", watch)));
  once.eng->injectScheduledTurn("1001", "Check the parcel.", "parcel", "lp123456", /*quietOk=*/false, /*once=*/true);
  TEST_ASSERT_TRUE_MESSAGE(once.saidAny(kLaterLine), once.all().c_str());

  // The dimension it must not swallow: a routine does not finish this turn's work later.
  Rig busy({"mistral"}, 2000);
  busy.standing = true;
  busy.script(ok("mistral", finalBody("mistral", "I'll rewrite the whole file next turn.")));
  busy.script(ok("mistral", finalBody("mistral", "That is not done. Ask me to continue.")));
  busy.turn("so where are we at?");
  TEST_ASSERT_TRUE_MESSAGE(busy.saidAny("That is not done"), busy.all().c_str());

  // The salvage path reads the same.
  const agent::SalvageOutcome s =
      agent::salvageForOwner("{\"reply\":\"I'll let you know when it does.\"", "truncated", true, orch::LaterWork::Standing);
  TEST_ASSERT_TRUE(s.text.find(kLaterLine) == std::string::npos);
  const agent::SalvageOutcome n =
      agent::salvageForOwner("{\"reply\":\"I'll finish it next turn.\"", "truncated", true, orch::LaterWork::Standing);
  TEST_ASSERT_TRUE(n.text.find(kLaterLine) != std::string::npos);
}

// The latch for a key or credit notice is one per episode: when a synthesis takes it, the
// notice goes out, or the owner is never told why summaries stopped.
static void test_failed_synthesis_keeps_the_next_step_notice() {
  struct Case { int status; const char* body; const char* words; };
  const Case cases[] = {
      {402, "{\"message\":\"Payment required: add credit to continue.\"}", "Add credit"},
      {401, "{\"message\":\"Unauthorized\"}", "rejected its key"},
  };
  for (const Case& c : cases) {
    orch::providerPark().clear();
    Rig r({"mistral"}, 2000);
    r.jobs->addFreshResult("job0008", "openai/gpt-5.2", "the findings");
    r.script(refused("mistral", c.status, c.body));
    r.eng->maybeConsolidate("1001");
    TEST_ASSERT_TRUE_MESSAGE(r.saidAny("the findings"), r.all().c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.saidAny(c.words), r.all().c_str());
    TEST_ASSERT_FALSE_MESSAGE(r.saidAny("not answered"), r.all().c_str());
    orch::providerPark().clear();
  }
}

// A chat line and a task alternating must not re-write the cached static block: its round
// figure is the configured one, this turn's tighter cap rides the dynamic block.
static void test_turn_shape_does_not_touch_the_cached_static_block() {
  Rig r({"mistral"}, 2000);
  r.cfg.rounds = 5;
  r.script(ok("mistral", finalBody("mistral")));
  r.script(ok("mistral", finalBody("mistral")));
  r.turn("hi");
  r.turn("search the web for the current price of a Raspberry Pi 5");
  const auto sysOf = [](const std::string& body) {
    JsonDocument d;
    deserializeJson(d, body);
    return std::string(d["messages"][0]["content"] | "");
  };
  const std::string a = sysOf(r.wire.http.seen[0].body), b = sysOf(r.wire.http.seen[1].body);
  TEST_ASSERT_TRUE(!a.empty());
  TEST_ASSERT_TRUE_MESSAGE(a == b, "the system message changed between a chat turn and a task turn");
  TEST_ASSERT_TRUE(a.find("(at most 5)") != std::string::npos);
  TEST_ASSERT_TRUE(r.wire.http.seen[0].body.find("(at most 2)") != std::string::npos);
  TEST_ASSERT_TRUE(r.wire.http.seen[1].body.find("(at most 2)") == std::string::npos);
}

// True statements about what is not happening are never corrected.
static void test_not_promises_are_left_alone() {
  for (const char* p : kNotPromises) {
    Rig r({"mistral"}, 2000);
    r.script(ok("mistral", finalBody("mistral", p)));
    r.turn("so where are we at?");
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.requests(), p);
    TEST_ASSERT_FALSE_MESSAGE(r.saidAny(kLaterLine), p);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_synthesis_never_fails_on_size);
  RUN_TEST(test_synthesis_at_lumis_size_keeps_its_results_whole);
  RUN_TEST(test_owner_turn_over_budget_answers_and_says_so);
  RUN_TEST(test_owner_is_told_what_was_left_out);
  RUN_TEST(test_turn_end_says_when_history_went);
  RUN_TEST(test_budget_refit_folds_read_rounds_before_the_conversation);
  RUN_TEST(test_a_refit_keeps_the_signed_thinking);
  RUN_TEST(test_round0_over_budget_without_the_fabric);
  RUN_TEST(test_failed_synthesis_sends_only_the_raw_results);
  RUN_TEST(test_real_overflow_refusals_trim_and_retry_once);
  RUN_TEST(test_zai_overflow_on_the_single_shot_path);
  RUN_TEST(test_fold_lines_survive_a_host_switch);
  RUN_TEST(test_overflow_gives_up_the_photos_after_older_history);
  RUN_TEST(test_overflow_twice_fails_over_with_the_trim);
  RUN_TEST(test_overflow_with_nowhere_to_go_is_honest);
  RUN_TEST(test_output_cap_refusal_is_not_an_overflow);
  RUN_TEST(test_overflow_retries_once_only);
  RUN_TEST(test_no_rerun_after_tools_ran_without_the_fabric);
  RUN_TEST(test_a_recovered_overflow_is_not_the_cause);
  RUN_TEST(test_a_later_overflow_gets_its_own_trim);
  RUN_TEST(test_host_loop_recovers_mid_turn);
  RUN_TEST(test_cut_on_the_only_host_continues);
  RUN_TEST(test_cut_after_tools_fails_over_with_the_work);
  RUN_TEST(test_a_recovered_cut_is_forgotten);
  RUN_TEST(test_cut_everywhere_ends_honestly_and_bounded);
  RUN_TEST(test_the_cut_note_goes_when_the_round_is_served);
  RUN_TEST(test_a_round_served_by_the_resend_forgets_its_cut);
  RUN_TEST(test_a_recovered_cut_is_not_the_cause);
  RUN_TEST(test_an_unrecovered_cut_stays_the_cause);
  RUN_TEST(test_a_rerun_gets_the_time_that_is_left);
  RUN_TEST(test_forced_placeholder_is_sent_back_once);
  RUN_TEST(test_placeholder_never_reaches_the_owner);
  RUN_TEST(test_a_real_task_keeps_its_rounds_and_chat_stays_capped);
  RUN_TEST(test_turn_shape_does_not_touch_the_cached_static_block);
  RUN_TEST(test_promise_gets_one_correction_round);
  RUN_TEST(test_stubborn_or_forced_promise_gets_the_line);
  RUN_TEST(test_salvaged_promise_gets_the_line);
  RUN_TEST(test_prose_promise_gets_the_line);
  RUN_TEST(test_unattended_promise_gets_the_line);
  RUN_TEST(test_unreadable_promise_gets_the_line);
  RUN_TEST(test_a_job_behind_the_promise_stands);
  RUN_TEST(test_a_routine_behind_the_promise_stands);
  RUN_TEST(test_failed_synthesis_keeps_the_next_step_notice);
  RUN_TEST(test_not_promises_are_left_alone);
  return UNITY_END();
}
