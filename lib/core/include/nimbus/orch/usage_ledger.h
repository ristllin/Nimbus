#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/model_ledger.h"
#include "nimbus/orch/turn_spend.h"

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
  // earlier usage from before per-model tracking (see UsageLedger::effectiveRate,
  // residualRate / estCents).
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
  // Since R4 (2026-10) OpenAI's cached and written tokens are split out of its
  // input count too (ledgerDelta), so tokensIn never holds them; for OpenAI the
  // split is RAM-only and the NVS blob carries the folded form (cacheFoldedInNvs).
  uint64_t    cacheRead  = 0;
  uint64_t    cacheWrite = 0;
  // R3 (22 Oct item): the part of cacheWrite written with the 1-hour TTL
  // (Anthropic; priced at 2x the input rate, the published rate, so the
  // spend ceiling never under-counts a slow chat's writes).
  uint64_t    cacheWrite1h = 0;

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
           (cacheWrite - cacheWrite1h) * centsPerMIn * 5ULL / 4000000ULL +  // 1.25x
           cacheWrite1h * centsPerMIn / 500000ULL +         // 2x input rate (R3)
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
// tier's cents x 10000 never wraps). cacheR / cacheW (owner ruling R4): the model's
// own cached and cache-write rates, set only when a built-in model row priced the
// input side and names them; 0 = 0.1x and 1.25x of `in`.
struct EffRate {
  uint64_t in = 0, out = 0;
  uint64_t cacheR = 0, cacheW = 0;
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
// model rows + lost-model spend + residual + calls add up EXACTLY to `cents`.
struct PriceSummary {
  uint64_t cents = 0;       // == UsageLedger::estCents(name)
  bool     approx = false;  // some priced part used a built-in estimate
  bool     anyPriced = false;   // some part with spend resolved to a price
  bool     fastPath = false;    // the legacy provider-rate formula, verbatim
  // Residual: earlier usage - period spend recorded before model tracking started on
  // this device (no model was ever known for it), priced at residualRate().
  uint64_t resIn = 0, resOut = 0, resCr = 0, resCw = 0;
  EffRate  resRate;
  uint64_t resCents = 0;
  // Spend recorded AFTER tracking started that no in-period row accounts for (its
  // model was lost to a storage failure, gate G1-G3): it may have been any model, so
  // it is priced at otherRate(), the provider's highest known price, like ~other.
  uint64_t othIn = 0, othOut = 0, othCr = 0, othCw = 0;
  EffRate  othRate;
  uint64_t othCents = 0;
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

class UsageHistory;   // below

// ---- the default daily spend warning (L19-2) ---------------------------------------
// Token, call and cents limits default to 0 (none), and on 19 Oct 2026 nothing told
// the owner that one morning cost about 80 USD. So every provider has a daily warning
// threshold by default: the larger of kDailyWarnFloorCents and kDailyWarnMedianFactor
// times the median of that provider's spending days over the last
// kDailyWarnHistoryDays full days of the usage history (a fresh device: the floor).
// The owner's own limit replaces the default: a provider with a cents limit set warns
// at that limit. When a day's total crosses the threshold the owner gets exactly one
// plain notice per provider per day (the latch lives in the provider park, on flash).
inline constexpr uint64_t kDailyWarnFloorCents = 500;   // 5 USD
inline constexpr uint32_t kDailyWarnMedianFactor = 3;
inline constexpr uint32_t kDailyWarnHistoryDays = 30;

struct DailyWarn {
  uint64_t spentCents = 0;       // today's total for the provider (the history's day)
  uint64_t thresholdCents = 0;   // where it warns
  uint64_t medianCents = 0;      // the median spending day (0 = no history)
  bool     ownerLimit = false;   // the threshold is the owner's own limit
  bool     crossed() const { return spentCents > 0 && spentCents >= thresholdCents; }
};

// Days since the epoch of a calendar date (the day key the latch uses: the device's
// local date). 0 for a date before 1970.
uint32_t civilDayKey(int year, int month, int day);

// The plain owner notice: "anthropic has cost about $12.40 today, past its daily
// warning of $5.00 ... Spending continues; set a limit in Usage & Budget to cap it."
std::string dailyWarnNotice(const std::string& prov, const DailyWarn& w);

// ---- one turn's spend in ledger tokens (owner ruling R4) ---------------------------
// The ONE place that knows what each provider's usage counts mean. It takes a
// SpendSlot (one provider's billed responses), never a bare TokenUsage, so a sum of
// two providers' usage cannot reach it.
//   anthropic: input_tokens EXCLUDES cache reads and writes -> (in, cacheR, cacheW);
//   openai:    input_tokens INCLUDES cached and cache-write tokens ->
//              (in - cached - written, cached, written), clamped so it never wraps;
//   any other: its cache counts are not split (billed at the input rate) -> (in, 0, 0).
TokenDelta ledgerDelta(const SpendSlot& s);
// Providers whose input count includes their cache tokens and whose cache split the
// usgLedger NVS blob does not carry (critique D2: the blob must not grow while it
// lives in NVS). Their period split lives in RAM and in the LittleFS model rows and
// history; serialize() writes the period input with the cache folded back in and the
// cache fields 0, and the load splits it again from the model rows (settleModels).
bool cacheFoldedInNvs(const std::string& prov);

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
  // + prompt-cache counters, as ledgerDelta returns them (tokIn never holds them). For a
  // cacheFoldedInNvs provider the budget-limit counter, the all-time-in-NVS form and the
  // attribution tag keep its native count (cache included), so they read as before.
  void recordTokens(const std::string& name, uint64_t tokIn, uint64_t tokOut,
                    uint64_t cacheRead, uint64_t cacheWrite,
                    uint32_t periodKey, const std::string& tag);
  // + the model (2026-10): runs the path above unchanged, rolls every model row to
  // its own provider's period, then files the same spend under (name, key) in the
  // model ledger, key = models().spendKey(model): the model (from usageModelKey), or
  // ~other for spend with no model once tracking has started, or nothing before it
  // (provider level only). The first record with a model starts tracking and first
  // keeps every provider's counters as its earlier usage (ProvTrack.pre). `dayKey`
  // (UTC days, 0 = unknown) stamps the model row's lastDay. Allocation-free once the
  // ledger is loaded.
  void recordTokens(const std::string& name, const TokenDelta& d, uint32_t periodKey,
                    const std::string& tag, const char* model, uint32_t dayKey);
  const std::vector<TagUsage>& tagEntries() const { return tags_; }
  // Whether a record may add a NEW (provider, tag) attribution row. The device turns it
  // off while the ledger lives in NVS (the rows ride the usgLedger blob, and per-host
  // filing creates pairs a single-host turn never did); spend of a new pair is still
  // counted in the provider entry and the model rows, only the audit row is not made.
  void setAllowNewTags(bool allow) { allowNewTags_ = allow; }
  bool allowNewTags() const { return allowNewTags_; }

  // ---- bounded attribution (v4.5.14) ---------------------------------------------
  // serialize() stays under kSerializedMaxBytes: the ledger lives in a flash file now,
  // and its copy still has to fit the legacy NVS value a rollback reads. Only the
  // audit rows (tags_) are ever dropped; provider entries (the budget's source of
  // truth) never are, and at realistic counters 12 of them stay under 2 KB.
  static constexpr size_t kSerializedMaxBytes = 3800;
  // Drop the attribution rows `keep` rejects (a deleted routine's "loop:<id>").
  // Returns how many went.
  size_t pruneTags(const std::function<bool(const std::string& tag)>& keep);
  // Keep serialize() under maxBytes: rows `keep` rejects go first (when given), then
  // the smallest rows (fewest tokens). Returns how many rows were dropped.
  size_t fitSerialized(size_t maxBytes,
                       const std::function<bool(const std::string& tag)>& keep = nullptr);

  // The per-model spend rows and prices. NOT part of serialize()/deserialize(): the
  // usgLedger NVS blob is byte-identical with or without model activity; the device
  // persists the model ledger to its own LittleFS files.
  ModelLedger&       models() { return models_; }
  const ModelLedger& models() const { return models_; }
  // Once, after the model rows are loaded beside a provider ledger stored apart from
  // them: a row of another period, or of a provider with no entry, loses its period
  // counters (they are not counted); a track of a provider with no entry is dropped.
  // Then, for each provider:
  //  - counters the spend file shows were re-created (they hold less all-time spend
  //    than its latest landed usgLedger write did, or less than that write's spend
  //    from before its period plus their period spend now: a settings-only reset, on
  //    this release or an earlier one, gate M1) describe none of it: every row of the
  //    provider and its earlier usage are forgotten, and all its spend this period is
  //    lost spend;
  //  - otherwise its in-period rows must add up exactly to its counters minus its
  //    earlier usage and its already-lost spend (every record files under a row once
  //    tracking has started). When they do not and `history` shows an earlier release
  //    recorded since the rows' latest record (it may have reset the counters first),
  //    the provider is forgotten as above. Rows only BEHIND (a spend-file write that
  //    did not land) stay, and what they miss is lost spend, when the counters' latest
  //    store landed (models().countersBehind() is false). Otherwise (a row above the
  //    counters, or a usgLedger write that did not land) no row's attribution can be
  //    trusted: every in-period row drops its period spend and the provider's spend
  //    since tracking began is lost spend.
  // Lost spend is priced at otherRate (gate G1, G3: never at a cheaper price than any
  // model it may have been). Returns true when the spend file was found to describe
  // counters that are gone (a track of a provider with no entry dropped, a provider
  // forgotten): the caller must store the result now, since what showed it (the
  // stored totals, the history) may not show it at a later load.
  bool rebaseModelRows(const UsageHistory* history = nullptr);
  // The device's load, after the spend and price files were read into models(): when
  // the history shows tracking started though no spend file says so (inferred), it is
  // on with no earlier usage; when nothing shows it (an update from an earlier
  // release, a first boot), it starts now with every provider's counters as earlier
  // usage (started, gate M2); then the rebase (forgot). When any of the three holds,
  // the caller stores the spend file now; while it fails to, the file on flash does
  // not hold this load's result, so the next record writes it first. When that write
  // fails after `forgot` and `removable` holds (the history shows tracking, so the
  // next load infers it with no earlier usage), the caller removes the spend file
  // instead (removing needs no free space); otherwise the file stays and the next load
  // decides again.
  struct Settled {
    bool inferred = false, started = false, forgot = false, removable = false;
    bool mustStore() const { return inferred || started || forgot; }
  };
  Settled settleModels(const UsageHistory& history);
  // After a usgLedger write that landed (and at the load): every provider's track keeps
  // its all-time totals as stored, and their part from before the stored period
  // (ProvTrack.storedTot*, storedBefore*), so a later load can tell re-created
  // counters from grown ones. Allocation-free once loaded.
  void noteCountersStored();
  // Whether the history shows tracking started though no spend file says so: a bucket
  // names a model, or the history is empty while the counters hold token spend.
  bool historyShowsTracking(const UsageHistory& history) const;
  // Whether `history` shows an earlier release recorded since tracking started: it has
  // buckets on the latest day a row recorded on and none of them names a model (an
  // earlier release rewrites every bucket without one). A day with no bucket at all is
  // a history write that did not land (a power cut), not an earlier release.
  bool earlierReleaseRecorded(const UsageHistory* history) const;

  // ---- per-model pricing (2026-10) ----
  // The price of (prov, model key), PER FIELD, strongest tier first: the owner's
  // model price, the Cumulo router's price (cumulo only), the owner's provider
  // fallback, the built-in model estimate, the built-in provider default, none.
  EffRate effectiveRate(const std::string& prov, const char* model) const;
  // The price of earlier usage (spend from before model tracking started, the
  // residual), per field: the owner's provider fallback, else the HIGHEST owner or
  // router model price of that provider (fail-closed), else the built-in provider
  // default. Never $0 while any price exists.
  EffRate residualRate(const std::string& prov) const;
  // The price of the "~other" fold row and of any spend recorded after tracking
  // started that lost its model (live spend of models the ledger cannot name, which
  // may be the provider's dearest): per field the HIGHEST price known for the
  // provider - its fallback, any owner or router model price, any built-in model
  // estimate, its built-in default (fail-closed).
  EffRate otherRate(const std::string& prov) const;
  // The part of the provider's period counters that is earlier usage: all of it
  // before tracking started, else its track's `pre` when that track is of the
  // provider's period (clamped to the counters), else nothing.
  TokenDelta earlierUsage(const ProviderBudget& e) const;
  // The price a model ROW is charged at: its effectiveRate, or otherRate for ~other.
  EffRate rowRate(const std::string& prov, const char* model) const;
  // micro-USD per 1000 calls: the owner's provider rate, else the built-in default.
  uint64_t callRate(const std::string& prov, RateSrc* src = nullptr) const;
  // One record's spend in micro-USD, through the same rates and the same formula as
  // priceProvider: `d` at rowRate(prov, model) (an empty model, or one with no price of
  // its own, at the provider's highest price: fail-closed) plus `calls` at callRate.
  // The monthly spend ceiling counts every record with it (lane DEF).
  uint64_t priceDeltaMicro(const std::string& prov, const char* model, const TokenDelta& d,
                           uint64_t calls = 0) const;
  // A turn's whole spend (every slot through ledgerDelta, the R4 cache split) the same way.
  uint64_t priceTurnMicro(const TurnSpend& spend) const;
  // THE one formula (UI, device.status, the $ gate): the provider's period estimate in
  // cents = each in-period model row at its own rowRate + spend since tracking began
  // that no row accounts for at otherRate + earlier usage at residualRate + calls at
  // callRate; cache reads and writes at the row's own cached and write rates when it
  // has them, else 0.1x and 1.25x of the input rate; one floor to cents; saturating (a
  // saturated total reads UINT64_MAX).
  // `sum` (optional) receives the breakdown; `rows` (optional, ModelLedger::kMaxRows
  // entries, indexed like models().rows()) receives each row's part.
  uint64_t priceProvider(const std::string& name, PriceSummary* sum = nullptr,
                         RowPrice* rows = nullptr) const;
  uint64_t estCents(const std::string& name) const { return priceProvider(name); }
  // False only when every part of the provider's spend resolves to no price at all
  // (e.g. a custom model the owner never priced).
  bool anyPriced(const std::string& name) const;
  // CUM-708: the price a model is RANKED by when the device picks the cheapest one
  // (the spawn-model fallback): input plus output, micro-USD per 1M tokens, from the
  // same tiers this ledger charges (the owner's model price, the Cumulo router's, the
  // owner's provider rate, the built-in model row). UINT64_MAX when only the
  // provider-wide estimate prices it (fail-closed money: an unknown model may cost
  // more than any priced one, so it is never the cheapest).
  uint64_t rankPrice(const std::string& prov, const char* model) const;
  // L19-2: what `prov` spent on history day `dayKey`, in cents: each of the day's
  // buckets at the price this ledger charges its model (rowRate; an unattributed
  // bucket at residualRate, calls at callRate), one floor to cents.
  uint64_t priceDay(const UsageHistory& h, const std::string& prov, uint32_t dayKey) const;
  // The median of priceDay over the `days` full days before `todayKey` on which
  // `prov` spent anything (0 when it spent nothing in that window).
  uint64_t medianDayCents(const UsageHistory& h, const std::string& prov, uint32_t todayKey,
                          uint32_t days = kDailyWarnHistoryDays) const;
  // Today's spend and the threshold it is held to (see DailyWarn).
  DailyWarn dailyWarnCheck(const UsageHistory& h, const std::string& prov, uint32_t todayKey) const;
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

  // ---- catching the counters up (C2) -----------------------------------------------
  // Spend whose model is not known any more, added to `name`'s counters for `periodKey`
  // through the same fold rules as a record (rolling the counters first like a record),
  // plus `calls` billable calls. Once tracking has started it is lost spend (its track's
  // `lost`, priced at otherRate: never cheaper than any model it may have been), so the
  // rows, earlier usage and lost spend still add up to the counters.
  void addSpend(const std::string& name, const TokenDelta& d, uint32_t calls, uint32_t periodKey);
  // L19-52: per provider, what the daily history holds for the provider's CURRENT period
  // (the history day of `todayKey`, UTC) beyond its counters; empty when every counter
  // is current. A counter ahead of the history is never lowered (the history can miss
  // spend: its write may not have landed). `todayKey` 0 (clock unset): empty.
  struct HistoryGap {
    std::string name;
    TokenDelta  d;
    uint32_t    calls = 0;
    uint32_t    periodKey = 0;
  };
  std::vector<HistoryGap> historyGaps(const UsageHistory& h, uint32_t todayKey) const;
  bool countersBehind(const UsageHistory& h, uint32_t todayKey) const {
    return !historyGaps(h, todayKey).empty();
  }
  // Add every gap (addSpend). A ledger loaded from a stale copy (a usgLedger write that
  // did not land, the 19 Oct counters lost between 05:01Z and 08:32Z) then matches the
  // history totals; a current ledger is untouched. Returns how many providers moved.
  size_t reconcileWithHistory(const UsageHistory& h, uint32_t todayKey);
  // Prism row 21: spend recorded into `now` since it was `base` (a ledger served from
  // the legacy NVS copy while the file store was not mounted), added to this ledger
  // (the file's): per provider, the period spend `now` holds beyond `base`, all of it
  // when `now` rolled to a new period.
  void mergeSpendSince(const UsageLedger& base, const UsageLedger& now);

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
  void            startTracking();
  void            loseAttribution(const ProviderBudget& e);
  void            addLost(const ProviderBudget& e, const TokenDelta& gap);
  void            resetProvider(const ProviderBudget& e);
  void            alignTrack(const ProviderBudget& e);
  void            resplitFoldedCache();
  static uint32_t periodOf(const void* self, const char* prov);   // ModelLedger::PeriodOf
  std::vector<ProviderBudget> entries_;
  std::vector<TagUsage>       tags_;
  ModelLedger                 models_;
  bool                        allowNewTags_ = true;
};
static_assert(ModelLedger::kMaxTracks >= UsageLedger::kMaxProviders,
              "every provider entry needs room for its model-tracking record");

// CUM-708: the cheapest of the comma-separated `csv` models of provider `slug` by the
// effective price the ledger uses (UsageLedger::rankPrice through the registered
// source; with none registered, the built-in table, cheapestChoice). The earlier item
// wins a tie; "" when no item is priced. The device registers its ledger once.
using RankPriceFn = std::function<uint64_t(const std::string& prov, const std::string& model)>;
void setRankPriceSource(RankPriceFn fn);
std::string cheapestByEffectivePrice(const char* slug, const std::string& csv);

// L19-2: whether the daily spend warning for `prov` is due now: today's spend
// (history day `todayKey`) crossed its threshold and the owner was not told yet on
// the device's local date `localDayKey`. True takes the latch (kept on flash by the
// provider park, so a restart does not repeat it) and fills `notice`; the caller
// sends it. A caller that cannot reach the owner right now must not call it.
bool dailyWarnDue(const UsageLedger& L, const UsageHistory& h, const std::string& prov, uint32_t todayKey,
                  uint32_t localDayKey, std::string& notice);

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
  // Whether a bucket of day `dayKey` names a model.
  bool hasModelOn(uint32_t dayKey) const;
  // Whether any bucket of day `dayKey` exists.
  bool hasBucketOn(uint32_t dayKey) const;

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
