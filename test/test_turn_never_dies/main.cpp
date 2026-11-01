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
#include "nimbus/harness/engine.h"
#include "nimbus/harness/jobs.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/provider_park.h"

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
    d.toolSpecs = [this](const orch::Principal&) { return specs; };
    d.mcpDispatch = [this](const std::string&, const orch::Principal&) {
      dispatched++;
      return std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}}");
    };
    d.hosts.fabric = [this](const std::vector<std::string>& hl, const std::string& ins, const std::string& inp,
                            std::string& out, std::string& err, const agent::HeadTools& ht, orch::TurnSpend* spend,
                            const std::function<void(const std::string&, const std::string&)>& notify) {
      return providers::runFabricLoop(pd, hl, ins, inp, out, err, ht, spend, notify);
    };
    for (const auto& h : keyed)
      d.hosts.add(h, [h](std::string&, const std::string&, const std::string&, std::string&, std::string& err,
                         const agent::HeadTools*, orch::TokenUsage*) {
        err = "single shot not wired for " + h;
        return false;
      });
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

// The same on an owner turn, and what the turn leaves out is said: the model reads
// it in the request, the log names it.
static void test_owner_turn_trims_older_history_first_and_says_so() {
  const char* host = "mistral";
  Rig r({host}, 52000);
  r.quotedHistory(8, 1500);
  r.script(ok(host, finalBody(host)));
  r.turn("so where are we at?");
  assertAnswered(r, "owner turn");
  TEST_ASSERT_EQUAL(1, (int)r.requests());
  assertBodiesFit(r, "owner turn");
  const std::string& body = r.wire.http.seen[0].body;
  TEST_ASSERT_TRUE_MESSAGE(body.find("old question 7") != std::string::npos, "the newest past turn is kept");
  if (LogCapture::contains("B budget")) {   // this size crossed: the oldest went first and it says so
    TEST_ASSERT_TRUE_MESSAGE(body.find("old question 0") == std::string::npos, "the oldest past turn went first");
    TEST_ASSERT_TRUE_MESSAGE(body.find("LEFT OUT") != std::string::npos, "the model is told what was left out");
    TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains("left out"), "the log names what was left out");
  }
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
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[0].body.find("old question 0") != std::string::npos, ref.name);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[1].body.find("old question 0") == std::string::npos, ref.name);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[1].body.find("LEFT OUT") != std::string::npos, ref.name);
  }
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
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[1].body.find("old question 0") != std::string::npos, ref.name);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[1].body.find("LEFT OUT") == std::string::npos, ref.name);
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
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[2].body.find("ran out of room") != std::string::npos, host);
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[2].body.find("Here is the first photo") != std::string::npos, host);
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
  TEST_ASSERT_TRUE(r.wire.http.seen.back().body.find("ran out of room") != std::string::npos);
  TEST_ASSERT_TRUE(r.wire.http.seen.back().body.find("toolcall1") != std::string::npos);
}

// Every way out still cut: the honest marker with what was written, no promise, and a
// bounded number of requests.
static void test_cut_everywhere_ends_honestly_and_bounded() {
  Rig r({"mistral"}, 2000);
  r.script(ok("mistral", toolBody("mistral", 1)));
  for (int i = 0; i < 4; i++) r.script(ok("mistral", cutBody("mistral")));
  r.turn("Use a web lookup and analyze these six photos in detail");
  TEST_ASSERT_EQUAL(5, (int)r.requests());
  TEST_ASSERT_TRUE_MESSAGE(r.saidAny("Here is the first photo"), r.all().c_str());
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
    TEST_ASSERT_TRUE_MESSAGE(r.wire.http.seen[0].body.find("several rounds (at most 5)") != std::string::npos, msg.c_str());
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
    "I logged it. 364 kcal left today.",
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
  Rig r({"mistral"}, 2000);
  const std::string prose =
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"I'll finish that next turn.\"}}]}";
  for (int i = 0; i < 3; i++) r.script(ok("mistral", prose));
  r.turn("so where are we at?");
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
  RUN_TEST(test_owner_turn_trims_older_history_first_and_says_so);
  RUN_TEST(test_real_overflow_refusals_trim_and_retry_once);
  RUN_TEST(test_overflow_twice_fails_over_with_the_trim);
  RUN_TEST(test_overflow_with_nowhere_to_go_is_honest);
  RUN_TEST(test_output_cap_refusal_is_not_an_overflow);
  RUN_TEST(test_cut_on_the_only_host_continues);
  RUN_TEST(test_cut_after_tools_fails_over_with_the_work);
  RUN_TEST(test_cut_everywhere_ends_honestly_and_bounded);
  RUN_TEST(test_forced_placeholder_is_sent_back_once);
  RUN_TEST(test_placeholder_never_reaches_the_owner);
  RUN_TEST(test_a_real_task_keeps_its_rounds_and_chat_stays_capped);
  RUN_TEST(test_promise_gets_one_correction_round);
  RUN_TEST(test_stubborn_or_forced_promise_gets_the_line);
  RUN_TEST(test_salvaged_promise_gets_the_line);
  RUN_TEST(test_prose_promise_gets_the_line);
  RUN_TEST(test_a_job_behind_the_promise_stands);
  RUN_TEST(test_not_promises_are_left_alone);
  return UNITY_END();
}
