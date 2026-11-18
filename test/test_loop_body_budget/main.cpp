#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/prompt_layout.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/body_budget.h"
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/transcript.h"
#include "nimbus/orch/turn_images.h"
#include "../../lib/harness/src/providers/image_parts.h"   // photoTokens, photoTextBudget, LoopImages
#include "../../lib/harness/src/providers/wire.h"          // wire::lastRound, the round fit

// A tool loop whose next request would outgrow the wire body budget never fails the
// whole turn after paying its rounds (Lumi, v4.6.0, 26 Oct 2026: the post-update turn
// ran 8 paid rounds on Mistral, the body grew 70 KB -> 86.9 KB, the forced final round
// was refused before the send, "request body over the 85 KB budget: 86865 B", and the
// turn failed with no answer). The refused request never left the device, so the loop
// folds the older tool rounds and runs the same round again.

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
using harness_test::bodyHas;
using harness_test::reqBody;
using nimbus::orch::HeadLoopConfig;
using nimbus::orch::HeadLoopHooks;
using nimbus::orch::HeadOutcome;
using nimbus::orch::HeadStep;
using nimbus::orch::HeadToolCall;
using nimbus::orch::HeadToolResult;
using nimbus::orch::Transcript;
namespace providers = agent::providers;

// The suite's real-wire legs are calibrated to the 85,000 B budget every head had in
// v5.1.1 (Lumi's turn shape crosses it); the per-wire legs set their own table.
void setUp() {
  LogCapture::install();
  providers::setTestBudgetOverride((uint32_t)providers::kWireBodyMax);
}
void tearDown() {
  providers::setTestBudgetOverride(0);
  providers::setTestBudgetSource(nullptr);
}

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
  TEST_ASSERT_FALSE(providers::bodyWithinBudget(std::string(providers::kWireBodyMax + 7, 'x'), providers::kWireBodyMax, err));
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

// Lane PROG: the real case's refit shape (Lumi, 4 Nov,
// evidence/lumi-v490-watch/stuck-errlog-20261104T162414Z.txt, log lines 932-933):
// round 2's body was 91322 B against the 85000 B budget, the first rung folded
// 5582 B of the replayed tool rounds, the round re-ran and refused again at
// 85882 B, and only the second cycle's fold made it fit - seven such double
// refusals over the turn, each extra one a full body build and parse cycle spent
// discovering what the first refusal already said. The refit now folds rungs
// until the overshoot is covered, so this shape re-runs the round exactly once.
// The one-rung-per-refusal policy this replaces needed the second refusal (the
// log lines above are its failing-first record; the matrix test above caught the
// same class as refused=12 against sent=9 before the fix landed).
static void test_real_case_refit_shape() {
  Transcript tr;
  int dispatched = 0, calls = 0, refused = 0;
  std::vector<size_t> sent;
  size_t fitFreed = 0;
  const size_t budget = 85000;
  tr.addUser("[USER]\\nwhat changed in the update?");
  auto bodySize = [&] {
    size_t bytes = 0;
    for (const auto& it : tr.entries()) bytes += it.text.size() + it.name.size() + it.id.size();
    return 78648 + bytes - fitFreed;   // tuned: round 2 refuses at exactly 91322 B
  };
  HeadLoopConfig cfg;
  cfg.maxRounds = 8;
  cfg.deadlineMs = 0;
  cfg.roundMinHeap = 0;
  cfg.maxToolResultBytes = 8192;
  cfg.maxTotalToolBytes = 65536;
  HeadLoopHooks h;
  h.nowMs = [] { return 1000u; };
  h.log = [](const std::string&) {};
  h.transcript = &tr;
  h.dispatch = [&](const HeadToolCall& c) {
    HeadToolResult r;
    r.id = c.id;
    r.name = c.name;
    // Round 0's big result is the fold's rung; round 1's five results are the
    // newest round the fold must never touch.
    r.output = dispatched++ == 0 ? std::string(5890, 'a') : std::string(1290, 'b');
    return r;
  };
  h.fitPrompt = [&](size_t want, nimbus::orch::FitScope) {
    const size_t got = want > 4000 ? 4000 : want;   // a prompt ladder with bounded rungs
    fitFreed += got;
    return got;
  };
  h.step = [&](bool allowTools, const std::vector<HeadToolResult>&, uint32_t, const std::string&) {
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
    if (!allowTools || calls >= 6) {
      s.finished = true;
      s.finalTurn = "{\"reply\":\"done\"}";
      return s;
    }
    const int n = calls == 0 ? 1 : 5;
    for (int i = 0; i < n; ++i) {
      HeadToolCall c;
      c.id = "c" + std::to_string(++calls);
      c.name = "memory_search";
      c.argsJson = "{\"q\":\"update notes\"}";
      s.toolCalls.push_back(c);
    }
    return s;
  };
  const HeadOutcome o = nimbus::orch::runHeadLoop(cfg, h);
  char msg[160];
  std::snprintf(msg, sizeof msg, "ok=%d refused=%d sent=%zu err=%s", (int)o.ok, refused,
                sent.size(), o.error.c_str());
  TEST_ASSERT_TRUE_MESSAGE(o.ok, msg);
  TEST_ASSERT_TRUE_MESSAGE(o.error.empty(), msg);
  // One refusal: the fold covers the whole 6322 B overshoot (plus room) before
  // the re-run, where the real case needed two.
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, refused, msg);
  // Rounds 0, 1 and 2 - no extra provider request spent on the refit.
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, (int)sent.size(), msg);
  // The newest round's results the model has not seen yet are never clipped:
  // all five round 1 results come back whole.
  int newestResults = 0;
  for (const auto& it : tr.entries())
    if (it.kind == nimbus::orch::TranscriptItem::Kind::ToolResult && it.text.size() == 1290) newestResults++;
  TEST_ASSERT_EQUAL_INT_MESSAGE(5, newestResults, "the newest round was clipped");
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
    ++n;   // sequenced out of the marker expression: ++n and n unsequenced in one
           // expression is UB, and on x86-64 the marker read the OLD value, so every
           // result was labeled |end{n-1}| and the whole marker layout shifted by one
    r.output = "entry " + std::to_string(n) + " " + std::string(2600, 'e') + " |end" + std::to_string(n) + "|";
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

// ---- every wire carries its own budget (lane W, v5.1.2) ---------------------------------
// The budget of a request is the one its host and model have (wireBudgetFor), read per
// round: a wire that reads a turn-global figure, another host's budget, the v5.1.1
// constant or the direct row on a routed request fails these. The table below stands in
// for the derived budgets (request_budget.h): a distinct figure per host and model,
// routed or not.
namespace {
nimbus::orch::RequestBudget tableBudget(const std::string& host, const std::string& model, bool routed) {
  nimbus::orch::RequestBudget b = nimbus::orch::requestBudgetFor(host, model, routed);
  uint32_t bytes = 69000;   // custom: the floor-shaped row
  if (host == "mistral") bytes = model.rfind("mistral-medium", 0) == 0 ? 71000 : 73000;
  else if (host == "openai") bytes = 75000;
  else if (host == "anthropic") bytes = 77000;
  else if (host == "zai") bytes = 79000;
  if (routed) bytes += 10000;
  b.wireBodyBytes = bytes;
  return b;
}
// The test's budget source for the rest of a case, and the override, both cleared after.
struct BudgetScope {
  uint32_t before;
  explicit BudgetScope(providers::WireBudgetFn fn, uint32_t bytes = 0) : before(providers::testBudgetOverride()) {
    providers::setTestBudgetSource(fn);
    providers::setTestBudgetOverride(bytes);
  }
  ~BudgetScope() {
    providers::setTestBudgetSource(nullptr);
    providers::setTestBudgetOverride(before);
  }
};

enum class WireKind { Anthropic, OpenAI, Mistral, ZaiLoop, CustomShot };
struct WireCase {
  const char* name;
  const char* host;    // the budget's host
  const char* model;
  bool routed;
  WireKind kind;
};
const WireCase kWires[] = {
    {"anthropic", "anthropic", "claude-sonnet-5-5", false, WireKind::Anthropic},
    {"openai", "openai", "gpt-5.6", false, WireKind::OpenAI},
    {"mistral", "mistral", "mistral-large-4-0", false, WireKind::Mistral},
    {"zai", "zai", "glm-5.3", false, WireKind::ZaiLoop},
    {"custom", "custom", "gpt-5.6-luna", false, WireKind::CustomShot},
    {"cumulo/anthropic", "anthropic", "claude-sonnet-5-5", true, WireKind::Anthropic},
    {"cumulo/openai", "openai", "gpt-5.6", true, WireKind::OpenAI},
    {"cumulo/mistral", "mistral", "mistral-large-4-0", true, WireKind::Mistral},
    {"cumulo/zai", "zai", "glm-5.3", true, WireKind::ZaiLoop},
};
std::string finalFor(WireKind k) {
  if (k == WireKind::Anthropic) return finalResponse("anthropic");
  if (k == WireKind::OpenAI) return finalResponse("openai");
  if (k == WireKind::CustomShot) return "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"{\\\"reply\\\":\\\"done\\\"}\"}}]}";
  return finalResponse("mistral");
}
struct WireRun {
  size_t sent = 0;
  size_t bodySize = 0;
  std::string err;
  agent::HttpRequest req;
};
// One round on the wire (the custom endpoint: its single shot) with this prompt and seed.
WireRun runWire(const WireCase& w, const std::string& ins, const std::string& seed) {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, finalFor(w.kind)});
  providers::ProviderDeps pd = d.contract();
  const std::string model = w.model;
  pd.orchModel = [model](const char*) { return model; };
  if (w.routed) {
    pd.routerBase = [] { return std::string("https://router.example"); };
    pd.routerKey = [] { return std::string("cumulo_sk_test"); };
  }
  if (w.kind == WireKind::ZaiLoop) {
    if (w.routed) {
      providers::configureRouterCompatHead(pd, "zai", model);
    } else {
      pd.customBase = [] { return std::string("https://api.z.ai"); };
      pd.customPathPrefix = [] { return std::string("/api/paas/v4"); };
      pd.customKey = [] { return std::string("zai-key"); };
      pd.customConv = [] { return std::string("openai"); };
      pd.customModel = [model] { return model; };
      providers::applyZaiHeadPolicy(pd);
    }
  }
  if (w.kind == WireKind::CustomShot) {
    pd.customBase = [] { return std::string("https://llm.example.lan"); };
    pd.customModel = [model] { return model; };
  }
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  Transcript tr;
  tr.addUser(seed);
  HeadStep st;
  std::string conv, out, err;
  switch (w.kind) {
    case WireKind::Anthropic: st = providers::antLoopStep(pd, ins, rig.ht, nullptr, tr)(true, {}, UINT32_MAX, ""); break;
    case WireKind::OpenAI: st = providers::oaiLoopStep(pd, ins, rig.ht, nullptr, tr)(true, {}, UINT32_MAX, ""); break;
    case WireKind::Mistral: st = providers::misLoopStep(pd, ins, rig.ht, nullptr, tr)(true, {}, UINT32_MAX, ""); break;
    case WireKind::ZaiLoop: st = providers::zaiLoopStep(pd, ins, rig.ht, nullptr, tr)(true, {}, UINT32_MAX, ""); break;
    case WireKind::CustomShot:
      st.ok = providers::orchTurnCustom(pd, conv, ins, seed, out, err, nullptr, nullptr);
      st.error = err;
      break;
  }
  WireRun r;
  r.sent = d.http.seen.size();
  if (r.sent) {
    r.req = d.http.seen[0];
    r.bodySize = d.http.seen[0].body.size();
  }
  r.err = st.ok ? std::string() : st.error;
  return r;
}
// The seed that makes this wire's body exactly `want` bytes under prompt `ins` (ASCII,
// so a seed byte is a body byte), found by measuring with the budget out of the way.
std::string seedFor(const WireCase& w, const std::string& ins, size_t want) {
  BudgetScope wide(nullptr, 1000000);
  std::string seed = "x";
  for (int i = 0; i < 4; ++i) {
    const size_t got = runWire(w, ins, seed).bodySize;
    if (got == want) return seed;
    seed.resize(seed.size() + want - got, 'x');
  }
  return seed;
}
}  // namespace

// DoD: under each head's own budget, every loop wire (and the custom single shot)
// accepts a body of exactly B and refuses B + 1 locally, with nothing sent.
static void test_every_wire_accepts_its_own_budget_and_refuses_one_more() {
  const std::string ins = "SYS " + std::string(40000, 's');
  for (const WireCase& w : kWires) {
    const size_t B = tableBudget(w.host, w.model, w.routed).wireBodyBytes;
    const std::string seed = seedFor(w, ins, B);
    BudgetScope table(tableBudget);
    LogCapture::install();
    const WireRun at = runWire(w, ins, seed);
    char msg[220];
    std::snprintf(msg, sizeof msg, "%s: budget %zu, body %zu, sent %zu, err %s", w.name, B, at.bodySize, at.sent,
                  at.err.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)at.sent, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)B, (uint32_t)at.bodySize, msg);
    const WireRun over = runWire(w, ins, seed + "x");
    std::snprintf(msg, sizeof msg, "%s: budget %zu, sent %zu, err %s", w.name, B, over.sent, over.err.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)over.sent, msg);   // refused locally: nothing left the device
    TEST_ASSERT_EQUAL_STRING_MESSAGE(nimbus::orch::bodyBudgetError(B, B + 1).c_str(), over.err.c_str(), msg);
    std::snprintf(msg, sizeof msg, "%s: accepts %zu B (1 request sent), refuses %zu B locally (0 sent)", w.name, B,
                  B + 1);
    TEST_MESSAGE(msg);
  }
}

// The class guard's counter-test: the same request under the v5.1.1 figure (85,000 B)
// is refused on every wire whose own budget is larger, and sent under its own.
static void test_the_wire_reads_the_budget_not_the_constant() {
  auto big = [](const std::string& host, const std::string& model, bool routed) {
    nimbus::orch::RequestBudget b = nimbus::orch::requestBudgetFor(host, model, routed);
    b.wireBodyBytes = 100000;
    return b;
  };
  const std::string ins = "SYS " + std::string(50000, 's');
  for (const WireCase& w : kWires) {
    const std::string seed = seedFor(w, ins, 95000);
    {
      BudgetScope own(+big);
      TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)runWire(w, ins, seed).sent, w.name);
    }
    BudgetScope legacy(nullptr, (uint32_t)providers::kWireBodyMax);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)runWire(w, ins, seed).sent, w.name);
  }
}

// A Large 4 head cut twice steps down to Medium on the same host (the fabric's one
// same-host step): Medium's budget is smaller, and the round that fit Large 4 is refused
// locally on Medium, before anything is sent there.
static void test_large4_to_medium_refuses_the_oversize_round_locally() {
  auto table = [](const std::string& host, const std::string& model, bool routed) {
    nimbus::orch::RequestBudget b = nimbus::orch::requestBudgetFor(host, model, routed);
    b.wireBodyBytes = model.rfind("mistral-medium", 0) == 0 ? 90000 : 120000;
    return b;
  };
  BudgetScope scope(+table);
  FakeProviderDeps d;
  const std::string cut =
      "{\"choices\":[{\"finish_reason\":\"length\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"abcdefghi\","
      "\"function\":{\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"do\"}}]}}]}";
  d.http.script.push_back({"", "", 200, cut});
  d.http.script.push_back({"", "", 200, cut});
  providers::ProviderDeps pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("mistral-large-4-0"); };
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  LogCapture::install();
  std::string out, err;
  const bool ok = providers::runFabricLoop(pd, {"mistral"}, "SYS " + std::string(50000, 's'),
                                           std::string(50000, 'u'), out, err, rig.ht, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains("one same-host step on mistral-medium-latest"), err.c_str());
  TEST_ASSERT_EQUAL_INT(2, (int)d.http.seen.size());   // the two Large 4 attempts; nothing reached Medium
  for (const auto& r : d.http.seen) TEST_ASSERT_TRUE(r.body.find("mistral-large-4-0") != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(err.find("request body over the 90 KB budget") == 0, err.c_str());
}

// The step-down holds for the rest of the turn (release review v5.1.2, finding 1): every
// rebuild of the host's step after it (the controller's prompt refit, a photo or a seed fit,
// all through RoundRecovery::fitPrompt) builds it on the stepped-down model, at that model's
// own budget. Before the fix the refit rebuilt the step on the configured Large 4: request 2
// went out on mistral-large-4-0 and nothing ever reached Medium.
static void test_large4_step_down_holds_through_the_refit() {
  auto table = [](const std::string& host, const std::string& model, bool routed) {
    nimbus::orch::RequestBudget b = nimbus::orch::requestBudgetFor(host, model, routed);
    b.wireBodyBytes = model.rfind("mistral-medium", 0) == 0 ? 90000 : 120000;
    return b;
  };
  BudgetScope scope(+table);
  for (const char* head : {"mistral-large-4-0", "mistral-large-latest"}) {
    FakeProviderDeps d;
    const std::string cut =
        "{\"choices\":[{\"finish_reason\":\"length\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"abcdefghi\","
        "\"function\":{\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"do\"}}]}}]}";
    const std::string tool =
        "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":"
        "\"abcdefghj\",\"function\":{\"name\":\"memory_search\",\"arguments\":\"{\\\"q\\\":\\\"x\\\"}\"}}]}}]}";
    d.http.script.push_back({"", "", 200, cut});
    d.http.script.push_back({"", "", 200, cut});
    d.http.script.push_back({"", "", 200, tool});
    d.http.script.push_back({"", "", 200, finalResponse("mistral")});
    providers::ProviderDeps pd = d.contract();
    const std::string m = head;
    pd.orchModel = [m](const char*) { return m; };
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    std::string prompt = "SYS " + std::string(50000, 's');
    const std::string input = std::string(50000, 'u');
    int refits = 0;
    rig.ht.fitPrompt = [&](size_t want, nimbus::orch::FitScope, std::string& ins, std::string& inp) {
      const size_t give = std::min(want, prompt.size() - 1000);
      prompt.resize(prompt.size() - give);
      ins = prompt;
      inp = input;
      ++refits;
      return give;
    };
    LogCapture::install();
    std::string out, err;
    const bool ok = providers::runFabricLoop(pd, {"mistral"}, prompt, input, out, err, rig.ht, nullptr, nullptr);
    TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
    TEST_ASSERT_TRUE_MESSAGE(LogCapture::contains("one same-host step on mistral-medium-latest"), head);
    TEST_ASSERT_TRUE_MESSAGE(refits > 0, head);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, (int)d.http.seen.size(), head);
    for (size_t i = 0; i < d.http.seen.size(); ++i) {
      const bool onMedium = d.http.seen[i].body.find("\"model\":\"mistral-medium-latest\"") != std::string::npos;
      char what[96];
      std::snprintf(what, sizeof what, "%s request %u", head, (unsigned)i);
      TEST_ASSERT_EQUAL_MESSAGE(i >= 2, onMedium, what);   // every request after the step is on Medium
      if (i >= 2) TEST_ASSERT_TRUE_MESSAGE(d.http.seen[i].body.size() <= 90000, what);   // at Medium's budget
    }
  }
}

// ---- photos inside the budget and the time fit -------------------------------------------
namespace {
// A JPEG of `w` x `h` (its frame header, then filler) of `len` bytes: what photoTokens reads.
std::vector<uint8_t> jpegOf(uint32_t w, uint32_t h, size_t len) {
  std::vector<uint8_t> b(len, 0);
  const uint8_t head[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, (uint8_t)(h >> 8), (uint8_t)h,
                          (uint8_t)(w >> 8), (uint8_t)w, 0x03};
  std::memcpy(b.data(), head, sizeof head);
  return b;
}
struct Album {
  std::vector<std::vector<uint8_t>> bytes;
  nimbus::orch::TurnImages set;
  int transcribed = 0;
  std::string text;
  Album(size_t n, size_t len, uint32_t w, uint32_t h) {
    set.nonce = "w4lb";
    set.uploadOnce = false;
    for (size_t i = 0; i < n; ++i) bytes.push_back(jpegOf(w, h, len));
    text = "[USER]\n";
    for (size_t i = 0; i < n; ++i) {
      nimbus::orch::TurnImage it;
      char ref[17];
      std::snprintf(ref, sizeof ref, "%016zx", i + 1);
      it.ref = ref;
      it.mime = "image/jpeg";
      it.bytes = bytes[i].data();
      it.len = bytes[i].size();
      it.k = (int)i + 1;
      it.n = (int)n;
      it.tag = "[image " + std::to_string(i + 1) + "/" + std::to_string(n) + " ref=" + ref + "]";
      it.segment = it.tag + " (attached: the photo itself is in this message)";
      text += it.segment + "\n";
      set.items.push_back(it);
    }
    text += "What is on these?";
    set.transcribe = [this](nimbus::orch::TurnImage& it) {
      ++transcribed;
      return it.tag + " (machine transcription: a shelf label)";
    };
  }
};
size_t imageParts(const std::string& body) {
  size_t n = 0;
  for (size_t p = body.find("\"image_url\""); p != std::string::npos; p = body.find("\"image_url\"", p + 1)) ++n;
  return n;
}
}  // namespace

// A photo round's text budget is the request budget less the photos' window cost, and a
// body over it is refused before the send; the same body with no photos is sent.
static void test_photo_round_text_budget_shrinks_by_the_estimate() {
  auto table = [](const std::string& host, const std::string& model, bool routed) {
    nimbus::orch::RequestBudget b = nimbus::orch::requestBudgetFor(host, model, routed);
    b.wireBodyBytes = model == "big" ? 200000 : 100000;
    b.rNomX100 = 250;
    return b;
  };
  BudgetScope scope(+table);
  Album album(3, 2000, 1280, 960);   // 1,600 tokens each on Anthropic (1,638 before its cap)
  const uint32_t tokens = providers::imageparts::photoTokens(&album.set, true, false,
                                                             providers::imageparts::Dialect::Anthropic);
  TEST_ASSERT_EQUAL_UINT32(4800, tokens);
  TEST_ASSERT_EQUAL_UINT32(188000, (uint32_t)providers::imageparts::photoTextBudget(table("anthropic", "big", false), tokens));
  // Never under the budget every head had in v5.1.1.
  TEST_ASSERT_EQUAL_UINT32(85000, (uint32_t)providers::imageparts::photoTextBudget(table("anthropic", "x", false), 60000));
  // On the wire: a 100,000 B budget less 4,800 photo tokens at 2.5 B is 88,000 B of text.
  const std::string ins = "SYS " + std::string(50000, 's');
  const std::string seed = album.text + std::string(42000, 'u');
  for (bool withPhotos : {true, false}) {
    FakeProviderDeps d;
    d.http.script.push_back({"", "", 200, finalResponse("anthropic")});
    providers::ProviderDeps pd = d.contract();
    pd.orchModel = [](const char*) { return std::string("claude-sonnet-5-5"); };
    FakeProviderDeps::ToolRig rig;
    d.fillTools(rig);
    const nimbus::orch::ActiveTurnImages active(withPhotos ? &album.set : nullptr);
    Transcript tr;
    tr.addUser(seed);
    const providers::imageparts::LoopImages photos(tr);
    const HeadStep st = providers::antLoopStep(pd, ins, rig.ht, nullptr, tr)(true, {}, UINT32_MAX, "");
    if (withPhotos) {
      TEST_ASSERT_EQUAL_INT(0, (int)d.http.seen.size());
      TEST_ASSERT_TRUE_MESSAGE(st.error.find("request body over the 88 KB budget") == 0, st.error.c_str());
    } else {
      TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)d.http.seen.size(), st.error.c_str());
    }
  }
}

namespace {
// The Mistral loop with a turn's album on Large 4, the DMA pool fragmented (the round's fit
// counts the transport's pacing), and a prompt whose older history the engine can give up
// down to `keepFloor` bytes (the turn's give-up order, in miniature).
struct AlbumRun {
  bool ok = false;
  std::string err;
  std::vector<agent::HttpRequest> seen;
  std::vector<agent::BodySplices> spliced;
  int historyGives = 0;
};
AlbumRun runAlbum(Album& album, size_t promptBytes, size_t seedPad, size_t keepFloor) {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, finalResponse("mistral")});
  providers::ProviderDeps pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("mistral-large-4-0"); };
  pd.dmaLargestBlock = [] { return (uint32_t)4000; };   // under kDmaChunkFloor: paced
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  AlbumRun r;
  // The engine's side of a refit hands back the whole prompt and input the next request
  // sends (engine.cpp fitPrompt): the "history" here is the prompt's tail.
  std::string prompt = "SYS " + std::string(promptBytes, 's');
  const std::string seed = album.text + std::string(seedPad, 'u');
  rig.ht.fitPrompt = [&r, &prompt, &seed, keepFloor](size_t want, nimbus::orch::FitScope scope, std::string& ins,
                                                      std::string& inp) {
    if (scope != nimbus::orch::FitScope::OlderHistory || prompt.size() <= keepFloor) return (size_t)0;
    const size_t give = std::min(want, prompt.size() - keepFloor);
    prompt.resize(prompt.size() - give);
    ins = prompt;
    inp = seed;
    ++r.historyGives;
    return give;
  };
  const nimbus::orch::ActiveTurnImages active(&album.set);
  std::string conv, out;
  r.ok = providers::orchTurnMistral(pd, conv, prompt, seed, out, r.err, &rig.ht, nullptr);
  r.seen = d.http.seen;
  r.spliced = d.http.spliced;
  return r;
}
}  // namespace

// Ruling 5 (photos come before older history): a 10-photo album on Large 4 with a
// fragmented heap goes natively. Its first build cannot carry the minimum final inside
// Mistral's 60 s window, so the round gives up older history first and goes with every
// photo as an image part, inside its fit.
static void test_album_on_large4_fragmented_goes_natively() {
  auto table = [](const std::string& host, const std::string& model, bool routed) {
    nimbus::orch::RequestBudget b = nimbus::orch::requestBudgetFor(host, model, routed);
    b.wireBodyBytes = 360000;
    b.rNomX100 = 360;
    return b;
  };
  BudgetScope scope(+table);
  Album album(10, 40000, 640, 480);
  LogCapture::install();
  const AlbumRun r = runAlbum(album, 55000, 50000, 8000);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.err.c_str());
  TEST_ASSERT_TRUE(r.historyGives > 0);                 // older history went first
  TEST_ASSERT_EQUAL_INT(1, (int)r.seen.size());         // nothing over the fit was sent
  TEST_ASSERT_EQUAL_INT(10, (int)r.spliced[0].size());  // every photo as an image part
  TEST_ASSERT_EQUAL_INT(0, album.transcribed);
  TEST_ASSERT_FALSE(LogCapture::contains("transcribed: did not fit"));
  const providers::wire::RoundRecord& rec = providers::wire::lastRound();
  const providers::wire::RoundBudget rb = providers::wire::roundBudget(providers::ProviderDeps(), "mistral",
                                                                       "mistral-large-4-0", false);
  TEST_ASSERT_TRUE(rec.load.fragmented);
  TEST_ASSERT_TRUE(rec.load.imageBytes > 0);
  TEST_ASSERT_TRUE(providers::wire::roundFitTokens(rb, rec.roundMs, rec.load) >= providers::wire::minFinalTokens(rb));
}

// The other half: an album no history cut can fit (2 MB of photos on a fragmented heap
// outlast the 60 s window by themselves) goes as transcriptions, said so in the log, once
// the older history is at its floor.
static void test_album_that_cannot_fit_goes_as_transcriptions() {
  auto table = [](const std::string& host, const std::string& model, bool routed) {
    nimbus::orch::RequestBudget b = nimbus::orch::requestBudgetFor(host, model, routed);
    b.wireBodyBytes = 360000;
    b.rNomX100 = 360;
    return b;
  };
  BudgetScope scope(+table);
  Album album(10, 200000, 1600, 1200);
  LogCapture::install();
  const AlbumRun r = runAlbum(album, 30000, 0, 8000);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.err.c_str());
  TEST_ASSERT_TRUE(r.historyGives > 0);   // history first, even here
  TEST_ASSERT_EQUAL_INT(1, (int)r.seen.size());
  TEST_ASSERT_EQUAL_INT(0, (int)r.spliced[0].size());   // no photo bytes on the wire
  TEST_ASSERT_EQUAL_INT(0, (int)imageParts(r.seen[0].body));
  TEST_ASSERT_TRUE(r.seen[0].body.find("machine transcription") != std::string::npos);
  TEST_ASSERT_TRUE(LogCapture::contains("transcribed: did not fit the mistral time window"));
}

// DoD: photo rounds never exceed the fit on any 50 or 60 s host. The class: albums of 1 to
// 10 photos and several sizes, healthy and fragmented, on every photo-capable head whose
// window is 60 s (Mistral direct and routed; Z.ai's GLM is text-only and never shows one):
// every request that left carried a fit of at least the head's minimum final.
static void test_photo_rounds_never_exceed_the_fit_on_short_windows() {
  auto table = [](const std::string& host, const std::string& model, bool routed) {
    nimbus::orch::RequestBudget b = nimbus::orch::requestBudgetFor(host, model, routed);
    b.wireBodyBytes = 360000;
    b.rNomX100 = 360;
    return b;
  };
  BudgetScope scope(+table);
  int native = 0, transcribed = 0;
  for (const char* model : {"mistral-large-4-0", "mistral-medium-latest"})
    for (bool routed : {false, true})
      for (bool fragmented : {false, true})
        for (size_t n : {1u, 3u, 10u})
          for (size_t len : {20000u, 120000u, 200000u}) {
            Album album(n, len, 1280, 960);
            FakeProviderDeps d;
            d.http.script.push_back({"", "", 200, finalResponse("mistral")});
            providers::ProviderDeps pd = d.contract();
            const std::string m = model;
            pd.orchModel = [m](const char*) { return m; };
            if (fragmented) pd.dmaLargestBlock = [] { return (uint32_t)4000; };
            if (routed) {
              pd.routerBase = [] { return std::string("https://router.example"); };
              pd.routerKey = [] { return std::string("cumulo_sk_test"); };
            }
            FakeProviderDeps::ToolRig rig;
            d.fillTools(rig);
            const nimbus::orch::ActiveTurnImages active(&album.set);
            std::string conv, out, err;
            const bool ok = providers::orchTurnMistral(pd, conv, "SYS " + std::string(30000, 's'), album.text, out,
                                                       err, &rig.ht, nullptr);
            char msg[160];
            std::snprintf(msg, sizeof msg, "%s routed=%d frag=%d n=%zu len=%zu: %s", model, (int)routed,
                          (int)fragmented, n, len, err.c_str());
            TEST_ASSERT_TRUE_MESSAGE(ok, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)d.http.seen.size(), msg);
            const providers::wire::RoundRecord& rec = providers::wire::lastRound();
            const providers::wire::RoundBudget rb =
                providers::wire::roundBudget(providers::ProviderDeps(), "mistral", m, routed);
            TEST_ASSERT_TRUE_MESSAGE(rec.roundMs <= 65000, msg);
            if (rec.load.imageBytes > 0) {
              ++native;
              TEST_ASSERT_TRUE_MESSAGE(
                  providers::wire::roundFitTokens(rb, rec.roundMs, rec.load) >= providers::wire::minFinalTokens(rb), msg);
            } else {
              ++transcribed;
            }
          }
  TEST_ASSERT_TRUE(native > 0);        // the matrix really sends photos
  TEST_ASSERT_TRUE(transcribed > 0);   // and really crosses the fit
}

// Lane B's caps send at least 1 token and leave it to the wire to refuse a round whose fit
// cannot carry the host's minimum final. A text round over v5.1.1's 85,000 B that is too big
// for its host's read window (a budget larger than the window allows) is refused with the
// size wording at the size whose fit carries it, so the turn gives up older history and the
// round goes. A round inside 85,000 B goes as v5.1.1 sent it, whatever its fit.
static void test_text_round_over_its_time_fit_is_refused_and_refit() {
  auto table = [](const std::string& host, const std::string& model, bool routed) {
    nimbus::orch::RequestBudget b = nimbus::orch::requestBudgetFor(host, model, routed);
    b.wireBodyBytes = 900000;   // more than Large 4's 60 s window can read and answer in
    return b;
  };
  BudgetScope scope(+table);
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, finalResponse("mistral")});
  providers::ProviderDeps pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("mistral-large-4-0"); };
  pd.dmaLargestBlock = [] { return (uint32_t)4000; };
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  agent::layout::PromptParts parts;
  parts.split = true;
  parts.stat = "SYS";
  parts.dyn = "[NOW] 09:00";
  for (int i = 0; i < 150; ++i) {
    parts.history.push_back({true, "question " + std::to_string(i) + " " + std::string(1600, 'q')});
    parts.history.push_back({false, "answer " + std::to_string(i) + " " + std::string(1600, 'a')});
  }
  int gives = 0;
  rig.ht.fitPrompt = [&parts, &gives](size_t want, nimbus::orch::FitScope, std::string& ins, std::string& inp) {
    size_t freed = 0;
    while (freed < want && parts.history.size() > 2) {
      freed += parts.history[0].text.size() + parts.history[1].text.size();
      parts.history.erase(parts.history.begin(), parts.history.begin() + 2);
    }
    ins = agent::layout::join(parts);
    inp = "hi";
    ++gives;
    return freed;
  };
  LogCapture::install();
  std::string conv, out, err;
  const bool ok = providers::orchTurnMistral(pd, conv, agent::layout::join(parts), "hi", out, err, &rig.ht, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
  TEST_ASSERT_TRUE(LogCapture::contains("round over its time fit"));
  TEST_ASSERT_TRUE(gives > 0);
  TEST_ASSERT_EQUAL_INT(1, (int)d.http.seen.size());   // only the round that fits was sent
  const providers::wire::RoundRecord& sent = providers::wire::lastRound();
  const providers::wire::RoundBudget rb =
      providers::wire::roundBudget(providers::ProviderDeps(), "mistral", "mistral-large-4-0", false);
  TEST_ASSERT_TRUE(sent.load.bodyBytes > providers::kWireBodyMax);
  TEST_ASSERT_TRUE(providers::wire::roundFitTokens(rb, 0, sent.load) >= providers::wire::minFinalTokens(rb));
  // Inside 85,000 B the round goes whatever its fit (an id with the slowest rates).
  FakeProviderDeps s;
  s.http.script.push_back({"", "", 200, finalResponse("mistral")});
  providers::ProviderDeps spd = s.contract();
  spd.orchModel = [](const char*) { return std::string("model-mistral"); };
  spd.dmaLargestBlock = [] { return (uint32_t)4000; };
  FakeProviderDeps::ToolRig srig;
  s.fillTools(srig);
  std::string sout, serr;
  TEST_ASSERT_TRUE_MESSAGE(providers::orchTurnMistral(spd, conv, "SYS " + std::string(50000, 's'),
                                                      std::string(30000, 'u'), sout, serr, &srig.ht, nullptr),
                           serr.c_str());
  TEST_ASSERT_EQUAL_INT(1, (int)s.http.seen.size());
}

// A head turn's newest message over its string budget gives up what that message holds (the
// turn's ladder a rung at a time: NextRung), never the tool rounds or the older history
// first, and the round goes once it fits.
static void test_seed_over_its_budget_gives_up_its_own_content() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, finalResponse("mistral")});
  providers::ProviderDeps pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  agent::layout::PromptParts parts;
  parts.split = true;
  parts.stat = "SYS";
  parts.dyn = "[NOW] 09:00\n[RELEVANT MEMORIES] " + std::string(20000, 'm');
  std::string input = std::string(40000, 'u');
  std::vector<nimbus::orch::FitScope> scopes;
  rig.ht.fitPrompt = [&](size_t want, nimbus::orch::FitScope scope, std::string& ins, std::string& inp) {
    scopes.push_back(scope);
    const size_t had = parts.dyn.size();
    parts.dyn = "[NOW] 09:00";   // the recall rung
    ins = agent::layout::join(parts);
    inp = input;
    (void)want;
    return had - parts.dyn.size();
  };
  LogCapture::install();
  std::string conv, out, err;
  TEST_ASSERT_TRUE_MESSAGE(
      providers::orchTurnMistral(pd, conv, agent::layout::join(parts), input, out, err, &rig.ht, nullptr), err.c_str());
  TEST_ASSERT_EQUAL_INT(1, (int)scopes.size());
  TEST_ASSERT_TRUE(scopes[0] == nimbus::orch::FitScope::NextRung);
  TEST_ASSERT_EQUAL_INT(1, (int)d.http.seen.size());
  TEST_ASSERT_TRUE(LogCapture::contains("newest message over its budget"));
}

// A round late in a turn (8 s left) never asks for less than the host's minimum final: the
// fit of its time would be a certain cut, and the socket deadline bounds it anyway (v5.1.1's
// floor was 3,000 tokens). A bench seam's forced cut still goes out as set, spent once.
static void test_a_short_round_keeps_the_minimum_final_and_seams_spend_once() {
  FakeProviderDeps d;
  d.http.script.push_back({"", "", 200, finalResponse("mistral")});
  providers::ProviderDeps pd = d.contract();
  pd.orchModel = [](const char*) { return std::string("mistral-large-4-0"); };
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  Transcript tr;
  tr.addUser("hi");
  const HeadStep st = providers::misLoopStep(pd, "SYS " + std::string(50000, 's'), rig.ht, nullptr, tr)(
      false, {}, providers::kMinRoundMs, "deadline");
  TEST_ASSERT_TRUE_MESSAGE(st.ok, st.error.c_str());
  const providers::wire::RoundRecord& sent = providers::wire::lastRound();
  const providers::wire::RoundBudget rb =
      providers::wire::roundBudget(providers::ProviderDeps(), "mistral", "mistral-large-4-0", false);
  TEST_ASSERT_TRUE(providers::wire::roundFitTokens(rb, sent.roundMs, sent.load) < providers::wire::minFinalTokens(rb));
  TEST_ASSERT_EQUAL_UINT32(providers::wire::minFinalTokens(rb), sent.cap.maxTokens);
  TEST_ASSERT_TRUE(bodyHas(d, 0, ("\"max_tokens\":" + std::to_string(sent.cap.maxTokens)).c_str()));
  // The bench's forced cut (setTestCutRounds): one round, 48 tokens, spent by that round.
  providers::setTestCutRounds(1, 48);
  FakeProviderDeps c;
  c.http.script.push_back({"", "", 200, finalResponse("mistral")});
  providers::ProviderDeps cpd = c.contract();
  cpd.orchModel = [](const char*) { return std::string("mistral-large-4-0"); };
  Transcript ctr;
  ctr.addUser("hi");
  providers::misLoopStep(cpd, "SYS", rig.ht, nullptr, ctr)(true, {}, UINT32_MAX, "");
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(c, 0, "\"max_tokens\":48"), reqBody(c, 0).substr(0, 200).c_str());
  TEST_ASSERT_TRUE(providers::outputCapFor("mistral", "mistral-large-4-0", providers::RoundKind::ToolRound, 0).maxTokens > 48);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_refusal_wording_round_trips);
  RUN_TEST(test_lumi_loop_finishes_with_an_answer);
  RUN_TEST(test_every_growing_loop_ends_with_an_answer);
  RUN_TEST(test_real_case_refit_shape);
  RUN_TEST(test_unfittable_round_fails_honestly);
  RUN_TEST(test_lumi_shape_on_every_loop_wire);
  RUN_TEST(test_fabric_refit_is_not_a_failed_step);
  RUN_TEST(test_big_write_arguments_still_answer);
  RUN_TEST(test_stub_arguments_keeps_pairs);
  RUN_TEST(test_big_write_on_every_loop_wire);
  RUN_TEST(test_every_wire_accepts_its_own_budget_and_refuses_one_more);
  RUN_TEST(test_the_wire_reads_the_budget_not_the_constant);
  RUN_TEST(test_large4_to_medium_refuses_the_oversize_round_locally);
  RUN_TEST(test_large4_step_down_holds_through_the_refit);
  RUN_TEST(test_photo_round_text_budget_shrinks_by_the_estimate);
  RUN_TEST(test_album_on_large4_fragmented_goes_natively);
  RUN_TEST(test_album_that_cannot_fit_goes_as_transcriptions);
  RUN_TEST(test_photo_rounds_never_exceed_the_fit_on_short_windows);
  RUN_TEST(test_text_round_over_its_time_fit_is_refused_and_refit);
  RUN_TEST(test_seed_over_its_budget_gives_up_its_own_content);
  RUN_TEST(test_a_short_round_keeps_the_minimum_final_and_seams_spend_once);
  return UNITY_END();
}
