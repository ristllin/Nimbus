#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/model_ledger.h"

// Per-provider monthly usage + budget ledger (owner ask: "limit budget, and see
// budget per provider - Tavily monthly calls, providers monthly tokens, and set the
// month reset per provider"). Portable + host-tested (NO Arduino).
//
// The device meters real billed token spend per LLM host and per search-call for
// Tavily-style providers, accumulated into a rolling monthly window that resets on a
// per-provider day-of-month. This core owns ALL the accounting + reset + over-budget
// decisions; the device glue only supplies the current wall-clock date (SNTP) and
// persists serialize()/deserialize() to NVS. Two limits per provider (0 = unlimited):
// a token ceiling (LLMs) and a call ceiling (search). overBudget gates a turn / a
// tool call before it spends.

namespace nimbus {
namespace orch {

// Rolling-month period key: months-since-year-0, shifted so a period begins on
// `resetDay`. Before the reset day we're still counting in the PREVIOUS period.
// resetDay clamped 1..28 (every month has these days - no Feb/30/31 edge cases).
uint32_t usagePeriodKey(int year, int month, int day, uint8_t resetDay);

struct ProviderBudget {
  std::string name;              // provider id: "openai" | "anthropic" | "mistral" | "tavily" | ...
  uint32_t    periodKey = 0;     // the reset-period these counters belong to (0 = none yet)
  uint64_t    tokens    = 0;     // billed tokens this period (LLM hosts)
  uint32_t    calls     = 0;     // billable calls this period (search providers)
  uint64_t    tokenLimit = 0;    // monthly token ceiling; 0 = unlimited
  uint32_t    callLimit  = 0;    // monthly call ceiling;  0 = unlimited
  uint8_t     resetDay   = 1;    // day-of-month the window rolls (1..28)
  // Cost-estimate additions (owner ask: graphs + estimated price). The in/out split
  // matters because output tokens bill ~5x input; totals never reset (all-time).
  uint64_t    tokensIn  = 0;     // prompt tokens this period
  uint64_t    tokensOut = 0;     // completion tokens this period
  uint64_t    totalIn   = 0;     // ALL-TIME prompt tokens (never rolls)
  uint64_t    totalOut  = 0;     // ALL-TIME completion tokens
  uint64_t    totalCalls = 0;    // ALL-TIME billable calls
  // Owner-editable provider price rates (integers - no float serialization drift):
  // cents per 1M input tokens, cents per 1M output tokens, cents per 1000 calls.
  // 0 = unset. Since per-model prices (2026-10) these are the provider FALLBACK:
  // they price every model of the provider that has no price of its own, and the
  // spend no model row covers (see UsageLedger::effectiveRate / estCents).
  uint32_t    centsPerMIn  = 0;
  uint32_t    centsPerMOut = 0;
  uint32_t    centsPerKCalls = 0;
  // W16 (owner): a DOLLAR ceiling for the period, in cents; 0 = none. Enforced
  // through UsageLedger::overCostBudget, which prices each model at its own rate
  // (owner, router, provider fallback, then the built-in estimates); it stays
  // inactive only when nothing at all is priced. Kept separate from tokenLimit:
  // either ceiling may gate alone.
  uint64_t    centsLimit = 0;
  // Prompt-cache period counters (v4.1.3 prism): Anthropic bills cache WRITES at
  // 1.25x and READS at 0.1x of the input rate, and EXCLUDES both from
  // input_tokens - without these the $ gate under-metered every cached turn.
  // Recorded ONLY for providers that exclude them from the prompt count (the
  // record site knows the host); OpenAI includes cached tokens in its prompt
  // count, so recording them there would double-meter.
  uint64_t    cacheRead  = 0;
  uint64_t    cacheWrite = 0;

  // The provider-rate-only term: period spend in cents priced at THIS provider's
  // own rates (integer math, floored per term). Since per-model prices (2026-10)
  // the one formula the UI, device.status and the budget gate read is
  // UsageLedger::estCents(name); it returns this value verbatim (the bit-exact
  // fast path) for a provider whose spend has no model rows this period and whose
  // own in/out rates are both set, so a ledger from before the upgrade keeps its
  // numbers to the cent.
  uint64_t estCents() const {
    return tokensIn * centsPerMIn / 1000000ULL +
           tokensOut * centsPerMOut / 1000000ULL +
           cacheRead * centsPerMIn / 10000000ULL +          // 0.1x input rate
           cacheWrite * centsPerMIn * 5ULL / 4000000ULL +   // 1.25x input rate
           (uint64_t)calls * centsPerKCalls / 1000ULL;
  }
};

// Spend ATTRIBUTION (2026-07 harness round): who spent these tokens. All-time
// per-(provider, tag) counters riding alongside the budget entries - the budget
// math is untouched (tags never gate anything); this closes the documented
// "sub-agent fan-out spend is NOT yet attributed" gap. Tags in use:
//   "turn" (owner-driven, the default) | "synthesis" (auto-consolidation) |
//   "loop:<id>" (Local Loops scheduled turns) | "spawn:<backend>" (sub-session
//   usage, recorded only where a poll returns REAL provider usage data).
struct TagUsage {
  std::string prov;
  std::string tag;
  uint64_t    tokIn  = 0;   // all-time (never rolls - attribution is an audit
  uint64_t    tokOut = 0;   // trail, not a budget window)
};

// ---- per-model pricing (2026-10) ------------------------------------------------
// Where a resolved price came from, strongest first. 0 in any tier means "unset" and
// the next tier is asked, field by field (input and output resolve separately).
enum class RateSrc : uint8_t {
  None = 0,       // nothing priced it
  Owner,          // the owner's price for this model
  Router,         // the Cumulo router's price (cumulo keys only, verify harvest)
  Provider,       // the owner's provider fallback (centsPerM x 10000)
  BuiltinModel,   // built-in estimate for this model (always shown '~')
  BuiltinProv,    // built-in provider default (always shown '~')
};
// Wire name for the UI badge: "owner" | "router" | "provider" | "est" | "none".
const char* rateSrcName(RateSrc s);
inline bool rateSrcIsEstimate(RateSrc s) {
  return s == RateSrc::BuiltinModel || s == RateSrc::BuiltinProv;
}

// A resolved price: micro-USD per 1M tokens (Cumulo's unit; u64 so the provider
// tier's cents x 10000 never wraps).
struct EffRate {
  uint64_t in = 0, out = 0;
  RateSrc  srcIn = RateSrc::None, srcOut = RateSrc::None;
  bool approx() const { return rateSrcIsEstimate(srcIn) || rateSrcIsEstimate(srcOut); }
  // One badge for the pair: the weakest tier that priced a field (an estimate in
  // either field marks the whole price '~'); None only when neither field is priced.
  RateSrc src() const {
    if (srcIn == RateSrc::None) return srcOut;
    if (srcOut == RateSrc::None) return srcIn;
    return srcIn > srcOut ? srcIn : srcOut;
  }
};

// Tokens x (micro-USD per 1M tokens) -> micro-USD, floored, saturating at UINT64_MAX:
// (t / 1e6) * r + (t % 1e6) * r / 1e6, computed without an intermediate overflow.
uint64_t costMicro(uint64_t tokens, uint64_t microPerM);

// The pieces of one provider's period estimate (UsageLedger::priceProvider). Every
// component's cents are the cumulative floor of the running micro total, so the
// model rows + residual + calls add up EXACTLY to `cents`.
struct PriceSummary {
  uint64_t cents = 0;       // == UsageLedger::estCents(name)
  bool     approx = false;  // some priced part used a built-in estimate
  bool     anyPriced = false;   // some part with spend resolved to a price
  bool     fastPath = false;    // the legacy provider-rate formula, verbatim
  // Residual: period spend no in-period model row accounts for (pre-upgrade spend,
  // a spend the model ledger could not attribute), priced at residualRate().
  uint64_t resIn = 0, resOut = 0, resCr = 0, resCw = 0;
  EffRate  resRate;
  uint64_t resCents = 0;
  // Calls (search providers), priced at callRate().
  uint64_t calls = 0, callRate = 0;   // callRate: micro-USD per 1000 calls
  RateSrc  callSrc = RateSrc::None;
  uint64_t callCents = 0;
};

// One model row's part of its provider's period estimate (all zero for a row of
// another provider, a stale row, or a row the fast path left unpriced).
struct RowPrice {
  TokenDelta take;       // the period tokens priced (clamped to the provider counters)
  EffRate    rate;
  uint64_t   cents = 0;
};

class UsageLedger {
 public:
  static constexpr size_t kMaxProviders = 12;
  static constexpr size_t kMaxTags      = 48;   // (provider, tag) pairs; bounded NVS blob

  // Add token spend to `name` for `periodKey`; rolls (zeroes) the counters first if
  // the stored period differs. Creates the entry if new. No-op past kMaxProviders.
  // The (in, out) overload also feeds the in/out split + the all-time totals; the
  // total-only form remains for callers without a split (counts as input-side).
  // The `tag` overload ADDITIONALLY accumulates the per-tag attribution counter
  // (empty tag = untagged, no attribution row) - existing callers are untouched.
  void recordTokens(const std::string& name, uint64_t n, uint32_t periodKey);
  void recordTokens(const std::string& name, uint64_t tokIn, uint64_t tokOut,
                    uint32_t periodKey);
  void recordTokens(const std::string& name, uint64_t tokIn, uint64_t tokOut,
                    uint32_t periodKey, const std::string& tag);
  // + prompt-cache counters (see the field note: excluded-from-input providers only).
  void recordTokens(const std::string& name, uint64_t tokIn, uint64_t tokOut,
                    uint64_t cacheRead, uint64_t cacheWrite,
                    uint32_t periodKey, const std::string& tag);
  // + the model (2026-10): runs the path above unchanged, rolls every model row to
  // its own provider's period, then files the same spend under (name, model) in the
  // model ledger when `model` is a non-empty key (from usageModelKey; "" = provider
  // level only). `dayKey` (UTC days, 0 = unknown) stamps the model row's lastDay.
  // Allocation-free once the ledger is loaded.
  void recordTokens(const std::string& name, const TokenDelta& d, uint32_t periodKey,
                    const std::string& tag, const char* model, uint32_t dayKey);
  const std::vector<TagUsage>& tagEntries() const { return tags_; }

  // The per-model spend rows and prices. NOT part of serialize()/deserialize(): the
  // usgLedger NVS blob is byte-identical with or without model activity; the device
  // persists the model ledger to its own LittleFS files.
  ModelLedger&       models() { return models_; }
  const ModelLedger& models() const { return models_; }
  // Once, after the model rows are loaded beside a provider ledger stored apart from
  // them (a settings-only reset or a failed NVS write can leave rows the provider
  // counters no longer cover): a row of another period, or of a provider with no
  // entry, loses its period counters, and so does every row of a provider whose
  // in-period rows add up to more than its counters in any field. That spend then
  // reads as the provider's residual; all-time totals are kept.
  void rebaseModelRows();

  // ---- per-model pricing (2026-10) ----
  // The price of (prov, model key), PER FIELD, strongest tier first: the owner's
  // model price, the Cumulo router's price (cumulo only), the owner's provider
  // fallback, the built-in model estimate, the built-in provider default, none.
  EffRate effectiveRate(const std::string& prov, const char* model) const;
  // The price of spend no model row covers (the residual), per field: the owner's
  // provider fallback, else the HIGHEST owner or router model price of that provider
  // (fail-closed), else the built-in provider default. Never $0 while any price exists.
  EffRate residualRate(const std::string& prov) const;
  // The price of the "~other" fold row (live spend of models the ledger could not
  // name, which may be the provider's dearest): per field the HIGHEST price known
  // for the provider - its fallback, any owner or router model price, any built-in
  // model estimate, its built-in default (fail-closed).
  EffRate otherRate(const std::string& prov) const;
  // The price a model ROW is charged at: its effectiveRate, or otherRate for ~other.
  EffRate rowRate(const std::string& prov, const char* model) const;
  // micro-USD per 1000 calls: the owner's provider rate, else the built-in default.
  uint64_t callRate(const std::string& prov, RateSrc* src = nullptr) const;
  // THE one formula (UI, device.status, the $ gate): the provider's period estimate in
  // cents = each in-period model row at its own effectiveRate + the residual at
  // residualRate + calls at callRate; cache reads at 0.1x and writes at 1.25x of the
  // input rate; one floor to cents; saturating (a saturated total reads UINT64_MAX).
  // `sum` (optional) receives the breakdown; `rows` (optional, ModelLedger::kMaxRows
  // entries, indexed like models().rows()) receives each row's part.
  uint64_t priceProvider(const std::string& name, PriceSummary* sum = nullptr,
                         RowPrice* rows = nullptr) const;
  uint64_t estCents(const std::string& name) const { return priceProvider(name); }
  // False only when every part of the provider's spend resolves to no price at all
  // (e.g. a custom model the owner never priced).
  bool anyPriced(const std::string& name) const;
  // A model row counts toward its provider's period only when it belongs to exactly
  // that period. Rows recorded before the clock synced carry into the first real
  // period with their provider (recordTokens rolls every row to its provider's
  // period), so a stale pre-sync row can never claim a later month's spend.
  static bool inPeriod(const ProviderBudget& e, const ModelSpend& r) {
    return r.periodKey == e.periodKey;
  }
  // Add one billable call (search providers); same roll semantics.
  void recordCall(const std::string& name, uint32_t periodKey);

  // Set the budget knobs for `name` (create if new). Never resets the live counters -
  // changing a limit mid-month keeps the accumulated usage. resetDay clamped 1..28.
  // centsLimit (W16): the period's $ ceiling in cents, 0 = none.
  void setLimits(const std::string& name, uint64_t tokenLimit, uint32_t callLimit,
                 uint8_t resetDay, uint64_t centsLimit = 0);
  // Set the provider fallback rates for `name` (create if new). They feed the $
  // estimate and so the $ ceiling (overCostBudget): enforcement inputs, not display
  // only. Units per the ProviderBudget fields; 0 = unset.
  void setRates(const std::string& name, uint32_t centsPerMIn, uint32_t centsPerMOut,
                uint32_t centsPerKCalls);

  // Over-budget tests. False when no entry, no limit set, or under the ceiling.
  // These read the LAST-RECORDED period's counters - the caller records first (which
  // rolls), then tests, so a fresh period reads as under budget.
  bool overTokenBudget(const std::string& name) const;
  bool overCallBudget(const std::string& name) const;
  // W16: the $ ceiling: centsLimit > 0 && anyPriced(name) && estCents(name) >=
  // centsLimit. Every price tier counts, the built-in '~' estimates and router
  // prices included (fail-closed money: a cap that silently never trips is worse);
  // it stays inactive only when nothing at all is priced.
  bool overCostBudget(const std::string& name) const;
  // ANY ceiling breached - the single gate a turn/tool call checks.
  //
  // ⚠ prism (the permanently-gated wedge): the counters roll only when a call
  // RECORDS, but the gate runs BEFORE the call - so a provider that ended a
  // month over its cap would stay gated FOREVER ("wait for the reset day" could
  // never come true). Callers that know today's date pass it; a stale periodKey
  // then reads as a FRESH period (under budget). The date-less form keeps the
  // legacy strict behavior for tests/tools with no clock.
  bool overBudget(const std::string& name) const {
    return overTokenBudget(name) || overCallBudget(name) || overCostBudget(name);
  }
  bool overBudget(const std::string& name, int year, int month, int day) const {
    const ProviderBudget* e = find(name);
    if (!e) return false;
    if (e->periodKey != usagePeriodKey(year, month, day, e->resetDay))
      return false;   // new period: the roll happens on the next record
    return overBudget(name);
  }

  const ProviderBudget* find(const std::string& name) const;
  const std::vector<ProviderBudget>& entries() const { return entries_; }

  // Compact, self-describing round-trippable form for NVS. One entry per ';'-group,
  // fields '|'-separated: name|periodKey|tokens|calls|tokenLimit|callLimit|resetDay
  // [|tokensIn|tokensOut|totalIn|totalOut|totalCalls|centsPerMIn|centsPerMOut|
  // centsPerKCalls]. The bracketed tail is the 2026-07-16 cost extension - an old
  // 7-field blob still parses (missing fields read 0), so no NVS migration.
  // Attribution tags append as '#'-prefixed groups ("#prov|tag|tokIn|tokOut");
  // an OLDER deserializer skips them (its 7-field minimum rejects the 4-field
  // group), so the blob stays downgrade-safe too.
  std::string serialize() const;
  void        deserialize(const std::string& s);

 private:
  ProviderBudget* findMut(const std::string& name);
  ProviderBudget* ensure(const std::string& name);
  static uint32_t periodOf(const void* self, const char* prov);   // ModelLedger::PeriodOf
  std::vector<ProviderBudget> entries_;
  std::vector<TagUsage>       tags_;
  ModelLedger                 models_;
};

// ---- daily usage history (the graphs' time series) ----------------------------
// One bucket per (UTC day, provider, model): prompt/completion tokens, calls and
// (2026-10) the prompt-cache counters. The device records into it alongside the
// ledger (only once the wall clock is sane) and persists it to LittleFS - bounded
// by prune() and the caps below. Rendering (sums for last week/month, cost
// estimates) happens client-side from the raw buckets and the server-resolved
// prices (GET /api/usage/models); this core keeps only honest counts.

struct DayUsage {
  uint32_t    dayKey = 0;   // days since the Unix epoch, UTC
  std::string prov;
  uint64_t    tokIn = 0, tokOut = 0;
  uint32_t    calls = 0;
  char        model[kModelKeyCap] = {0};   // "" = unattributed (or recorded before 2026-10)
  uint64_t    cacheR = 0, cacheW = 0;
};

class UsageHistory {
 public:
  static constexpr size_t kMaxEntries = 720;   // 60 days x kMaxPerDay
  // Distinct (provider, model) buckets per day: the 13th folds into that provider's
  // "~other" bucket, so 60 days of history always fit kMaxEntries.
  static constexpr size_t kMaxPerDay = 12;

  // Merge spend into the (dayKey, prov, model) bucket, creating it if new (model ""
  // = unattributed; kOtherModel = the provider's fold bucket). The oldest bucket is
  // evicted first when kMaxEntries is reached. Allocation-free once the bucket
  // store is reserved (the first record or load reserves kMaxEntries once).
  void record(const std::string& prov, const char* model, uint32_t dayKey, const TokenDelta& d,
              uint32_t calls);
  // The pre-2026-10 form: an unattributed bucket with no cache counters.
  void record(const std::string& prov, uint32_t dayKey, uint64_t tokIn,
              uint64_t tokOut, uint32_t calls) {
    TokenDelta d;
    d.in = tokIn;
    d.out = tokOut;
    record(prov, "", dayKey, d, calls);
  }

  // Drop buckets older than keepDays relative to `todayKey` (and enforce the entry
  // cap). Returns the number removed.
  int prune(uint32_t todayKey, uint32_t keepDays = 60);

  const std::vector<DayUsage>& entries() const { return entries_; }

  // Same compact text form as the ledger, one ';'-group per bucket:
  // day|prov|tokIn|tokOut|calls[|model[|cacheR[|cacheW]]]. The trailing fields are
  // omitted when empty or 0. Downgrade-safe: an older parser splits at most 5 parts
  // and ignores the rest, so it still reads the calls.
  std::string serialize() const;
  void        deserialize(const std::string& s);

 private:
  DayUsage* find(uint32_t dayKey, const std::string& prov, const char* model);
  DayUsage* bucketFor(const std::string& prov, const char* model, uint32_t dayKey);
  size_t    countDay(uint32_t dayKey) const;
  size_t    countDayProv(uint32_t dayKey, const std::string& prov) const;
  bool      foldOnePair(uint32_t dayKey);
  DayUsage* push(const std::string& prov, const char* model, uint32_t dayKey);
  std::vector<DayUsage> entries_;
};

// The JSON object GET /api/usage/history serves for one bucket, written into `out`
// (cap bytes, NUL-terminated): {"d","prov","in","out","calls"} plus "m", "cr" and
// "cw" when non-empty / non-zero. `first` omits the leading ','. Returns the bytes
// written, or 0 when it does not fit (the caller stops). kHistoryJsonEntryMax bounds
// the widest bucket (15-char provider, 47-char model, every counter at its max).
inline constexpr size_t kHistoryJsonEntryMax = 240;
size_t historyEntryJson(const DayUsage& e, bool first, char* out, size_t cap);

}  // namespace orch
}  // namespace nimbus
