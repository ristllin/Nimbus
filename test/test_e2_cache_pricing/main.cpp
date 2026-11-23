// test_e2_cache_pricing - lane E2-FIX6 (second read C9, C11): a request with 1-hour cache writes
// costs the same on every path the owner or a limit reads a price from, for every built-in
// Anthropic head row (the long-prompt tier included):
//   1. the engine's spend guard: plain::headRatesFor -> guardCostFor -> requestMicroUsd;
//   2. the monthly ceiling's commit: TurnSpend -> UsageLedger::priceTurnMicro;
//   3. the usage view, the provider caps and their seeds: UsageLedger::priceProvider with model
//      rows tracked (the path a device with its model table takes);
//   4. the daily warning: UsageLedger::priceDay over the day's history bucket;
//   5. a ledger read back from its stored text (the 1-hour part survives a restart).
// Red on a4f9c0621: paths 1 (long tier), 3 and 4 priced the 1-hour part at the 5-minute rate.
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/harness/plain/guards.h"
#include "nimbus/orch/model_price_defaults.h"
#include "nimbus/orch/turn_spend.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/usage_ledger.h"

namespace orch = nimbus::orch;
namespace plain = agent::plain;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint32_t kDay = 20415;      // a day key (UTC days)
constexpr uint32_t kPeriod = 202611;  // any period key, the same for every record

struct Req {
  uint32_t in, out, cacheR, cacheW, cacheW1h;
};

orch::TokenUsage usageOf(const Req& r) {
  orch::TokenUsage u;
  u.promptTokens = r.in;
  u.completionTokens = r.out;
  u.cacheReadTokens = r.cacheR;
  u.cacheWriteTokens = r.cacheW;
  u.cacheWrite1hTokens = r.cacheW1h;
  u.maxRequestIn = r.in + r.cacheR + r.cacheW;
  u.responses = 1;
  return u;
}

// What the provider bills, by hand from the rows: the reference every path is held to.
uint64_t billMicro(const char* model, const Req& r) {
  const orch::BuiltinPrice* row = orch::builtinModelPrice("anthropic", model);
  char key[orch::kModelKeyCap];
  if (orch::builtinLongTierKey("anthropic", model, r.in + r.cacheR + r.cacheW, key)) row = orch::builtinModelPrice("anthropic", key);
  const uint64_t in = row->microIn, out = row->microOut;
  const uint64_t read = row->microCacheR ? row->microCacheR : in / 10;
  const uint64_t write = row->microCacheW ? row->microCacheW : in * 5 / 4;
  return ((uint64_t)r.in * in + (uint64_t)r.out * out + (uint64_t)r.cacheR * read +
          (uint64_t)(r.cacheW - r.cacheW1h) * write + (uint64_t)r.cacheW1h * in * 2) / 1000000ULL;
}

uint64_t guardMicro(const char* model, const Req& r) {
  plain::HeadRates rates;
  // [base-begin]
  TEST_ASSERT_TRUE(plain::headRatesFor("anthropic", model, rates));
  // [base-end]
  orch::tx::HeadBudget b;
  const plain::HeadCost c = plain::guardCostFor(b, rates);
  return plain::requestMicroUsd(c, plain::RequestTokens{r.in, r.out, r.cacheR, r.cacheW, r.cacheW1h});
}

orch::TurnSpend spendOf(const char* model, const Req& r) {
  orch::TurnSpend s;
  s.add("anthropic", model, usageOf(r));
  return s;
}

// A tracked ledger holding this one request (a model row, as a device with its model table).
void recordTracked(orch::UsageLedger& L, orch::UsageHistory& H, const orch::TurnSpend& s) {
  for (size_t i = 0; i < s.n; ++i) {
    const orch::SpendSlot& sl = s.slot[i];
    const orch::TokenDelta d = orch::ledgerDelta(sl);
    L.recordTokens(sl.host, d, kPeriod, "turn", sl.model, kDay);
    H.record(sl.host, sl.model, kDay, d, 0);
  }
}

void within(uint64_t want, uint64_t got, const char* what, const char* model) {
  char msg[160];
  std::snprintf(msg, sizeof msg, "%s for %s: %llu uUSD against the bill's %llu", what, model,
                (unsigned long long)got, (unsigned long long)want);
  TEST_ASSERT_TRUE_MESSAGE(got + 6 >= want && got <= want + 6, msg);   // per-term rounding only
}
void withinCents(uint64_t wantMicro, uint64_t gotCents, const char* what, const char* model) {
  char msg[160];
  std::snprintf(msg, sizeof msg, "%s for %s: %llu cents against the bill's %llu uUSD", what, model,
                (unsigned long long)gotCents, (unsigned long long)wantMicro);
  TEST_ASSERT_TRUE_MESSAGE(gotCents + 1 >= wantMicro / 10000 && gotCents <= wantMicro / 10000 + 1, msg);
}

void onePath(const char* model, const Req& r) {
  const uint64_t bill = billMicro(model, r);
  within(bill, guardMicro(model, r), "the engine's spend guard", model);
  orch::UsageLedger L;
  orch::UsageHistory H;
  const orch::TurnSpend s = spendOf(model, r);
  within(bill, L.priceTurnMicro(s), "the ceiling's commit", model);
  recordTracked(L, H, s);
  withinCents(bill, L.priceProvider("anthropic"), "the usage view (model rows)", model);
  withinCents(bill, L.priceDay(H, "anthropic", kDay), "the daily warning", model);
  orch::UsageLedger R;
  R.deserialize(L.serialize());
  R.models().deserializeSpend(L.models().serializeSpend());
  withinCents(bill, R.priceProvider("anthropic"), "the usage view after a restart", model);
  orch::UsageHistory H2;
  H2.deserialize(H.serialize());
  withinCents(bill, L.priceDay(H2, "anthropic", kDay), "the daily warning after a restart", model);
}

}  // namespace

static void test_every_anthropic_row_costs_the_same_on_every_path() {
  int rows = 0;
  for (const orch::BuiltinPrice& p : orch::kBuiltinPrices) {
    if (std::strcmp(p.slot, "anthropic") != 0 || std::strcmp(p.family, "*") == 0) continue;
    if (std::strstr(p.family, orch::kLongTierSuffix)) continue;   // reached through its base row
    ++rows;
    onePath(p.family, Req{40, 900, 52000, 41000, 30000});            // a warm turn's shape, large
    onePath(p.family, Req{40, 900, 3000000, 2500000, 2400000});      // a month of them in one
    if (p.longOver) onePath(p.family, Req{40, 900, 60000, p.longOver, p.longOver - 1000});   // the long tier
  }
  TEST_ASSERT_TRUE(rows >= 3);
}

// A period roll: the 1-hour part rolls with the writes (it was kept, outgrew them and wrapped estCents).
static void test_the_one_hour_part_rolls_with_its_period() {
  orch::UsageLedger L;
  orch::TokenDelta d;
  d.cacheW = 1000;
  d.cacheW1h = 800;
  L.recordTokens("anthropic", d, kPeriod, "turn", nullptr, kDay);
  orch::TokenDelta e;
  e.in = 10;
  L.recordTokens("anthropic", e, kPeriod + 1, "turn", nullptr, kDay + 31);   // the next period
  const orch::ProviderBudget* p = L.find("anthropic");
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL(0, (int)p->cacheWrite);
  TEST_ASSERT_EQUAL(0, (int)p->cacheWrite1h);
  TEST_ASSERT_TRUE(p->estCents() < 1000);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_anthropic_row_costs_the_same_on_every_path);
  RUN_TEST(test_the_one_hour_part_rolls_with_its_period);
  return UNITY_END();
}
