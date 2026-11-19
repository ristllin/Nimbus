// test_request_budget - the one request budget (request_budget.h) and every ContextBudget
// field derived from it (budget.h deriveRequestContext), as properties over every
// recorded model, the four router upstreams and the custom endpoint (lane B of the
// history fix, plan v3 protocols and RULINGS-v3). It also writes the generated table
// docs/reference/request-budgets.md: re-bless with
//   GOLDEN_UPDATE=1 pio test -e native -f test_request_budget
// after reading the diff.
#include <unity.h>

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../support/recorded_models.h"
#include "nimbus/orch/compact.h"   // kChatSummaryMax
#include "nimbus/harness/conversation.h"   // historyDayBytes - the day floor of v5.1.1's window
#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/providers.h"   // flatPromptBudget - the wires' flat cap
#include "nimbus/orch/budget.h"
#include "nimbus/orch/connectors_wire.h"
#include "nimbus/orch/model_family.h"
#include "nimbus/orch/model_window.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/wire_limits.h"

using namespace nimbus::orch;
namespace providers = agent::providers;

void setUp() {
  installHistoryProfile(kDefaultHistoryProfile);
  installListedCtxLookup(&recorded::listedLookup);
  clearObservedRatios();
  installBudgetLink(BudgetLink::Device);
}
void tearDown() {
  setRoutedBudgetLiftForTest(-1);
  installListedCtxLookup(nullptr);
  clearObservedRatios();
  installBudgetLink(BudgetLink::Device);
}

// ---- the rows: every recorded model, the router's four upstreams, the custom endpoint --

struct Head {
  std::string host;
  std::string model;
  bool routed = false;
};

static const Head kRouted[] = {
    {"cumulo", "openai/gpt-5.6", true},
    {"cumulo", "anthropic/claude-sonnet-5-5", true},
    {"cumulo", "mistral/mistral-large-4-0", true},
    {"cumulo", "zai/glm-5.3", true},
    {"cumulo", "gpt-5.6", true},
};
static const Head kCustom[] = {{"custom", "gpt-5.6-luna", false}, {"custom", "llama-3-70b", false}};

static std::vector<Head> allHeads() {
  std::vector<Head> out;
  for (const recorded::Model& m : recorded::models()) out.push_back({m.provider, m.id, false});
  for (const Head& h : kRouted) out.push_back(h);
  for (const Head& h : kCustom) out.push_back(h);
  return out;
}

static std::string nameOf(const Head& h) { return h.host + "/" + h.model; }

static double rNom(const RequestBudget& b) { return b.rNomX100 / 100.0; }
static double rSafe(const RequestBudget& b) { return b.rSafeX100 / 100.0; }

static bool atFloor(const RequestBudget& b) {
  return b.bound == BudgetBound::Floor || b.bound == BudgetBound::Held;
}

// The worst round at the budget: a fragmented heap, nothing cached, minFinal at the
// decode floor of the target's context.
static uint32_t worstRoundMs(const RequestBudget& b) {
  return roundOverheadMs(b.link, b.wireBodyBytes, true, b.targetTokens, b.prefillTokPerS) +
         (uint32_t)((uint64_t)b.minFinalTokens * 1000 / b.decodeTokPerS);
}

// ---- the bounds ------------------------------------------------------------------------

static void test_every_head_inside_the_ruling_and_the_window() {
  for (const Head& h : allHeads()) {
    const RequestBudget b = requestBudgetFor(h.host, h.model, h.routed);
    const std::string what = nameOf(h);
    TEST_ASSERT_TRUE_MESSAGE(b.targetTokens <= kBudgetTargetClampTokens, what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(b.windowTokens > 0 && b.rNomX100 > 0 && b.rSafeX100 > 0, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)(b.wireBodyBytes * 100ull / b.rNomX100), b.targetTokens, what.c_str());
    if (!atFloor(b))
      TEST_ASSERT_TRUE_MESSAGE(b.targetTokens <= 0.75 * b.windowTokens * rSafe(b) / rNom(b) + 1, what.c_str());
    // The floor: never under it, and it is never more than 0.85 of the window at rSafe.
    TEST_ASSERT_TRUE_MESSAGE(b.wireBodyBytes >= b.floorBytes, what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(b.floorBytes <= kLegacyWireBodyBytes, what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(b.floorBytes <= 0.85 * b.windowTokens * rSafe(b) + 1, what.c_str());
    if (b.windowTokens >= 40000) TEST_ASSERT_TRUE_MESSAGE(b.wireBodyBytes >= kLegacyWireBodyBytes, what.c_str());
  }
}

// Every head above the floor fits its worst round (fragmented heap, cold prompt, minFinal
// at the decode floor) inside its read window. A head at the floor is v5.1.1's budget
// (no head below it); its rounds fit through the load-aware caps (test_output_caps).
static void test_every_head_above_the_floor_fits_its_worst_round() {
  for (BudgetLink link : {BudgetLink::Device, BudgetLink::Server}) {
    for (const Head& h : allHeads()) {
      const RequestBudget b = requestBudgetFor(h.host, h.model, h.routed, link);
      if (atFloor(b)) continue;
      const std::string what = nameOf(h) + " " + std::to_string(worstRoundMs(b)) + " ms";
      TEST_ASSERT_TRUE_MESSAGE(worstRoundMs(b) <= b.readWindowMs, what.c_str());
      TEST_ASSERT_TRUE_MESSAGE(b.latencyTokens * 1000ull >= b.targetTokens * 999ull, what.c_str());
    }
  }
}

// Haiku 5.5's price tier: a prompt over 100K tokens bills at the long rate, so its budget
// stays under 0.9 x 100k x 2.0 B (the dense Claude ratio).
static void test_haiku_inside_its_price_tier() {
  const RequestBudget b = requestBudgetFor("anthropic", "claude-haiku-5-5", false);
  TEST_ASSERT_EQUAL_UINT32(100000, b.tierTokens);
  TEST_ASSERT_TRUE(b.wireBodyBytes <= 180000);
  TEST_ASSERT_EQUAL((int)BudgetBound::Tier, (int)b.bound);
}

// The figures the plan quotes come out of the one formula (RULINGS-v3: Large 4 at a
// 50 tok/s decode floor and a 1,500-token minFinal still reaches the 100k clamp).
static void test_the_plan_figures() {
  struct Row { const char* host; const char* model; uint32_t bytes; BudgetBound bound; };
  static const Row kRows[] = {
      {"mistral", "mistral-large-4-0", 360000, BudgetBound::Clamp},
      {"mistral", "mistral-medium-latest", 151000, BudgetBound::Latency},
      {"mistral", "mistral-small-latest", 151000, BudgetBound::Latency},
      {"openai", "gpt-5.6-luna", 360000, BudgetBound::Clamp},   // 3.6 B/token after the live calibration
      {"openai", "gpt-5.6", 360000, BudgetBound::Clamp},
      {"anthropic", "claude-sonnet-5-5", 250000, BudgetBound::Clamp},
      {"anthropic", "claude-opus-5-5", 250000, BudgetBound::Clamp},
      {"anthropic", "claude-haiku-5-5", 180000, BudgetBound::Tier},
      {"openai", "gpt-4o", 240000, BudgetBound::Window},
      {"custom", "gpt-5.6-luna", 85000, BudgetBound::Floor},
  };
  for (const Row& r : kRows) {
    const RequestBudget b = requestBudgetFor(r.host, r.model, false);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.bytes, b.wireBodyBytes, r.model);
    TEST_ASSERT_EQUAL_MESSAGE((int)r.bound, (int)b.bound, r.model);
  }
  TEST_ASSERT_EQUAL_UINT32(100000, requestBudgetFor("mistral", "mistral-large-4-0", false).targetTokens);
  TEST_ASSERT_EQUAL_UINT32(48709, requestBudgetFor("mistral", "mistral-medium-latest", false).targetTokens);
  TEST_ASSERT_EQUAL_UINT32(72000, requestBudgetFor("anthropic", "claude-haiku-5-5", false).targetTokens);
  // glm-5.3 is latency-bound under its 50 s window once every slice's pace is counted.
  const RequestBudget glm = requestBudgetFor("zai", "glm-5.3", false);
  TEST_ASSERT_EQUAL((int)BudgetBound::Latency, (int)glm.bound);
  TEST_ASSERT_TRUE(glm.wireBodyBytes > 150000 && glm.wireBodyBytes < 211000);
  TEST_ASSERT_EQUAL_UINT32(1600, glm.minFinalTokens);
  // Large 4: the forced reasoner's minFinal and the measured decode floor.
  const RequestBudget l4 = requestBudgetFor("mistral", "mistral-large-4-0", false);
  TEST_ASSERT_EQUAL_UINT32(1500, l4.minFinalTokens);
  TEST_ASSERT_EQUAL_UINT32(50, l4.decodeTokPerS);
  TEST_ASSERT_EQUAL_UINT32(60000, l4.readWindowMs);
  TEST_ASSERT_EQUAL_UINT32(65000, l4.roundMs);
}

// A bigger window never shrinks a budget (the listed window drives it here).
static uint32_t g_window = 0;
static uint32_t windowLookup(const char*, const char*) { return g_window; }
static void test_monotonic_in_the_window() {
  installListedCtxLookup(&windowLookup);
  struct HM { const char* host; const char* model; };
  for (const HM& hm : {HM{"zai", "glm-4.6"}, HM{"openai", "gpt-x"}, HM{"anthropic", "claude-haiku-5-5"},
                       HM{"mistral", "mistral-medium-latest"}, HM{"mistral", "voxtral-mini-latest"}}) {
    uint32_t prev = 0;
    for (g_window = 4000; g_window <= 2000000; g_window += 4000) {
      const uint32_t bytes = requestBudgetFor(hm.host, hm.model, false).wireBodyBytes;
      TEST_ASSERT_TRUE_MESSAGE(bytes >= prev, hm.model);
      prev = bytes;
    }
  }
}

// ---- routed heads, custom, the link -------------------------------------------------------

// v5.1.2 (kRoutedBudgetLift false): every routed head, on cumulo or a direct host routed,
// sits exactly on the floor; with the lift a cumulo selector is its upstream's routed row.
static void test_routed_heads_at_the_floor_until_the_lift() {
  setRoutedBudgetLiftForTest(0);   // RB-BG: the off state, whatever the compiled line says
  for (const Head& h : kRouted) {
    const RequestBudget b = requestBudgetFor(h.host, h.model, true);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.floorBytes, b.wireBodyBytes, h.model.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(kLegacyWireBodyBytes, b.wireBodyBytes, h.model.c_str());
    TEST_ASSERT_TRUE_MESSAGE(b.routed && b.readWindowMs <= kRoutedReadWindowMs, h.model.c_str());
    TEST_ASSERT_TRUE_MESSAGE(b.roundMs <= kRoutedReadWindowMs, h.model.c_str());
  }
  TEST_ASSERT_EQUAL_UINT32(kLegacyWireBodyBytes, requestBudgetFor("anthropic", "claude-sonnet-5-5", true).wireBodyBytes);
  struct Pick { const char* up; const char* id; };
  for (const Pick& p : {Pick{"openai", "gpt-5.6"}, Pick{"anthropic", "claude-sonnet-5-5"},
                        Pick{"mistral", "mistral-large-4-0"}, Pick{"zai", "glm-5.3"}}) {
    const RequestBudget c = requestBudgetFor("cumulo", std::string(p.up) + "/" + p.id, true, BudgetLink::Device, true);
    const RequestBudget d = requestBudgetFor(p.up, p.id, true, BudgetLink::Device, true);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(d.wireBodyBytes, c.wireBodyBytes, p.id);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(d.windowTokens, c.windowTokens, p.id);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(d.readWindowMs, c.readWindowMs, p.id);
    TEST_ASSERT_TRUE_MESSAGE(c.wireBodyBytes > kLegacyWireBodyBytes, p.id);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(std::min<uint32_t>(readWindowMsFor(p.up, p.id, false), 90000), c.readWindowMs, p.id);
  }
  setRoutedBudgetLiftForTest(-1);
  TEST_ASSERT_EQUAL(kRoutedBudgetLift, routedBudgetLift());
}

static void test_custom_gets_the_floor_on_the_flat_wire() {
  for (const Head& h : kCustom) {
    const RequestBudget b = requestBudgetFor(h.host, h.model, false);
    TEST_ASSERT_EQUAL_UINT32(kLegacyWireBodyBytes, b.wireBodyBytes);
    TEST_ASSERT_TRUE(b.flat);
    TEST_ASSERT_EQUAL((int)BudgetBound::Floor, (int)b.bound);
  }
  TEST_ASSERT_FALSE(requestBudgetFor("zai", "glm-5.3", false).flat);
  // A custom endpoint's id is the server's own: a prefix that names a router upstream (what
  // Groq, vLLM or OpenRouter serve) or a GLM name never takes that provider's budget or rates.
  for (const char* id : {"openai/gpt-oss-120b", "anthropic/claude-sonnet-5", "glm-5.3", "mistral/mistral-large-4-0"}) {
    const RequestBudget b = requestBudgetFor("custom", id, false);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(kLegacyWireBodyBytes, b.wireBodyBytes, id);
    TEST_ASSERT_TRUE_MESSAGE(b.flat, id);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(300, b.rNomX100, id);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1000, b.prefillTokPerS, id);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3000, b.minFinalTokens, id);
  }
  // A GLM name on OpenAI's wire reads OpenAI's figures, not Z.ai's.
  TEST_ASSERT_EQUAL_UINT16(360, requestBudgetFor("openai", "glm-5.3", false).rNomX100);
  TEST_ASSERT_EQUAL_UINT32(25000, prefillFloorTokPerS("openai", "glm-5.3"));
}

// nimbusd: no upload or pace term (RULINGS-v3), so a Linux VN's latency-bound heads go
// further; nothing else changes and the installed link is what the 3-argument form reads.
static void test_server_link_drops_upload_and_pace() {
  const RequestBudget dev = requestBudgetFor("zai", "glm-5.3", false, BudgetLink::Device);
  const RequestBudget vn = requestBudgetFor("zai", "glm-5.3", false, BudgetLink::Server);
  TEST_ASSERT_TRUE(vn.wireBodyBytes > dev.wireBodyBytes);
  TEST_ASSERT_EQUAL_UINT32(kBudgetConnectMs + 1000, roundOverheadMs(BudgetLink::Server, 390000, true, 9000, 9000));
  TEST_ASSERT_TRUE(roundOverheadMs(BudgetLink::Device, 390000, true, 9000, 9000) > 15000);
  installBudgetLink(BudgetLink::Server);
  TEST_ASSERT_EQUAL_UINT32(vn.wireBodyBytes, requestBudgetFor("zai", "glm-5.3", false).wireBodyBytes);
  TEST_ASSERT_EQUAL((int)BudgetLink::Server, (int)requestBudgetFor("zai", "glm-5.3", false).link);
}

// ---- the tighten-only memo ---------------------------------------------------------------

static void test_the_memo_only_ever_lowers() {
  const RequestBudget before = requestBudgetFor("openai", "gpt-4o", false);
  noteObservedRatio("openai", "gpt-4o", 500000, 100000);   // 5.0 B/token: looser, ignored
  TEST_ASSERT_EQUAL_UINT32(before.wireBodyBytes, requestBudgetFor("openai", "gpt-4o", false).wireBodyBytes);
  noteObservedRatio("openai", "gpt-4o", 20000, 1000);      // under 2,048 tokens: ignored
  TEST_ASSERT_EQUAL_UINT16(0, observedRatioX100("openai", "gpt-4o"));
  noteObservedRatio("openai", "gpt-4o", 200000, 100000);   // 2.0 B/token: denser than rSafe
  TEST_ASSERT_EQUAL_UINT16(200, observedRatioX100("openai", "gpt-4o"));
  const RequestBudget dense = requestBudgetFor("openai", "gpt-4o", false);
  TEST_ASSERT_TRUE(dense.wireBodyBytes < before.wireBodyBytes);
  noteObservedRatio("openai", "gpt-4o", 300000, 100000);   // a looser one never raises it back
  TEST_ASSERT_EQUAL_UINT16(200, observedRatioX100("openai", "gpt-4o"));
  noteObservedRatio("openai", "gpt-4o", 50000, 100000);    // 0.5: held at the 1.5 floor
  TEST_ASSERT_EQUAL_UINT16(150, observedRatioX100("openai", "gpt-4o"));
  // Never under the floor, never on another head, and the clamp-bound heads keep theirs.
  TEST_ASSERT_TRUE(requestBudgetFor("openai", "gpt-4o", false).wireBodyBytes >= kLegacyWireBodyBytes);
  TEST_ASSERT_EQUAL_UINT16(0, observedRatioX100("openai", "gpt-5.6"));
  noteObservedRatio("anthropic", "claude-sonnet-5-5", 200000, 100000);
  TEST_ASSERT_EQUAL_UINT32(250000, requestBudgetFor("anthropic", "claude-sonnet-5-5", false).wireBodyBytes);
  // A full memo never evicts a head it holds (that would loosen it): a new head waits.
  for (int i = 0; i < 40; i++) noteObservedRatio("openai", "gpt-x-" + std::to_string(i), 200000, 100000);
  TEST_ASSERT_EQUAL_UINT16(150, observedRatioX100("openai", "gpt-4o"));
  TEST_ASSERT_EQUAL_UINT16(0, observedRatioX100("openai", "gpt-x-39"));
  // The router's selector and the direct id share a wire, so they share a memo.
  TEST_ASSERT_EQUAL_UINT16(150, observedRatioX100("cumulo", "openai/gpt-4o"));
}

// ---- every ContextBudget field: no shrink, the floor branch ---------------------------------

static std::map<std::string, JsonObjectConst> g_v511Rows;
static JsonDocument g_v511;
static const std::map<std::string, JsonObjectConst>& v511Rows() {
  if (g_v511Rows.empty()) {
    TEST_ASSERT_FALSE(deserializeJson(g_v511, recorded::readFixture("test/support/fixtures/budgets_v5_1_1.json")));
    for (JsonObjectConst r : g_v511["models"].as<JsonArrayConst>())
      g_v511Rows[std::string(r["provider"] | "") + "/" + (r["id"] | "")] = r;
  }
  return g_v511Rows;
}

static void assertNoShrink(const ContextBudget& c, JsonObjectConst old, const std::string& what) {
  const char* m = what.c_str();
  TEST_ASSERT_TRUE_MESSAGE(c.systemPromptBytes >= (old["systemPromptBytes"] | 0), m);
  const char* const kFields[] = {"chatSummaryBytes", "recentConvBytes", "toolResultBytes", "toolTotalBytes",
                                 "briefBytes", "foldSliceBytes", "historyMaxBytes", "historySoftBytes",
                                 "historyKeepBytes", "historyRows", "replayBytes", "perTurnFloorBytes",
                                 "historyMessageCap", "recallBytes"};
  const size_t have[] = {c.chatSummaryBytes, c.recentConvBytes, c.toolResultBytes, c.toolTotalBytes,
                         c.briefBytes, c.foldSliceBytes, c.historyMaxBytes, c.historySoftBytes,
                         c.historyKeepBytes, c.historyRows, c.replayBytes, c.perTurnFloorBytes,
                         c.historyMessageCap, c.recallBytes};
  for (size_t i = 0; i < sizeof(have) / sizeof(have[0]); i++) {
    size_t floor = (size_t)(old[kFields[i]] | 0u);
    // The summary: what a summary could actually be, v5.1.1's figure within what the fold store
    // keeps (v5.1.1's store kept 4,096 B; a gpt-6 row's 8,192 B was never stored). Lane CTXR1.
    if (i == 0) floor = std::min(floor, kChatSummaryMax);
    TEST_ASSERT_TRUE_MESSAGE(have[i] >= floor, (what + " " + kFields[i]).c_str());
  }
}

// No field shrinks (RULINGS-v3 item 8), field by field against budgets_v5_1_1.json, for
// every recorded model. foldStepBytes is the one new mark (soft - keep): the fold slice
// field keeps max(v5.1.1, foldStep).
static void test_no_field_below_v5_1_1_for_every_recorded_model() {
  const auto& rows = v511Rows();
  TEST_ASSERT_EQUAL((int)recorded::models().size(), (int)rows.size());
  for (const recorded::Model& m : recorded::models()) {
    const std::string key = m.provider + "/" + m.id;
    const ContextBudget c = deriveRequestContext(requestBudgetFor(m.provider, m.id, false), {});
    assertNoShrink(c, rows.at(key), key);
    TEST_ASSERT_TRUE_MESSAGE(c.foldSliceBytes >= c.foldStepBytes, key.c_str());
  }
}

// RULINGS-v3 item 3, over every model, the routed heads and custom: the history either
// has room (historyMax > 0, above v5.1.1's) or takes the floor branch, which is v5.1.1's
// window and the 48 KB trigger; the automatic fold trigger is never under 24 KB.
static void test_history_room_or_the_floor_branch_for_every_head() {
  size_t floors = 0;
  for (const Head& h : allHeads()) {
    const RequestBudget rb = requestBudgetFor(h.host, h.model, h.routed);
    const ContextBudget c = deriveRequestContext(rb, {});
    const ContextBudget old = deriveBudget(rb.v511CtxTokens, {});
    const std::string what = nameOf(h);
    TEST_ASSERT_TRUE_MESSAGE(c.floorBranch || c.historyMaxBytes > old.historyMaxBytes, what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(foldTriggerBytes(c, kCompactAtKbDefault) >= kFoldTriggerMinBytes, what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(c.historyKeepBytes <= c.historySoftBytes && c.historySoftBytes <= c.historyMaxBytes,
                             what.c_str());
    // Lane CTXR1: the summary a fold is told and capped at is never more than the fold store keeps.
    TEST_ASSERT_TRUE_MESSAGE(c.chatSummaryBytes <= kChatSummaryMax, what.c_str());
    if (!c.floorBranch) {
      TEST_ASSERT_TRUE_MESSAGE(c.foldStepBytes <= foldDigestCap(c.chatSummaryBytes), what.c_str());
      continue;
    }
    floors++;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(old.recentConvBytes, c.historyMaxBytes, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(old.historySoftBytes, c.historySoftBytes, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(48 * 1024, c.foldTriggerBytes, what.c_str());
  }
  TEST_ASSERT_TRUE(floors > 0);   // the custom endpoint and the routed heads at least
}

// A simulated week at the floor stays under the daily cap of 12 paid folds: a heavy owner
// day (160 exchanges of 3 KB, about ten times Lumi's 15 Nov average) folds every time the
// history passes the trigger.
static void test_a_week_at_the_floor_stays_under_the_daily_fold_cap() {
  for (const Head& h : allHeads()) {
    const ContextBudget c = deriveRequestContext(requestBudgetFor(h.host, h.model, h.routed), {});
    if (!c.floorBranch) continue;
    const size_t trigger = foldTriggerBytes(c, kCompactAtKbDefault);
    size_t pending = 0;
    for (int day = 0; day < 7; day++) {
      int folds = 0;
      for (int turn = 0; turn < 160; turn++) {
        pending += 3 * 1024;
        if (pending < trigger) continue;
        folds++;
        pending = c.historyKeepBytes < pending ? c.historyKeepBytes : 0;
      }
      TEST_ASSERT_TRUE_MESSAGE(folds <= 12, nameOf(h).c_str());
    }
  }
}

// A simulated fortnight on EVERY head (lane CTXR1): a household's 45 exchanges of 3 KB a day,
// the fold driver's rules (due at the trigger, counted on the bytes after thru; one cycle takes
// whole exchanges up to the kept tail and at most foldCycleBytes; 12 paid folds a local day on
// the device). The backlog never grows: at the end of every day what lies after thru is within
// one cycle of the trigger. With a cycle bounded by the steady-state step (soft - keep, 11.7 KB
// on Mistral Medium) a busy day at the cap left rows hidden that no later day caught up.
static void test_a_fortnight_on_every_head_works_off_its_backlog() {
  constexpr size_t kExchange = 3 * 1024;
  for (const Head& h : allHeads()) {
    const ContextBudget c = deriveRequestContext(requestBudgetFor(h.host, h.model, h.routed), {});
    const size_t trigger = foldTriggerBytes(c, kCompactAtKbDefault);
    const size_t keep = std::min(c.historyKeepBytes, trigger > kExchange ? trigger - kExchange : 0);
    const size_t cycle = foldCycleBytes(c);
    size_t after = 0;   // conversation bytes after thru
    for (int day = 0; day < 14; day++) {
      int paid = 0;
      for (int turn = 0; turn < 45; turn++) {
        after += kExchange;
        if (after < trigger || paid >= 12) continue;
        const size_t take = std::min(cycle, after - keep) / kExchange * kExchange;   // whole exchanges
        if (!take) continue;
        after -= take;
        paid++;
      }
      TEST_ASSERT_TRUE_MESSAGE(after <= trigger + cycle, (nameOf(h) + " day " + std::to_string(day)).c_str());
    }
  }
}

// The derived figures for the heads the plan quotes (replay at least the wire's v5.1.1 cap).
static void test_the_derived_fields_for_the_main_heads() {
  BudgetOverrides uncapped;
  uncapped.noProfile = true;   // lane B's formula itself; the profile caps are test_profile_*
  const ContextBudget l4 = deriveRequestContext(requestBudgetFor("mistral", "mistral-large-4-0", false), uncapped);
  TEST_ASSERT_EQUAL_INT(43200, l4.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(54000, l4.replayBytes);
  TEST_ASSERT_EQUAL_UINT32(6144, l4.chatSummaryBytes);
  TEST_ASSERT_EQUAL_UINT32(12288, l4.historyMessageCap);
  TEST_ASSERT_EQUAL_UINT32(206080, l4.historyMaxBytes);
  TEST_ASSERT_EQUAL_UINT32(164864, l4.historySoftBytes);
  TEST_ASSERT_EQUAL_UINT32(164864 - foldDigestCap(6144), l4.historyKeepBytes);
  TEST_ASSERT_EQUAL_UINT32(foldDigestCap(6144), l4.foldStepBytes);
  TEST_ASSERT_EQUAL_UINT32(164864, l4.foldTriggerBytes);
  TEST_ASSERT_FALSE(l4.floorBranch);
  const ContextBudget son = deriveRequestContext(requestBudgetFor("anthropic", "claude-sonnet-5-5", false), uncapped);
  TEST_ASSERT_EQUAL_INT(32768, son.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(8192, son.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(37500, son.replayBytes);
  TEST_ASSERT_EQUAL_UINT32(118080, son.historyMaxBytes);
  TEST_ASSERT_EQUAL_UINT32(59040, son.historyKeepBytes);
  TEST_ASSERT_EQUAL_UINT32(35424, son.foldStepBytes);
  const ContextBudget med = deriveRequestContext(requestBudgetFor("mistral", "mistral-medium-latest", false), {});
  TEST_ASSERT_EQUAL_UINT32(24576, med.replayBytes);   // Mistral's v5.1.1 replay, not 15% (22,650)
  TEST_ASSERT_EQUAL_UINT32(4096, med.chatSummaryBytes);
  TEST_ASSERT_FALSE(med.floorBranch);
  const ContextBudget cus = deriveRequestContext(requestBudgetFor("custom", "gpt-5.6-luna", false), {});
  TEST_ASSERT_TRUE(cus.floorBranch);
}

// The owner's compactAtKB: 0 off, 48 derived, another value the soft mark clamped to
// [8 KB, max - 8 KB]; the floor branch reads it as v5.1.1 does. Owner overrides of the
// system, result and total caps win verbatim, as in deriveBudget.
static void test_owner_settings() {
  BudgetOverrides uncapped;
  uncapped.noProfile = true;
  const ContextBudget l4 = deriveRequestContext(requestBudgetFor("mistral", "mistral-large-4-0", false), uncapped);
  TEST_ASSERT_EQUAL_UINT32(0, foldTriggerBytes(l4, 0));
  TEST_ASSERT_EQUAL_UINT32(l4.historySoftBytes, foldTriggerBytes(l4, kCompactAtKbDefault));
  TEST_ASSERT_EQUAL_UINT32(96 * 1024, foldTriggerBytes(l4, 96));
  TEST_ASSERT_EQUAL_UINT32(l4.historyMaxBytes - 8192, foldTriggerBytes(l4, 512));
  TEST_ASSERT_EQUAL_UINT32(8192, foldTriggerBytes(l4, 4));
  const ContextBudget floor = deriveRequestContext(requestBudgetFor("custom", "x", false), {});
  TEST_ASSERT_EQUAL_UINT32(48 * 1024, foldTriggerBytes(floor, kCompactAtKbDefault));
  TEST_ASSERT_EQUAL_UINT32(96 * 1024, foldTriggerBytes(floor, 96));
  TEST_ASSERT_EQUAL_UINT32(0, foldTriggerBytes(floor, 0));
  BudgetOverrides ov;
  ov.maxContextBytes = 20000;
  ov.toolResultCap = 2048;
  ov.toolTotalCap = 4096;
  const ContextBudget o = deriveRequestContext(requestBudgetFor("mistral", "mistral-large-4-0", false), ov);
  TEST_ASSERT_EQUAL_INT(20000, o.systemPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(2048, o.toolResultBytes);
  TEST_ASSERT_EQUAL_UINT32(4096, o.toolTotalBytes);
}

// ---- every id that means Large 4 (release review v5.1.2, finding 3) --------------------------

// One recorded Mistral id and whether its row is Large 4. The newest recorded list decides an
// id both lists name (Mistral moved mistral-large-latest from mistral-large-2512 to Large 4
// between the 2026-10 and the 14 Nov 2026 lists).
struct MistralId {
  std::string id;
  bool large4 = false;
  bool reasoning = false;
};
static std::vector<MistralId> recordedMistralIds() {
  std::vector<MistralId> out;
  std::map<std::string, bool> seen;
  for (const char* list : {"models_2026-11", "models_2026-10"}) {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, recorded::readFixture(std::string("test/support/fixtures/") + list +
                                                                 "/mistral.json")));
    for (JsonObjectConst m : doc["data"].as<JsonArrayConst>()) {
      std::vector<std::string> ids{m["id"] | ""};
      for (JsonVariantConst a : m["aliases"].as<JsonArrayConst>()) ids.push_back(a | "");
      bool large4 = std::string(m["name"] | "").rfind("mistral-large-4", 0) == 0;
      for (const std::string& id : ids) large4 = large4 || id.rfind("mistral-large-4", 0) == 0;
      const bool reasoning = m["capabilities"]["reasoning"] | false;
      for (const std::string& id : ids) {
        if (id.empty() || seen[id]) continue;
        seen[id] = true;
        out.push_back({id, large4, reasoning});
      }
    }
  }
  return out;
}

static bool sameRates(const RequestBudget& a, const RequestBudget& b) {
  return a.wireBodyBytes == b.wireBodyBytes && a.bound == b.bound && a.rNomX100 == b.rNomX100 &&
         a.prefillTokPerS == b.prefillTokPerS && a.decodeTokPerS == b.decodeTokPerS &&
         a.minFinalTokens == b.minFinalTokens && a.windowTokens == b.windowTokens;
}

// The class: every id a recorded list gives Large 4 (the row itself and each of its aliases)
// gets Large 4's budget, rates, effort and step-down, direct and as a router pick, and no
// other id does. Before the fix mistral-large-latest, the id the November list stores for
// Large 4, read as "any other Mistral id": 85,000 B at the floor.
static void test_every_id_that_means_large_4_gets_its_row() {
  const RequestBudget l4 = requestBudgetFor("mistral", "mistral-large-4-0", false);
  const RequestBudget l4Lift = requestBudgetFor("mistral", "mistral-large-4-0", true, BudgetLink::Device, true);
  int large4 = 0, others = 0;
  for (const MistralId& r : recordedMistralIds()) {
    const std::string what = r.id + (r.large4 ? " (Large 4)" : "");
    TEST_ASSERT_EQUAL_MESSAGE(r.large4, isMistralLarge4(r.id), what.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(r.large4, isMistralLarge4("mistral/" + r.id), what.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(r.large4, modelRefusesBuiltinConnectors(r.id), what.c_str());   // the 3004 refusal
    const bool glm = r.id.rfind("glm-", 0) == 0 || r.id.rfind("zai-glm", 0) == 0;
    TEST_ASSERT_EQUAL_MESSAGE(r.large4 || glm, mistralForcedReasoner(r.id), what.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(r.large4 || glm, mistralForcedReasoner("mistral/" + r.id), what.c_str());
    if (!r.large4) {
      if (r.reasoning)   // a model that reasons takes "low" exactly when it is forced to
        TEST_ASSERT_EQUAL_STRING_MESSAGE(glm ? "low" : "none", providers::mistralHeadEffort(r.id), what.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", providers::sameHostNonReasoningModel("mistral", r.id), what.c_str());
      others++;
      continue;
    }
    large4++;
    TEST_ASSERT_TRUE_MESSAGE(sameRates(l4, requestBudgetFor("mistral", r.id, false)), what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(sameRates(l4, requestBudgetFor("mistral", "mistral/" + r.id, false)), what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(
        sameRates(l4Lift, requestBudgetFor("cumulo", "mistral/" + r.id, true, BudgetLink::Device, true)), what.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("low", providers::mistralHeadEffort(r.id), what.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("low", providers::mistralHeadEffort("mistral/" + r.id), what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(providers::mistralTakesEffort(r.id), what.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("mistral-medium-latest", providers::sameHostNonReasoningModel("mistral", r.id),
                                     what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(providers::slowestTokensPerSecond("mistral", "mistral-large-4-0"),
                                     providers::slowestTokensPerSecond("mistral", r.id), what.c_str());
    const providers::OutputCap a = providers::outputCapFor("mistral", "mistral-large-4-0", providers::RoundKind::FinalRound, 65000);
    const providers::OutputCap c = providers::outputCapFor("mistral", r.id, providers::RoundKind::FinalRound, 65000);
    TEST_ASSERT_TRUE_MESSAGE(a.maxTokens == c.maxTokens && a.retryTokens == c.retryTokens, what.c_str());
  }
  TEST_ASSERT_TRUE(large4 >= 3);   // mistral-large-4, -4-0, -latest
  TEST_ASSERT_TRUE(others > 20);
  TEST_ASSERT_TRUE(isMistralLarge4("mistral-large-latest"));
  TEST_ASSERT_FALSE(isMistralLarge4("mistral-large-2512"));
}

// ---- the turn's form (lane L): a flat turn is sized from the flat prompt budget --------------

#include "table_doc.h"   // wireOf (a Cumulo head's upstream); the table it writes is checked last

// A router upstream the native wires do not drive: the compat single shot, flat always.
static const Head kCompat[] = {{"cumulo", "groq/llama-3-70b", true}};

static std::vector<Head> headsOfEveryForm() {
  std::vector<Head> out = allHeads();
  out.insert(out.end(), std::begin(kCompat), std::end(kCompat));
  return out;
}

// What a request carries besides the history, as the one formula counts it (budget.h): the
// tools array, the static system block, the summary, the per-turn block, the slack, the
// replay and 5% of the budget.
static uint64_t overheadOf(const ContextBudget& c, uint64_t budget) {
  return (34 + 24 + 8 + 8) * 1024ull + c.chatSummaryBytes + c.replayBytes + budget / 20;
}

static void assertSameContext(const ContextBudget& a, const ContextBudget& b, const std::string& what) {
  const size_t as[] = {a.ctxTokens, (size_t)a.systemPromptBytes, a.chatSummaryBytes, a.recentConvBytes, a.toolResultBytes,
                       a.toolTotalBytes, a.briefBytes, a.foldSliceBytes, a.historyMaxBytes, a.historySoftBytes,
                       a.historyKeepBytes, a.historyRows, a.replayBytes, a.foldStepBytes, a.perTurnFloorBytes,
                       a.historyMessageCap, a.recallBytes, a.foldTriggerBytes, (size_t)a.floorBranch};
  const size_t bs[] = {b.ctxTokens, (size_t)b.systemPromptBytes, b.chatSummaryBytes, b.recentConvBytes, b.toolResultBytes,
                       b.toolTotalBytes, b.briefBytes, b.foldSliceBytes, b.historyMaxBytes, b.historySoftBytes,
                       b.historyKeepBytes, b.historyRows, b.replayBytes, b.foldStepBytes, b.perTurnFloorBytes,
                       b.historyMessageCap, b.recallBytes, b.foldTriggerBytes, (size_t)b.floorBranch};
  for (size_t i = 0; i < sizeof(as) / sizeof(as[0]); i++)
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)bs[i], (uint32_t)as[i], (what + " field " + std::to_string(i)).c_str());
}

// The form a head turn takes on its wire, as the wires send it (the oracle the budget's own
// flag is held to; test_wire_fit_class checks the same on real requests): Anthropic and OpenAI
// send a split prompt on a single shot too (anthropic.cpp, openai.cpp); Mistral's Conversations
// single shot and Z.ai's chat-completions single shot are one flat string, the form of a turn
// with the tool loop off (mistral.cpp, custom.cpp); a custom endpoint and a router upstream
// with no tool loop have no other form. A Cumulo head is its upstream.
static bool flatOnTheWire(const Head& h, bool loopOn) {
  const std::string wire = wireOf(h).host;
  if (wire == "mistral" || wire == "zai") return !loopOn;
  return wire != "anthropic" && wire != "openai";
}

static void test_the_form_of_every_head_turn() {
  size_t flatOff = 0;
  for (const Head& h : headsOfEveryForm())
    for (const bool loopOn : {true, false}) {
      const bool want = flatOnTheWire(h, loopOn);
      const RequestBudget b = turnBudgetFor(h.host, h.model, loopOn);
      TEST_ASSERT_EQUAL_MESSAGE(want, b.flatTurn, (nameOf(h) + (loopOn ? " loop on" : " loop off")).c_str());
      // The body budget is the wire's own in either form: only the turn's context follows the form.
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(requestBudgetFor(h.host, h.model, h.routed).wireBodyBytes, b.wireBodyBytes,
                                       nameOf(h).c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(want ? std::min(b.wireBodyBytes, kFlatPromptBytes) : b.wireBodyBytes,
                                       b.contextBytes(), nameOf(h).c_str());
      flatOff += !loopOn && b.flatTurn;
    }
  TEST_ASSERT_TRUE(flatOff > 60);   // every Mistral and Z.ai model, custom, the routed Mistral and Z.ai heads
  TEST_ASSERT_EQUAL_UINT32(kJsonStrGuard - 4096, kFlatPromptBytes);
  TEST_ASSERT_EQUAL_UINT32(providers::flatPromptBudget(), kFlatPromptBytes);   // the wires refuse over the same figure
  // A host the form rule does not know (none is configured before a key is set) is sized as
  // before: no form is assumed for it, in either loop state.
  for (const char* host : {"", "gemini"})
    for (const bool loopOn : {true, false}) {
      const RequestBudget b = turnBudgetFor(host, "some-model", loopOn);
      TEST_ASSERT_FALSE_MESSAGE(b.flatTurn, host);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.wireBodyBytes, b.contextBytes(), host);
    }
  // requestBudgetFor is a single request's body budget and names no form, on any wire.
  for (const Head& h : headsOfEveryForm())
    TEST_ASSERT_FALSE_MESSAGE(requestBudgetFor(h.host, h.model, h.routed).flatTurn, nameOf(h).c_str());
}

// Lane RB-BG: what a measured floor head's request needs at its high water: the measured
// fixed overhead of its wire, the summary at its cap, the margin, and the window in wire bytes.
static uint64_t floorNeed(const ContextBudget& c, const RequestBudget& rb) {
  const bool anthropic = rb.measuredWire == 1;
  const uint64_t fixed = anthropic ? kFloorFixedAnthropicBytes : kFloorFixedOpenAIBytes;
  const unsigned pct = anthropic ? kFloorWirePerWindowAnthropicPct : kFloorWirePerWindowOpenAIPct;
  return fixed + c.chatSummaryBytes + kFloorMarginBytes + kFloorRoundReserveBytes +
         (uint64_t)c.historyMaxBytes * pct / 100;
}

// How far over the real cap of its form one head's turn budget puts the history (0: it fits).
// The cap is the wire's: the body budget for a split prompt, the flat prompt budget for one
// string. Derived marks: beside the history the formula counts overheadOf. The floor branch
// on a flat turn: v5.1.1's window (its high water, or the owner's day up to its cap) beside
// what one string carries with it, the formula's system block, the summary and the per-turn
// block (no tools array and no replay ride a single shot). The floor branch on a split
// prompt is v5.1.1's budget itself (RULINGS-v3: no head below it), the floor pass's case in
// test_wire_fit_class, and is not measured here.
static uint64_t historyOverTheCap(const Head& h, bool loopOn, BudgetLink link, bool lift) {
  const RequestBudget rb = turnBudgetFor(h.host, h.model, loopOn, link, lift);
  const ContextBudget c = deriveRequestContext(rb, {});
  const bool flat = flatOnTheWire(h, loopOn);
  const uint64_t cap = flat ? kFlatPromptBytes : rb.wireBodyBytes;
  uint64_t need = 0;
  if (c.floorMeasured)   // lane RB-BG: the measured overhead, the window in wire bytes
    need = floorNeed(c, rb);
  else if (!c.floorBranch)
    need = c.historyMaxBytes + overheadOf(c, cap);
  else if (flat && rb.wireBodyBytes >= kFlatPromptBytes)   // the string is the cap (a tiny window's body is smaller)
    need = std::max(c.historyMaxBytes, agent::historyDayBytes(rb.v511CtxTokens)) + (24 + 8) * 1024ull + c.chatSummaryBytes;
  return need > cap ? need - cap : 0;
}

// The class (lane L): over every recorded model, the routed heads, the custom endpoint and a
// compat upstream, with the tool loop on and off, on a device and a Virtual Nimbus, before and
// after the routed lift: the history the budget admits, plus what the formula counts beside
// it, never exceeds the real cap of the form the turn goes out in. Sized from the body budget,
// a loop-off Large 4 head got 206,080 B of history for a 55,904 B string.
static void test_the_derived_history_fits_the_form_of_the_turn() {
  size_t cases = 0, over = 0;
  uint64_t worstBy = 0;
  std::string worst;
  for (const BudgetLink link : {BudgetLink::Device, BudgetLink::Server})
    for (const bool lift : {false, true})
      for (const bool loopOn : {false, true})
        for (const Head& h : headsOfEveryForm()) {
          const uint64_t by = historyOverTheCap(h, loopOn, link, lift);
          cases++;
          over += by ? 1 : 0;
          if (by <= worstBy) continue;
          worstBy = by;
          worst = nameOf(h) + (loopOn ? " loop on" : " loop off") + (link == BudgetLink::Server ? " (server link)" : "");
        }
  const std::string what = std::to_string(over) + " of " + std::to_string(cases) + " head forms put the history over " +
                           "the cap of their form; the worst, " + worst + ", by " + std::to_string(worstBy) + " B";
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)over, what.c_str());
  TEST_ASSERT_TRUE(cases > 1500);
}

// A flat turn is sized as v5.1.1 sized it (RULINGS-v3 item 3, the floor branch): v5.1.1's
// history exactly, its 48 KB trigger, and never the large-target summary or message cap.
static void assertSizedAsV511(const Head& h, bool loopOn, BudgetLink link, bool lift) {
  const RequestBudget rb = turnBudgetFor(h.host, h.model, loopOn, link, lift);
  const ContextBudget c = deriveRequestContext(rb, {});
  const ContextBudget old = deriveBudget(rb.v511CtxTokens, {});
  const std::string what = nameOf(h) + (loopOn ? " loop on" : " loop off");
  TEST_ASSERT_TRUE_MESSAGE(c.floorBranch, what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(old.recentConvBytes, c.historyMaxBytes, what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(old.recentConvBytes, c.recentConvBytes, what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(old.historySoftBytes, c.historySoftBytes, what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(old.historyKeepBytes, c.historyKeepBytes, what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(old.historyRows, c.historyRows, what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(48 * 1024, c.foldTriggerBytes, what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(48 * 1024, foldTriggerBytes(c, kCompactAtKbDefault), what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(std::max<size_t>(old.chatSummaryBytes, 4096), c.chatSummaryBytes, what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(old.historyMessageCap, c.historyMessageCap, what.c_str());
}
static void test_a_flat_turn_is_sized_as_v5_1_1_sized_it() {
  size_t flat = 0;
  for (const BudgetLink link : {BudgetLink::Device, BudgetLink::Server})
    for (const bool lift : {false, true})
      for (const bool loopOn : {false, true})
        for (const Head& h : headsOfEveryForm()) {
          if (!flatOnTheWire(h, loopOn)) continue;
          assertSizedAsV511(h, loopOn, link, lift);
          flat++;
        }
  TEST_ASSERT_TRUE(flat > 280);   // every Mistral and Z.ai model with the loop off, custom and compat in both
}

// With the tool loop on, the context of every head's turn is the one the request budget gave
// before the turn had a form (deriveRequestContext over requestBudgetFor, which this change
// leaves untouched): field by field, on both links, with and without an owner's prompt
// budget. That holds for the two wires that are flat with the loop on too (custom, a compat
// upstream, both on the 85,000 B floor): sized from the flat prompt budget, every field
// comes out as the floor gave it. The one exception is deliberate: after the routed lift a
// compat upstream would get body-sized marks for its one string, and stays on the floor
// branch instead.
static void test_a_loop_on_turn_is_the_request_budget_unchanged() {
  size_t flatOn = 0;
  for (const BudgetLink link : {BudgetLink::Device, BudgetLink::Server})
    for (const bool lift : {false, true})
      for (const Head& h : headsOfEveryForm()) {
        const RequestBudget a = requestBudgetFor(h.host, h.model, h.routed, link, lift);
        const RequestBudget t = turnBudgetFor(h.host, h.model, true, link, lift);
        const std::string what = nameOf(h) + (lift ? " (lifted)" : "");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.wireBodyBytes, t.wireBodyBytes, what.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.targetTokens, t.targetTokens, what.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.decodeTokPerS, t.decodeTokPerS, what.c_str());
        TEST_ASSERT_EQUAL_MESSAGE((int)a.bound, (int)t.bound, what.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(a.routed, t.routed, what.c_str());
        flatOn += t.flatTurn;
        // A flat turn (custom, a compat upstream) is sized from its one string: the floor branch.
        // Lane RB-BG: the request budget alone names no form, so at the floor it reads as a split
        // prompt and gets the measured floor marks; the turn never does (test_a_flat_turn_*).
        if (t.flatTurn) {
          TEST_ASSERT_TRUE_MESSAGE(deriveRequestContext(t, {}).floorBranch, what.c_str());
          continue;
        }
        for (const int prompt : {0, 20000}) {
          BudgetOverrides ov;
          ov.maxContextBytes = prompt;
          assertSameContext(deriveRequestContext(t, ov), deriveRequestContext(a, ov), what);
        }
        // Anthropic and OpenAI: the loop setting changes nothing (a single shot is a split prompt).
        const std::string wire = wireOf(h).host;
        if (wire == "anthropic" || wire == "openai")
          assertSameContext(deriveRequestContext(turnBudgetFor(h.host, h.model, false, link, lift), {}),
                            deriveRequestContext(a, {}), what + " loop off");
      }
  TEST_ASSERT_EQUAL(4 * 3, (int)flatOn);   // the two custom heads and the compat upstream, each link and lift
}

// The figures of the three heads that regressed (v5.1.1's window and trigger again), and the
// body budget their wires keep.
static void test_the_loop_off_figures_for_the_main_heads() {
  struct Row { const char* host; const char* model; uint32_t body; size_t histMax; size_t onMax; };
  for (const Row& r : {Row{"mistral", "mistral-large-4-0", 360000, 5242, 75000},
                       Row{"mistral", "mistral-medium-latest", 151000, 5242, 39002},
                       Row{"zai", "glm-5.3", 188000, 4096, 70528}}) {
    const RequestBudget rb = turnBudgetFor(r.host, r.model, false);
    const ContextBudget off = deriveRequestContext(rb, {});
    TEST_ASSERT_TRUE_MESSAGE(rb.flatTurn, r.model);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.body, rb.wireBodyBytes, r.model);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(55904, rb.contextBytes(), r.model);
    TEST_ASSERT_TRUE_MESSAGE(off.floorBranch, r.model);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.histMax, off.historyMaxBytes, r.model);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.histMax / 2, off.historyKeepBytes, r.model);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(48 * 1024, off.foldTriggerBytes, r.model);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4096, off.chatSummaryBytes, r.model);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(6144, off.historyMessageCap, r.model);
    TEST_ASSERT_EQUAL_INT_MESSAGE(32768, off.systemPromptBytes, r.model);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(8192, off.perTurnFloorBytes, r.model);   // recall always has room (lane H)
    const ContextBudget on = headContextBudget(r.host, r.model, true, {});
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.onMax, on.historyMaxBytes, r.model);
    TEST_ASSERT_FALSE_MESSAGE(on.floorBranch, r.model);
    assertSameContext(headContextBudget(r.host, r.model, false, {}), off, r.model);
  }
  // The routed Mistral and Z.ai heads follow the same rule (at the floor in v5.1.2 either way).
  TEST_ASSERT_TRUE(turnBudgetFor("cumulo", "mistral/mistral-large-4-0", false).flatTurn);
  TEST_ASSERT_TRUE(turnBudgetFor("cumulo", "zai/glm-5.3", false).flatTurn);
  TEST_ASSERT_FALSE(turnBudgetFor("cumulo", "mistral/mistral-large-4-0", true).flatTurn);
  TEST_ASSERT_FALSE(turnBudgetFor("cumulo", "anthropic/claude-sonnet-5-5", false).flatTurn);
  // After the routed lift a loop-off routed Large 4 would have the room on its body, not in its string.
  const RequestBudget lifted = turnBudgetFor("cumulo", "mistral/mistral-large-4-0", false, BudgetLink::Device, true);
  TEST_ASSERT_TRUE(lifted.wireBodyBytes > kLegacyWireBodyBytes && deriveRequestContext(lifted, {}).floorBranch);
}


// ---- lane RB-BG: the measured floor, the profiles, no head under v5.1.2 ----------------------

static JsonDocument g_v512;
static std::map<std::string, JsonObjectConst> g_v512Rows;
static const std::map<std::string, JsonObjectConst>& v512Rows() {
  if (g_v512Rows.empty()) {
    TEST_ASSERT_FALSE(deserializeJson(g_v512, recorded::readFixture("test/support/fixtures/budgets_v5_1_2.json")));
    for (JsonObjectConst r : g_v512["models"].as<JsonArrayConst>())
      g_v512Rows[std::string(r["link"] | "") + ":" + (r["provider"] | "") + "/" + (r["id"] | "")] = r;
  }
  return g_v512Rows;
}

// The history marks a profile may lower on a direct head (it never raises one).
static bool profileMark(const char* field) {
  for (const char* f : {"historyMaxBytes", "historySoftBytes", "historyKeepBytes", "foldStepBytes", "historyRows",
                        "foldTriggerBytes", "foldSliceBytes"})
    if (std::string(f) == field) return true;
  return false;
}

// No head goes below its v5.1.2 budget (the orchestrator's standing ruling): field by field
// against budgets_v5_1_2.json (every recorded model, the routed and custom heads, the server
// link's main heads), under every profile. The body budget is unchanged; a history mark may
// sit under v5.1.2's only on a direct head the profile caps, and then it is that profile's.
static void test_no_head_below_its_v5_1_2_budget() {
  const auto& rows = v512Rows();
  TEST_ASSERT_TRUE(rows.size() > 200);
  for (const HistoryProfile p : {HistoryProfile::Tight, HistoryProfile::Lean, HistoryProfile::Standard,
                                 HistoryProfile::Wide}) {
    installHistoryProfile(p);
    for (const auto& kv : rows) {
      JsonObjectConst r = kv.second;
      const BudgetLink link = std::string(r["link"] | "") == "server" ? BudgetLink::Server : BudgetLink::Device;
      const std::string host = r["provider"] | "", model = r["id"] | "";
      const RequestBudget rb = requestBudgetFor(host, model, host == "cumulo", link);
      const ContextBudget c = deriveRequestContext(rb, {});
      const std::string what = kv.first + " " + historyProfileSlug(p);
      // The body: v5.1.2's while the routed lift is off, at least that once it is on.
      if (routedBudgetLift()) TEST_ASSERT_TRUE_MESSAGE(rb.wireBodyBytes >= (r["wireBodyBytes"] | 0u), what.c_str());
      else TEST_ASSERT_EQUAL_UINT32_MESSAGE(r["wireBodyBytes"] | 0u, rb.wireBodyBytes, what.c_str());
      const char* const kFields[] = {"systemPromptBytes", "toolResultBytes", "recallBytes", "replayBytes",
                                     "chatSummaryBytes", "historyMessageCap", "historyMaxBytes", "historySoftBytes",
                                     "historyKeepBytes", "foldSliceBytes", "historyRows", "foldTriggerBytes"};
      const size_t have[] = {(size_t)c.systemPromptBytes, c.toolResultBytes, c.recallBytes, c.replayBytes,
                             c.chatSummaryBytes, c.historyMessageCap, c.historyMaxBytes, c.historySoftBytes,
                             c.historyKeepBytes, c.foldSliceBytes, c.historyRows, foldTriggerBytes(c, kCompactAtKbDefault)};
      const HistoryProfileMarks m = historyProfileMarks(p);
      const size_t due = std::max<size_t>(m.high, kFoldTriggerMinBytes);
      const bool capped = !c.floorBranch && !c.floorMeasured && c.historySoftBytes == due;
      for (size_t i = 0; i < sizeof(have) / sizeof(have[0]); i++) {
        const size_t was = (size_t)(r[kFields[i]] | 0u);
        if (capped && profileMark(kFields[i])) continue;
        TEST_ASSERT_TRUE_MESSAGE(have[i] >= was, (what + " " + kFields[i] + " " + std::to_string(have[i]) + " < " +
                                                  std::to_string(was)).c_str());
      }
      if (capped) {   // the profile's own marks, and never under v5.1.1's
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(due, c.foldTriggerBytes, what.c_str());
        TEST_ASSERT_TRUE_MESSAGE(c.historyKeepBytes >= m.low || c.historyKeepBytes >= c.historySoftBytes -
                                     foldDigestCap(c.chatSummaryBytes), what.c_str());
        TEST_ASSERT_TRUE_MESSAGE(c.historyKeepBytes <= (size_t)(r["historyKeepBytes"] | 0u), what.c_str());
      }
    }
  }
  installHistoryProfile(kDefaultHistoryProfile);
}

// The class over every head and form: the history budget is positive or the floor branch is
// taken; a measured floor head's request fits its cap with the measured overhead; the fold
// trigger never drops under kFoldTriggerMinBytes; the marks are ordered; no profile moves a
// floor head; a profile never raises a mark over what the head's body gives.
static void test_every_head_has_a_history_that_fits_under_every_profile() {
  size_t measured = 0, capped = 0, cases = 0;
  for (const BudgetLink link : {BudgetLink::Device, BudgetLink::Server})
    for (const bool lift : {false, true})
      for (const bool loopOn : {false, true})
        for (const Head& h : headsOfEveryForm()) {
          const RequestBudget rb = turnBudgetFor(h.host, h.model, loopOn, link, lift);
          BudgetOverrides none;
          none.noProfile = true;
          const ContextBudget own = deriveRequestContext(rb, none);
          for (const HistoryProfile p : {HistoryProfile::Tight, HistoryProfile::Lean, HistoryProfile::Standard,
                                         HistoryProfile::Wide}) {
            installHistoryProfile(p);
            const ContextBudget c = deriveRequestContext(rb, {});
            const std::string what = nameOf(h) + (loopOn ? " on " : " off ") + historyProfileSlug(p);
            cases++;
            TEST_ASSERT_TRUE_MESSAGE(c.historyMaxBytes > 0, what.c_str());
            TEST_ASSERT_TRUE_MESSAGE(c.floorBranch || c.historyMaxBytes > deriveBudget(rb.v511CtxTokens, {}).historyMaxBytes,
                                     what.c_str());
            TEST_ASSERT_FALSE_MESSAGE(c.floorBranch && c.floorMeasured, what.c_str());
            TEST_ASSERT_TRUE_MESSAGE(foldTriggerBytes(c, kCompactAtKbDefault) >= kFoldTriggerMinBytes, what.c_str());
            TEST_ASSERT_TRUE_MESSAGE(c.historyKeepBytes <= c.historySoftBytes && c.historySoftBytes <= c.historyMaxBytes,
                                     what.c_str());
            TEST_ASSERT_TRUE_MESSAGE(c.historyMaxBytes <= own.historyMaxBytes && c.historySoftBytes <= own.historySoftBytes &&
                                         c.historyKeepBytes <= own.historyKeepBytes,
                                     what.c_str());
            TEST_ASSERT_TRUE_MESSAGE(c.floorBranch || c.foldStepBytes <= foldDigestCap(c.chatSummaryBytes),
                                     (what + " step " + std::to_string(c.foldStepBytes) + " soft " +
                                      std::to_string(c.historySoftBytes) + " keep " + std::to_string(c.historyKeepBytes) +
                                      " floor " + std::to_string(c.floorBranch) + std::to_string(c.floorMeasured))
                                         .c_str());
            if (c.floorBranch || c.floorMeasured) {   // a floor head: no profile moves it
              TEST_ASSERT_EQUAL_UINT32_MESSAGE(own.historyMaxBytes, c.historyMaxBytes, what.c_str());
              TEST_ASSERT_EQUAL_UINT32_MESSAGE(own.historyKeepBytes, c.historyKeepBytes, what.c_str());
            }
            if (c.floorMeasured) {
              measured++;
              TEST_ASSERT_FALSE_MESSAGE(rb.flatTurn || rb.flat || !rb.measuredWire, what.c_str());
              TEST_ASSERT_TRUE_MESSAGE(rb.wireBodyBytes <= kLegacyWireBodyBytes, what.c_str());
              TEST_ASSERT_TRUE_MESSAGE(floorNeed(c, rb) <= rb.wireBodyBytes, what.c_str());
              TEST_ASSERT_EQUAL_UINT32_MESSAGE(48 * 1024, foldTriggerBytes(c, kCompactAtKbDefault), what.c_str());
            }
            if (!c.floorBranch && !c.floorMeasured && c.historySoftBytes < own.historySoftBytes) {
              capped++;
              TEST_ASSERT_EQUAL_UINT32_MESSAGE(std::max<size_t>(historyProfileMarks(p).high, kFoldTriggerMinBytes),
                                               c.historySoftBytes, what.c_str());
              TEST_ASSERT_EQUAL_UINT32_MESSAGE(c.historySoftBytes, foldTriggerBytes(c, kCompactAtKbDefault), what.c_str());
            }
          }
        }
  installHistoryProfile(kDefaultHistoryProfile);
  TEST_ASSERT_TRUE(cases > 6000);
  TEST_ASSERT_TRUE(measured > 0);   // the routed Anthropic head at least
  TEST_ASSERT_TRUE(capped > 0);
}

// The plan's sizes: direct heads default to standard (60,000 / 30,000 B), each profile's
// marks, and the order tight < lean < standard < wide on every field of a large head.
static void test_the_profile_figures() {
  TEST_ASSERT_TRUE(installedHistoryProfile() == HistoryProfile::Standard);
  const ContextBudget son = headContextBudget("anthropic", "claude-sonnet-5-5", true, {});
  TEST_ASSERT_EQUAL_UINT32(60000, son.historySoftBytes);
  TEST_ASSERT_EQUAL_UINT32(30000, son.historyKeepBytes);
  TEST_ASSERT_EQUAL_UINT32(75000, son.historyMaxBytes);
  TEST_ASSERT_EQUAL_UINT32(60000, foldTriggerBytes(son, kCompactAtKbDefault));
  const struct { HistoryProfile p; uint32_t h, l; } kWant[] = {{HistoryProfile::Tight, 24000, 12000},
                                                               {HistoryProfile::Lean, 40000, 20000},
                                                               {HistoryProfile::Standard, 60000, 30000},
                                                               {HistoryProfile::Wide, 100000, 50000}};
  size_t prevSoft = 0, prevKeep = 0, prevMax = 0;
  for (const auto& w : kWant) {
    TEST_ASSERT_EQUAL_UINT32(w.h, historyProfileMarks(w.p).high);
    TEST_ASSERT_EQUAL_UINT32(w.l, historyProfileMarks(w.p).low);
    HistoryProfile back;
    TEST_ASSERT_TRUE(parseHistoryProfile(historyProfileSlug(w.p), back) && back == w.p);
    installHistoryProfile(w.p);
    const ContextBudget c = headContextBudget("openai", "gpt-5.6", true, {});
    TEST_ASSERT_EQUAL_UINT32(std::max<uint32_t>(w.h, kFoldTriggerMinBytes), c.historySoftBytes);   // tight: 24 KB
    TEST_ASSERT_EQUAL_UINT32(w.l, c.historyKeepBytes);
    TEST_ASSERT_TRUE(c.historySoftBytes > prevSoft && c.historyKeepBytes > prevKeep && c.historyMaxBytes > prevMax);
    prevSoft = c.historySoftBytes;
    prevKeep = c.historyKeepBytes;
    prevMax = c.historyMaxBytes;
  }
  installHistoryProfile(kDefaultHistoryProfile);
  HistoryProfile x;
  TEST_ASSERT_FALSE(parseHistoryProfile("huge", x));
  TEST_ASSERT_TRUE(parseHistoryProfile("LEAN", x) && x == HistoryProfile::Lean);
}

// Folds a day at the floor for `days` of `turns` exchanges of `ex` B: the fold driver's real rule
// (due when a shift hid rows, the window past `high`, or the bytes after thru at `trigger`; it
// takes everything but the kept tail, at most `cycle`, whole exchanges, 12 paid folds a day).
struct FloorDays {
  std::vector<int> folds;
  std::vector<size_t> after;   // window bytes after thru at each day's end
  size_t minCarried = SIZE_MAX;   // the least the request carried after the first fold
};
static FloorDays foldDays(size_t high, size_t keep, size_t trigger, size_t cycle, const std::vector<std::pair<int, size_t>>& days) {
  FloorDays out;
  size_t after = 0;
  bool folded = false;
  for (const auto& d : days) {
    int folds = 0;
    for (int t = 0; t < d.first; t++) {
      after += d.second;
      if (folded) out.minCarried = std::min(out.minCarried, std::min(after, high));
      if (!(after > high || after >= trigger) || folds >= 12) continue;
      const size_t take = std::min(cycle, after > keep ? after - keep : 0) / d.second * d.second;
      if (!take) continue;
      after -= take;
      folds++;
      folded = true;
    }
    out.folds.push_back(folds);
    out.after.push_back(after);
  }
  return out;
}

// A simulated fortnight at the measured floor. On the owner's pace (about 10 exchanges of 600 B
// a day) it folds at most four times a day and on average at most once a day more than v5.1.2's
// floor window did on the same head (each fold is small: about a quarter of the room),
// and the request never falls back under 3/4 of the measured room once the first fold landed
// (v5.1.2: half of an 8 KB window). On a day ten times heavier than the owner's busiest the
// daily cap holds and the next two ordinary days work the backlog off.
static void test_a_fortnight_at_the_measured_floor() {
  setRoutedBudgetLiftForTest(0);   // the floor, whatever kRoutedBudgetLift says
  size_t heads = 0;
  for (const Head& h : kRouted) {
    const RequestBudget rb = turnBudgetFor(h.host, h.model, true);
    const ContextBudget c = deriveRequestContext(rb, {});
    if (!c.floorMeasured) continue;
    heads++;
    const ContextBudget old = deriveBudget(rb.v511CtxTokens, {});
    const size_t cycle = foldCycleBytes(c), trigger = foldTriggerBytes(c, kCompactAtKbDefault);
    const std::vector<std::pair<int, size_t>> owner(14, {10, 600});
    const FloorDays now = foldDays(c.historyMaxBytes, c.historyKeepBytes, trigger, cycle, owner);
    const FloorDays was = foldDays(old.recentConvBytes, old.recentConvBytes / 2, 48 * 1024, foldCycleBytes(old), owner);
    int nowTotal = 0, wasTotal = 0;
    for (size_t d = 0; d < owner.size(); d++) {
      nowTotal += now.folds[d];
      wasTotal += was.folds[d];
      TEST_ASSERT_TRUE_MESSAGE(now.folds[d] <= 4, (nameOf(h) + " day " + std::to_string(d)).c_str());
    }
    TEST_ASSERT_TRUE_MESSAGE(nowTotal <= wasTotal + (int)owner.size(), nameOf(h).c_str());   // one more a day at most
    TEST_ASSERT_TRUE_MESSAGE(now.minCarried + 600 >= c.historyKeepBytes, nameOf(h).c_str());
    TEST_ASSERT_TRUE_MESSAGE(now.minCarried > was.minCarried, nameOf(h).c_str());
    std::vector<std::pair<int, size_t>> heavy(owner);
    heavy[3] = {160, 3072};
    const FloorDays h3 = foldDays(c.historyMaxBytes, c.historyKeepBytes, trigger, cycle, heavy);
    TEST_ASSERT_TRUE_MESSAGE(h3.folds[3] <= 12, nameOf(h).c_str());
    for (size_t d = 6; d < heavy.size(); d++)
      TEST_ASSERT_TRUE_MESSAGE(h3.after[d] <= c.historyMaxBytes, (nameOf(h) + " backlog day " + std::to_string(d)).c_str());
  }
  TEST_ASSERT_TRUE(heads > 0);
  setRoutedBudgetLiftForTest(-1);
}

// The measured floor's formula on constructed budgets (mutation survivors): each measured wire's
// own overhead and ratio; a body at or under the overhead, a flat turn, a flat wire or an
// unmeasured wire keeps the floor branch; under tight, a larger compactAtKB never folds earlier.
static void test_the_measured_floor_formula_on_its_edges() {
  const RequestBudget base = requestBudgetFor("cumulo", "anthropic/claude-sonnet-5-5", true, BudgetLink::Device, false);
  TEST_ASSERT_EQUAL(1, base.measuredWire);
  const ContextBudget a = deriveRequestContext(base, {});
  const size_t wantA = (85000 - kFloorFixedAnthropicBytes - a.chatSummaryBytes - kFloorMarginBytes - kFloorRoundReserveBytes) *
                       100 / kFloorWirePerWindowAnthropicPct;
  TEST_ASSERT_TRUE(a.floorMeasured);
  TEST_ASSERT_EQUAL_UINT32(wantA, a.historyMaxBytes);
  RequestBudget o = base;   // the same body on the OpenAI wire, with a summary that leaves it room
  o.measuredWire = 2;
  o.v511ReplayBytes = 24576;
  const ContextBudget oc = deriveRequestContext(o, {});
  const size_t wantO = (85000 - kFloorFixedOpenAIBytes - oc.chatSummaryBytes - kFloorMarginBytes - kFloorRoundReserveBytes) *
                       100 / kFloorWirePerWindowOpenAIPct;
  TEST_ASSERT_EQUAL_UINT32(wantO, oc.historyMaxBytes);
  TEST_ASSERT_TRUE(oc.historyMaxBytes < a.historyMaxBytes);
  RequestBudget small = base;   // a body under the overhead: no wrap, the floor branch
  small.wireBodyBytes = 60000;
  TEST_ASSERT_TRUE(deriveRequestContext(small, {}).floorBranch);
  RequestBudget flat = base;
  flat.flatTurn = true;
  TEST_ASSERT_TRUE(deriveRequestContext(flat, {}).floorBranch);
  flat = base;
  flat.flat = true;
  TEST_ASSERT_TRUE(deriveRequestContext(flat, {}).floorBranch);
  RequestBudget un = base;
  un.measuredWire = 0;
  TEST_ASSERT_TRUE(deriveRequestContext(un, {}).floorBranch);
  installHistoryProfile(HistoryProfile::Tight);
  const ContextBudget t = headContextBudget("anthropic", "claude-sonnet-5-5", true, {});
  for (const uint32_t kb : {48u, 64u, 100u, 200u})
    TEST_ASSERT_TRUE_MESSAGE(foldTriggerBytes(t, kb) >= foldTriggerBytes(t, kCompactAtKbDefault), std::to_string(kb).c_str());
  installHistoryProfile(kDefaultHistoryProfile);
}

// ---- the generated table (table_doc.h, included above) ---------------------------------------

static void test_the_generated_table_is_current() {
  const std::string want = budgetTableDoc();
  const char* path = "docs/reference/request-budgets.md";
  const char* e = std::getenv("GOLDEN_UPDATE");
  if (e && std::string(e) == "1") {
    std::ofstream(path) << want;
    TEST_IGNORE_MESSAGE("request-budgets.md written: read the diff, then commit it");
  }
  std::ifstream f(path);
  if (!f.good()) TEST_FAIL_MESSAGE("missing docs/reference/request-budgets.md - run with GOLDEN_UPDATE=1");
  const std::string have((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  TEST_ASSERT_TRUE_MESSAGE(have == want, "request-budgets.md drifted from the code - re-bless with GOLDEN_UPDATE=1");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_head_inside_the_ruling_and_the_window);
  RUN_TEST(test_every_head_above_the_floor_fits_its_worst_round);
  RUN_TEST(test_haiku_inside_its_price_tier);
  RUN_TEST(test_the_plan_figures);
  RUN_TEST(test_monotonic_in_the_window);
  RUN_TEST(test_routed_heads_at_the_floor_until_the_lift);
  RUN_TEST(test_custom_gets_the_floor_on_the_flat_wire);
  RUN_TEST(test_server_link_drops_upload_and_pace);
  RUN_TEST(test_the_memo_only_ever_lowers);
  RUN_TEST(test_no_field_below_v5_1_1_for_every_recorded_model);
  RUN_TEST(test_history_room_or_the_floor_branch_for_every_head);
  RUN_TEST(test_a_week_at_the_floor_stays_under_the_daily_fold_cap);
  RUN_TEST(test_a_fortnight_on_every_head_works_off_its_backlog);
  RUN_TEST(test_the_derived_fields_for_the_main_heads);
  RUN_TEST(test_owner_settings);
  RUN_TEST(test_the_form_of_every_head_turn);
  RUN_TEST(test_the_derived_history_fits_the_form_of_the_turn);
  RUN_TEST(test_a_flat_turn_is_sized_as_v5_1_1_sized_it);
  RUN_TEST(test_a_loop_on_turn_is_the_request_budget_unchanged);
  RUN_TEST(test_the_loop_off_figures_for_the_main_heads);
  RUN_TEST(test_every_id_that_means_large_4_gets_its_row);
  RUN_TEST(test_no_head_below_its_v5_1_2_budget);
  RUN_TEST(test_every_head_has_a_history_that_fits_under_every_profile);
  RUN_TEST(test_the_profile_figures);
  RUN_TEST(test_a_fortnight_at_the_measured_floor);
  RUN_TEST(test_the_measured_floor_formula_on_its_edges);
  RUN_TEST(test_the_generated_table_is_current);
  return UNITY_END();
}
