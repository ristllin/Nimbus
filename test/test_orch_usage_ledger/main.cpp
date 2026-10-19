#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/turn_spend.h"
#include "nimbus/orch/usage_ledger.h"
#include "nimbus/orch/usage_models_json.h"

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

// The $ cap gates on the priced estimate. D1 (2026-10): with no OWNER rates the
// built-in estimates still price the spend, so the cap arms (fail-closed money);
// only a provider nothing prices at all ("cannot tell") gates NOTHING.
static void test_cost_budget_gates_only_with_rates() {
  UsageLedger L;
  L.setLimits("openai", 0, 0, 1, /*centsLimit=*/500);        // $5, no token cap
  L.recordTokens("openai", 3000000, 1000000, 1);              // 3M in / 1M out
  // Built-in openai default ~$2.50/M in, ~$10/M out: 750 + 1000 = 1750 cents.
  TEST_ASSERT_EQUAL_UINT64(1750, L.estCents("openai"));
  TEST_ASSERT_TRUE_MESSAGE(L.overBudget("openai"), "D1: built-in estimates arm the $ cap");
  UsageLedger C;                                              // custom: no price anywhere
  C.setLimits("custom", 0, 0, 1, 1);
  C.recordTokens("custom", 3000000, 1000000, 1);
  TEST_ASSERT_FALSE_MESSAGE(C.overBudget("custom"), "nothing priced -> must not gate");
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

// ---- per-model prices (2026-10) --------------------------------------------------

using nimbus::orch::EffRate;
using nimbus::orch::ModelLedger;
using nimbus::orch::PriceSummary;
using nimbus::orch::RateSrc;
using nimbus::orch::UsageHistory;
using nimbus::orch::DayUsage;

using nimbus::orch::RowPrice;
using nimbus::orch::TokenDelta;

static TokenDelta td(uint64_t in, uint64_t out = 0, uint64_t cr = 0, uint64_t cw = 0) {
  TokenDelta d;
  d.in = in;
  d.out = out;
  d.cacheR = cr;
  d.cacheW = cw;
  return d;
}

// One turn the way the device records it: provider ledger + the model row.
static void turn(UsageLedger& L, const char* prov, const char* model, const TokenDelta& d,
                 uint32_t period = 24300) {
  L.recordTokens(prov, d, period, "turn", model, 20650);
}
static void turn(UsageLedger& L, const char* prov, const char* model, uint64_t in,
                 uint64_t out, uint32_t period = 24300) {
  turn(L, prov, model, td(in, out), period);
}

// THE OWNER CASE ("different models cost differently for the same provider"): two
// models of one provider, each at its own price, add up - not one provider rate.
static void test_owner_case_two_models_one_provider_sum_correctly() {
  UsageLedger L;
  L.models().setOwnerRate("openai", "gpt-5.6", 5000000, 30000000);        // $5 / $30
  L.models().setOwnerRate("openai", "gpt-5.6-luna", 200000, 1200000);     // $0.20 / $1.20
  turn(L, "openai", "gpt-5.6", 1000000, 100000);        // 500 + 300 = 800 cents
  turn(L, "openai", "gpt-5.6-luna", 10000000, 1000000);  // 200 + 120 = 320 cents
  TEST_ASSERT_EQUAL_UINT64(1120, L.estCents("openai"));
  // The old provider-only answer would price all 11M/1.1M at one rate: never this.
  L.setLimits("openai", 0, 0, 1, 1120);
  TEST_ASSERT_TRUE(L.overCostBudget("openai"));
  L.setLimits("openai", 0, 0, 1, 1121);
  TEST_ASSERT_FALSE(L.overCostBudget("openai"));
  // The parts add up exactly to the whole.
  PriceSummary s;
  RowPrice rowCents[ModelLedger::kMaxRows];
  TEST_ASSERT_EQUAL_UINT64(1120, L.priceProvider("openai", &s, rowCents));
  TEST_ASSERT_EQUAL_UINT64(800, rowCents[0].cents);
  TEST_ASSERT_EQUAL_UINT64(320, rowCents[1].cents);
  TEST_ASSERT_EQUAL_UINT64(10000000, rowCents[1].take.in);
  TEST_ASSERT_EQUAL((int)RateSrc::Owner, (int)rowCents[1].rate.src());
  TEST_ASSERT_EQUAL_UINT64(0, s.resCents);
  TEST_ASSERT_FALSE(s.approx);
  TEST_ASSERT_FALSE(s.fastPath);
  // Odd counts at odd prices, computed by hand: 1,234,567 tokens x 2,345,678 and
  // 76,543 x 13,579,135 micro-USD per 1M = 2,895,896 + 1,039,387 = 3,935,283 micro-USD
  // = 393 cents (each term floored to micro, one floor to cents at the end).
  UsageLedger O;
  O.models().setOwnerRate("openai", "gpt-5.6", 2345678, 13579135);
  turn(O, "openai", "gpt-5.6", 1234567, 76543);
  TEST_ASSERT_EQUAL_UINT64(393, O.estCents("openai"));
}

// Per-field resolution: owner model > router (cumulo) > provider fallback > built-in
// model (~) > built-in provider estimate (~) > none; 0 = unset, per field. A model with
// no row of its own is estimated at the provider's HIGHEST built-in price (owner ruling
// R4, fail-closed money), not the cheaper '*' default.
static void test_rate_resolution_is_per_field() {
  UsageLedger L;
  EffRate r = L.effectiveRate("openai", "gpt-5.6");
  TEST_ASSERT_EQUAL((int)RateSrc::BuiltinModel, (int)r.srcIn);
  TEST_ASSERT_EQUAL_UINT64(5000000, r.in);
  r = L.effectiveRate("openai", "some-new-model");
  TEST_ASSERT_EQUAL((int)RateSrc::BuiltinProv, (int)r.srcIn);
  TEST_ASSERT_EQUAL_UINT64(10000000, r.in);    // gpt-6-astra's $10, the top built-in
  TEST_ASSERT_EQUAL_UINT64(50000000, r.out);
  L.setRates("openai", 300, 0, 0);                     // provider fallback, input only
  r = L.effectiveRate("openai", "gpt-5.6");
  TEST_ASSERT_EQUAL((int)RateSrc::Provider, (int)r.srcIn);
  TEST_ASSERT_EQUAL_UINT64(3000000, r.in);
  TEST_ASSERT_EQUAL((int)RateSrc::BuiltinModel, (int)r.srcOut);
  L.models().setOwnerRate("openai", "gpt-5.6", 0, 7);   // owner output only
  r = L.effectiveRate("openai", "gpt-5.6");
  TEST_ASSERT_EQUAL((int)RateSrc::Provider, (int)r.srcIn);
  TEST_ASSERT_EQUAL((int)RateSrc::Owner, (int)r.srcOut);
  TEST_ASSERT_EQUAL_UINT64(7, r.out);
  TEST_ASSERT_EQUAL((int)RateSrc::Provider, (int)r.src());   // the weakest priced tier
  // The owner's model price beats the provider fallback in the same field.
  L.models().setOwnerRate("openai", "gpt-5.6", 1234, 7);
  r = L.effectiveRate("openai", "gpt-5.6");
  TEST_ASSERT_EQUAL((int)RateSrc::Owner, (int)r.srcIn);
  TEST_ASSERT_EQUAL_UINT64(1234, r.in);
  TEST_ASSERT_EQUAL((int)RateSrc::Owner, (int)r.src());
  // Router prices count only for cumulo keys.
  std::vector<nimbus::orch::ModelRate> rr(1);
  std::strcpy(rr[0].prov, "cumulo");
  std::strcpy(rr[0].model, "openai/gpt-5.6");
  rr[0].microIn = 4000000;
  rr[0].microOut = 20000000;
  L.models().replaceRouterRates(rr);
  r = L.effectiveRate("cumulo", "openai/gpt-5.6");
  TEST_ASSERT_EQUAL((int)RateSrc::Router, (int)r.srcIn);
  TEST_ASSERT_EQUAL_UINT64(4000000, r.in);
  L.models().setOwnerRate("cumulo", "openai/gpt-5.6", 1, 0);
  r = L.effectiveRate("cumulo", "openai/gpt-5.6");
  TEST_ASSERT_EQUAL((int)RateSrc::Owner, (int)r.srcIn);       // the owner beats the router
  TEST_ASSERT_EQUAL((int)RateSrc::Router, (int)r.srcOut);
  // Custom has no built-in price: none.
  r = L.effectiveRate("custom", "llama3.1:8b");
  TEST_ASSERT_EQUAL((int)RateSrc::None, (int)r.src());
  TEST_ASSERT_EQUAL_STRING("none", nimbus::orch::rateSrcName(r.src()));
  TEST_ASSERT_EQUAL_STRING("est", nimbus::orch::rateSrcName(RateSrc::BuiltinProv));
}

// Spend no model row covers (pre-upgrade spend) is priced at the provider fallback,
// else the HIGHEST model price set for that provider (fail-closed), else the default.
static void test_residual_pricing_rule() {
  UsageLedger L;
  L.recordTokens("openai", 1000000, 0, 24300);          // unattributed spend
  TEST_ASSERT_EQUAL_UINT64(250, L.estCents("openai"));   // built-in default $2.50/M
  TEST_ASSERT_EQUAL((int)RateSrc::BuiltinProv, (int)L.residualRate("openai").srcIn);
  L.models().setOwnerRate("openai", "gpt-5.6-luna", 200000, 1200000);
  L.models().setOwnerRate("openai", "gpt-6-astra", 10000000, 50000000);
  TEST_ASSERT_EQUAL_UINT64(1000, L.estCents("openai"));  // the highest: $10/M
  TEST_ASSERT_EQUAL((int)RateSrc::Owner, (int)L.residualRate("openai").srcIn);
  L.setRates("openai", 100, 100, 0);
  // Provider fallback complete + no model rows: the legacy formula (fast path).
  TEST_ASSERT_EQUAL_UINT64(100, L.estCents("openai"));
  // Cumulo: the highest router price counts too.
  UsageLedger C;
  C.recordTokens("cumulo", 1000000, 0, 24300);
  TEST_ASSERT_EQUAL_UINT64(250, C.estCents("cumulo"));   // openai default (router default upstream)
  std::vector<nimbus::orch::ModelRate> rr(1);
  std::strcpy(rr[0].prov, "cumulo");
  std::strcpy(rr[0].model, "anthropic/claude-opus-5");
  rr[0].microIn = 6000000;
  C.models().replaceRouterRates(rr);
  TEST_ASSERT_EQUAL_UINT64(600, C.estCents("cumulo"));
  // Residual + model rows together: the rows take their share, the rest is residual.
  UsageLedger M;
  M.recordTokens("anthropic", 2000000, 0, 24300);                 // pre-upgrade
  turn(M, "anthropic", "claude-haiku-4-5", 1000000, 0);           // ~$1/M = 100c
  PriceSummary s;
  TEST_ASSERT_EQUAL_UINT64(100 + 600, M.priceProvider("anthropic", &s));   // residual 2M at ~$3/M
  TEST_ASSERT_EQUAL_UINT64(2000000, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(600, s.resCents);
  TEST_ASSERT_TRUE(s.approx);
}

// A model row can never claim more than its provider recorded this period (clamp),
// and rows from another period are ignored (stale).
static void test_clamp_and_stale_rows_ignored() {
  UsageLedger L;
  L.models().setOwnerRate("openai", "gpt-5.6", 10000000, 0);
  turn(L, "openai", "gpt-5.6", 1000000, 0, 24300);
  L.setRates("openai", 100, 100, 0);
  turn(L, "openai", "other-model", 1000000, 0, 24301);   // new period: gpt-5.6 row is stale
  // other-model: provider fallback $1/M = 100c; the stale gpt-5.6 row adds nothing.
  TEST_ASSERT_EQUAL_UINT64(100, L.estCents("openai"));
  // A row whose counters exceed the provider's (spend filed under the row only) is
  // clamped to the provider counter.
  UsageLedger C;
  C.models().record("zai", "glm-5.3", td(5000000), 24300, 0);   // row only, no provider spend
  C.recordTokens("zai", 1000000, 0, 24300);
  PriceSummary s;
  RowPrice rows[ModelLedger::kMaxRows];
  // 1M tokens at ~$1.40/M, not 5M.
  TEST_ASSERT_EQUAL_UINT64(140, C.priceProvider("zai", &s, rows));
  TEST_ASSERT_EQUAL_UINT64(140, rows[0].cents);
  TEST_ASSERT_EQUAL_UINT64(1000000, rows[0].take.in);
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
}

static std::string mk(int i) { return "m-" + std::to_string(i); }

// A row recorded before the clock synced carries into the first real period with
// its provider and rolls with it afterwards: it never claims a later month's spend
// at its own (cheaper) price.
static void test_pre_sync_row_never_claims_a_later_month() {
  UsageLedger L;
  turn(L, "openai", "gpt-5.6-luna", 1000000, 0, 0);       // before SNTP: ~$0.20/M
  turn(L, "openai", "gpt-6-astra", 1000000, 0, 24300);    // first sync: both carried
  TEST_ASSERT_EQUAL_UINT64(20 + 1000, L.estCents("openai"));
  turn(L, "openai", "gpt-6-astra", 1000000, 0, 24301);    // the next month
  TEST_ASSERT_EQUAL_UINT64(1000, L.estCents("openai"));  // ~$10/M, not luna's $0.20
  TEST_ASSERT_EQUAL_UINT64(0, L.models().findRow("openai", "gpt-5.6-luna")->tokIn);
}

// Each provider rolls on its own reset day: one that moved to a new period never
// evicts another provider's rows that still carry spend in that provider's period.
static void test_eviction_respects_each_providers_period() {
  UsageLedger L;
  turn(L, "openai", "gpt-6-astra", 1000000, 0, 24300);
  for (int i = 1; i < 8; ++i) turn(L, "openai", mk(i).c_str(), 1000, 0, 24300);
  for (int i = 0; i < 8; ++i) turn(L, "mistral", mk(i).c_str(), 1000, 0, 24300);
  for (int i = 0; i < 8; ++i) turn(L, "zai", mk(i).c_str(), 1000, 0, 24300);
  for (int i = 0; i < 7; ++i) turn(L, "anthropic", mk(i).c_str(), 1000, 0, 24300);
  turn(L, "cumulo", "openai/gpt-5.6", 1000, 0, 24300);
  TEST_ASSERT_EQUAL(ModelLedger::kMaxRows, L.models().rows().size());
  const uint64_t before = L.estCents("openai");
  turn(L, "anthropic", "claude-opus-5", 1000, 0, 24301);   // anthropic's reset day came
  TEST_ASSERT_NOT_NULL(L.models().findRow("anthropic", "claude-opus-5"));
  TEST_ASSERT_NOT_NULL(L.models().findRow("openai", "gpt-6-astra"));
  for (int i = 1; i < 8; ++i) TEST_ASSERT_NOT_NULL(L.models().findRow("openai", mk(i).c_str()));
  TEST_ASSERT_EQUAL_UINT64(before, L.estCents("openai"));
}

// The ~other row holds live spend of models the ledger could not name - possibly the
// provider's dearest - so it is priced at the highest price known for the provider.
static void test_other_row_priced_at_highest_known_price() {
  UsageLedger L;
  for (int i = 0; i < 8; ++i) turn(L, "openai", mk(i).c_str(), 1000, 0);
  turn(L, "openai", "gpt-6-astra", 1000000, 1000000);   // the 9th model this period
  TEST_ASSERT_NULL(L.models().findRow("openai", "gpt-6-astra"));
  const EffRate o = L.rowRate("openai", nimbus::orch::kOtherModel);
  TEST_ASSERT_EQUAL_UINT64(10000000, o.in);    // gpt-6-astra's built-in, not '*' 2.5
  TEST_ASSERT_EQUAL_UINT64(50000000, o.out);
  // 8 x 1000 tokens of unknown models at the top built-in $10/M (8 cents, R4) + astra
  // 1M/1M at $10/$50.
  TEST_ASSERT_EQUAL_UINT64(8 + 1000 + 5000, L.estCents("openai"));
  // An owner model price above every built-in one raises it; an owner default below
  // them does not lower it.
  L.models().setOwnerRate("openai", mk(1).c_str(), 20000000, 0);
  L.setRates("openai", 100, 100, 0);
  const EffRate o2 = L.rowRate("openai", nimbus::orch::kOtherModel);
  TEST_ASSERT_EQUAL_UINT64(20000000, o2.in);
  TEST_ASSERT_EQUAL((int)RateSrc::Owner, (int)o2.srcIn);
  TEST_ASSERT_EQUAL_UINT64(50000000, o2.out);
  // Cumulo's ~other reads every upstream's built-in rows.
  TEST_ASSERT_EQUAL_UINT64(10000000, UsageLedger().rowRate("cumulo", nimbus::orch::kOtherModel).in);
}

// Model rows and the provider counters are stored apart on the device: at load, rows
// of another period or of a provider with no entry lose their period spend (all-time
// totals kept: never counted), and when a provider's rows do not add up to its
// counters a store failed, so its rows lose their attribution and that spend is
// priced at the provider's highest known price (gate G1), never a cheaper one.
static void test_rebase_drops_rows_the_counters_do_not_cover() {
  UsageLedger L;
  L.recordTokens("openai", 1000000, 0, 24300);                       // NVS counters
  L.models().record("openai", "gpt-5.6-luna", td(5000000), 24300, 9);  // more than that
  L.models().record("openai", "gpt-6-astra", td(1000), 24300, 9);
  L.models().record("zai", "glm-5.3", td(7), 24300, 9);              // no provider entry
  L.recordTokens("anthropic", 10, 0, 24301);
  L.models().record("anthropic", "claude-opus-5", td(10), 24300, 9);  // another period
  L.rebaseModelRows();
  for (const auto& r : L.models().rows()) TEST_ASSERT_EQUAL_UINT64(0, r.tokIn);
  TEST_ASSERT_EQUAL_UINT64(7, L.models().findRow("zai", "glm-5.3")->totIn);
  TEST_ASSERT_EQUAL_UINT64(10, L.models().findRow("anthropic", "claude-opus-5")->totIn);
  // openai's rows left with their period spend (totals too): its model is unknown.
  TEST_ASSERT_EQUAL_UINT64(0, L.models().findRow("openai", "gpt-5.6-luna")->totIn);
  PriceSummary s;
  // The whole 1M is lost spend now, at openai's highest price (gpt-6-astra's ~$10/M),
  // never the cheaper '*' $2.50/M (250c) round 1 charged.
  TEST_ASSERT_EQUAL_UINT64(1000, L.priceProvider("openai", &s));
  TEST_ASSERT_EQUAL_UINT64(1000000, s.othIn);
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(1000000, L.models().track("openai")->lost.in);
  // Rows the counters do cover are untouched.
  UsageLedger K;
  turn(K, "openai", "gpt-6-astra", 1000000, 0);
  K.rebaseModelRows();
  TEST_ASSERT_EQUAL_UINT64(1000000, K.models().findRow("openai", "gpt-6-astra")->tokIn);
  TEST_ASSERT_EQUAL_UINT64(1000, K.estCents("openai"));
}

// The pieces of one restart: the usgLedger blob, the spend file and the price files
// as they last landed, loaded and rebased the way the device does it.
static UsageLedger restart(const std::string& nvs, const std::string& spend,
                           const std::string& owner, const std::string& router = "",
                           bool* forgot = nullptr) {
  UsageLedger R;
  R.deserialize(nvs);
  R.models().deserializeSpend(spend);
  R.models().deserializeOwnerRates(owner);
  R.models().deserializeRouterRates(router);
  const bool f = R.rebaseModelRows();
  if (forgot) *forgot = f;
  return R;
}

// Gate G1: a failed usgLedger write leaves the spend file AHEAD of the NVS counters.
// After a restart that spend is never repriced at the owner's cheaper default.
static void test_gate_g1_failed_nvs_write_never_reprices_cheaper() {
  UsageLedger L;
  L.setRates("openai", 20, 20, 0);                                 // a $0.20/M default
  L.models().setOwnerRate("openai", "gpt-6-astra", 10000000, 50000000);
  turn(L, "openai", "gpt-6-astra", 1000000, 100000);               // $10 + $5
  const std::string nvs = L.serialize();                            // the last good blob
  turn(L, "openai", "gpt-6-astra", 1000000, 100000);               // its NVS write fails
  L.models().setCountersBehind(true);                               // as the device marks it
  const std::string owner = L.models().serializeOwnerRates();
  UsageLedger R = restart(nvs, L.models().serializeSpend(), owner);
  // The counters hold one turn, all gpt-6-astra: 1500c. Round 1 repriced it at the
  // $0.20 default (22c).
  TEST_ASSERT_EQUAL_UINT64(1500, R.estCents("openai"));
  R.setLimits("openai", 0, 0, 1, 1500);
  TEST_ASSERT_TRUE(R.overCostBudget("openai"));
  // The rebase is kept with the rows: the next restart finds nothing new to drop, and
  // new turns are attributed again and survive restarts at their own price.
  UsageLedger R2 = restart(nvs, R.models().serializeSpend(), owner);
  TEST_ASSERT_EQUAL_UINT64(1500, R2.estCents("openai"));
  turn(R2, "openai", "gpt-6-astra", 1000000, 0);
  R2.models().setCountersBehind(false);                             // this NVS write lands
  UsageLedger R3 = restart(R2.serialize(), R2.models().serializeSpend(), owner);
  TEST_ASSERT_EQUAL_UINT64(2500, R3.estCents("openai"));
  TEST_ASSERT_EQUAL_UINT64(1000000, R3.models().findRow("openai", "gpt-6-astra")->tokIn);
  TEST_ASSERT_EQUAL_UINT64(1000000, R3.models().track("openai")->lost.in);   // counted once
}

// A row the load clears for another period follows its provider's period, so a turn
// recorded before the clock syncs lands in the period the provider counts in (at the
// row's own price), not in a stale one where it would read as lost spend.
static void test_rebased_row_follows_its_provider_period() {
  UsageLedger N;
  N.setLimits("openai", 0, 0, 1, 0);                  // a provider that never synced: period 0
  ModelLedger F;
  F.record("openai", "gpt-5.6", td(1000), 24321, 1);  // a row of an old period
  UsageLedger R = restart(N.serialize(), F.serializeSpend(), "");
  TEST_ASSERT_EQUAL_UINT32(0, R.models().findRow("openai", "gpt-5.6")->periodKey);
  turn(R, "openai", "gpt-5.6", 500000, 50000, 0);     // before SNTP
  PriceSummary s;
  TEST_ASSERT_EQUAL_UINT64(250 + 150, R.priceProvider("openai", &s));   // gpt-5.6's ~$5/$30
  TEST_ASSERT_EQUAL_UINT64(0, s.othIn);
}

// Gate G2: all 32 rows hold spend this period and a provider with no row records:
// it gets a row (another provider's smallest row folds into its own ~other), so the
// spend is priced at its model's own price, never the cheaper provider default.
static void test_gate_g2_full_ledger_new_provider_still_gets_a_row() {
  UsageLedger L;
  for (const char* p : {"openai", "mistral", "zai", "cumulo"})
    for (int i = 0; i < 8; ++i) turn(L, p, mk(i).c_str(), 1000, 0);
  TEST_ASSERT_EQUAL(ModelLedger::kMaxRows, L.models().rows().size());
  uint64_t before[4];
  const char* others[] = {"openai", "mistral", "zai", "cumulo"};
  for (int k = 0; k < 4; ++k) before[k] = L.estCents(others[k]);
  turn(L, "anthropic", "claude-opus-5", 1000000, 1000000);
  TEST_ASSERT_NOT_NULL(L.models().findRow("anthropic", "claude-opus-5"));
  // claude-opus-5's own ~$5/$25: 3000c (round 1: the residual at '*' $3/$15, 1800c).
  TEST_ASSERT_EQUAL_UINT64(3000, L.estCents("anthropic"));
  PriceSummary s;
  L.priceProvider("anthropic", &s);
  TEST_ASSERT_EQUAL_UINT64(0, s.othIn + s.resIn);
  for (int k = 0; k < 4; ++k) TEST_ASSERT_TRUE(L.estCents(others[k]) >= before[k]);
}

// Gate G3: a failed spend-file write leaves the file BEHIND the counters. After a
// restart the period is never repriced at the owner's cheaper default; with no file
// at all but a history that shows tracking started (the device checks), neither.
static void test_gate_g3_failed_model_file_write_never_reprices_cheaper() {
  UsageLedger L;
  L.setRates("openai", 20, 20, 0);                                  // a $0.20/M default
  L.models().setOwnerRate("openai", "gpt-5.6", 5000000, 30000000);
  turn(L, "openai", "gpt-5.6", 1000000, 100000);                    // 500 + 300
  const std::string file = L.models().serializeSpend();              // last file that landed
  turn(L, "openai", "gpt-5.6", 1000000, 100000);                    // its file write fails
  const std::string owner = L.models().serializeOwnerRates();
  TEST_ASSERT_EQUAL_UINT64(1600, L.estCents("openai"));
  UsageLedger R = restart(L.serialize(), file, owner);
  // Round 1: 800 + the second turn at the $0.20 default = 822c, and a 1600c cap lifted.
  // The file is only behind the counters (their latest store landed): its row is
  // still true and stays, and the turn it misses is lost spend at openai's highest
  // price (gpt-6-astra's ~$10/$50): 800 + 1000 + 500.
  TEST_ASSERT_EQUAL_UINT64(2300, R.estCents("openai"));
  TEST_ASSERT_EQUAL_UINT64(1000000, R.models().findRow("openai", "gpt-5.6")->tokIn);
  R.setLimits("openai", 0, 0, 1, 1600);
  TEST_ASSERT_TRUE(R.overCostBudget("openai"));
  // ...and the next restart sees the same thing (the gap is kept as lost spend).
  UsageLedger R2 = restart(R.serialize(), R.models().serializeSpend(), owner);
  TEST_ASSERT_EQUAL_UINT64(2300, R2.estCents("openai"));
  // With the counters' latest store marked as not landed, rows that match still stay;
  // rows that are behind may hide rows that are ahead, so none is kept: all of the
  // provider's spend at the highest price.
  L.models().setCountersBehind(true);
  UsageLedger B = restart(L.serialize(), L.models().serializeSpend(), owner);
  TEST_ASSERT_EQUAL_UINT64(1600, B.estCents("openai"));
  ModelLedger stale;
  stale.deserializeSpend(file);
  stale.setCountersBehind(true);
  UsageLedger X = restart(L.serialize(), stale.serializeSpend(), owner);
  TEST_ASSERT_EQUAL_UINT64(0, X.models().findRow("openai", "gpt-5.6")->tokIn);
  TEST_ASSERT_EQUAL_UINT64(3000, X.estCents("openai"));   // 2M + 200K at ~$10/$50
  // No spend file at all, and the device saw a model bucket in the history.
  UsageLedger N;
  N.deserialize(L.serialize());
  N.models().deserializeOwnerRates(owner);
  N.models().markTracking();
  N.rebaseModelRows();
  TEST_ASSERT_TRUE(N.estCents("openai") >= 1600);
}

// The device's load in a test: the stores as they last landed, then settleModels with
// the history as stored (store.cpp loadModelsLocked).
static UsageLedger load(const std::string& nvs, const std::string& spend, const std::string& owner,
                        const UsageHistory& history, UsageLedger::Settled* settled = nullptr) {
  UsageLedger R;
  R.deserialize(nvs);
  R.models().deserializeSpend(spend);
  R.models().deserializeOwnerRates(owner);
  const UsageLedger::Settled s = R.settleModels(history);
  if (settled) *settled = s;
  return R;
}

// The history an earlier release leaves: our lines read (extra fields ignored) and
// rewritten without models, plus its own model-less buckets.
static UsageHistory earlierReleaseHistory(const UsageHistory& ours) {
  UsageHistory h;
  for (const auto& b : ours.entries()) h.record(b.prov, b.dayKey, b.tokIn, b.tokOut, b.calls);
  return h;
}

// Back to an earlier release and updated again: the earlier release keeps counting
// in usgLedger but never touches the spend file, so after the update the rows are
// only behind. When the history it rewrote shows it recorded, it may have reset the
// counters first (gate M1), so the provider's rows and earlier usage are not trusted:
// all its spend this period is priced at its highest price. When none of its history
// writes landed, the rows stay (each model at its own price) and its spend, whose
// model is unknown, is priced at the provider's highest price.
static void test_downgrade_and_back_keeps_the_rows() {
  UsageLedger L;
  L.models().setOwnerRate("openai", "gpt-5.6-luna", 200000, 1200000);
  turn(L, "openai", "gpt-5.6-luna", 1000000, 100000);               // 20 + 12
  turn(L, "openai", "gpt-6-astra", 1000000, 0);                     // 1000
  L.noteCountersStored();
  const std::string file = L.models().serializeSpend();
  const std::string owner = L.models().serializeOwnerRates();
  UsageHistory ours;
  ours.record("openai", "gpt-5.6-luna", 20650, td(1000000, 100000), 0);
  ours.record("openai", "gpt-6-astra", 20650, td(1000000), 0);
  UsageLedger old;                                                   // v4.5.12 boots
  old.deserialize(L.serialize());
  old.recordTokens("openai", 1000, 0, 24300);
  UsageLedger R = load(old.serialize(), file, owner, ours);         // its history never landed
  TEST_ASSERT_EQUAL_UINT64(1000000, R.models().findRow("openai", "gpt-5.6-luna")->tokIn);
  TEST_ASSERT_EQUAL_UINT64(1000000, R.models().findRow("openai", "gpt-6-astra")->tokIn);
  PriceSummary s;
  TEST_ASSERT_EQUAL_UINT64(1032 + 1, R.priceProvider("openai", &s));   // 1000 tokens at ~$10/M
  TEST_ASSERT_EQUAL_UINT64(1000, s.othIn);
  UsageHistory rewritten = earlierReleaseHistory(ours);
  rewritten.record("openai", 20651, 1000, 0, 0);
  UsageLedger W = load(old.serialize(), file, owner, rewritten);   // its history landed
  TEST_ASSERT_EQUAL_UINT64(0, W.models().findRow("openai", "gpt-5.6-luna")->tokIn);
  W.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(2001000, s.othIn);                        // everything at ~$10/M
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
}

// Gate M1: a settings-only reset on an earlier release (NVS erased, /data kept), new
// spend there, and an update back in the same period. The spend file's track and rows
// describe counters that no longer exist; the new spend is never priced below the
// provider's highest price, whether it is smaller or larger than what the file knew.
static void test_gate_m1_reset_on_an_earlier_release_never_prices_new_spend_low() {
  UsageLedger L;
  L.recordTokens("openai", 1400000, 180000, 24300);                 // earlier usage
  turn(L, "openai", "gpt-5.6-luna", 2000000, 200000);                // tracking starts
  L.noteCountersStored();                                             // its usgLedger write landed
  const std::string file = L.models().serializeSpend();
  UsageHistory ours;
  ours.record("openai", 20650, 1400000, 180000, 0);
  ours.record("openai", "gpt-5.6-luna", 20650, td(2000000, 200000), 0);
  auto afterReset = [](uint64_t in, uint64_t out) {   // the earlier release, reset
    UsageLedger old;
    old.recordTokens("openai", in, out, 24300);
    return old.serialize();
  };
  // 6M / 600K of gpt-6-astra (true price 6000 + 3000 cents): MORE than the file knew
  // (3.4M), on a device with no earlier period. The earlier release's history shows it.
  UsageHistory rewritten = earlierReleaseHistory(ours);
  rewritten.record("openai", 20651, 6000000, 600000, 0);
  UsageLedger A = load(afterReset(6000000, 600000), file, "", rewritten);
  PriceSummary s;
  TEST_ASSERT_EQUAL_UINT64(9000, A.priceProvider("openai", &s));     // all at ~$10/$50
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(0, A.models().findRow("openai", "gpt-5.6-luna")->totIn);
  // LESS than the file knew (1M): the stored totals (3.4M) show the re-creation even
  // with no history evidence.
  UsageLedger B = load(afterReset(1000000, 100000), file, "", ours);
  TEST_ASSERT_EQUAL_UINT64(1000 + 500, B.priceProvider("openai", &s));
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  // A device with spend before the tracking period: the counters' spend before this
  // period can never shrink, so 9M re-created (more than every total the file knew,
  // 8.4M) shows even with no history evidence.
  UsageLedger P;
  P.recordTokens("openai", 5000000, 0, 24299);                       // last month
  P.recordTokens("openai", 1400000, 180000, 24300);
  turn(P, "openai", "gpt-5.6-luna", 2000000, 200000);
  P.noteCountersStored();
  UsageLedger C = load(afterReset(9000000, 900000), P.models().serializeSpend(), "", ours);
  TEST_ASSERT_EQUAL_UINT64(9000 + 4500, C.priceProvider("openai", &s));
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  // The next load sees nothing new to drop (the reset is stored with the file).
  UsageLedger D = load(afterReset(9000000, 900000), C.models().serializeSpend(), "", ours);
  TEST_ASSERT_EQUAL_UINT64(9000 + 4500, D.priceProvider("openai", &s));
  // The same trip right after the update boot (no turn since): the totals the boot
  // stored with its spend file already show it.
  UsageLedger P0;
  P0.recordTokens("openai", 5000000, 0, 24299);
  P0.recordTokens("openai", 1400000, 180000, 24300);
  UsageHistory h0;
  h0.record("openai", 20650, 1400000, 180000, 0);
  const std::string bootFile = load(P0.serialize(), "", "", h0).models().serializeSpend();
  UsageLedger C0 = load(afterReset(9000000, 900000), bootFile, "", h0);
  TEST_ASSERT_EQUAL_UINT64(9000 + 4500, C0.priceProvider("openai", &s));
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  // The latest usgLedger write did not land ("!n": the rows may hold spend the counters
  // lack): the totals of the last write that did land (3.4M) still show 2M re-created.
  L.models().setCountersBehind(true);
  UsageLedger E = load(afterReset(2000000, 200000), L.models().serializeSpend(), "", ours);
  TEST_ASSERT_EQUAL_UINT64(2000 + 1000, E.priceProvider("openai", &s));
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  // Either direction alone shows it: more input than the file knew, less output.
  L.models().setCountersBehind(false);
  UsageLedger F = load(afterReset(5000000, 100000), L.models().serializeSpend(), "", ours);
  F.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(5000000, s.othIn);
  // A spend file from before the stored totals (a track with no such fields) still
  // shows counters below the totals when tracking started: its earlier usage is gone.
  UsageLedger V = load(afterReset(1000000, 100000),
                       "!v|1;!t;!p|openai|24300|1000000|100000|0|0|0|0|0|0|1400000|180000", "", ours);
  V.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(1000000, s.othIn);
  // The history evidence is the latest row day's: a model bucket only on an older day
  // does not show that day was recorded by this release.
  UsageLedger T;
  turn(T, "openai", "gpt-5.6", 1000, 0);                             // day 20650
  UsageHistory older;
  older.record("openai", "gpt-5.6", 20649, td(5), 0);
  older.record("openai", 20650, 1000, 0, 0);
  TEST_ASSERT_TRUE(T.earlierReleaseRecorded(&older));
  older.record("openai", "gpt-5.6", 20650, td(1000), 0);
  TEST_ASSERT_FALSE(T.earlierReleaseRecorded(&older));
  TEST_ASSERT_FALSE(T.earlierReleaseRecorded(nullptr));
  // Gate P2N: a history write lost to a power cut leaves NO bucket on the latest row day;
  // that is not an earlier release (which rewrites buckets without a model), so the
  // period is not repriced and the per-model totals survive.
  UsageHistory lost;
  lost.record("openai", "gpt-5.6", 20649, td(5), 0);
  TEST_ASSERT_FALSE(lost.hasBucketOn(20650));
  TEST_ASSERT_FALSE(T.earlierReleaseRecorded(&lost));
  lost.record("openai", 20650, 1000, 0, 0);                         // an earlier release's rewrite
  TEST_ASSERT_TRUE(T.earlierReleaseRecorded(&lost));
}

// Gate M2: an update from an earlier release (its history has buckets, none with a
// model). Tracking starts at the load with the stored counters as earlier usage and the
// spend file is stored then, so a later turn whose spend-file write fails never reads
// as earlier usage after a restart. When that first store fails too, the next record
// stores the spend file BEFORE its usgLedger write (spendFileFirst): a failure between
// the two leaves the file ahead (fail-closed).
static void test_gate_m2_failed_first_spend_file_write_never_makes_earlier_usage() {
  UsageLedger v12;                                                    // v4.5.12's counters
  v12.recordTokens("cumulo", 500000, 50000, 24300);
  const std::string nvs0 = v12.serialize();
  UsageHistory h12;
  h12.record("cumulo", 20650, 500000, 50000, 0);                     // its 5-field history
  UsageLedger::Settled st;
  UsageLedger L = load(nvs0, "", "", h12, &st);                      // the update boot
  TEST_ASSERT_TRUE(st.started);
  TEST_ASSERT_TRUE(st.mustStore());
  TEST_ASSERT_TRUE(L.models().tracking());
  TEST_ASSERT_EQUAL_UINT64(500000, L.models().track("cumulo")->pre.in);
  const std::string bootFile = L.models().serializeSpend();          // stored at the load
  // anthropic/claude-opus-5 via Cumulo, 1M / 1M (true price ~$5/$25: 3000 cents); its
  // usgLedger write lands, its spend-file and history writes do not.
  turn(L, "cumulo", "anthropic/claude-opus-5", 1000000, 1000000);
  L.noteCountersStored();
  const std::string nvs1 = L.serialize();
  UsageLedger R = load(nvs1, bootFile, "", h12);
  PriceSummary s;
  const uint64_t est = R.priceProvider("cumulo", &s);
  TEST_ASSERT_EQUAL_UINT64(500000, s.resIn);                         // only v4.5.12's spend
  TEST_ASSERT_EQUAL_UINT64(1000000, s.othIn);                        // the turn: highest price
  TEST_ASSERT_TRUE(est >= 125 + 50 + 3000);                          // earlier usage + true
  // The store at the load failed: the first record goes file first. Its usgLedger write
  // then fails (a power cut): the file is ahead, so the turn is not counted and the
  // row drops it. Its history write never happened either, so the history cannot show
  // that no earlier release ran since (gate M1): the stored counters lose their
  // earlier usage too, all priced at the highest price (fail-closed).
  UsageLedger F = load(nvs0, "", "", h12);
  F.models().setSpendFileStored(false);
  TEST_ASSERT_TRUE(F.models().spendFileFirst());
  turn(F, "cumulo", "anthropic/claude-opus-5", 1000000, 1000000);
  F.models().setCountersBehind(true);                                // as the device marks it
  UsageLedger G = load(nvs0, F.models().serializeSpend(), "", h12);
  G.priceProvider("cumulo", &s);
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(500000, s.othIn);
  TEST_ASSERT_EQUAL_UINT64(0, G.models().findRow("cumulo", "anthropic/claude-opus-5")->tokIn);
  // With the turn's history bucket on flash (only its usgLedger write failed), the
  // stored counters keep their earlier usage at the residual price.
  UsageHistory h13 = h12;
  h13.record("cumulo", "anthropic/claude-opus-5", 20650, td(1000000, 1000000), 0);
  UsageLedger G2 = load(nvs0, F.models().serializeSpend(), "", h13);
  G2.priceProvider("cumulo", &s);
  TEST_ASSERT_EQUAL_UINT64(500000, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(0, s.othIn);
  TEST_ASSERT_EQUAL_UINT64(0, G2.models().findRow("cumulo", "anthropic/claude-opus-5")->tokIn);
}

// A device on this release, in the device's store order (store.cpp loadModelsLocked,
// recordProviderTokens): each store lands unless the settings store or /data fails.
struct Dev {
  UsageLedger g;
  UsageHistory hram;
  std::string nvs, file, hist;
  bool nvsFails = false, dataFails = false;
  UsageLedger::Settled load() {
    g = UsageLedger();
    g.deserialize(nvs);
    g.models().deserializeSpend(file);
    hram = UsageHistory();
    hram.deserialize(hist);
    const UsageLedger::Settled s = g.settleModels(hram);
    const bool current = !s.mustStore() || storeFile();
    if (!current && s.removable) file.clear();
    g.models().setSpendFileStored(current && !file.empty());
    g.models().setSpendFileStale(!current && !file.empty());
    return s;
  }
  bool storeFile() {
    if (dataFails) return false;
    file = g.models().serializeSpend();
    g.models().setSpendFileStored(true);
    return true;
  }
  void record(const char* p, const char* model, const TokenDelta& d, uint32_t per = 24300,
              uint32_t day = 20650) {
    const char* key = g.models().spendKey(model);
    g.recordTokens(p, d, per, "turn", key, day);
    if (*key && g.models().spendFileFirst()) {
      g.models().setCountersBehind(true);
      storeFile();
    }
    if (!nvsFails) {
      nvs = g.serialize();
      g.noteCountersStored();
    }
    g.models().setCountersBehind(nvsFails);
    if (*key) storeFile();
    hram.record(p, g.models().historyKey(key), day, d, 0);
    if (!dataFails) hist = hram.serialize();
  }
  // A trip to an earlier release with a settings-only reset there: it records `in` /
  // `out` for `p` (provider level) and rewrites the history without models when
  // `landed`.
  void tripWithReset(const char* p, uint64_t in, uint64_t out, bool landed, uint32_t day = 20651) {
    UsageLedger old;
    old.recordTokens(p, in, out, 24300);
    nvs = old.serialize();
    if (!landed) return;
    UsageHistory h;
    h.deserialize(hist);
    UsageHistory rewritten = earlierReleaseHistory(h);
    rewritten.record(p, day, in, out, 0);
    hist = rewritten.serialize();
  }
};

// Gate M1 review: the history evidence reads every row's day, also the rows of a
// provider the earlier release did not use again (the rebase clears their period
// spend), so a provider that only had earlier usage this period (no row of its own) is
// not taken for one that kept counting.
static void test_gate_m1_history_evidence_reads_every_row() {
  UsageLedger v12;
  v12.recordTokens("openai", 1400000, 180000, 24300);
  UsageHistory h12;
  h12.record("openai", 20649, 1400000, 180000, 0);
  Dev d;
  d.nvs = v12.serialize();
  d.hist = h12.serialize();
  d.load();                                                          // the update boot
  d.record("anthropic", "claude-haiku-4-5", td(500000, 50000));      // only anthropic is used
  d.tripWithReset("openai", 6000000, 600000, true);                  // its history landed
  d.load();
  PriceSummary s;
  TEST_ASSERT_EQUAL_UINT64(9000, d.g.priceProvider("openai", &s));  // all at ~$10/$50
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(6000000, s.othIn);
  // A row recorded before the clock synced has no day: with no dated row the history
  // cannot show an earlier release ran, and only the stored totals can (the documented
  // narrow hole, gate M1).
  UsageLedger U;
  U.recordTokens("openai", td(5), 0, "turn", "gpt-5.6", 0);
  TEST_ASSERT_FALSE(U.earlierReleaseRecorded(&h12));
}

// The spend file is removed when its store at a load fails after a dropped track only
// while the history shows tracking (the next load infers it): after an earlier release
// rewrote the history, the file stays, so a plain restart never starts tracking afresh
// and takes spend counted after it started for earlier usage.
static void test_failed_store_after_a_dropped_track_keeps_the_file_unless_inferred() {
  UsageLedger v12;
  v12.recordTokens("openai", 300000, 30000, 24300);
  v12.recordTokens("anthropic", 200000, 20000, 24300);
  UsageHistory h12;
  h12.record("openai", 20640, 300000, 30000, 0);
  h12.record("anthropic", 20640, 200000, 20000, 0);
  Dev d;
  d.nvs = v12.serialize();
  d.hist = h12.serialize();
  d.load();
  d.record("openai", "gpt-5.6-luna", td(1000000, 100000));
  d.record("anthropic", "claude-haiku-4-5", td(500000, 50000));
  d.tripWithReset("openai", 2000000, 200000, true);                  // anthropic: no entry now
  d.dataFails = true;                                                // /data has no free block
  UsageLedger::Settled st = d.load();
  TEST_ASSERT_TRUE(st.forgot);
  TEST_ASSERT_FALSE(st.removable);
  TEST_ASSERT_FALSE(d.file.empty());
  TEST_ASSERT_TRUE(d.g.models().spendFileFirst());   // the file on flash lacks the result
  PriceSummary s;
  const uint64_t est = d.g.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(2000 + 1000, est);                        // all at ~$10/$50
  st = d.load();                                                     // a plain restart
  TEST_ASSERT_FALSE(st.started);
  TEST_ASSERT_EQUAL_UINT64(est, d.g.priceProvider("openai", &s));
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  // A settings-only reset on this release keeps a history with model buckets: the
  // file is removed when its store fails, and the next load infers tracking.
  Dev r;
  r.nvs = v12.serialize();
  r.hist = h12.serialize();
  r.load();
  r.record("openai", "gpt-5.6-luna", td(1000000, 100000));
  r.nvs.clear();
  r.dataFails = true;
  st = r.load();
  TEST_ASSERT_TRUE(st.forgot);
  TEST_ASSERT_TRUE(st.removable);
  TEST_ASSERT_TRUE(r.file.empty());
  TEST_ASSERT_TRUE(r.load().inferred);
}

// Counters found re-created at a load are forgotten in the spend file at once (gate
// M1): what showed it (the stored totals, the history) may not show it at a later
// load, once the counters grow past the old totals or a later turn writes a model
// bucket on the same day. While that store fails, the next record writes the file
// first.
static void test_gate_m1_forgotten_counters_are_stored_at_load() {
  for (int viaHistory = 0; viaHistory < 2; ++viaHistory) {
    UsageLedger v12;
    v12.recordTokens("openai", 1400000, 180000, 24300);
    UsageHistory h12;
    h12.record("openai", 20649, 1400000, 180000, 0);
    Dev d;
    d.nvs = v12.serialize();
    d.hist = h12.serialize();
    d.load();
    d.record("openai", "gpt-5.6-luna", td(2000000, 200000));
    // 3M/300K below the stored 3.4M, or 4M/400K above it with the history landed.
    d.tripWithReset("openai", viaHistory ? 4000000 : 3000000, viaHistory ? 400000 : 300000,
                    viaHistory != 0, 20650);
    const UsageLedger::Settled st = d.load();
    TEST_ASSERT_TRUE(st.forgot);
    TEST_ASSERT_TRUE(st.mustStore());
    // A later turn of gpt-6-astra: its usgLedger write lands, its spend-file write does
    // not; its history write lands too.
    const uint64_t in = viaHistory ? 100000 : 1000000;
    d.dataFails = true;
    d.record("openai", "gpt-6-astra", td(in, in / 10));
    d.hist = d.hram.serialize();
    d.dataFails = false;
    d.load();
    PriceSummary s;
    const EffRate astra = d.g.effectiveRate("openai", "gpt-6-astra");
    const uint64_t allIn = (viaHistory ? 4000000 : 3000000) + in;
    const uint64_t truth = (nimbus::orch::costMicro(allIn, astra.in) +
                            nimbus::orch::costMicro(allIn / 10, astra.out)) / 10000;
    TEST_ASSERT_TRUE(d.g.priceProvider("openai", &s) >= truth);
    TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  }
  // The store at that load failed: the next record goes file first, and until a write
  // of the file lands its history bucket names no model, so the history keeps showing
  // that an earlier release rewrote it.
  UsageLedger v12;
  v12.recordTokens("openai", 1400000, 180000, 24300);
  UsageHistory h12;
  h12.record("openai", 20649, 1400000, 180000, 0);
  Dev f;
  f.nvs = v12.serialize();
  f.hist = h12.serialize();
  f.load();
  f.record("openai", "gpt-5.6-luna", td(2000000, 200000));
  f.tripWithReset("openai", 4000000, 400000, true, 20650);
  f.dataFails = true;
  TEST_ASSERT_TRUE(f.load().forgot);
  TEST_ASSERT_TRUE(f.g.models().spendFileFirst());
  TEST_ASSERT_EQUAL_STRING("", f.g.models().historyKey("gpt-6-astra"));
  f.record("openai", "gpt-6-astra", td(100000, 10000));   // only usgLedger and history land
  f.hist = f.hram.serialize();
  f.dataFails = false;
  f.load();
  PriceSummary s;
  f.g.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(4100000, s.othIn);              // all at the highest price
  f.record("openai", "gpt-6-astra", td(100000, 10000));   // its file write lands
  TEST_ASSERT_EQUAL_STRING("gpt-6-astra", f.g.models().historyKey("gpt-6-astra"));
}

// The re-created-counter check never fires on counters that only grew: a provider
// still in a pre-2026-07-16 group (period spend, no all-time), a usgLedger write that
// failed just after a period roll, and a settings-only reset on this release followed
// by new spend. Each keeps its estimate across a plain restart.
static void test_re_created_check_never_fires_on_counters_that_grew() {
  UsageLedger v;
  v.deserialize("mistral|24294|3000000|0|0|0|1");                    // the 7-field group
  v.setRates("mistral", 20, 20, 0);
  UsageHistory h;
  h.record("mistral", 20400, 3000000, 0, 0);
  Dev a;
  a.nvs = v.serialize();
  a.hist = h.serialize();
  PriceSummary s;
  for (int boot = 0; boot < 2; ++boot) {
    a.load();
    TEST_ASSERT_EQUAL_UINT64(a.g.find("mistral")->estCents(), a.g.priceProvider("mistral", &s));
    TEST_ASSERT_EQUAL_UINT64(0, s.othIn);
  }
  Dev b;
  b.load();
  b.record("openai", "gpt-5.6-luna", td(1000000, 100000), 24300, 20650);
  b.nvsFails = true;                                                 // a new period's write fails
  b.record("openai", "gpt-5.6-luna", td(2000000, 200000), 24301, 20680);
  b.load();
  b.nvsFails = false;
  b.record("openai", "gpt-5.6-luna", td(1000000, 100000), 24301, 20681);
  const uint64_t est = b.g.priceProvider("openai", &s);
  b.load();
  TEST_ASSERT_EQUAL_UINT64(est, b.g.priceProvider("openai", &s));
  TEST_ASSERT_EQUAL_UINT64(0, s.othIn);
  Dev c;
  c.load();
  c.record("openai", "gpt-5.6-luna", td(5000000, 500000));
  c.nvs.clear();                                                     // settings-only reset
  c.load();
  c.record("openai", "gpt-5.6-luna", td(1000000, 100000));
  const uint64_t est2 = c.g.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(0, s.othIn);
  c.load();
  TEST_ASSERT_EQUAL_UINT64(est2, c.g.priceProvider("openai", &s));
  TEST_ASSERT_EQUAL_UINT64(0, s.othIn);
}

// A spend file can keep more earlier usage than the stored counters hold: spend
// recorded with no model before tracking started whose usgLedger write did not land,
// or a track that outlived a settings-only reset while new spend was counted. Such a
// track is not trusted: its earlier usage is dropped at load, so later spend is never
// taken for earlier usage (it is priced as lost spend: fail-closed).
static void test_stale_earlier_usage_is_dropped_at_load() {
  UsageLedger L;
  L.recordTokens("openai", 1000000, 0, 24300);                      // stored
  const std::string nvs = L.serialize();
  L.recordTokens("openai", 9000000, 0, 24300);                      // its write fails
  turn(L, "openai", "gpt-6-astra", 1000, 0);                        // tracking: pre = 10M
  L.models().setCountersBehind(true);
  TEST_ASSERT_EQUAL_UINT64(10000000, L.models().track("openai")->pre.in);
  UsageLedger R = restart(nvs, L.models().serializeSpend(), "");
  TEST_ASSERT_EQUAL_UINT64(0, R.models().track("openai")->pre.in);
  TEST_ASSERT_EQUAL_UINT64(0, R.models().track("openai")->preTotIn);
  turn(R, "openai", "gpt-6-astra", 5000000, 0);                     // new spend at its own price
  PriceSummary s;
  R.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(1000 + 5000, s.cents);                   // 6M at ~$10/M
  // The settings-only reset: the provider is gone at the first load (its track goes,
  // and the device stores that at once); were the old track ever loaded beside new
  // counters of the same period, its earlier usage above them is dropped the same way.
  UsageLedger T;
  T.recordTokens("openai", 1400000, 180000, 24300);
  turn(T, "openai", "gpt-5.6", 1000, 0);                            // pre = 1.4M / 180K
  const std::string stale = T.models().serializeSpend();
  bool orphaned = false;
  UsageLedger A = restart("", stale, "", "", &orphaned);           // NVS erased
  TEST_ASSERT_TRUE(orphaned);
  TEST_ASSERT_NULL(A.models().track("openai"));
  UsageLedger N;                                                    // new spend after the reset
  N.setLimits("openai", 0, 0, 1, 1500);
  N.recordTokens("openai", 1200000, 150000, 24300);
  UsageLedger B = restart(N.serialize(), stale, "", "", &orphaned);
  TEST_ASSERT_TRUE(orphaned);                                       // forgotten: stored at once
  B.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);                             // never earlier usage
  TEST_ASSERT_TRUE(B.overCostBudget("openai"));                     // 1.2M/150K at ~$10/$50
}

// Earlier usage (spend from before tracking started on this device) keeps the
// residual price, across restarts, until its period ends; spend with no model after
// tracking started files under ~other (so the rows keep adding up to the counters).
static void test_earlier_usage_keeps_the_residual_price() {
  UsageLedger L;
  L.setRates("openai", 20, 20, 0);
  L.recordTokens("openai", 2000000, 0, 24300);                     // v4.5.12's spend
  L.models().setOwnerRate("openai", "gpt-5.6", 5000000, 30000000);
  turn(L, "openai", "gpt-5.6", 1000000, 100000);                   // 800c
  PriceSummary s;
  TEST_ASSERT_EQUAL_UINT64(800 + 40, L.priceProvider("openai", &s));   // 2M at $0.20
  TEST_ASSERT_EQUAL_UINT64(2000000, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(0, s.othIn);
  TEST_ASSERT_EQUAL_UINT64(2000000, L.models().track("openai")->pre.in);
  const std::string owner = L.models().serializeOwnerRates();
  UsageLedger R = restart(L.serialize(), L.models().serializeSpend(), owner);
  TEST_ASSERT_EQUAL_UINT64(840, R.estCents("openai"));
  // No model after tracking started: ~other, at the provider's highest price.
  R.recordTokens("openai", td(1000), 24300, "turn", "", 20650);
  TEST_ASSERT_NOT_NULL(R.models().findRow("openai", nimbus::orch::kOtherModel));
  UsageLedger R2 = restart(R.serialize(), R.models().serializeSpend(), owner);
  TEST_ASSERT_EQUAL_UINT64(R.estCents("openai"), R2.estCents("openai"));
  TEST_ASSERT_NOT_NULL(R2.models().findRow("openai", nimbus::orch::kOtherModel));
  TEST_ASSERT_EQUAL_UINT64(1000, R2.models().findRow("openai", nimbus::orch::kOtherModel)->tokIn);
  // The next period has no earlier usage.
  turn(R2, "openai", "gpt-5.6", 1000000, 0, 24301);
  R2.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(500, R2.estCents("openai"));
  // Not even when the provider rolled outside the model path (its track still names
  // the old period): the new period's spend is never taken for earlier usage.
  UsageLedger K;
  K.recordTokens("openai", 2000000, 0, 24300);
  turn(K, "openai", "gpt-5.6", 1000, 0);
  K.recordTokens("openai", 1000000, 0, 24301);   // provider level: the track is not synced
  K.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(0, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(1000000, s.othIn);
  // Tracking that starts before the clock syncs: the earlier usage carries into the
  // first real period with its provider, like the counters and the rows do.
  UsageLedger P;
  P.recordTokens("openai", 2000000, 0, 0);        // before SNTP, before the upgrade
  turn(P, "openai", "gpt-5.6", 1000, 0, 0);       // tracking starts, clock unsynced
  turn(P, "openai", "gpt-5.6", 1000, 0, 24300);   // first sync: everything carries
  P.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(2000000, s.resIn);
  TEST_ASSERT_EQUAL_UINT64(0, s.othIn);
}

// The pieces of the class test below.
struct TrueRec {
  std::string prov, model;   // model "" = earlier usage (priced at the residual rate)
  TokenDelta  d;
  uint32_t    period;
};

// Each counted token at its own model's price (earlier usage at the residual price),
// the same cache multipliers and floors as the estimate, in micro-dollars.
static uint64_t trueMicro(const UsageLedger& L, const std::vector<TrueRec>& live, const char* p) {
  const nimbus::orch::ProviderBudget* e = L.find(p);
  if (!e) return 0;
  std::vector<std::pair<std::string, TokenDelta>> agg;
  for (const TrueRec& r : live) {
    if (r.prov != p || r.period != e->periodKey) continue;
    auto it = agg.begin();
    while (it != agg.end() && it->first != r.model) ++it;
    if (it == agg.end()) { agg.push_back({r.model, TokenDelta{}}); it = agg.end() - 1; }
    it->second.in += r.d.in;
    it->second.out += r.d.out;
    it->second.cacheR += r.d.cacheR;
    it->second.cacheW += r.d.cacheW;
  }
  uint64_t micro = 0;
  for (const auto& a : agg) {
    const EffRate rt = a.first.empty() ? L.residualRate(p) : L.effectiveRate(p, a.first.c_str());
    micro += nimbus::orch::costMicro(a.second.in, rt.in) + nimbus::orch::costMicro(a.second.out, rt.out) +
             nimbus::orch::costMicro(a.second.cacheR, rt.in) / 10 +
             nimbus::orch::costMicro(a.second.cacheW * 5, rt.in) / 4;
  }
  return micro;
}

// Whether an estimate of `got` cents is below the true price (`trueMicro`). The
// estimate floors each priced part (a row, the lost spend, the earlier usage) to a
// micro-dollar per term where the truth floors each model once, so a model whose spend
// is split over parts at the same price reads under 4 micro-dollars per part lower (at
// most 14 parts): a cent short only when the truth is within 64 micro-dollars above a
// cent. The bit-exact legacy formula floors each of its four token terms to a cent.
static bool belowTruth(uint64_t got, uint64_t trueMicro, bool fastPath) {
  const uint64_t want = trueMicro / 10000;
  if (got + (fastPath ? 4 : 0) >= want) return false;
  return got + 1 != want || trueMicro % 10000 >= 64;
}

// The class test's tolerance ignores only that: two cents short, or a cent short with
// the truth further above the cent, still fail.
static void test_class_check_tolerates_only_per_part_flooring() {
  TEST_ASSERT_FALSE(belowTruth(11, 110000, false));
  TEST_ASSERT_FALSE(belowTruth(10, 110063, false));   // split flooring: tolerated
  TEST_ASSERT_TRUE(belowTruth(10, 110064, false));
  TEST_ASSERT_TRUE(belowTruth(9, 110000, false));
  TEST_ASSERT_TRUE(belowTruth(10, 119999, false));
  TEST_ASSERT_FALSE(belowTruth(7, 110000, true));     // the legacy formula's four floors
  TEST_ASSERT_TRUE(belowTruth(6, 110000, true));
}

// Class (gate G1-G3, M1, M2): for ANY sequence of turns (named models, spend with no
// model, more models than rows, each provider on its own reset day, records before the
// clock syncs, price edits), restarts after store failures (usgLedger, spend-file and
// history writes that did not land, in any mix, the first spend-file write included),
// settings-only resets, Erase Storage on a device with no SD card, and trips to an
// earlier release and back (with or without a settings-only reset there), estCents is
// never below pricing every counted token at its true model's own price (earlier usage
// at the residual price). The device's protocol is modeled as written: the update boot
// and every restart load through UsageLedger::settleModels with the history as stored
// (store.cpp loadModelsLocked: the spend file is stored then, and removed when that
// fails only while Settled::removable), a record stores the spend file first while
// the file on flash lacks what the load decided (ModelLedger::spendFileFirst) and its
// history bucket names no model while a stale one is there (historyKey), an earlier
// release rewrites the history without models. Spend counted while nothing stored
// shows tracking started (no spend file on flash) is earlier usage by definition, as
// on a device that never ran this release. Two modeled exclusions, the documented
// narrow holes:
//  - gate M2: no restart while the spend file is not on flash because its writes did
//    not land (not because the device removed it with no write tried since), the
//    stored counters hold spend of this period recorded after tracking started, and the
//    stored history does not show tracking; nor a trip to an earlier release (it
//    rewrites the history) while the file is not on flash and the stored counters hold
//    such spend;
//  - gate M1: a provider whose stored counters were re-created (a settings-only reset,
//    on this release or an earlier one) while the spend file on flash still describes
//    the old ones, when the history does not show an earlier release recorded since
//    the rows' latest record and the new counters hold at least every total the file
//    recorded and as much spend from before their period (the device cannot tell them
//    apart): skipped until its period rolls.
// One simulated device for the class test: the ledger in RAM, what its stores last
// took (the usgLedger blob, the spend file, the price files, the history), and the
// true record of every counted spend.
struct ClassSim {
  static constexpr int kProvs = 5;
  const char* provs[kProvs] = {"openai", "anthropic", "mistral", "zai", "cumulo"};
  const std::vector<std::vector<std::string>> models = {
      {"gpt-5.6", "gpt-5.6-luna", "gpt-6-astra", "gpt-5.6-terra", "o-1", "o-2", "o-3", "o-4", "o-5", "o-6"},
      {"claude-opus-5", "claude-sonnet-5", "claude-haiku-4-5", "a-1", "a-2", "a-3", "a-4", "a-5", "a-6", "a-7"},
      {"mistral-large", "mistral-small", "mistral-medium", "m-1", "m-2", "m-3", "m-4", "m-5", "m-6", "m-7"},
      {"glm-5.3", "glm-5.2", "glm-5.3-flash", "z-1", "z-2", "z-3", "z-4", "z-5", "z-6", "z-7"},
      {"openai/gpt-5.6", "anthropic/claude-opus-5", "zai/glm-5.3", "openai/c-1", "openai/c-2",
       "openai/c-3", "openai/c-4", "openai/c-5", "openai/c-6", "openai/c-7"}};
  uint64_t seed;
  UsageLedger L;
  UsageHistory hram;   // the history in RAM
  std::vector<TrueRec> live;
  std::string nvs, file, owner, router, histFlash;
  size_t nvsLive = 0;
  bool recreated[kProvs] = {};   // the stored counters are not the ones the file describes
  bool seen[kProvs] = {};        // ...and a load since told them apart
  bool removed = false;          // the device removed the spend file (no write tried since)
  bool stale = false;            // the file on flash lacks the latest load's result
  uint32_t lastDk = 0;           // the day of the latest count() (0 = before the clock synced)
  uint32_t skipPer[kProvs] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
  uint32_t per[kProvs] = {24300, 24300, 24300, 24300, 24300};   // each provider's period
  uint32_t day = 20650;
  int restarts = 0, drops = 0, downgrades = 0, resets = 0, oldResets = 0, firstFails = 0;
  int detected = 0, undetectable = 0;   // re-created counters at a load
  uint64_t nvsOk = 85, fileOk = 75;   // percent of writes that land, per simulated device

  explicit ClassSim(uint64_t s) : seed(s) {
    nvsOk = 50 + rnd(46);    // from a flaky settings store to a healthy one
    fileOk = 40 + rnd(56);   // the same for /data
  }
  uint64_t rnd(uint64_t mod) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (seed >> 33) % mod;
  }
  TokenDelta spend(int p, uint64_t scale) {   // cache traffic only for anthropic
    return td(rnd(scale), rnd(scale / 10), p == 1 ? rnd(scale * 3 / 10) : 0, p == 1 ? rnd(scale / 30) : 0);
  }
  // Record d for provider p in ledger R and keep its truth: counted in the period the
  // provider holds after it; a first sync carries the pre-sync records along. A record
  // before the clock syncs has period 0 and day 0 (no history bucket, no lastDay).
  // `model` null = an earlier release's provider-level record. Returns the key it filed
  // under and keeps its day in lastDk; writes its history bucket to H when given.
  std::string count(UsageLedger& R, int p, const std::string& trueModel, const TokenDelta& d,
                    const char* model, UsageHistory* H) {
    const uint32_t rp = rnd(10) ? per[p] : 0, dk = rp ? day : 0;
    const nimbus::orch::ProviderBudget* e = R.find(provs[p]);
    const uint32_t before = e ? e->periodKey : 0;
    if (model) R.recordTokens(provs[p], d, rp, "turn", model, dk);
    else R.recordTokens(provs[p], d.in, d.out, d.cacheR, d.cacheW, rp, "turn");
    e = R.find(provs[p]);
    const uint32_t after = e ? e->periodKey : 0;
    if (before == 0 && after != 0)
      for (TrueRec& r : live)
        if (r.prov == provs[p] && r.period == 0) r.period = after;
    live.push_back({provs[p], trueModel, d, after});
    const char* key = model ? R.models().spendKey(model) : "";
    if (dk && H) H->record(provs[p], key, dk, d, 0);
    lastDk = dk;
    return std::string(key);
  }
  void storeNvs() {   // a usgLedger write that lands (store.cpp persistLedgerLocked)
    nvs = L.serialize();
    nvsLive = live.size();
    L.models().setCountersBehind(false);
    L.noteCountersStored();
  }
  bool storeFile() {   // a spend-file write; true when it lands
    removed = false;   // a write was tried since
    if (rnd(100) >= fileOk) return false;
    file = L.models().serializeSpend();
    L.models().setSpendFileStored(true);
    for (int p = 0; p < kProvs; ++p) recreated[p] = seen[p] = false;   // it describes RAM
    stale = false;
    return true;
  }
  // Whether the stored counters hold spend of their period recorded after tracking
  // started (a record with a true model; zero-token records count for nothing).
  bool nvsHoldsTracked() const {
    UsageLedger R;
    R.deserialize(nvs);
    for (size_t i = 0; i < nvsLive; ++i) {
      const TrueRec& r = live[i];
      if (r.model.empty() || (!r.d.in && !r.d.out && !r.d.cacheR && !r.d.cacheW)) continue;
      const nimbus::orch::ProviderBudget* e = R.find(r.prov);
      if (e && e->periodKey == r.period) return true;
    }
    return false;
  }
  // One direction of re-created counters at or above every floor the spend file gives
  // (UsageLedger::rebaseModelRows): the totals it recorded, and its spend from before
  // its period plus their period spend.
  static bool floorsMiss(uint64_t stored, uint64_t preTot, uint64_t before, uint64_t total,
                         uint64_t period) {
    if (total < (stored > preTot ? stored : preTot)) return false;
    return total < period || total - period >= before;
  }
  // The gate M1 exclusion, read from what is stored before the load (R: the stored
  // counters with the stored spend file, not settled yet).
  void markUndetectable(const UsageLedger& R, const UsageHistory& H) {
    const bool evidence = R.earlierReleaseRecorded(&H);
    for (int p = 0; p < kProvs; ++p) {
      const nimbus::orch::ProviderBudget* e = R.find(provs[p]);
      const nimbus::orch::ProvTrack* t = R.models().track(provs[p]);
      if (!recreated[p] || !e || !t) continue;
      // Once a load told them apart, the device knows (it stores that, or writes the
      // file first): no later load is excused.
      const bool miss =
          !seen[p] && !evidence &&
          floorsMiss(t->storedTotIn, t->preTotIn, t->storedBeforeIn, e->totalIn, e->tokensIn) &&
          floorsMiss(t->storedTotOut, t->preTotOut, t->storedBeforeOut, e->totalOut, e->tokensOut);
      ++(miss ? undetectable : detected);
      if (miss) skipPer[p] = e->periodKey;
      seen[p] = seen[p] || !miss;
    }
  }
  void storeHistory() {
    if (rnd(100) < fileOk) histFlash = hram.serialize();
  }
  // The previous firmware's spend (earlier usage, stored, with its model-less
  // history), then the update boot.
  void earlier() {
    for (int p = 0; p < kProvs; ++p) {
      if (rnd(3)) L.setRates(provs[p], (uint32_t)rnd(400), (uint32_t)rnd(2000), 0);
      if (!rnd(2)) continue;
      count(L, p, "", spend(p, 3000000), nullptr, &hram);
      storeHistory();
    }
    storeNvs();
    restartStep();
  }
  // One turn: a named model, or none (filed under ~other once tracking started; its
  // true model is still one of the provider's). Each store may fail to take it, in
  // the device's order (store.cpp recordProviderTokens).
  void turnStep() {
    const int p = (int)rnd(kProvs);
    const std::string& m = models[p][rnd(10)];
    const bool named = rnd(6) != 0, tracked = L.models().tracking();
    const TokenDelta d = spend(p, 2000000);
    const std::string key =
        count(L, p, named || tracked ? m : std::string(), d, named ? m.c_str() : "", nullptr);
    const uint32_t dk = lastDk;
    const bool keyed = named || tracked;
    // While the spend file on flash lacks what the load decided (none there, or its
    // store failed), every tracked record writes it first (gate M1, M2).
    if (keyed && (file.empty() || stale)) TEST_ASSERT_TRUE(L.models().spendFileFirst());
    if (keyed && L.models().spendFileFirst()) {   // marked ahead, as the device does
      L.models().setCountersBehind(true);
      storeFile();
    }
    if (rnd(100) < nvsOk) storeNvs();
    else L.models().setCountersBehind(true);   // the write did not land
    if (keyed) storeFile();
    if (dk) hram.record(provs[p], L.models().historyKey(key.c_str()), dk, d, 0);
    storeHistory();
  }
  void priceStep() {
    const int p = (int)rnd(kProvs);
    const uint64_t kind = rnd(4);
    if (kind < 2) {
      L.models().setOwnerRate(provs[p], models[p][rnd(10)].c_str(), (uint32_t)rnd(12000000),
                              (uint32_t)rnd(60000000));
      owner = L.models().serializeOwnerRates();
    } else if (kind == 2) {
      L.setRates(provs[p], (uint32_t)rnd(400), (uint32_t)rnd(2000), 0);   // an owner save
      storeNvs();
    } else {
      std::vector<nimbus::orch::ModelRate> rr(1);
      std::strcpy(rr[0].prov, "cumulo");
      std::strcpy(rr[0].model, models[4][rnd(10)].c_str());
      rr[0].microIn = (uint32_t)rnd(15000000);
      rr[0].microOut = (uint32_t)rnd(70000000);
      L.models().replaceRouterRates(rr);
      router = L.models().serializeRouterRates();
    }
  }
  // The gate M2 exclusion: the spend file is not on flash because its writes did not
  // land (not removed by the device with no write tried since), the stored counters
  // hold tracked spend of their period, and the stored history does not show tracking.
  bool hole() const {
    if (!file.empty() || removed || !nvsHoldsTracked()) return false;
    UsageLedger R;
    R.deserialize(nvs);
    UsageHistory H;
    H.deserialize(histFlash);
    return !R.historyShowsTracking(H);
  }
  // A restart, loaded the way the device loads (store.cpp loadModelsLocked).
  void restartStep() {
    UsageLedger R;
    R.deserialize(nvs);
    R.models().deserializeSpend(file);
    R.models().deserializeOwnerRates(owner);
    R.models().deserializeRouterRates(router);
    hram = UsageHistory();
    hram.deserialize(histFlash);
    markUndetectable(R, hram);
    const UsageLedger::Settled s = R.settleModels(hram);
    // Tracking starts afresh only when nothing stored shows it started, so no stored
    // spend can be tracked spend (the exclusions keep the documented holes out).
    if (s.started) TEST_ASSERT_FALSE(nvsHoldsTracked());
    L = R;
    stale = false;
    const bool current = !s.mustStore() || storeFile();
    if (!current) {
      firstFails += file.empty() ? 1 : 0;
      if (s.removable) file.clear();   // the write failed: the device removes the file
      removed = s.removable;
      stale = !file.empty();           // the file on flash lacks this load's result
    }
    L.models().setSpendFileStored(current && !file.empty());
    L.models().setSpendFileStale(stale);
    live.resize(nvsLive);
    ++restarts;
    for (const auto& t : L.models().tracks()) drops += (t.lost.in || t.lost.out) ? 1 : 0;
  }
  // The earlier release's history: it reads our lines (the extra fields ignored),
  // merges the buckets of a day and provider, and rewrites them without models.
  static UsageHistory stripModels(const std::string& flash) {
    UsageHistory in, out;
    in.deserialize(flash);
    for (const auto& b : in.entries()) out.record(b.prov, b.dayKey, b.tokIn, b.tokOut, b.calls);
    return out;
  }
  // The stored counters are gone (a settings-only reset): the spend file on flash
  // describes counters that no longer exist, and nothing skipped is counted any more.
  void countersErased() {
    live.clear();
    nvsLive = 0;
    for (int p = 0; p < kProvs; ++p) {
      recreated[p] = true;
      seen[p] = false;
      skipPer[p] = UINT32_MAX;
    }
  }
  // A downgrade and back: the earlier release reads usgLedger (or starts empty after a
  // settings-only reset there), records a few turns provider level and stores them,
  // rewriting the history without models; the spend file stays as it was. Then update
  // again. Its spend is tracked spend (a true model) when the spend file on flash shows
  // tracking started; with none there, it is earlier usage.
  void downgradeStep() {
    if (file.empty() && nvsHoldsTracked()) return;   // the gate M2 hole: excluded
    const bool reset = rnd(10) < 3, tracked = !file.empty();
    UsageLedger old;
    if (reset) {
      countersErased();
      ++oldResets;
    } else {
      old.deserialize(nvs);
      live.resize(nvsLive);
    }
    UsageHistory oh = stripModels(histFlash);
    for (uint64_t n = 1 + rnd(3); n > 0; --n) {
      const int p = (int)rnd(kProvs);
      count(old, p, tracked ? models[p][rnd(10)] : std::string(), spend(p, 6000000), nullptr, &oh);
      if (rnd(100) < fileOk) histFlash = oh.serialize();
    }
    nvs = old.serialize();
    nvsLive = live.size();
    ++downgrades;
    restartStep();
  }
  // A settings-only reset: NVS erased (the counters and provider rates with it),
  // /data kept.
  void settingsResetStep() {
    nvs.clear();
    countersErased();
    ++resets;
    restartStep();
  }
  // Erase Storage on a device with no SD card: /data (spend file, prices, history)
  // erased, NVS kept.
  void eraseStorageStep() {
    file.clear();
    removed = false;
    owner.clear();
    router.clear();
    histFlash.clear();
    restartStep();
  }
  void rollStep() {   // some providers reach their reset day
    for (int p = 0; p < kProvs; ++p)
      if (rnd(2)) ++per[p];
    day += 30;
  }
  void step() {
    const uint64_t op = rnd(100);
    if (op < 68) {
      turnStep();
    } else if (op < 76) {
      rollStep();
    } else if (op < 82) {
      priceStep();
    } else if (op < 86) {
      downgradeStep();
    } else if (op < 88) {
      settingsResetStep();
    } else if (op < 89) {
      eraseStorageStep();
    } else if (!hole()) {
      restartStep();
    }
  }
  // Every provider: the estimate is never below the true price of what it counts.
  void check(int iter, int at) {
    for (int i = 0; i < kProvs; ++i) {
      const char* p = provs[i];
      const nimbus::orch::ProviderBudget* e = L.find(p);
      if (e && skipPer[i] == 0 && e->periodKey != 0) skipPer[i] = e->periodKey;   // carried
      if (e && e->periodKey == skipPer[i]) continue;   // the gate M1 exclusion
      const uint64_t want = trueMicro(L, live, p);
      PriceSummary s;
      const uint64_t got = L.priceProvider(p, &s);
      if (s.fastPath) TEST_ASSERT_EQUAL_UINT64(L.find(p)->estCents(), got);
      if (!belowTruth(got, want, s.fastPath)) continue;
      char msg[160];
      std::snprintf(msg, sizeof(msg), "iter %d step %d %s: est %llu < true %llu", iter, at, p,
                    (unsigned long long)got, (unsigned long long)(want / 10000));
      TEST_FAIL_MESSAGE(msg);
    }
  }
};

static void test_class_estimate_never_below_true_model_prices() {
  int restarts = 0, drops = 0, downgrades = 0, resets = 0, oldResets = 0, firstFails = 0;
  int detected = 0, undetectable = 0;
  for (int iter = 0; iter < 1000; ++iter) {
    ClassSim sim(4242 + (uint64_t)iter * 7919);
    sim.earlier();
    sim.check(iter, -1);
    for (int at = 0; at < 40; ++at) {
      sim.step();
      sim.check(iter, at);
    }
    restarts += sim.restarts;
    drops += sim.drops;
    downgrades += sim.downgrades;
    resets += sim.resets;
    oldResets += sim.oldResets;
    firstFails += sim.firstFails;
    detected += sim.detected;
    undetectable += sim.undetectable;
  }
  TEST_ASSERT_TRUE(restarts > 1000);   // the sequences really restart...
  TEST_ASSERT_TRUE(drops > 100);       // ...really hit store failures...
  TEST_ASSERT_TRUE(downgrades > 500);  // ...really go back to an earlier release...
  TEST_ASSERT_TRUE(resets > 200);      // ...really reset their settings...
  TEST_ASSERT_TRUE(oldResets > 100);   // ...also on the earlier release...
  TEST_ASSERT_TRUE(firstFails > 100);  // ...really fail the first spend-file write...
  TEST_ASSERT_TRUE(detected > 100);    // ...and really meet re-created counters, which the
  TEST_ASSERT_TRUE(undetectable * 10 < detected);   // file shows in all but a few cases
}

// Leaving the fast path never lowers the estimate: the same counters filed under a
// model row (priced at the provider's own rates) floor once instead of per term.
static void test_first_model_row_never_lowers_the_provider_formula() {
  uint64_t seed = 777;
  auto rnd = [&](uint64_t mod) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (seed >> 33) % mod;
  };
  for (int i = 0; i < 500; ++i) {
    UsageLedger L;
    L.setRates("openai", 1 + (uint32_t)rnd(5000), 1 + (uint32_t)rnd(20000), 0);
    turn(L, "openai", "x-1", td(rnd(50000000), rnd(5000000), rnd(9000000), rnd(900000)));
    PriceSummary s;
    const uint64_t got = L.priceProvider("openai", &s);
    const uint64_t legacy = L.find("openai")->estCents();
    TEST_ASSERT_FALSE(s.fastPath);
    TEST_ASSERT_TRUE(got >= legacy);
    TEST_ASSERT_TRUE(got <= legacy + 4);   // four floored terms, each under a cent
  }
}

static void test_model_rows_meter_cache_traffic() {
  UsageLedger L;
  L.models().setOwnerRate("anthropic", "claude-opus-5", 3000000, 15000000);   // $3/M in
  turn(L, "anthropic", "claude-opus-5", td(100000, 0, /*cr=*/2000000, /*cw=*/400000));
  // in 30c + reads 2M x $3 x 0.1 = 60c + writes 0.4M x $3 x 1.25 = 150c
  TEST_ASSERT_EQUAL_UINT64(30 + 60 + 150, L.estCents("anthropic"));
}

// The bit-exact migration: a provider with no model rows this period and complete
// provider in/out rates returns ProviderBudget::estCents() verbatim, across many
// ledgers (rates, spend, cache and calls), so no existing ledger moves by a cent.
static void test_fast_path_equals_provider_formula() {
  uint64_t seed = 12345;
  auto rnd = [&](uint64_t mod) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (seed >> 33) % mod;
  };
  for (int i = 0; i < 500; ++i) {
    UsageLedger L;
    L.setRates("openai", 1 + (uint32_t)rnd(5000), 1 + (uint32_t)rnd(20000), (uint32_t)rnd(3) ? (uint32_t)rnd(900) + 1 : 0);
    L.recordTokens("openai", rnd(50000000), rnd(5000000), rnd(9000000), rnd(900000), 24300, "turn");
    if (L.find("openai")->centsPerKCalls)
      for (int c = 0; c < (int)rnd(5); ++c) L.recordCall("openai", 24300);
    PriceSummary s;
    TEST_ASSERT_EQUAL_UINT64(L.find("openai")->estCents(), L.priceProvider("openai", &s));
    TEST_ASSERT_TRUE(s.fastPath);
    TEST_ASSERT_EQUAL_UINT64(s.cents, s.resCents + s.callCents);
  }
}

// The parts (model rows + residual + calls) add up EXACTLY to the estimate, for any
// mix - the Usage page lists them and they must sum to the tile.
static void test_breakdown_parts_add_up_exactly() {
  uint64_t seed = 99;
  auto rnd = [&](uint64_t mod) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (seed >> 33) % mod;
  };
  const char* models[] = {"gpt-5.6", "gpt-5.6-luna", "gpt-6-astra", "x-1", "x-2"};
  for (int i = 0; i < 300; ++i) {
    UsageLedger L;
    if (rnd(2)) L.recordTokens("openai", rnd(3000000), rnd(300000), 24300);   // residual
    for (int k = 0; k < 5; ++k)
      if (rnd(2)) turn(L, "openai", models[k], rnd(4000000), rnd(400000));
    // Spend no row accounts for after tracking started (its model was lost).
    if (rnd(2)) L.recordTokens("openai", rnd(1000000), rnd(100000), 24300);
    if (rnd(2)) L.models().setOwnerRate("openai", "x-1", (uint32_t)rnd(9000000) + 1, (uint32_t)rnd(9000000));
    PriceSummary s;
    RowPrice rows[ModelLedger::kMaxRows];
    const uint64_t total = L.priceProvider("openai", &s, rows);
    uint64_t sum = s.resCents + s.othCents + s.callCents;
    for (size_t r = 0; r < ModelLedger::kMaxRows; ++r) sum += rows[r].cents;
    TEST_ASSERT_EQUAL_UINT64(total, sum);
    TEST_ASSERT_EQUAL_UINT64(total, L.estCents("openai"));
  }
}

// D1 arming + the edges: estimates arm the cap; "none" never trips; a saturated
// estimate trips; the dated un-gate still opens a new period.
static void test_d1_arming_none_saturation_and_dated_ungate() {
  UsageLedger L;
  L.setLimits("tavily", 0, 0, 1, 800);           // $8
  for (int i = 0; i < 999; ++i) L.recordCall("tavily", 24300);
  TEST_ASSERT_FALSE(L.overCostBudget("tavily"));  // 999 x ~$8/1000 = 799c
  L.recordCall("tavily", 24300);
  TEST_ASSERT_EQUAL_UINT64(800, L.estCents("tavily"));
  TEST_ASSERT_TRUE(L.overCostBudget("tavily"));   // armed by the built-in call price
  TEST_ASSERT_TRUE(L.anyPriced("tavily"));
  // Nothing priced: never trips, however large the spend.
  UsageLedger N;
  N.setLimits("custom", 0, 0, 1, 1);
  turn(N, "custom", "llama3.1:8b", 900000000, 900000000);
  TEST_ASSERT_FALSE(N.anyPriced("custom"));
  TEST_ASSERT_EQUAL_UINT64(0, N.estCents("custom"));
  TEST_ASSERT_FALSE(N.overCostBudget("custom"));
  // ...until the owner prices the model.
  N.models().setOwnerRate("custom", "llama3.1:8b", 100000, 0);   // $0.10/M in: $90
  TEST_ASSERT_EQUAL_UINT64(9000, N.estCents("custom"));
  TEST_ASSERT_TRUE(N.overCostBudget("custom"));
  // Saturation trips (never wraps to a small number), whatever the cap.
  UsageLedger S;
  S.setLimits("openai", 0, 0, 1, UINT64_MAX - 1);
  S.models().setOwnerRate("openai", "gpt-5.6", UINT32_MAX, UINT32_MAX);
  turn(S, "openai", "gpt-5.6", UINT64_MAX / 2, UINT64_MAX / 2);
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, S.estCents("openai"));
  TEST_ASSERT_TRUE(S.overCostBudget("openai"));
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, nimbus::orch::costMicro(UINT64_MAX, UINT64_MAX));
  TEST_ASSERT_EQUAL_UINT64(1250000, nimbus::orch::costMicro(500000, 2500000));
  TEST_ASSERT_EQUAL_UINT64(0, nimbus::orch::costMicro(1, 999999));   // floored
  // The dated un-gate: over in August, a new period reads under budget.
  UsageLedger D;
  D.setLimits("openai", 0, 0, 1, 100);
  turn(D, "openai", "gpt-5.6", 1000000, 0, usagePeriodKey(2026, 8, 15, 1));
  TEST_ASSERT_TRUE(D.overBudget("openai", 2026, 8, 20));
  TEST_ASSERT_FALSE(D.overBudget("openai", 2026, 9, 2));
}

// usgLedger (the NVS blob) is byte-identical with or without model activity: the
// model ledger never enters serialize(), and deserialize() never touches it.
static void test_usg_ledger_blob_byte_identical_after_model_activity() {
  UsageLedger A, B;
  A.setLimits("openai", 1000, 0, 5, 700);
  B.setLimits("openai", 1000, 0, 5, 700);
  A.recordTokens("openai", 1200, 300, 7, 9, 24300, "turn");
  B.recordTokens("openai", td(1200, 300, 7, 9), 24300, "turn", "gpt-5.6", 20650);
  B.models().setOwnerRate("openai", "gpt-5.6", 1, 2);
  TEST_ASSERT_EQUAL_STRING(A.serialize().c_str(), B.serialize().c_str());
  B.deserialize(A.serialize());
  TEST_ASSERT_EQUAL(1, (int)B.models().rows().size());    // untouched by the NVS load
  TEST_ASSERT_EQUAL(1, (int)B.models().ownerRates().size());
}

// The frozen pre-2026-10 history parser (a verbatim copy of the old code): older
// firmware reading a NEW history line must still see the right calls.
static void frozenLegacyHistoryParse(const std::string& s, std::vector<DayUsage>& out) {
  out.clear();
  auto splitBars = [](const std::string& group, std::string* parts, int maxParts) {
    int np = 0;
    size_t p = 0;
    while (np < maxParts) {
      size_t bar = group.find('|', p);
      if (bar == std::string::npos) { parts[np++] = group.substr(p); break; }
      parts[np++] = group.substr(p, bar - p);
      p = bar + 1;
    }
    return np;
  };
  size_t i = 0, n = s.size();
  while (i < n) {
    size_t semi = s.find(';', i);
    if (semi == std::string::npos) semi = n;
    std::string group = s.substr(i, semi - i);
    i = semi + 1;
    if (group.empty()) continue;
    std::string parts[5];
    int np = splitBars(group, parts, 5);
    if (np < 5 || parts[1].empty()) continue;
    if (out.size() >= 720) break;
    DayUsage d;
    d.dayKey = (uint32_t)strtoul(parts[0].c_str(), nullptr, 10);
    d.prov   = parts[1];
    d.tokIn  = (uint64_t)strtoull(parts[2].c_str(), nullptr, 10);
    d.tokOut = (uint64_t)strtoull(parts[3].c_str(), nullptr, 10);
    d.calls  = (uint32_t)strtoul(parts[4].c_str(), nullptr, 10);
    if (d.dayKey == 0) continue;
    out.push_back(std::move(d));
  }
}

static void test_history_8_fields_round_trip_and_downgrade_safe() {
  UsageHistory H;
  H.record("anthropic", "claude-opus-5", 20650, td(7000, 300, 2000, 400), 0);
  H.record("anthropic", "claude-haiku-4-5", 20650, td(100, 10), 0);
  H.record("anthropic", "", 20650, td(5, 5), 0);                  // unattributed
  H.record("tavily", 20650, 0, 0, 3);                             // legacy form
  H.record("openai", "gpt-5.6", 20651, td(1, 1, 7), 2);           // cacheR only
  TEST_ASSERT_EQUAL(5, (int)H.entries().size());
  const std::string blob = H.serialize();
  TEST_ASSERT_TRUE(blob.find("20650|anthropic|7000|300|0|claude-opus-5|2000|400") != std::string::npos);
  TEST_ASSERT_TRUE(blob.find("20650|anthropic|5|5|0;") != std::string::npos);   // tail omitted
  TEST_ASSERT_TRUE(blob.find("20651|openai|1|1|2|gpt-5.6|7") != std::string::npos);
  UsageHistory R;
  R.deserialize(blob);
  TEST_ASSERT_EQUAL_STRING(blob.c_str(), R.serialize().c_str());
  TEST_ASSERT_EQUAL_STRING("claude-opus-5", R.entries()[0].model);
  TEST_ASSERT_EQUAL_UINT64(400, R.entries()[0].cacheW);
  // Older firmware: every bucket parses, and the calls survive the extra fields.
  std::vector<DayUsage> legacy;
  frozenLegacyHistoryParse(blob, legacy);
  TEST_ASSERT_EQUAL(5, (int)legacy.size());
  TEST_ASSERT_EQUAL_UINT32(0, legacy[0].calls);
  TEST_ASSERT_EQUAL_UINT32(3, legacy[3].calls);
  TEST_ASSERT_EQUAL_UINT32(2, legacy[4].calls);
  TEST_ASSERT_EQUAL_UINT64(7000, legacy[0].tokIn);
  // A pre-upgrade 5-field file still loads (model "", no cache).
  UsageHistory P;
  P.deserialize("20650|anthropic|10|2|1;20651|openai|5|5|0");
  TEST_ASSERT_EQUAL(2, (int)P.entries().size());
  TEST_ASSERT_EQUAL_STRING("", P.entries()[0].model);
  // An unsafe model on disk loads unattributed.
  P.deserialize("20650|anthropic|10|2|1|bad model|0|0");
  TEST_ASSERT_EQUAL_STRING("", P.entries()[0].model);
}

// The per-day cap: at most 12 (prov, model) buckets a day, the 13th folding into
// ~other with the day's provider totals intact, so 60 days of history always fit
// the 720 cap (the oldest day goes first, never a recent one).
static void test_history_per_day_cap_holds_60_day_retention() {
  UsageHistory H;
  const char* provs[] = {"openai", "anthropic", "cumulo"};
  const uint32_t today = 20700;
  uint64_t perDayProv[61][3] = {};
  for (uint32_t d = 0; d <= 60; ++d) {
    for (int m = 0; m < 20; ++m) {
      const int p = m % 3;
      const std::string model = "model-" + std::to_string(m) + "x";
      H.record(provs[p], model.c_str(), today - 60 + d, td(10 + m, 1), 0);
      perDayProv[d][p] += 10 + m + 1;
    }
    H.prune(today - 60 + d, 60);
  }
  TEST_ASSERT_TRUE(H.entries().size() <= UsageHistory::kMaxEntries);
  for (uint32_t d = 1; d <= 60; ++d) {   // the last 60 days are all intact
    size_t n = 0;
    uint64_t tot[3] = {};
    for (const auto& e : H.entries()) {
      if (e.dayKey != today - 60 + d) continue;
      ++n;
      for (int p = 0; p < 3; ++p)
        if (e.prov == provs[p]) tot[p] += e.tokIn + e.tokOut;
    }
    TEST_ASSERT_EQUAL(UsageHistory::kMaxPerDay, n);
    for (int p = 0; p < 3; ++p) TEST_ASSERT_EQUAL_UINT64(perDayProv[d][p], tot[p]);
  }
  // A full day folds a new provider in without losing its spend.
  UsageHistory F;
  for (int m = 0; m < 12; ++m) {
    const std::string model = "m" + std::to_string(m) + "x";
    F.record("openai", model.c_str(), 20000, td(5), 0);
  }
  F.record("zai", "glm-5.3", 20000, td(7), 0);
  TEST_ASSERT_EQUAL(12, (int)F.entries().size());
  uint64_t oai = 0, zai = 0;
  for (const auto& e : F.entries()) (e.prov == "zai" ? zai : oai) += e.tokIn;
  TEST_ASSERT_EQUAL_UINT64(60, oai);
  TEST_ASSERT_EQUAL_UINT64(7, zai);
}

// The widest history bucket fits its JSON bound (the PSRAM buffer is sized from it).
static void test_history_json_worst_case_fits() {
  DayUsage e;
  e.dayKey = UINT32_MAX;
  e.prov = "abcdefghijklmno";                 // 15 chars (a provider id max)
  e.tokIn = e.tokOut = e.cacheR = e.cacheW = UINT64_MAX;
  e.calls = UINT32_MAX;
  std::memset(e.model, 'm', nimbus::orch::kModelKeyCap - 1);
  e.model[nimbus::orch::kModelKeyCap - 1] = 0;
  char buf[nimbus::orch::kHistoryJsonEntryMax];
  const size_t n = nimbus::orch::historyEntryJson(e, false, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE(n < sizeof(buf));
  TEST_ASSERT_EQUAL_CHAR(',', buf[0]);
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
  // Too small a buffer is refused, never truncated mid-object.
  char tiny[64];
  TEST_ASSERT_EQUAL(0, (int)nimbus::orch::historyEntryJson(e, true, tiny, sizeof(tiny)));
  DayUsage plain;
  plain.dayKey = 5;
  plain.prov = "openai";
  TEST_ASSERT_TRUE(nimbus::orch::historyEntryJson(plain, true, buf, sizeof(buf)) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"d\":5,\"prov\":\"openai\",\"in\":0,\"out\":0,\"calls\":0}", buf);
}

// ---- GET /api/usage/models (the JSON body) -----------------------------------------

using nimbus::orch::EditableModels;

static std::string keyOf(char c, int i) {   // a 47-char model key
  std::string k = std::to_string(i) + "-";
  k += std::string(nimbus::orch::kModelKeyCap - 1 - k.size(), c);
  return k;
}

// The worst case fits the buffer the device sizes from the counts, and parses: every
// row taken, 47-char keys, 15-char providers, the full editable lists, every
// counter saturated.
static void test_usage_models_json_worst_case_fits_and_parses() {
  UsageLedger L;
  const char* provs[] = {"openai", "anthropic", "mistral", "abcdefghijklmno"};
  for (int p = 0; p < 4; ++p) {
    L.setRates(provs[p], UINT32_MAX, UINT32_MAX, UINT32_MAX);
    for (int i = 0; i < 8; ++i)
      turn(L, provs[p], keyOf('a' + (char)p, i).c_str(),
           td(UINT64_MAX / 3, UINT64_MAX / 3, UINT64_MAX / 3, UINT64_MAX / 3));
    L.recordCall(provs[p], 24300);
  }
  for (int i = 0; i < 32; ++i)
    L.models().setOwnerRate("openai", keyOf('z', i).c_str(), UINT32_MAX, UINT32_MAX);
  std::vector<EditableModels> ed;
  for (const char* p : {"cumulo", "openai", "anthropic", "mistral", "zai", "custom"}) {
    EditableModels e;
    e.prov = p;
    for (int i = 0; i < (int)nimbus::orch::kMaxEditable; ++i) e.csv += (i ? "," : "") + keyOf('e', i);
    ed.push_back(e);
  }
  TEST_ASSERT_EQUAL(ModelLedger::kMaxRows, L.models().rows().size());
  const size_t cap = nimbus::orch::usageModelsJsonCap(L, ed);
  std::vector<char> buf(cap);
  const size_t n = nimbus::orch::writeUsageModelsJson(L, ed, buf.data(), cap);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE(n < cap);
  JsonDocument d;
  TEST_ASSERT_TRUE(deserializeJson(d, buf.data(), n) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL(32, (int)d["models"].as<JsonArrayConst>().size());
  TEST_ASSERT_EQUAL((int)nimbus::orch::kMaxEditable, (int)d["editable"]["openai"].as<JsonArrayConst>().size());
  // Too small a buffer is refused, never a truncated document.
  std::vector<char> small(cap / 4);
  TEST_ASSERT_EQUAL(0, (int)nimbus::orch::writeUsageModelsJson(L, ed, small.data(), small.size()));
}

// The body for this ledger and these editable lists, parsed.
static void usageModelsDoc(const UsageLedger& L, const std::vector<EditableModels>& ed,
                           JsonDocument& d) {
  const size_t cap = nimbus::orch::usageModelsJsonCap(L, ed);
  std::vector<char> buf(cap);
  const size_t n = nimbus::orch::writeUsageModelsJson(L, ed, buf.data(), cap);
  TEST_ASSERT_TRUE(deserializeJson(d, buf.data(), n) == DeserializationError::Ok);
}

// One provider's parts in the body: its models' est + its residual est + callEst.
// `others` counts its "~other" entries; `other` receives the last one.
static uint64_t partsOf(JsonDocument& d, const char* p, int* others = nullptr,
                        JsonObjectConst* other = nullptr) {
  uint64_t sum = d["provRate"][p]["callEst"] | 0ULL;
  for (JsonObjectConst r : d["residual"].as<JsonArrayConst>())
    if (std::strcmp(r["prov"] | "", p) == 0) sum += r["est"] | 0ULL;
  for (JsonObjectConst m : d["models"].as<JsonArrayConst>()) {
    if (std::strcmp(m["prov"] | "", p) != 0) continue;
    sum += m["est"] | 0ULL;
    if (std::strcmp(m["m"] | "", "~other") != 0) continue;
    if (others) ++*others;
    if (other) *other = m;
  }
  return sum;
}

// One month of spend for the body tests: earlier usage and two models for anthropic,
// an owner-priced model for openai, Tavily searches.
static void bodyLedger(UsageLedger& L, std::vector<EditableModels>& ed) {
  L.recordTokens("anthropic", 3000000, 100000, 24300);                    // pre-upgrade spend
  turn(L, "anthropic", "claude-opus-5", 1000000, 200000);
  turn(L, "anthropic", "claude-haiku-4-5", 5000000, 100000);
  L.models().setOwnerRate("openai", "gpt-5.6-luna", 210000, 0);
  turn(L, "openai", "gpt-5.6-luna", 7000000, 900000);
  for (int i = 0; i < 37; ++i) L.recordCall("tavily", 24300);
  EditableModels e;
  e.prov = "openai";
  e.csv = "gpt-5.6,gpt-5.6-luna";
  ed.push_back(e);
}

// The page's parts add up to the provider tile: per provider, the models' est + the
// residual est + callEst == provRate.est == UsageLedger::estCents (byProvider.estCents).
static void test_usage_models_json_parts_add_up_to_the_estimate() {
  UsageLedger L;
  std::vector<EditableModels> ed;
  bodyLedger(L, ed);
  JsonDocument d;
  usageModelsDoc(L, ed, d);
  TEST_ASSERT_EQUAL_STRING("2026-10", d["asOf"] | "");
  for (const char* p : {"anthropic", "openai", "tavily"}) {
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(L.estCents(p), d["provRate"][p]["est"] | 0ULL, p);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(L.estCents(p), partsOf(d, p), p);
    // One month, no cache traffic: the all-time estimate is this period's.
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(L.estCents(p), d["provRate"][p]["allEst"] | 0ULL, p);
    // G4: every provider publishes its fold price (otherRate) beside the residual's.
    const EffRate o = L.otherRate(p);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(o.in, d["provRate"][p]["oIn"] | 0ULL, p);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(o.out, d["provRate"][p]["oOut"] | 0ULL, p);
  }
  TEST_ASSERT_EQUAL_UINT64(10000000, d["provRate"]["openai"]["oIn"] | 0ULL);
  TEST_ASSERT_EQUAL_STRING("est", d["provRate"]["openai"]["oSrc"] | "");
  TEST_ASSERT_EQUAL_UINT64(210000, d["provRate"]["openai"]["rIn"] | 0ULL);   // residual: owner's
  // The editable rows carry the resolved price, its source and the owner's own.
  JsonArrayConst oa = d["editable"]["openai"].as<JsonArrayConst>();
  TEST_ASSERT_EQUAL_STRING("est", oa[0]["src"] | "");
  // Input priced by the owner, output still the built-in estimate: the badge is the
  // weaker of the two, so the row says "estimate" until both are the owner's.
  TEST_ASSERT_EQUAL_STRING("est", oa[1]["src"] | "");
  TEST_ASSERT_EQUAL_UINT32(210000, oa[1]["oIn"] | 0u);
  TEST_ASSERT_EQUAL_UINT32(210000, oa[1]["rIn"] | 0u);
  TEST_ASSERT_EQUAL_UINT32(1200000, oa[1]["rOut"] | 0u);
  TEST_ASSERT_TRUE(d["provRate"]["anthropic"]["approx"] | false);
  TEST_ASSERT_EQUAL_STRING("est", d["provRate"]["tavily"]["srcCall"] | "");
}

// Spend whose model was lost (here: provider-level spend after tracking started) is
// shown in the provider's "~other" entry at the fold price - made when the provider
// has no ~other row, merged into it when it has one - so the parts still add up, and
// the all-time estimate adds it at the same price.
static void test_usage_models_json_shows_lost_spend_at_the_fold_price() {
  UsageLedger L;
  std::vector<EditableModels> ed;
  bodyLedger(L, ed);
  JsonDocument d;
  usageModelsDoc(L, ed, d);
  for (int i = 0; i < 9; ++i) turn(L, "mistral", mk(i).c_str(), 1000000, 0);   // the 9th: ~other
  L.recordTokens("mistral", 500000, 0, 24300);
  L.recordTokens("openai", 1000000, 0, 24301);   // a month later
  JsonDocument d2;
  usageModelsDoc(L, ed, d2);
  // openai: 1M lost input tokens at its fold price (gpt-6-astra's ~$10/M), as a made
  // "~other" entry; the period is just that, and the all-time estimate adds it.
  TEST_ASSERT_EQUAL_UINT64(1000, L.estCents("openai"));
  int others = 0;
  JsonObjectConst o;
  TEST_ASSERT_EQUAL_UINT64(1000, partsOf(d2, "openai", &others, &o));
  TEST_ASSERT_EQUAL(1, others);
  TEST_ASSERT_EQUAL_UINT64(1000000, o["pIn"] | 0ULL);
  TEST_ASSERT_EQUAL_UINT64(1000, o["est"] | 0ULL);
  TEST_ASSERT_EQUAL_UINT64(10000000, o["rIn"] | 0ULL);
  TEST_ASSERT_EQUAL_UINT64((d["provRate"]["openai"]["allEst"] | 0ULL) + 1000,
                           d2["provRate"]["openai"]["allEst"] | 0ULL);
  // mistral: one ~other entry holds the 9th model and the lost spend.
  others = 0;
  TEST_ASSERT_EQUAL_UINT64(1900, partsOf(d2, "mistral", &others, &o));   // 9.5M at ~$2/M
  TEST_ASSERT_EQUAL_UINT64(1900, L.estCents("mistral"));
  TEST_ASSERT_EQUAL(1, others);
  TEST_ASSERT_EQUAL_UINT64(1500000, o["pIn"] | 0ULL);
  TEST_ASSERT_EQUAL_UINT64(300, o["est"] | 0ULL);
}


// ---- owner ruling R4: the cache calculation and price rows (lane T) ---------------

using nimbus::orch::SpendSlot;
using nimbus::orch::TurnSpend;
using nimbus::orch::ledgerDelta;

static SpendSlot slotOf(const char* host, uint32_t in, uint32_t out, uint32_t cr, uint32_t cw) {
  SpendSlot s;
  std::snprintf(s.host, sizeof(s.host), "%s", host);
  s.in = in;
  s.out = out;
  s.cacheR = cr;
  s.cacheW = cw;
  s.billed = 1;
  return s;
}

// The fields of one provider group in a serialized blob ("" when absent).
static std::vector<std::string> nvsGroup(const std::string& blob, const char* prov) {
  std::vector<std::string> f;
  size_t p = 0;
  while (p <= blob.size()) {
    size_t semi = blob.find(';', p);
    if (semi == std::string::npos) semi = blob.size();
    const std::string g = blob.substr(p, semi - p);
    if (g.compare(0, std::strlen(prov) + 1, std::string(prov) + "|") == 0) {
      size_t q = 0;
      while (q <= g.size()) {
        size_t bar = g.find('|', q);
        if (bar == std::string::npos) bar = g.size();
        f.push_back(g.substr(q, bar - q));
        q = bar + 1;
      }
      return f;
    }
    p = semi + 1;
  }
  return f;
}

// OpenAI's input_tokens INCLUDES its cached and cache-write tokens: the ledger files
// (in - cached - written, cached, written) and prices each at the row's own rate.
// gpt-5.6-sol: $5 in, $30 out, cache read $0.50, write $6.25.
static void test_r4_openai_cache_split_priced_at_row_rates() {
  const TokenDelta d = ledgerDelta(slotOf("openai", 3000000, 100000, 2000000, 400000));
  TEST_ASSERT_EQUAL_UINT64(600000, d.in);
  TEST_ASSERT_EQUAL_UINT64(2000000, d.cacheR);
  TEST_ASSERT_EQUAL_UINT64(400000, d.cacheW);
  UsageLedger L;
  turn(L, "openai", "gpt-5.6-sol", d);
  // 0.6M x $5 = $3.00, 0.1M x $30 = $3.00, 2M x $0.50 = $1.00, 0.4M x $6.25 = $2.50.
  TEST_ASSERT_EQUAL_UINT64(950, L.estCents("openai"));
  // Counter-test: the pre-R4 record (cache billed as input) costs $18.00.
  UsageLedger Old;
  turn(Old, "openai", "gpt-5.6-sol", td(3000000, 100000));
  TEST_ASSERT_EQUAL_UINT64(1800, Old.estCents("openai"));
  // The budget-limit counter keeps the native count (cached tokens still count).
  TEST_ASSERT_EQUAL_UINT64(3100000, L.find("openai")->tokens);
  TEST_ASSERT_EQUAL_UINT64(600000, L.find("openai")->tokensIn);
  // A row with no cache rate of its own uses 0.1x / 1.25x of its input rate.
  UsageLedger T;
  turn(T, "openai", "gpt-5.6-terra", ledgerDelta(slotOf("openai", 3000000, 0, 2000000, 400000)));
  // 0.6M x $2 + 2M x $0.20 + 0.4M x $2.50 = 1.20 + 0.40 + 1.00
  TEST_ASSERT_EQUAL_UINT64(260, T.estCents("openai"));
}

// claude-haiku-5-5: $0.10 / $0.50, cache read $0.01, 5-minute write $0.125.
static void test_r4_haiku_5_5_row_and_cache_rates() {
  UsageLedger L;
  turn(L, "anthropic", "claude-haiku-5-5",
       ledgerDelta(slotOf("anthropic", 10000000, 1000000, 20000000, 4000000)));
  // 10M x 0.10 + 1M x 0.50 + 20M x 0.01 + 4M x 0.125 = 1.00 + 0.50 + 0.20 + 0.50
  TEST_ASSERT_EQUAL_UINT64(220, L.estCents("anthropic"));
  const EffRate r = L.effectiveRate("anthropic", "claude-haiku-5-5");
  TEST_ASSERT_EQUAL_UINT64(10000, r.cacheR);
  TEST_ASSERT_EQUAL_UINT64(125000, r.cacheW);
  // An owner input price drops the row's cache rates (they belong to its input price).
  L.models().setOwnerRate("anthropic", "claude-haiku-5-5", 1000000, 0);
  TEST_ASSERT_EQUAL_UINT64(0, L.effectiveRate("anthropic", "claude-haiku-5-5").cacheR);
  // The price rows the program runs on (TASK R4), each its own row.
  struct Row { const char* prov; const char* m; uint64_t in, out; } rows[] = {
      {"anthropic", "claude-opus-5-5", 4000000, 20000000},
      {"anthropic", "claude-sonnet-5-5", 2000000, 10000000},
      {"anthropic", "claude-fable-5-1", 10000000, 50000000},
      {"openai", "gpt-5.6-sol", 5000000, 30000000},
      {"openai", "gpt-5.6", 5000000, 30000000},
      {"openai", "gpt-6-luna", 100000, 500000},
      {"openai", "gpt-6-sol", 2000000, 10000000},
      {"openai", "gpt-6.1-sol", 2000000, 10000000},
      {"mistral", "mistral-small-latest", 200000, 600000},
  };
  for (const Row& x : rows) {
    const EffRate e = UsageLedger().effectiveRate(x.prov, x.m);
    TEST_ASSERT_EQUAL((int)RateSrc::BuiltinModel, (int)e.srcIn);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(x.in, e.in, x.m);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(x.out, e.out, x.m);
  }
  // gpt-5.6 is labelled with its promotion; the price stays the regular one.
  const nimbus::orch::BuiltinPrice* sol = nimbus::orch::builtinModelPrice("openai", "gpt-5.6-sol");
  TEST_ASSERT_NOT_NULL(sol);
  TEST_ASSERT_NOT_NULL(sol->note);
  TEST_ASSERT_NOT_NULL(std::strstr(sol->note, "promo $4 / $20"));
}

// A request whose prompt is over claude-haiku-5-5's 100K tier bills at $0.50 / $2.50:
// its spend files under the tier's own row.
static void test_r4_haiku_long_prompt_tier() {
  nimbus::orch::TokenUsage big;
  big.promptTokens = 1000;
  big.cacheReadTokens = 120000;
  big.completionTokens = 1000000;
  big.responses = 1;
  big.maxRequestIn = 121000;
  TurnSpend sp;
  sp.add("anthropic", "claude-haiku-5-5", big);
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5:long", sp.slot[0].model);
  UsageLedger L;
  turn(L, "anthropic", sp.slot[0].model, ledgerDelta(sp.slot[0]));
  // 1000 x 0.50 + 1M x 2.50 + 120K x 0.05 (0.1x of 0.50) = 0.0005 + 2.50 + 0.006
  TEST_ASSERT_EQUAL_UINT64(250, L.estCents("anthropic"));
  nimbus::orch::TokenUsage small = big;
  small.maxRequestIn = 100000;   // AT the tier is still the base price
  TurnSpend sp2;
  sp2.add("anthropic", "claude-haiku-5-5", small);
  TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", sp2.slot[0].model);
}

// The unknown-id price is never below the provider's top built-in price, for every
// slot and any id with no row (and a Cumulo key reads its upstream's top price).
static void test_r4_unknown_id_never_below_top_builtin_price() {
  const char* slots[] = {"openai", "anthropic", "mistral", "zai"};
  const char* ids[] = {"zz-new-1", "gpt-9-nova", "claude-quill-6", "mistral-tiny-3", "glm-7"};
  for (const char* slot : slots) {
    uint32_t hiIn = 0, hiOut = 0;
    nimbus::orch::builtinHighestPrice(slot, hiIn, hiOut);
    TEST_ASSERT_TRUE(hiIn > 0 && hiOut > 0);
    for (const char* id : ids) {
      if (nimbus::orch::builtinModelPrice(slot, id)) continue;
      const EffRate r = UsageLedger().effectiveRate(slot, id);
      TEST_ASSERT_TRUE_MESSAGE(r.in >= hiIn && r.out >= hiOut, id);
      TEST_ASSERT_TRUE_MESSAGE(r.approx(), id);   // shown as an estimate
    }
  }
  uint32_t aIn = 0, aOut = 0;
  nimbus::orch::builtinHighestPrice("anthropic", aIn, aOut);
  const EffRate c = UsageLedger().effectiveRate("cumulo", "anthropic/claude-quill-6");
  TEST_ASSERT_TRUE(c.in >= aIn && c.out >= aOut);
}

// Critique D2: OpenAI's cache split never reaches the usgLedger NVS blob while it lives
// in NVS. The blob is byte-identical to the pre-R4 record of the same usage (cache folded
// into the input, cache fields 0); the load puts the split back from the model rows.
static void test_r4_openai_cache_fields_stay_zero_in_nvs_and_reload() {
  UsageLedger L;
  turn(L, "openai", "gpt-5.6-sol", ledgerDelta(slotOf("openai", 3000000, 100000, 2000000, 400000)));
  turn(L, "anthropic", "claude-haiku-5-5",
       ledgerDelta(slotOf("anthropic", 1000, 100, 9743, 9703)));
  UsageLedger Old;   // what v4.5.13 recorded for the same responses
  turn(Old, "openai", "gpt-5.6-sol", td(3000000, 100000));
  turn(Old, "anthropic", "claude-haiku-5-5", td(1000, 100, 9743, 9703));
  const std::string nvs = L.serialize();
  TEST_ASSERT_EQUAL_STRING(Old.serialize().c_str(), nvs.c_str());
  const std::vector<std::string> g = nvsGroup(nvs, "openai");
  TEST_ASSERT_EQUAL(18, (int)g.size());
  TEST_ASSERT_EQUAL_STRING("0", g[16].c_str());
  TEST_ASSERT_EQUAL_STRING("0", g[17].c_str());
  // Anthropic's own split still rides the blob (it always has).
  TEST_ASSERT_EQUAL_STRING("9743", nvsGroup(nvs, "anthropic")[16].c_str());
  // Restart: the split comes back from the rows, the estimate is unchanged, nothing lost.
  UsageHistory h;
  h.record("openai", "gpt-5.6-sol", 20650, td(600000, 100000, 2000000, 400000), 0);
  h.record("anthropic", "claude-haiku-5-5", 20650, td(1000, 100, 9743, 9703), 0);
  UsageLedger R = load(nvs, L.models().serializeSpend(), "", h);
  TEST_ASSERT_EQUAL_UINT64(600000, R.find("openai")->tokensIn);
  TEST_ASSERT_EQUAL_UINT64(2000000, R.find("openai")->cacheRead);
  TEST_ASSERT_EQUAL_UINT64(400000, R.find("openai")->cacheWrite);
  TEST_ASSERT_EQUAL_UINT64(L.estCents("openai"), R.estCents("openai"));
  TEST_ASSERT_EQUAL_UINT64(L.estCents("anthropic"), R.estCents("anthropic"));
  PriceSummary s;
  R.priceProvider("openai", &s);
  TEST_ASSERT_EQUAL_UINT64(0, s.othIn + s.othCr + s.othCw);
  // And again (the reloaded ledger stores and reloads the same).
  TEST_ASSERT_EQUAL_STRING(nvs.c_str(), R.serialize().c_str());
  UsageLedger R2 = load(R.serialize(), R.models().serializeSpend(), "", h);
  TEST_ASSERT_EQUAL_UINT64(L.estCents("openai"), R2.estCents("openai"));
}

// Fresh device (absent NVS, no spend file, no history - the first boot after an update
// the owner QAs): a switched turn files each host under its own model, the stored blob
// holds OpenAI's cache folded (fields 0) and grows no tag row, and a restart restores
// the split from the rows with the same estimate.
static void test_r4_fresh_device_switched_turn() {
  UsageLedger L = load("", "", "", UsageHistory());
  L.setAllowNewTags(false);   // what store.cpp does at load while the ledger is in NVS
  const TokenDelta ant = ledgerDelta(slotOf("anthropic", 696, 119, 0, 9717));
  const TokenDelta oai = ledgerDelta(slotOf("openai", 17717, 79, 8816, 0));
  turn(L, "anthropic", "claude-haiku-5-5", ant);
  turn(L, "openai", "gpt-5.6-luna", oai);
  TEST_ASSERT_EQUAL_UINT64(9717, L.find("anthropic")->cacheWrite);
  TEST_ASSERT_EQUAL_UINT64(17717 - 8816, L.find("openai")->tokensIn);
  TEST_ASSERT_EQUAL_UINT64(8816, L.find("openai")->cacheRead);
  TEST_ASSERT_EQUAL(0, (int)L.tagEntries().size());
  const std::string nvs = L.serialize();
  const std::vector<std::string> g = nvsGroup(nvs, "openai");
  TEST_ASSERT_EQUAL(18, (int)g.size());
  TEST_ASSERT_EQUAL_STRING("17717", g[7].c_str());   // input in its native, cache-included form
  TEST_ASSERT_EQUAL_STRING("0", g[16].c_str());
  TEST_ASSERT_EQUAL_STRING("0", g[17].c_str());
  UsageHistory h;
  h.record("anthropic", "claude-haiku-5-5", 20650, ant, 0);
  h.record("openai", "gpt-5.6-luna", 20650, oai, 0);
  UsageLedger R = load(nvs, L.models().serializeSpend(), "", h);
  TEST_ASSERT_EQUAL_UINT64(8816, R.find("openai")->cacheRead);
  TEST_ASSERT_EQUAL_UINT64(L.estCents("openai"), R.estCents("openai"));
  TEST_ASSERT_EQUAL_UINT64(L.estCents("anthropic"), R.estCents("anthropic"));
}

// The split cannot be put back when the rows do not hold it (a spend file that did not
// land): the folded form stays, its cache billed at the input rate - never lower.
static void test_r4_resplit_fails_closed_without_rows() {
  UsageLedger L;
  turn(L, "openai", "gpt-5.6-sol", ledgerDelta(slotOf("openai", 3000000, 100000, 2000000, 400000)));
  UsageHistory h;
  h.record("openai", "gpt-5.6-sol", 20650, td(600000, 100000, 2000000, 400000), 0);
  UsageLedger R = load(L.serialize(), "", "", h);
  TEST_ASSERT_EQUAL_UINT64(0, R.find("openai")->cacheRead);
  TEST_ASSERT_TRUE(R.estCents("openai") >= L.estCents("openai"));
}

// While the ledger lives in NVS, per-host filing must not create (provider, tag) rows a
// single-host turn never did: spend is counted, the audit row is not made.
static void test_r4_no_new_tag_rows_while_in_nvs() {
  UsageLedger L;
  L.recordTokens("openai", td(100, 10), 24300, "loop:lp1", "gpt-5.6", 20650);
  L.setAllowNewTags(false);
  L.recordTokens("openai", td(100, 10), 24300, "loop:lp1", "gpt-5.6", 20650);   // existing pair
  L.recordTokens("anthropic", td(500, 50), 24300, "loop:lp1", "claude-haiku-5-5", 20650);
  TEST_ASSERT_EQUAL(1, (int)L.tagEntries().size());
  TEST_ASSERT_EQUAL_UINT64(200, L.tagEntries()[0].tokIn);
  TEST_ASSERT_EQUAL_UINT64(500, L.find("anthropic")->tokensIn);
  TEST_ASSERT_NOT_NULL(L.models().findRow("anthropic", "claude-haiku-5-5"));
  TEST_ASSERT_NULL(std::strstr(L.serialize().c_str(), "#anthropic|loop:lp1"));
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
  RUN_TEST(test_owner_case_two_models_one_provider_sum_correctly);
  RUN_TEST(test_rate_resolution_is_per_field);
  RUN_TEST(test_residual_pricing_rule);
  RUN_TEST(test_clamp_and_stale_rows_ignored);
  RUN_TEST(test_model_rows_meter_cache_traffic);
  RUN_TEST(test_fast_path_equals_provider_formula);
  RUN_TEST(test_pre_sync_row_never_claims_a_later_month);
  RUN_TEST(test_eviction_respects_each_providers_period);
  RUN_TEST(test_other_row_priced_at_highest_known_price);
  RUN_TEST(test_rebase_drops_rows_the_counters_do_not_cover);
  RUN_TEST(test_gate_g1_failed_nvs_write_never_reprices_cheaper);
  RUN_TEST(test_rebased_row_follows_its_provider_period);
  RUN_TEST(test_gate_g2_full_ledger_new_provider_still_gets_a_row);
  RUN_TEST(test_gate_g3_failed_model_file_write_never_reprices_cheaper);
  RUN_TEST(test_downgrade_and_back_keeps_the_rows);
  RUN_TEST(test_gate_m1_reset_on_an_earlier_release_never_prices_new_spend_low);
  RUN_TEST(test_gate_m2_failed_first_spend_file_write_never_makes_earlier_usage);
  RUN_TEST(test_gate_m1_history_evidence_reads_every_row);
  RUN_TEST(test_failed_store_after_a_dropped_track_keeps_the_file_unless_inferred);
  RUN_TEST(test_gate_m1_forgotten_counters_are_stored_at_load);
  RUN_TEST(test_re_created_check_never_fires_on_counters_that_grew);
  RUN_TEST(test_stale_earlier_usage_is_dropped_at_load);
  RUN_TEST(test_earlier_usage_keeps_the_residual_price);
  RUN_TEST(test_class_check_tolerates_only_per_part_flooring);
  RUN_TEST(test_class_estimate_never_below_true_model_prices);
  RUN_TEST(test_first_model_row_never_lowers_the_provider_formula);
  RUN_TEST(test_breakdown_parts_add_up_exactly);
  RUN_TEST(test_d1_arming_none_saturation_and_dated_ungate);
  RUN_TEST(test_usg_ledger_blob_byte_identical_after_model_activity);
  RUN_TEST(test_history_8_fields_round_trip_and_downgrade_safe);
  RUN_TEST(test_history_per_day_cap_holds_60_day_retention);
  RUN_TEST(test_history_json_worst_case_fits);
  RUN_TEST(test_usage_models_json_worst_case_fits_and_parses);
  RUN_TEST(test_usage_models_json_parts_add_up_to_the_estimate);
  RUN_TEST(test_usage_models_json_shows_lost_spend_at_the_fold_price);
  RUN_TEST(test_r4_openai_cache_split_priced_at_row_rates);
  RUN_TEST(test_r4_haiku_5_5_row_and_cache_rates);
  RUN_TEST(test_r4_haiku_long_prompt_tier);
  RUN_TEST(test_r4_unknown_id_never_below_top_builtin_price);
  RUN_TEST(test_r4_openai_cache_fields_stay_zero_in_nvs_and_reload);
  RUN_TEST(test_r4_fresh_device_switched_turn);
  RUN_TEST(test_r4_resplit_fails_closed_without_rows);
  RUN_TEST(test_r4_no_new_tag_rows_while_in_nvs);
  return UNITY_END();
}
