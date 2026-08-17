#pragma once
#include <cstdint>
#include <string>
#include <vector>

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
  // Owner-editable price rates (integers - no float serialization drift):
  // cents per 1M input tokens, cents per 1M output tokens, cents per 1000 calls.
  // 0 = unset (the UI falls back to labelled default estimates).
  uint32_t    centsPerMIn  = 0;
  uint32_t    centsPerMOut = 0;
  uint32_t    centsPerKCalls = 0;
  // W16 (owner): a DOLLAR ceiling for the period, in cents; 0 = none. Enforced
  // via the owner rates above - with all rates unset the estimate is always $0,
  // so a $ limit alone can never trip (the UI says so; setting prices is what
  // arms it). Kept separate from tokenLimit: either ceiling may gate alone.
  uint64_t    centsLimit = 0;
  // Prompt-cache period counters (v4.1.3 prism): Anthropic bills cache WRITES at
  // 1.25x and READS at 0.1x of the input rate, and EXCLUDES both from
  // input_tokens - without these the $ gate under-metered every cached turn.
  // Recorded ONLY for providers that exclude them from the prompt count (the
  // record site knows the host); OpenAI includes cached tokens in its prompt
  // count, so recording them there would double-meter.
  uint64_t    cacheRead  = 0;
  uint64_t    cacheWrite = 0;

  // Estimated period spend in cents from the owner rates (integer math; u64 -
  // no overflow until ~10^13 tokens). THE one formula: UI, model tool and the
  // budget gate all read this, so they can never disagree.
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
  const std::vector<TagUsage>& tagEntries() const { return tags_; }
  // Add one billable call (search providers); same roll semantics.
  void recordCall(const std::string& name, uint32_t periodKey);

  // Set the budget knobs for `name` (create if new). Never resets the live counters -
  // changing a limit mid-month keeps the accumulated usage. resetDay clamped 1..28.
  // centsLimit (W16): the period's $ ceiling in cents, 0 = none.
  void setLimits(const std::string& name, uint64_t tokenLimit, uint32_t callLimit,
                 uint8_t resetDay, uint64_t centsLimit = 0);
  // Set the price rates for `name` (create if new); display-time multipliers only -
  // never affect budget enforcement. Units per the ProviderBudget fields; 0 = unset.
  void setRates(const std::string& name, uint32_t centsPerMIn, uint32_t centsPerMOut,
                uint32_t centsPerKCalls);

  // Over-budget tests. False when no entry, no limit set, or under the ceiling.
  // These read the LAST-RECORDED period's counters - the caller records first (which
  // rolls), then tests, so a fresh period reads as under budget.
  bool overTokenBudget(const std::string& name) const;
  bool overCallBudget(const std::string& name) const;
  // W16: the $ ceiling, via the owner rates. False when no limit, no entry, or
  // no rates (a $ limit with no prices cannot honestly trip).
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
  std::vector<ProviderBudget> entries_;
  std::vector<TagUsage>       tags_;
};

// ---- daily usage history (the graphs' time series) ----------------------------
// One bucket per (UTC day, provider): prompt/completion tokens + calls. The device
// records into it alongside the ledger (only once the wall clock is sane) and
// persists it to LittleFS - bounded by prune(), so ~60 days x a handful of
// providers stays a few KB. Rendering (sums for last week/month, cost estimates)
// happens client-side from the raw buckets; this core keeps only honest counts.

struct DayUsage {
  uint32_t    dayKey = 0;   // days since the Unix epoch, UTC
  std::string prov;
  uint64_t    tokIn = 0, tokOut = 0;
  uint32_t    calls = 0;
};

class UsageHistory {
 public:
  static constexpr size_t kMaxEntries = 720;   // ~60 days x 12 providers

  // Merge spend into the (dayKey, prov) bucket, creating it if new. Oldest buckets
  // are pruned first when the cap would be exceeded.
  void record(const std::string& prov, uint32_t dayKey, uint64_t tokIn,
              uint64_t tokOut, uint32_t calls);

  // Drop buckets older than keepDays relative to `todayKey` (and enforce the entry
  // cap). Returns the number removed.
  int prune(uint32_t todayKey, uint32_t keepDays = 60);

  const std::vector<DayUsage>& entries() const { return entries_; }

  // Same compact text form as the ledger: day|prov|tokIn|tokOut|calls per ';'-group.
  std::string serialize() const;
  void        deserialize(const std::string& s);

 private:
  std::vector<DayUsage> entries_;
};

}  // namespace orch
}  // namespace nimbus
