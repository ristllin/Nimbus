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
      c.argsJson = "{\"q\":\"update notes\"}";
      s.toolCalls.push_back(c);
      return s;
    };
    return nimbus::orch::runHeadLoop(cfg, h);
  }
};
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
    TEST_ASSERT_TRUE_MESSAGE(d.http.seen[7].body.find("|end7|") != std::string::npos, msg);   // newest whole
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_refusal_wording_round_trips);
  RUN_TEST(test_lumi_loop_finishes_with_an_answer);
  RUN_TEST(test_every_growing_loop_ends_with_an_answer);
  RUN_TEST(test_unfittable_round_fails_honestly);
  RUN_TEST(test_lumi_shape_on_every_loop_wire);
  RUN_TEST(test_fabric_refit_is_not_a_failed_step);
  return UNITY_END();
}
