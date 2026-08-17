#include <unity.h>

#include "nimbus/orch/usage_ledger.h"

using nimbus::orch::ProviderBudget;
using nimbus::orch::UsageLedger;
using nimbus::orch::usagePeriodKey;

void setUp() {}
void tearDown() {}

// ---- period key (rolling month with a per-provider reset day) ----------------

static void test_period_key_default_reset_day_1() {
  // resetDay 1: the key is just months-since-year-0; each calendar month is a period.
  uint32_t jan = usagePeriodKey(2026, 1, 15, 1);
  uint32_t feb = usagePeriodKey(2026, 2, 1, 1);
  uint32_t feb28 = usagePeriodKey(2026, 2, 28, 1);
  TEST_ASSERT_EQUAL_UINT32(jan + 1, feb);          // consecutive months differ by 1
  TEST_ASSERT_EQUAL_UINT32(feb, feb28);            // same month -> same key
  TEST_ASSERT_NOT_EQUAL(jan, feb);
}

static void test_period_key_custom_reset_day() {
  // resetDay 15: days 1..14 belong to the PREVIOUS window; 15..end to the new one.
  uint32_t before = usagePeriodKey(2026, 3, 14, 15);   // still Feb-15 window
  uint32_t on     = usagePeriodKey(2026, 3, 15, 15);   // rolls to the Mar-15 window
  uint32_t after  = usagePeriodKey(2026, 3, 20, 15);
  TEST_ASSERT_EQUAL_UINT32(on, after);
  TEST_ASSERT_EQUAL_UINT32(before + 1, on);
  // The day-14 window matches late February in the same reset scheme.
  TEST_ASSERT_EQUAL_UINT32(usagePeriodKey(2026, 2, 20, 15), before);
}

static void test_period_key_clamps_reset_day() {
  // resetDay 0 -> 1, resetDay 40 -> 28; both stay monotonic + stable.
  TEST_ASSERT_EQUAL_UINT32(usagePeriodKey(2026, 5, 10, 1), usagePeriodKey(2026, 5, 10, 0));
  uint32_t k = usagePeriodKey(2026, 5, 28, 40);
  TEST_ASSERT_EQUAL_UINT32(k, usagePeriodKey(2026, 5, 28, 28));
}

// ---- accumulation + monthly roll --------------------------------------------

static void test_record_and_roll() {
  UsageLedger L;
  L.recordTokens("openai", 100, 24300);
  L.recordTokens("openai", 250, 24300);
  const ProviderBudget* e = L.find("openai");
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_UINT64(350, e->tokens);
  TEST_ASSERT_EQUAL_UINT32(24300, e->periodKey);

  // A new period zeroes the counters, keeps the entry.
  L.recordTokens("openai", 40, 24301);
  e = L.find("openai");
  TEST_ASSERT_EQUAL_UINT64(40, e->tokens);
  TEST_ASSERT_EQUAL_UINT32(24301, e->periodKey);
}

static void test_calls_separate_from_tokens() {
  UsageLedger L;
  L.recordCall("tavily", 24300);
  L.recordCall("tavily", 24300);
  L.recordCall("tavily", 24300);
  const ProviderBudget* e = L.find("tavily");
  TEST_ASSERT_EQUAL_UINT32(3, e->calls);
  TEST_ASSERT_EQUAL_UINT64(0, e->tokens);
}

static void test_limits_persist_across_roll() {
  UsageLedger L;
  L.setLimits("openai", /*tokenLimit=*/1000, /*callLimit=*/0, /*resetDay=*/15);
  L.recordTokens("openai", 900, 24300);
  TEST_ASSERT_FALSE(L.overTokenBudget("openai"));   // 900 < 1000
  L.recordTokens("openai", 100, 24300);
  TEST_ASSERT_TRUE(L.overTokenBudget("openai"));    // 1000 >= 1000
  TEST_ASSERT_TRUE(L.overBudget("openai"));

  // Roll to a new month: counters reset, the limit + resetDay survive.
  L.recordTokens("openai", 10, 24301);
  const ProviderBudget* e = L.find("openai");
  TEST_ASSERT_EQUAL_UINT64(1000, e->tokenLimit);
  TEST_ASSERT_EQUAL_UINT8(15, e->resetDay);
  TEST_ASSERT_FALSE(L.overTokenBudget("openai"));   // fresh window -> under budget
}

static void test_call_budget_and_unlimited() {
  UsageLedger L;
  // Unlimited by default (limit 0) -> never over budget however many calls.
  L.recordCall("tavily", 24300);
  L.recordCall("tavily", 24300);
  TEST_ASSERT_FALSE(L.overCallBudget("tavily"));
  // Now cap at 3 calls.
  L.setLimits("tavily", /*tokenLimit=*/0, /*callLimit=*/3, /*resetDay=*/1);
  TEST_ASSERT_FALSE(L.overCallBudget("tavily"));    // 2 < 3
  L.recordCall("tavily", 24300);
  TEST_ASSERT_TRUE(L.overCallBudget("tavily"));     // 3 >= 3
}

static void test_over_budget_false_for_unknown() {
  UsageLedger L;
  TEST_ASSERT_FALSE(L.overBudget("nope"));
  TEST_ASSERT_NULL(L.find("nope"));
}

// ---- persistence round-trip --------------------------------------------------

static void test_serialize_roundtrip() {
  UsageLedger L;
  L.setLimits("openai", 500000, 0, 15);
  L.recordTokens("openai", 13687, 24301);
  L.setLimits("tavily", 0, 1000, 1);
  L.recordCall("tavily", 24301);

  std::string blob = L.serialize();
  TEST_ASSERT_TRUE(blob.size() > 0);

  UsageLedger R;
  R.deserialize(blob);
  const ProviderBudget* o = R.find("openai");
  const ProviderBudget* t = R.find("tavily");
  TEST_ASSERT_NOT_NULL(o);
  TEST_ASSERT_NOT_NULL(t);
  TEST_ASSERT_EQUAL_UINT64(13687, o->tokens);
  TEST_ASSERT_EQUAL_UINT64(500000, o->tokenLimit);
  TEST_ASSERT_EQUAL_UINT8(15, o->resetDay);
  TEST_ASSERT_EQUAL_UINT32(24301, o->periodKey);
  TEST_ASSERT_EQUAL_UINT32(1, t->calls);
  TEST_ASSERT_EQUAL_UINT32(1000, t->callLimit);
}

static void test_deserialize_garbage_is_safe() {
  UsageLedger L;
  L.deserialize("");                         // empty
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)L.entries().size());
  L.deserialize("garbage;;openai|only|two"); // malformed groups skipped, no crash
  TEST_ASSERT_TRUE(L.entries().size() <= 1);
}

// ---- cost extension (2026-07-16): in/out split, all-time totals, rates ---------

static void test_inout_split_and_alltime_totals() {
  UsageLedger L;
  L.recordTokens("anthropic", /*in=*/7000, /*out=*/300, 24300);
  L.recordTokens("anthropic", 1000, 200, 24300);
  const ProviderBudget* e = L.find("anthropic");
  TEST_ASSERT_EQUAL_UINT64(8500, e->tokens);      // budget counter = in+out
  TEST_ASSERT_EQUAL_UINT64(8000, e->tokensIn);
  TEST_ASSERT_EQUAL_UINT64(500,  e->tokensOut);
  TEST_ASSERT_EQUAL_UINT64(8000, e->totalIn);
  TEST_ASSERT_EQUAL_UINT64(500,  e->totalOut);

  // A month roll zeroes the PERIOD split but keeps the all-time totals + rates.
  L.setRates("anthropic", 300, 1500, 0);
  L.recordTokens("anthropic", 100, 50, 24301);
  e = L.find("anthropic");
  TEST_ASSERT_EQUAL_UINT64(100, e->tokensIn);     // fresh period
  TEST_ASSERT_EQUAL_UINT64(50,  e->tokensOut);
  TEST_ASSERT_EQUAL_UINT64(8100, e->totalIn);     // all-time keeps accumulating
  TEST_ASSERT_EQUAL_UINT64(550,  e->totalOut);
  TEST_ASSERT_EQUAL_UINT32(300,  e->centsPerMIn);
  TEST_ASSERT_EQUAL_UINT32(1500, e->centsPerMOut);

  // All-time calls survive the roll too.
  L.recordCall("tavily", 24300);
  L.recordCall("tavily", 24301);
  TEST_ASSERT_EQUAL_UINT64(2, L.find("tavily")->totalCalls);
  TEST_ASSERT_EQUAL_UINT32(1, L.find("tavily")->calls);   // period rolled

  // Round-trip carries every new field.
  UsageLedger R;
  R.deserialize(L.serialize());
  const ProviderBudget* a = R.find("anthropic");
  TEST_ASSERT_EQUAL_UINT64(8100, a->totalIn);
  TEST_ASSERT_EQUAL_UINT64(550,  a->totalOut);
  TEST_ASSERT_EQUAL_UINT32(1500, a->centsPerMOut);
  TEST_ASSERT_EQUAL_UINT64(2, R.find("tavily")->totalCalls);
}

static void test_old_7field_blob_still_parses() {
  // A pre-extension NVS blob (exactly 7 fields) must load with the new fields 0 -
  // no migration, no data loss on upgrade. EXCEPT the period split: legacy `tokens`
  // is real period spend, attributed to the input side so the "This period" tile
  // agrees with the budget bar on the same page (review finding).
  UsageLedger L;
  L.deserialize("openai|24300|5000|0|100000|0|15");
  const ProviderBudget* e = L.find("openai");
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_UINT64(5000, e->tokens);
  TEST_ASSERT_EQUAL_UINT64(100000, e->tokenLimit);
  TEST_ASSERT_EQUAL_UINT8(15, e->resetDay);
  TEST_ASSERT_EQUAL_UINT64(5000, e->tokensIn);    // migrated: period surfaces agree
  TEST_ASSERT_EQUAL_UINT64(0, e->tokensOut);
  TEST_ASSERT_EQUAL_UINT64(0, e->totalIn);
  TEST_ASSERT_EQUAL_UINT32(0, e->centsPerMIn);
}

// Review (2026-07-16, budget-wipe): a record with periodKey 0 (wall clock not yet
// synced after a power-cycle) must NEVER roll a stored real period to 0 - that was
// durably wiping month-to-date spend and reopening a nearly-closed budget gate.
// And the first post-sync roll (0 -> P) must CARRY the pre-sync counters, not zero.
static void test_unsynced_clock_never_wipes_period() {
  UsageLedger L;
  L.setLimits("anthropic", 500000, 0, 1);
  L.recordTokens("anthropic", 480000, 0, 24300);   // month nearly at the cap
  // Reboot, clock unsynced -> a queued turn records with periodKey 0.
  L.recordTokens("anthropic", 1000, 100, 0);
  const ProviderBudget* e = L.find("anthropic");
  TEST_ASSERT_EQUAL_UINT32(24300, e->periodKey);      // stored period KEPT
  TEST_ASSERT_EQUAL_UINT64(481100, e->tokens);        // spend counted, not wiped
  TEST_ASSERT_FALSE(L.overTokenBudget("anthropic"));
  L.recordTokens("anthropic", 20000, 0, 0);           // still unsynced, crosses the cap
  TEST_ASSERT_TRUE(L.overTokenBudget("anthropic"));   // the gate still closes
  // SNTP lands, same month -> same key: counters keep accumulating normally.
  L.recordTokens("anthropic", 10, 0, 24300);
  TEST_ASSERT_EQUAL_UINT64(501110, L.find("anthropic")->tokens);
}

static void test_first_sync_carries_period0_counters() {
  UsageLedger L;
  // Fresh device: everything accumulated under key 0 before the first SNTP sync.
  L.recordTokens("openai", 3000, 500, 0);
  L.recordCall("tavily", 0);
  // First synced record: the period-0 counters CARRY into the real period.
  L.recordTokens("openai", 100, 10, 24300);
  const ProviderBudget* e = L.find("openai");
  TEST_ASSERT_EQUAL_UINT32(24300, e->periodKey);
  TEST_ASSERT_EQUAL_UINT64(3610, e->tokens);          // 3500 carried + 110 new
  TEST_ASSERT_EQUAL_UINT64(3100, e->tokensIn);
  L.recordCall("tavily", 24300);
  TEST_ASSERT_EQUAL_UINT32(2, L.find("tavily")->calls);   // 1 carried + 1 new
}

// ---- UsageHistory (the graphs' daily buckets) -----------------------------------

// ---- attribution tags (2026-07 harness round) --------------------------------

static void test_attribution_tags_accumulate_and_roundtrip() {
  UsageLedger L;
  L.recordTokens("anthropic", 100, 20, 5, "turn");
  L.recordTokens("anthropic", 50, 10, 5, "turn");          // merges into one row
  L.recordTokens("anthropic", 30, 5, 5, "loop:L3");
  L.recordTokens("openai", 200, 40, 5, "spawn:openai");
  L.recordTokens("openai", 9, 1, 5, "");                   // untagged: budget only

  // Budget math untouched: the provider counters carry ALL spend, tagged or not.
  TEST_ASSERT_EQUAL(215, (int)L.find("anthropic")->tokens);
  TEST_ASSERT_EQUAL(250, (int)L.find("openai")->tokens);

  TEST_ASSERT_EQUAL(3, (int)L.tagEntries().size());        // no row for ""
  TEST_ASSERT_EQUAL_STRING("turn", L.tagEntries()[0].tag.c_str());
  TEST_ASSERT_EQUAL(150, (int)L.tagEntries()[0].tokIn);
  TEST_ASSERT_EQUAL(30, (int)L.tagEntries()[0].tokOut);
  TEST_ASSERT_EQUAL_STRING("loop:L3", L.tagEntries()[1].tag.c_str());
  TEST_ASSERT_EQUAL_STRING("spawn:openai", L.tagEntries()[2].tag.c_str());

  // Round-trip through the NVS blob.
  UsageLedger M;
  M.deserialize(L.serialize());
  TEST_ASSERT_EQUAL(3, (int)M.tagEntries().size());
  TEST_ASSERT_EQUAL_STRING("anthropic", M.tagEntries()[0].prov.c_str());
  TEST_ASSERT_EQUAL(150, (int)M.tagEntries()[0].tokIn);
  TEST_ASSERT_EQUAL_STRING("spawn:openai", M.tagEntries()[2].tag.c_str());
  TEST_ASSERT_EQUAL(200, (int)M.tagEntries()[2].tokIn);
  TEST_ASSERT_EQUAL(215, (int)M.find("anthropic")->tokens);   // entries intact
}

static void test_attribution_blob_is_downgrade_safe() {
  // A '#' tag group must be SKIPPED by the entry parser (its 7-field minimum),
  // never turned into a bogus provider - this is what makes the extended blob
  // safe to read on older firmware.
  UsageLedger L;
  L.deserialize("anthropic|5|120|0|0|0|1;#anthropic|turn|100|20");
  TEST_ASSERT_EQUAL(1, (int)L.entries().size());
  TEST_ASSERT_EQUAL_STRING("anthropic", L.entries()[0].name.c_str());
  TEST_ASSERT_EQUAL(1, (int)L.tagEntries().size());   // the NEW parser reads it
  // Malformed tag groups are skipped safely.
  UsageLedger M;
  M.deserialize("#justonefield;#a|b;anthropic|5|120|0|0|0|1");
  TEST_ASSERT_EQUAL(0, (int)M.tagEntries().size());
  TEST_ASSERT_EQUAL(1, (int)M.entries().size());
}

static void test_history_record_merge_and_roundtrip() {
  nimbus::orch::UsageHistory H;
  H.record("anthropic", 20650, 7000, 300, 0);
  H.record("anthropic", 20650, 1000, 100, 0);   // same day+prov -> merged
  H.record("openai",    20650, 500,  50,  0);   // same day, other provider
  H.record("tavily",    20651, 0,    0,   3);   // next day, calls
  TEST_ASSERT_EQUAL(3, (int)H.entries().size());
  TEST_ASSERT_EQUAL_UINT64(8000, H.entries()[0].tokIn);
  TEST_ASSERT_EQUAL_UINT64(400,  H.entries()[0].tokOut);
  TEST_ASSERT_EQUAL_UINT32(3,    H.entries()[2].calls);

  nimbus::orch::UsageHistory R;
  R.deserialize(H.serialize());
  TEST_ASSERT_EQUAL(3, (int)R.entries().size());
  TEST_ASSERT_EQUAL_UINT64(8000, R.entries()[0].tokIn);
  TEST_ASSERT_EQUAL_STRING("tavily", R.entries()[2].prov.c_str());

  // dayKey 0 (no sane clock) is refused - an undated bucket would corrupt graphs.
  H.record("openai", 0, 100, 100, 0);
  TEST_ASSERT_EQUAL(3, (int)H.entries().size());
}

static void test_history_prune_and_cap() {
  nimbus::orch::UsageHistory H;
  H.record("openai", 20500, 10, 1, 0);   // ancient
  H.record("openai", 20649, 10, 1, 0);   // yesterday-ish
  H.record("openai", 20650, 10, 1, 0);   // today
  int removed = H.prune(/*today=*/20650, /*keepDays=*/60);
  TEST_ASSERT_EQUAL(1, removed);         // only the ancient bucket dropped
  TEST_ASSERT_EQUAL(2, (int)H.entries().size());

  // The entry cap evicts the OLDEST bucket, never the newest.
  nimbus::orch::UsageHistory C;
  for (uint32_t d = 0; d < nimbus::orch::UsageHistory::kMaxEntries; ++d)
    C.record("p", 10000 + d, 1, 0, 0);
  TEST_ASSERT_EQUAL((int)nimbus::orch::UsageHistory::kMaxEntries, (int)C.entries().size());
  C.record("p", 99999, 1, 0, 0);
  TEST_ASSERT_EQUAL((int)nimbus::orch::UsageHistory::kMaxEntries, (int)C.entries().size());
  bool haveNewest = false, haveOldest = false;
  for (const auto& e : C.entries()) {
    if (e.dayKey == 99999) haveNewest = true;
    if (e.dayKey == 10000) haveOldest = true;
  }
  TEST_ASSERT_TRUE(haveNewest);
  TEST_ASSERT_FALSE(haveOldest);
}


// ---- W16: $ ceiling (centsLimit) --------------------------------------------

// The $ cap gates via the owner rates; with NO rates it must gate NOTHING (the
// estimate is structurally $0 - "cannot tell" must not refuse a provider).
static void test_cost_budget_gates_only_with_rates() {
  UsageLedger L;
  L.setLimits("openai", 0, 0, 1, /*centsLimit=*/500);        // $5, no token cap
  L.recordTokens("openai", 3000000, 1000000, 1);              // 3M in / 1M out
  TEST_ASSERT_FALSE_MESSAGE(L.overBudget("openai"), "no rates -> must not gate");
  L.setRates("openai", 250, 1000, 0);                         // $2.50/M in, $10/M out
  // est = 3M*250/1M + 1M*1000/1M = 750 + 1000 = 1750 cents >= 500 -> over.
  TEST_ASSERT_EQUAL_UINT64(1750, L.find("openai")->estCents());
  TEST_ASSERT_TRUE_MESSAGE(L.overBudget("openai"), "$ cap with rates must gate");
  // Token/call caps unset - it is the $ ceiling specifically that tripped.
  TEST_ASSERT_FALSE(L.overTokenBudget("openai"));
  TEST_ASSERT_TRUE(L.overCostBudget("openai"));
}

static void test_cost_budget_under_the_cap_allows() {
  UsageLedger L;
  L.setLimits("anthropic", 0, 0, 1, 10000);                   // $100
  L.setRates("anthropic", 300, 1500, 0);
  L.recordTokens("anthropic", 1000000, 200000, 1);            // $3 + $3 = $6
  TEST_ASSERT_EQUAL_UINT64(600, L.find("anthropic")->estCents());
  TEST_ASSERT_FALSE(L.overBudget("anthropic"));
}

// centsLimit must survive serialize -> deserialize AND a period roll (limits
// persist, counters reset - same contract as the token limit).
static void test_cents_limit_roundtrip_and_roll() {
  UsageLedger L;
  L.setLimits("mistral", 0, 0, 5, 2500);
  L.setRates("mistral", 100, 300, 0);
  L.recordTokens("mistral", 500000, 100000, 10);
  UsageLedger M;
  M.deserialize(L.serialize());
  TEST_ASSERT_EQUAL_UINT64(2500, M.find("mistral")->centsLimit);
  TEST_ASSERT_EQUAL_UINT64(L.find("mistral")->estCents(), M.find("mistral")->estCents());
  M.recordTokens("mistral", 1, 1, 11);                        // new period -> counters roll
  TEST_ASSERT_EQUAL_UINT64(2500, M.find("mistral")->centsLimit);   // ...limit stays
  TEST_ASSERT_TRUE(M.find("mistral")->estCents() < 10);            // spend reset
}

// A pre-W16 15-field blob (no centsLimit) parses with centsLimit=0 - no NVS
// migration, and an OLD firmware ignores the appended field (downgrade-safe).
static void test_pre_w16_blob_reads_no_cents_limit() {
  UsageLedger L;
  L.deserialize("openai|500|100|2|0|0|1|60|40|60|40|2|250|1000|0");
  const ProviderBudget* e = L.find("openai");
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL_UINT64(0, e->centsLimit);
  TEST_ASSERT_FALSE(L.overCostBudget("openai"));
}


// ⚠ prism (the caching under-meter): Anthropic EXCLUDES cache reads/writes from
// input_tokens but bills them (0.1x / 1.25x input rate). Without cache counters
// the $ gate trips LATE relative to the invoice on every cached turn.
static void test_est_cents_meters_cache_traffic() {
  UsageLedger L;
  L.setRates("anthropic", 300, 1500, 0);        // $3/M in
  L.recordTokens("anthropic", 100000, 0, /*cacheRead=*/2000000, /*cacheWrite=*/400000,
                 1, "turn");
  // in: 0.1M*300/1M=30c; reads: 2M*300*0.1/1M=60c; writes: 0.4M*300*1.25/1M=150c
  TEST_ASSERT_EQUAL_UINT64(30 + 60 + 150, L.find("anthropic")->estCents());
  // Cache counters roll with the period like every other period counter.
  L.recordTokens("anthropic", 1, 0, 0, 0, 2, "turn");
  TEST_ASSERT_EQUAL_UINT64(0, L.find("anthropic")->cacheRead);
}

// ⚠ prism (the forever-gated wedge): the gate runs BEFORE the call, the roll
// happens ON a record, and a gated provider never records - so month M's
// over-budget used to gate month M+1 forever. The dated overBudget reads a
// stale periodKey as a FRESH period.
static void test_dated_gate_ungates_a_new_period() {
  UsageLedger L;
  L.setLimits("openai", 100, 0, 1, 0);
  L.recordTokens("openai", 200, 0, usagePeriodKey(2026, 8, 15, 1));  // over in Aug
  TEST_ASSERT_TRUE(L.overBudget("openai"));                          // legacy strict form
  TEST_ASSERT_TRUE(L.overBudget("openai", 2026, 8, 20));             // still August: gated
  TEST_ASSERT_FALSE_MESSAGE(L.overBudget("openai", 2026, 9, 2),
      "a NEW period must read under-budget even though nothing recorded yet");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_period_key_default_reset_day_1);
  RUN_TEST(test_period_key_custom_reset_day);
  RUN_TEST(test_period_key_clamps_reset_day);
  RUN_TEST(test_record_and_roll);
  RUN_TEST(test_calls_separate_from_tokens);
  RUN_TEST(test_limits_persist_across_roll);
  RUN_TEST(test_call_budget_and_unlimited);
  RUN_TEST(test_over_budget_false_for_unknown);
  RUN_TEST(test_serialize_roundtrip);
  RUN_TEST(test_deserialize_garbage_is_safe);
  RUN_TEST(test_inout_split_and_alltime_totals);
  RUN_TEST(test_old_7field_blob_still_parses);
  RUN_TEST(test_unsynced_clock_never_wipes_period);
  RUN_TEST(test_first_sync_carries_period0_counters);
  RUN_TEST(test_attribution_tags_accumulate_and_roundtrip);
  RUN_TEST(test_attribution_blob_is_downgrade_safe);
  RUN_TEST(test_history_record_merge_and_roundtrip);
  RUN_TEST(test_history_prune_and_cap);
  RUN_TEST(test_cost_budget_gates_only_with_rates);
  RUN_TEST(test_cost_budget_under_the_cap_allows);
  RUN_TEST(test_cents_limit_roundtrip_and_roll);
  RUN_TEST(test_pre_w16_blob_reads_no_cents_limit);
  RUN_TEST(test_est_cents_meters_cache_traffic);
  RUN_TEST(test_dated_gate_ungates_a_new_period);
  return UNITY_END();
}
