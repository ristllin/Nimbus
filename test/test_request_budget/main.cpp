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
#include "nimbus/harness/conversation.h"   // historyDayBytes - the day floor of v5.1.1's window
#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/providers.h"   // flatPromptBudget - the wires' flat cap
#include "nimbus/orch/budget.h"
#include "nimbus/orch/model_window.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/wire_limits.h"

using namespace nimbus::orch;
namespace providers = agent::providers;

void setUp() {
  installListedCtxLookup(&recorded::listedLookup);
  clearObservedRatios();
  installBudgetLink(BudgetLink::Device);
}
void tearDown() {
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
  TEST_ASSERT_FALSE(kRoutedBudgetLift);
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
  for (size_t i = 0; i < sizeof(have) / sizeof(have[0]); i++)
    TEST_ASSERT_TRUE_MESSAGE(have[i] >= (size_t)(old[kFields[i]] | 0u), (what + " " + kFields[i]).c_str());
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

// The derived figures for the heads the plan quotes (replay at least the wire's v5.1.1 cap).
static void test_the_derived_fields_for_the_main_heads() {
  const ContextBudget l4 = deriveRequestContext(requestBudgetFor("mistral", "mistral-large-4-0", false), {});
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
  const ContextBudget son = deriveRequestContext(requestBudgetFor("anthropic", "claude-sonnet-5-5", false), {});
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
  const ContextBudget l4 = deriveRequestContext(requestBudgetFor("mistral", "mistral-large-4-0", false), {});
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
  if (!c.floorBranch)
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
        if (t.flatTurn && a.wireBodyBytes > kLegacyWireBodyBytes) {   // the lifted compat upstream
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
  for (const Row& r : {Row{"mistral", "mistral-large-4-0", 360000, 5242, 206080},
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
  RUN_TEST(test_the_derived_fields_for_the_main_heads);
  RUN_TEST(test_owner_settings);
  RUN_TEST(test_the_form_of_every_head_turn);
  RUN_TEST(test_the_derived_history_fits_the_form_of_the_turn);
  RUN_TEST(test_a_flat_turn_is_sized_as_v5_1_1_sized_it);
  RUN_TEST(test_a_loop_on_turn_is_the_request_budget_unchanged);
  RUN_TEST(test_the_loop_off_figures_for_the_main_heads);
  RUN_TEST(test_the_generated_table_is_current);
  return UNITY_END();
}
