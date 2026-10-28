#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/body_budget.h"
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/transcript.h"

// A tool loop whose next request would outgrow the wire body budget never fails the
// whole turn after paying its rounds (Lumi, v4.6.0, 26 Oct 2026: the post-update turn
// ran 8 paid rounds on Mistral, the body grew 70 KB -> 86.9 KB, the forced final round
// was refused before the send, "request body over the 85 KB budget: 86865 B", and the
// turn failed with no answer). The refused request never left the device, so the loop
// folds the older tool rounds and runs the same round again.

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using nimbus::orch::HeadLoopConfig;
using nimbus::orch::HeadLoopHooks;
using nimbus::orch::HeadOutcome;
using nimbus::orch::HeadStep;
using nimbus::orch::HeadToolCall;
using nimbus::orch::HeadToolResult;
using nimbus::orch::Transcript;
namespace providers = agent::providers;

void setUp() { LogCapture::install(); }
void tearDown() {}

// ---- the refusal's one wording --------------------------------------------------
static void test_refusal_wording_round_trips() {
  const std::string e = nimbus::orch::bodyBudgetError(85000, 86865);
  TEST_ASSERT_EQUAL_STRING("request body over the 85 KB budget: 86865 B", e.c_str());   // Lumi's line
  size_t budget = 0, size = 0;
  TEST_ASSERT_TRUE(nimbus::orch::parseBodyBudgetError(e, budget, size));
  TEST_ASSERT_EQUAL_UINT32(85000, (uint32_t)budget);
  TEST_ASSERT_EQUAL_UINT32(86865, (uint32_t)size);
  // The provider guard speaks it.
  std::string err;
  TEST_ASSERT_FALSE(providers::bodyWithinBudget(std::string(providers::kWireBodyMax + 7, 'x'), err));
  TEST_ASSERT_TRUE(nimbus::orch::parseBodyBudgetError(err, budget, size));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)providers::kWireBodyMax, (uint32_t)budget);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)providers::kWireBodyMax + 7, (uint32_t)size);
  // Nothing else reads as one.
  for (const char* other : {"", "request body over the KB budget: 1 B", "request body over the 85 KB budget: B",
                            "mistral chat HTTP 400: request body over the 85 KB budget: 9 B", "network"})
    TEST_ASSERT_FALSE_MESSAGE(nimbus::orch::parseBodyBudgetError(other, budget, size), other);
}

// ---- the controller, over a growing loop ------------------------------------------
namespace {
// A provider whose body is a fixed base plus everything the transcript replays (the
// seeded message included, escaped at 5 %), refusing over `budget` exactly the way
// bodyWithinBudget does. Tool rounds until `toolRounds`, then the final answer.
struct GrowingLoop {
  size_t base = 70000, budget = providers::kWireBodyMax, resultBytes = 3000;
  size_t argsBytes = 0;   // > 0: the first call is one big write with arguments this size
  int toolRounds = 8;
  Transcript tr;
  std::vector<size_t> sent;
  int refused = 0, calls = 0, dispatched = 0;
  // Every sent request carried the newest result whole, whenever it could fit next
  // to the base and the one-line stubs the folded rounds leave (~300 B each).
  bool newestAlwaysSeen = true;
  std::vector<std::string> log;

  bool withTranscript = true;
  size_t bodySize() const {
    size_t bytes = 0;
    for (const auto& it : tr.entries()) bytes += it.text.size() + it.name.size() + it.id.size();
    if (!withTranscript) bytes = (size_t)dispatched * resultBytes;   // the provider's own replay
    return base + bytes + bytes / 20;
  }
  std::string resultText(int n) const {
    std::string r = "result " + std::to_string(n) + " ";
    r.resize(resultBytes > 16 ? resultBytes - 16 : 0, 'r');
    return r + "|end" + std::to_string(n) + "|";
  }
  bool carries(const std::string& tail) const {
    for (const auto& it : tr.entries())
      if (it.kind == nimbus::orch::TranscriptItem::Kind::ToolResult && it.text.find(tail) != std::string::npos)
        return true;
    return false;
  }
  HeadOutcome run() {
    tr.addUser("[USER]\nwhat changed in the update?");
    HeadLoopConfig cfg;
    cfg.maxRounds = toolRounds;
    cfg.deadlineMs = 0;
    cfg.roundMinHeap = 0;
    cfg.maxToolResultBytes = 8192;   // Lumi's auto budget: res=8192 tot=65536
    cfg.maxTotalToolBytes = 65536;
    HeadLoopHooks h;
    h.nowMs = [] { return 1000u; };
    h.log = [this](const std::string& m) { log.push_back(m); };
    h.transcript = withTranscript ? &tr : nullptr;
    h.dispatch = [this](const HeadToolCall& c) {
      HeadToolResult r;
      r.id = c.id;
      r.name = c.name;
      r.output = resultText(++dispatched);
      return r;
    };
    h.step = [this](bool allowTools, const std::vector<HeadToolResult>&, uint32_t, const std::string&) {
      HeadStep s;
      const size_t size = bodySize();
      if (size > budget) {
        refused++;
        s.ok = false;
        s.kind = nimbus::orch::StepErr::BadRequest;
        s.error = nimbus::orch::bodyBudgetError(budget, size);
        return s;
      }
      sent.push_back(size);
      if (dispatched > 0 && resultBytes + 512 + 300 * (size_t)toolRounds < budget - base)
        newestAlwaysSeen = newestAlwaysSeen && carries("|end" + std::to_string(dispatched) + "|");
      if (!allowTools || calls >= toolRounds) {
        s.finished = true;
        s.finalTurn = "{\"reply\":\"done\"}";
        return s;
      }
      HeadToolCall c;
      c.id = "c" + std::to_string(++calls);
      c.name = "memory_search";
      c.argsJson = argsBytes && calls == 1 ? "{\"text\":\"" + std::string(argsBytes, 'w') + "\"}"
                                           : std::string("{\"q\":\"update notes\"}");
      s.toolCalls.push_back(c);
      return s;
    };
    return nimbus::orch::runHeadLoop(cfg, h);
  }
};
// Does any sent request body hold `mark` verbatim (the whole-page contract's
// "the model received this round's result whole at least once")?
static bool anyBodyHas(const FakeProviderDeps& d, const std::string& mark) {
  for (const auto& r : d.http.seen)
    if (r.body.find(mark) != std::string::npos) return true;
  return false;
}

// Which |endN| markers each sent request carries, so a lost-round failure names
// itself: "req0: - req1: 1 2 req2: 1 2 3 ...".
static std::string markerLayout(const FakeProviderDeps& d) {
  std::string layout;
  for (size_t i = 0; i < d.http.seen.size(); i++) {
    std::string has;
    for (size_t m = 1; m <= 8; m++)
      if (d.http.seen[i].body.find("|end" + std::to_string(m) + "|") != std::string::npos)
        has += " " + std::to_string(m);
    layout += " req" + std::to_string(i) + ":" + (has.empty() ? " -" : has);
  }
  return layout;
}
}  // namespace

// Lumi's shape: a 70 KB base and results that push the body past the budget.
static void test_lumi_loop_finishes_with_an_answer() {
  GrowingLoop g;
  const HeadOutcome o = g.run();
  TEST_ASSERT_TRUE_MESSAGE(o.ok, o.error.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"reply\":\"done\"}", o.finalTurn.c_str());
  TEST_ASSERT_TRUE(g.refused > 0);                       // the class really happened
  TEST_ASSERT_EQUAL_INT(g.toolRounds + 1, (int)g.sent.size());   // no extra paid request
  for (size_t s : g.sent) TEST_ASSERT_TRUE(s <= g.budget);
  TEST_ASSERT_TRUE(g.newestAlwaysSeen);
  bool logged = false;
  for (const auto& l : g.log) logged = logged || l.find("B budget - folded") != std::string::npos;
  TEST_ASSERT_TRUE(logged);
}

// The class: across bases and result sizes, every loop ends with an answer, every
// request that went out was within the budget, and nothing was sent twice.
static void test_every_growing_loop_ends_with_an_answer() {
  int refits = 0;
  for (size_t base : {40000u, 60000u, 70000u, 78000u, 82000u})
    for (size_t result : {600u, 2500u, 6000u, 8000u})
      for (int rounds : {2, 8}) {
        GrowingLoop g;
        g.base = base;
        g.resultBytes = result;
        g.toolRounds = rounds;
        const HeadOutcome o = g.run();
        char msg[160];
        std::snprintf(msg, sizeof msg, "base %zu result %zu rounds %d: %s", base, result, rounds,
                      o.error.c_str());
        TEST_ASSERT_TRUE_MESSAGE(o.ok, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(rounds + 1, (int)g.sent.size(), msg);
        for (size_t s : g.sent) TEST_ASSERT_TRUE_MESSAGE(s <= g.budget, msg);
        TEST_ASSERT_TRUE_MESSAGE(g.newestAlwaysSeen, msg);
        TEST_ASSERT_TRUE_MESSAGE(g.refused <= 3 * (rounds + 1), msg);   // bounded: at most 3 refits a round
        refits += g.refused;
      }
  TEST_ASSERT_TRUE(refits > 10);   // the matrix really crosses the budget
}

// One big write (merge review): an executed call whose ARGUMENTS are the oversize
// part - an artifact.save of 6-24 KB on a 60-70 KB base. Folding older rounds and
// clipping results cannot shrink it; its arguments become a short stub (the call ran,
// its id and name stay paired with its result) and the turn still ends with an answer.
static void test_big_write_arguments_still_answer() {
  for (size_t base : {60000u, 70000u})
    for (size_t args : {6000u, 12000u, 18000u, 24000u})
      for (int rounds : {1, 3}) {
        GrowingLoop g;
        g.base = base;
        g.argsBytes = args;
        g.resultBytes = 120;   // "saved" and friends
        g.toolRounds = rounds;
        const HeadOutcome o = g.run();
        char msg[160];
        std::snprintf(msg, sizeof msg, "base %zu args %zu rounds %d: %s", base, args, rounds, o.error.c_str());
        TEST_ASSERT_TRUE_MESSAGE(o.ok, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(rounds + 1, (int)g.sent.size(), msg);
        for (size_t sz : g.sent) TEST_ASSERT_TRUE_MESSAGE(sz <= g.budget, msg);
      }
}

// Counter-tests: a base that alone is over the budget (nothing to fold) fails
// honestly with the refusal and never spins; without a transcript the old failure
// stands.
static void test_unfittable_round_fails_honestly() {
  GrowingLoop g;
  g.base = providers::kWireBodyMax + 100;
  const HeadOutcome o = g.run();
  TEST_ASSERT_FALSE(o.ok);
  TEST_ASSERT_EQUAL_INT(1, g.refused);
  TEST_ASSERT_TRUE(o.error.find("request body over the 85 KB budget") == 0);
  TEST_ASSERT_TRUE(o.kind == nimbus::orch::StepErr::BadRequest);
  // A loop with no canonical transcript to fold keeps the old, honest failure.
  GrowingLoop n;
  n.withTranscript = false;
  const HeadOutcome on = n.run();
  TEST_ASSERT_FALSE(on.ok);
  TEST_ASSERT_EQUAL_INT(1, n.refused);
  TEST_ASSERT_TRUE(on.error.find("request body over") == 0);
}

// ---- the real provider wires ------------------------------------------------------
namespace {
std::vector<nimbus::orch::ToolRegistry::Spec> deviceShapedSpecs() {
  std::vector<nimbus::orch::ToolRegistry::Spec> specs;
  const std::string schema =
      R"({"type":"object","properties":{"project":{"type":"string"},"name":{"type":"string"},)"
      R"("q":{"type":"string"},"text":{"type":"string"},"id":{"type":"string"}}})";
  for (int i = 0; i < 42; i++) specs.push_back({"tool_" + std::to_string(i), std::string(309, 'd'), schema});
  return specs;
}
std::string toolResponse(const std::string& host, int n) {
  char id[16];
  std::snprintf(id, sizeof id, "tc%07d", n);
  if (host == "anthropic")
    return std::string("{\"content\":[{\"type\":\"tool_use\",\"id\":\"") + id +
           "\",\"name\":\"memory_search\",\"input\":{\"q\":\"notes\"}}],\"stop_reason\":\"tool_use\"}";
  if (host == "openai")
    return std::string("{\"id\":\"r") + id + "\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"" + id +
           "\",\"name\":\"memory_search\",\"arguments\":\"{\\\"q\\\":\\\"notes\\\"}\"}]}";
  return std::string("{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"") + id +
         "\",\"function\":{\"name\":\"memory_search\",\"arguments\":\"{\\\"q\\\":\\\"notes\\\"}\"}}]}}]}";
}
// A tool round whose one call carries `bytes` of text in its arguments (a big write).
std::string bigWriteResponse(const std::string& host, size_t bytes) {
  const std::string text(bytes, 'w');
  if (host == "anthropic")
    return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tc0000001\",\"name\":\"memory_search\","
           "\"input\":{\"q\":\"" + text + "\"}}],\"stop_reason\":\"tool_use\"}";
  if (host == "openai")
    return "{\"id\":\"rw1\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"tc0000001\","
           "\"name\":\"memory_search\",\"arguments\":\"{\\\"q\\\":\\\"" + text + "\\\"}\"}]}";
  return "{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"tc0000001\","
         "\"function\":{\"name\":\"memory_search\",\"arguments\":\"{\\\"q\\\":\\\"" + text + "\\\"}\"}}]}}]}";
}
std::string finalResponse(const std::string& host) {
  if (host == "anthropic")
    return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\","
           "\"input\":{\"reply\":\"done\"}}],\"stop_reason\":\"tool_use\"}";
  if (host == "openai")
    return "{\"id\":\"rf\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_f\","
           "\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}]}";
  return "{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"abc123def\","
         "\"function\":{\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}}]}}]}";
}
// Lumi's turn shape on a provider rig: eight tool rounds of 2.6 KB results, then the
// answer, over the 42-tool manifest, a device-scale prompt and a long seed (a ~70 KB
// round-0 body).
const std::string kLumiIns = "You are the head. " + std::string(30000, 's');
const std::string kLumiUser = "[FIRMWARE UPDATED] notes " + std::string(14000, 'u');
void lumiRig(FakeProviderDeps& d, FakeProviderDeps::ToolRig& rig, const std::string& host, int& n) {
  for (int i = 1; i <= 8; i++) d.http.script.push_back({"", "", 200, toolResponse(host, i)});
  d.http.script.push_back({"", "", 200, finalResponse(host)});
  d.fillTools(rig);
  for (const auto& s : deviceShapedSpecs()) rig.ht.specs.push_back(s);
  rig.ht.cfg.maxRounds = 8;
  rig.ht.cfg.maxToolResultBytes = 8192;
  rig.ht.cfg.maxTotalToolBytes = 65536;
  rig.ht.dispatch = [&n](const HeadToolCall& c) {
    HeadToolResult r;
    r.id = c.id;
    r.name = c.name;
    r.output = "entry " + std::to_string(++n) + " " + std::string(2600, 'e') + " |end" + std::to_string(n) + "|";
    return r;
  };
}
}  // namespace

// Lumi's turn on every loop wire: a ~70 KB round-0 body (the 42-tool manifest, a
// device-scale prompt, a long seed), eight rounds of 2.6 KB results. Each wire's
// own replay cap sits above the room left, so the refit is what keeps it sending.
static void test_lumi_shape_on_every_loop_wire() {
  bool refitSeen = false;
  for (const char* host : {"anthropic", "openai", "mistral"}) {
    LogCapture::install();
    FakeProviderDeps d;
    const std::string h = host;
    FakeProviderDeps::ToolRig rig;
    int n = 0;
    lumiRig(d, rig, h, n);
    auto pd = d.contract();
    std::string conv, out, err;
    bool ok = false;
    if (h == "anthropic") ok = providers::orchTurnAnthropic(pd, conv, kLumiIns, kLumiUser, out, err, &rig.ht, nullptr);
    if (h == "openai") ok = providers::orchTurnOpenAI(pd, conv, kLumiIns, kLumiUser, out, err, &rig.ht, nullptr);
    if (h == "mistral") ok = providers::orchTurnMistral(pd, conv, kLumiIns, kLumiUser, out, err, &rig.ht, nullptr);
    size_t largest = 0;
    for (const auto& r : d.http.seen) largest = std::max(largest, r.body.size());
    char msg[200];
    std::snprintf(msg, sizeof msg, "%s: ok=%d requests=%zu round0=%zu largest=%zu refit=%d err=%s", host, (int)ok,
                  d.http.seen.size(), d.http.seen.empty() ? 0 : d.http.seen[0].body.size(), largest,
                  (int)LogCapture::contains("B budget - folded"), err.c_str());
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE_MESSAGE(ok, msg);
    TEST_ASSERT_TRUE_MESSAGE(out.find("done") != std::string::npos, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(9, (int)d.http.seen.size(), msg);   // 8 tool rounds + the answer, no extra
    TEST_ASSERT_TRUE_MESSAGE(d.http.seen[0].body.size() >= 62000, msg);   // the scenario really is Lumi-sized
    for (const auto& r : d.http.seen) TEST_ASSERT_TRUE_MESSAGE(r.body.size() <= providers::kWireBodyMax, msg);
    // The whole-page contract (CUM-848's guard, restated on the CI evidence of
    // runs 40631977181/40640475595: a host's fold can repack WHICH request first
    // carries a round's result, so "request k carries round k" was expectation
    // rigidity, not the contract): every round's result must reach the model
    // WHOLE at least once - a result no request ever carries whole is an unseen
    // result the ladder lost, exactly the clip the fold may never take. A
    // failure dumps the marker layout so a genuine loss names itself.
    for (size_t k = 1; k < d.http.seen.size() && k <= 8; k++) {
      const std::string mark = "|end" + std::to_string(k) + "|";
      if (anyBodyHas(d, mark)) continue;
      TEST_ASSERT_TRUE_MESSAGE(false, (std::string(msg) + " A6 round " + std::to_string(k) +
                                       "'s result never reached the model whole (" + mark +
                                       " in no request); markers per request:" + markerLayout(d))
                                      .c_str());
    }
    refitSeen = refitSeen || LogCapture::contains("B budget - folded");
  }
  TEST_ASSERT_TRUE(refitSeen);
}

// The production path (the fabric loop): a refit is not a failed step. The engine's
// chain, whose FIRST cause is what the owner hears when a turn fails, never records
// a refusal the loop recovered from; a refusal no refit can fit still fails the turn
// with that refusal as its error.
static void test_fabric_refit_is_not_a_failed_step() {
  FakeProviderDeps d;
  FakeProviderDeps::ToolRig rig;
  int n = 0;
  lumiRig(d, rig, "mistral", n);
  auto pd = d.contract();
  std::vector<std::string> stepErrors;
  rig.ht.onStepError = [&stepErrors](const std::string&, const std::string& e) { stepErrors.push_back(e); };
  LogCapture::install();
  std::string out, err;
  nimbus::orch::TurnSpend sp;
  const bool ok = providers::runFabricLoop(pd, {"mistral"}, kLumiIns, kLumiUser, out, err, rig.ht, &sp, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_TRUE(LogCapture::contains("B budget - folded"));   // the refit really ran
  TEST_ASSERT_EQUAL_INT(0, (int)stepErrors.size());
  TEST_ASSERT_EQUAL_INT(9, (int)d.http.seen.size());
  // Nothing can fit (the prompt alone is over the budget): the turn fails with the
  // refusal as its error, and still nothing is sent.
  FakeProviderDeps big;
  auto pdb = big.contract();
  FakeProviderDeps::ToolRig rb;
  big.fillTools(rb);
  for (const auto& s : deviceShapedSpecs()) rb.ht.specs.push_back(s);
  std::string out2, err2;
  const bool ok2 = providers::runFabricLoop(pdb, {"mistral"}, std::string(59000, 's'), std::string(40000, 'u'),
                                            out2, err2, rb.ht, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok2);
  TEST_ASSERT_TRUE_MESSAGE(err2.find("request body over the 85 KB budget") == 0, err2.c_str());
  TEST_ASSERT_EQUAL_INT(0, (int)big.http.seen.size());
}

// The stub itself: valid JSON arguments, id and name kept (the pair with its result
// stays), small arguments and final-answer correction pairs left whole, the replay
// count follows.
static void test_stub_arguments_keeps_pairs() {
  Transcript tr;
  tr.addUser("[USER] save it");
  tr.addToolCall({"c1", "artifact_save", "{\"text\":\"" + std::string(18000, 'w') + "\"}"}, 0, "");
  tr.addToolResult({"c1", "artifact_save", "saved a1", false}, 0);
  tr.addToolCall({"c2", "memory_search", "{\"q\":\"tea\"}"}, 1, "");
  tr.addToolResult({"c2", "memory_search", "no hits", false}, 1);
  tr.addToolCall({nimbus::orch::kFinalCheckCallId, "orch_turn", "{\"reply\":\"" + std::string(2000, 'r') + "\"}"}, 1,
                 "");
  const size_t before = tr.toolBytes();
  const size_t freed = tr.stubArguments(512);
  TEST_ASSERT_TRUE(freed > 17000);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(before - freed), (uint32_t)tr.toolBytes());
  const auto& e = tr.entries();
  TEST_ASSERT_EQUAL_STRING("c1", e[1].id.c_str());
  TEST_ASSERT_EQUAL_STRING("artifact_save", e[1].name.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"omitted\":\"18011 bytes; this call already ran\"}", e[1].text.c_str());
  JsonDocument parsed;
  TEST_ASSERT_TRUE(deserializeJson(parsed, e[1].text) == DeserializationError::Ok);
  TEST_ASSERT_TRUE(parsed.is<JsonObject>());
  TEST_ASSERT_EQUAL_STRING("{\"q\":\"tea\"}", e[3].text.c_str());   // small: whole
  TEST_ASSERT_TRUE(e[5].text.size() > 2000);                           // orch_turn: whole
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)tr.stubArguments(512));        // idempotent
}

// The big write on every real wire: the model saves 18 KB in round 0 (it runs), and
// round 1's request would carry it again over the budget. The refit stubs the executed
// call's arguments; each wire still renders a valid call/result pair; the owner gets an
// answer with no extra request.
static void test_big_write_on_every_loop_wire() {
  for (const char* host : {"anthropic", "openai", "mistral"}) {
    LogCapture::install();
    FakeProviderDeps d;
    const std::string h = host;
    d.http.script.push_back({"", "", 200, bigWriteResponse(h, 18000)});
    d.http.script.push_back({"", "", 200, finalResponse(h)});
    auto pd = d.contract();
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    for (const auto& sp : deviceShapedSpecs()) rig.ht.specs.push_back(sp);
    rig.ht.cfg.maxRounds = 8;
    rig.ht.cfg.maxToolResultBytes = 8192;
    rig.ht.cfg.maxTotalToolBytes = 65536;
    rig.result = "saved artifact a1 (18000 B)";
    std::string conv, out, err;
    bool ok = false;
    if (h == "anthropic") ok = providers::orchTurnAnthropic(pd, conv, kLumiIns, kLumiUser, out, err, &rig.ht, nullptr);
    if (h == "openai") ok = providers::orchTurnOpenAI(pd, conv, kLumiIns, kLumiUser, out, err, &rig.ht, nullptr);
    if (h == "mistral") ok = providers::orchTurnMistral(pd, conv, kLumiIns, kLumiUser, out, err, &rig.ht, nullptr);
    char msg[200];
    std::snprintf(msg, sizeof msg, "%s: ok=%d requests=%zu round0=%zu err=%s", host, (int)ok, d.http.seen.size(),
                  d.http.seen.empty() ? 0 : d.http.seen[0].body.size(), err.c_str());
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE_MESSAGE(ok, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)rig.dispatched.size(), msg);   // the write ran once
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, (int)d.http.seen.size(), msg);      // no extra request
    for (const auto& r : d.http.seen) TEST_ASSERT_TRUE_MESSAGE(r.body.size() <= providers::kWireBodyMax, msg);
    const std::string& second = d.http.seen[1].body;
    TEST_ASSERT_TRUE_MESSAGE(second.find("this call already ran") != std::string::npos, msg);
    TEST_ASSERT_TRUE_MESSAGE(second.find("tc0000001") != std::string::npos, msg);   // the pair stays
    TEST_ASSERT_TRUE_MESSAGE(second.find("saved artifact a1") != std::string::npos, msg);   // its result too
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_refusal_wording_round_trips);
  RUN_TEST(test_lumi_loop_finishes_with_an_answer);
  RUN_TEST(test_every_growing_loop_ends_with_an_answer);
  RUN_TEST(test_unfittable_round_fails_honestly);
  RUN_TEST(test_lumi_shape_on_every_loop_wire);
  RUN_TEST(test_fabric_refit_is_not_a_failed_step);
  RUN_TEST(test_big_write_arguments_still_answer);
  RUN_TEST(test_stub_arguments_keeps_pairs);
  RUN_TEST(test_big_write_on_every_loop_wire);
  return UNITY_END();
}
