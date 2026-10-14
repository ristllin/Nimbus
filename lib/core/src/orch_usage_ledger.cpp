#include "nimbus/orch/usage_ledger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nimbus/orch/model_price_defaults.h"

namespace nimbus {
namespace orch {

uint32_t usagePeriodKey(int year, int month, int day, uint8_t resetDay) {
  if (resetDay < 1) resetDay = 1;
  if (resetDay > 28) resetDay = 28;
  // months-since-year-0; a period that starts on resetDay means days before it still
  // belong to the previous month's window.
  long m0 = (long)year * 12 + (month - 1);
  if (day < (int)resetDay) m0 -= 1;
  if (m0 < 0) m0 = 0;
  return (uint32_t)m0;
}

ProviderBudget* UsageLedger::findMut(const std::string& name) {
  for (auto& e : entries_)
    if (e.name == name) return &e;
  return nullptr;
}

const ProviderBudget* UsageLedger::find(const std::string& name) const {
  for (const auto& e : entries_)
    if (e.name == name) return &e;
  return nullptr;
}

uint32_t UsageLedger::periodOf(const void* self, const char* prov) {
  for (const auto& e : static_cast<const UsageLedger*>(self)->entries_)
    if (e.name == prov) return e.periodKey;
  return 0;   // no entry: keep the row's period
}

ProviderBudget* UsageLedger::ensure(const std::string& name) {
  ProviderBudget* e = findMut(name);
  if (e) return e;
  if (entries_.size() >= kMaxProviders) return nullptr;
  entries_.push_back(ProviderBudget{});
  entries_.back().name = name;
  return &entries_.back();
}

// Roll the counters to `periodKey` if the stored period differs (a new month zeroes
// the period counters; limits/resetDay/rates/all-time totals persist).
// Clock-sanity guards (review 2026-07-16 - the owner's monthly cap must survive a
// reboot): key 0 means "wall clock not synced yet".
//  - stored P, incoming 0: KEEP counting in the stored period - a pre-SNTP record
//    after a power-cycle must never wipe month-to-date spend (it was durably
//    re-opening a nearly-closed budget gate).
//  - stored 0, incoming P (first sync): CARRY the accumulated counters into the
//    real period - that spend just happened, it belongs to the current window.
static void rollTo(ProviderBudget* e, uint32_t periodKey) {
  if (e->periodKey == periodKey) return;
  if (periodKey == 0) return;                            // unsynced: keep the stored period
  if (e->periodKey == 0) { e->periodKey = periodKey; return; }  // first sync: carry
  e->periodKey = periodKey;
  e->tokens = 0;
  e->calls = 0;
  e->tokensIn = 0;
  e->tokensOut = 0;
  e->cacheRead = 0;
  e->cacheWrite = 0;
}

void UsageLedger::recordTokens(const std::string& name, uint64_t n, uint32_t periodKey) {
  recordTokens(name, n, 0, periodKey);   // splitless caller: count as input-side
}

void UsageLedger::recordTokens(const std::string& name, uint64_t tokIn, uint64_t tokOut,
                               uint32_t periodKey) {
  ProviderBudget* e = ensure(name);
  if (!e) return;
  rollTo(e, periodKey);
  e->tokens    += tokIn + tokOut;   // the budget-limit counter stays the total
  e->tokensIn  += tokIn;
  e->tokensOut += tokOut;
  e->totalIn   += tokIn;            // all-time - never rolls
  e->totalOut  += tokOut;
}

void UsageLedger::recordTokens(const std::string& name, uint64_t tokIn, uint64_t tokOut,
                               uint64_t cacheRead, uint64_t cacheWrite,
                               uint32_t periodKey, const std::string& tag) {
  recordTokens(name, tokIn, tokOut, periodKey, tag);
  if (cacheRead == 0 && cacheWrite == 0) return;
  ProviderBudget* e = ensure(name);
  if (!e) return;
  // No rollTo here: the call above already rolled to this periodKey.
  e->cacheRead += cacheRead;
  e->cacheWrite += cacheWrite;
}

void UsageLedger::recordTokens(const std::string& name, const TokenDelta& d,
                               uint32_t periodKey, const std::string& tag,
                               const char* model, uint32_t dayKey) {
  // The first record with a model starts tracking: what every provider counted so far
  // is earlier usage, kept before this record adds to it.
  if (model && *model && !models_.tracking()) startTracking();
  const char* key = models_.spendKey(model);
  recordTokens(name, d.in, d.out, d.cacheR, d.cacheW, periodKey, tag);
  // Every row follows its own provider's period: a provider that rolled zeroes its
  // rows' period counters, one that first synced carries its pre-sync rows along.
  models_.syncPeriods(&UsageLedger::periodOf, this);
  if (!*key) return;   // before tracking: provider level only (earlier usage)
  // The model row follows its provider's period (already rolled above): when the
  // provider kept its stored period on an unsynced record (key 0), the row files
  // under that same period, so the two never disagree about "this period".
  const ProviderBudget* e = find(name);
  if (!e) return;   // the provider ledger is full: nothing to attribute against
  models_.record(name.c_str(), key, d, e->periodKey, dayKey);
}

void UsageLedger::recordTokens(const std::string& name, uint64_t tokIn, uint64_t tokOut,
                               uint32_t periodKey, const std::string& tag) {
  recordTokens(name, tokIn, tokOut, periodKey);   // budget math is unchanged
  if (tag.empty()) return;                        // untagged spend: no attribution row
  for (auto& t : tags_) {
    if (t.prov == name && t.tag == tag) {
      t.tokIn  += tokIn;
      t.tokOut += tokOut;
      return;
    }
  }
  if (tags_.size() >= kMaxTags) return;           // bounded audit trail, never a crash
  TagUsage t;
  t.prov = name;
  t.tag = tag;
  t.tokIn = tokIn;
  t.tokOut = tokOut;
  tags_.push_back(std::move(t));
}

void UsageLedger::recordCall(const std::string& name, uint32_t periodKey) {
  ProviderBudget* e = ensure(name);
  if (!e) return;
  rollTo(e, periodKey);
  e->calls += 1;
  e->totalCalls += 1;   // all-time - never rolls
}

void UsageLedger::setRates(const std::string& name, uint32_t centsPerMIn,
                           uint32_t centsPerMOut, uint32_t centsPerKCalls) {
  ProviderBudget* e = ensure(name);
  if (!e) return;
  e->centsPerMIn = centsPerMIn;
  e->centsPerMOut = centsPerMOut;
  e->centsPerKCalls = centsPerKCalls;
}

void UsageLedger::setLimits(const std::string& name, uint64_t tokenLimit,
                            uint32_t callLimit, uint8_t resetDay,
                            uint64_t centsLimit) {
  ProviderBudget* e = ensure(name);
  if (!e) return;
  e->tokenLimit = tokenLimit;
  e->callLimit = callLimit;
  if (resetDay < 1) resetDay = 1;
  if (resetDay > 28) resetDay = 28;
  e->resetDay = resetDay;
  e->centsLimit = centsLimit;
}

bool UsageLedger::overTokenBudget(const std::string& name) const {
  const ProviderBudget* e = find(name);
  return e && e->tokenLimit > 0 && e->tokens >= e->tokenLimit;
}

bool UsageLedger::overCallBudget(const std::string& name) const {
  const ProviderBudget* e = find(name);
  return e && e->callLimit > 0 && e->calls >= e->callLimit;
}

bool UsageLedger::overCostBudget(const std::string& name) const {
  const ProviderBudget* e = find(name);
  if (!e || e->centsLimit == 0) return false;
  // D1 (2026-10): every price tier counts, the built-in estimates included; only a
  // provider whose whole spend resolves to no price at all ("cannot tell") gates
  // nothing. Reversing D1 is this one line: test the provider's own rates instead.
  PriceSummary sum;
  const uint64_t cents = priceProvider(name, &sum);
  return sum.anyPriced && cents >= e->centsLimit;
}

// ---- per-model pricing (2026-10) ------------------------------------------------

const char* rateSrcName(RateSrc s) {
  switch (s) {
    case RateSrc::Owner:        return "owner";
    case RateSrc::Router:       return "router";
    case RateSrc::Provider:     return "provider";
    case RateSrc::BuiltinModel:
    case RateSrc::BuiltinProv:  return "est";
    case RateSrc::None:         break;
  }
  return "none";
}

namespace {

constexpr uint64_t kM = 1000000ULL;

uint64_t mulSat(uint64_t a, uint64_t b, bool& sat) {
  if (a != 0 && b > UINT64_MAX / a) { sat = true; return UINT64_MAX; }
  return a * b;
}
uint64_t addSat(uint64_t a, uint64_t b, bool& sat) {
  if (UINT64_MAX - a < b) { sat = true; return UINT64_MAX; }
  return a + b;
}

// floor(t * r / 1e6) without an intermediate overflow: with t = a*M + b and
// r = c*M + d it is a*r + b*c + floor(b*d / M) (b, d < M, so b*c and b*d fit).
uint64_t costMicroSat(uint64_t t, uint64_t r, bool& sat) {
  const uint64_t a = t / kM, b = t % kM, c = r / kM, d = r % kM;
  uint64_t v = mulSat(a, r, sat);
  v = addSat(v, mulSat(b, c, sat), sat);
  return addSat(v, b * d / kM, sat);
}

// Pick one field through the tiers: the first non-zero value wins.
void pickField(uint64_t& v, RateSrc& src, uint64_t cand, RateSrc candSrc) {
  if (v == 0 && cand != 0) { v = cand; src = candSrc; }
}

// Raise one field to a higher candidate (a tie keeps the tier offered first).
void raiseField(uint64_t& v, RateSrc& src, uint64_t cand, RateSrc candSrc) {
  if (cand > v) { v = cand; src = candSrc; }
}

// The running micro-USD total of one provider's estimate. Each component's cents are
// floor(running total) - floor(running total before it), so the parts add up
// exactly to the one floor of the whole; a saturated total reads UINT64_MAX.
struct MicroSum {
  uint64_t micro = 0, cents = 0;
  bool     sat = false;
  // Tokens at a rate; cache reads at 0.1x and writes at 1.25x of the input rate.
  void addTokens(const TokenDelta& t, const EffRate& r) {
    micro = addSat(micro, costMicroSat(t.in, r.in, sat), sat);
    micro = addSat(micro, costMicroSat(t.out, r.out, sat), sat);
    micro = addSat(micro, costMicroSat(t.cacheR, r.in, sat) / 10, sat);
    micro = addSat(micro, costMicroSat(mulSat(t.cacheW, 5, sat), r.in, sat) / 4, sat);
  }
  void addCalls(uint64_t calls, uint64_t microPerK) {
    micro = addSat(micro, mulSat(calls, microPerK, sat) / 1000ULL, sat);
  }
  uint64_t take() {   // this component's cents
    const uint64_t now = sat ? UINT64_MAX : micro / 10000ULL;
    const uint64_t part = now - cents;
    cents = now;
    return part;
  }
};

// Whether a component with this spend resolved to a price, and whether that price
// leaned on a built-in estimate.
void notePriced(PriceSummary& s, const TokenDelta& t, const EffRate& r) {
  const bool inSide = (t.in || t.cacheR || t.cacheW) && r.in;
  const bool outSide = t.out && r.out;
  if (inSide || outSide) s.anyPriced = true;
  if ((inSide && rateSrcIsEstimate(r.srcIn)) || (outSide && rateSrcIsEstimate(r.srcOut)))
    s.approx = true;
}

// The legacy formula's terms cannot wrap (so the fast path stays exact).
bool legacyFits(const ProviderBudget& e) {
  const uint64_t r = e.centsPerMIn > e.centsPerMOut ? e.centsPerMIn : e.centsPerMOut;
  const uint64_t lim = r ? UINT64_MAX / (r * 5ULL) : UINT64_MAX;
  return e.tokensIn <= lim && e.tokensOut <= lim && e.cacheRead <= lim && e.cacheWrite <= lim;
}

TokenDelta periodTokens(const ProviderBudget& e) {
  TokenDelta t;
  t.in = e.tokensIn;
  t.out = e.tokensOut;
  t.cacheR = e.cacheRead;
  t.cacheW = e.cacheWrite;
  return t;
}

uint64_t minU(uint64_t a, uint64_t b) { return a < b ? a : b; }

// a - b per field, floored at 0.
TokenDelta minus(const TokenDelta& a, const TokenDelta& b) {
  TokenDelta t;
  t.in = a.in > b.in ? a.in - b.in : 0;
  t.out = a.out > b.out ? a.out - b.out : 0;
  t.cacheR = a.cacheR > b.cacheR ? a.cacheR - b.cacheR : 0;
  t.cacheW = a.cacheW > b.cacheW ? a.cacheW - b.cacheW : 0;
  return t;
}

bool sameDelta(const TokenDelta& a, const TokenDelta& b) {
  return a.in == b.in && a.out == b.out && a.cacheR == b.cacheR && a.cacheW == b.cacheW;
}

// A row's share, clamped field by field to what its provider still has unclaimed.
TokenDelta claim(const ModelSpend& r, TokenDelta& rem) {
  TokenDelta t;
  t.in = minU(r.tokIn, rem.in);
  t.out = minU(r.tokOut, rem.out);
  t.cacheR = minU(r.cacheR, rem.cacheR);
  t.cacheW = minU(r.cacheW, rem.cacheW);
  rem.in -= t.in;
  rem.out -= t.out;
  rem.cacheR -= t.cacheR;
  rem.cacheW -= t.cacheW;
  return t;
}

// The bit-exact fast path: the provider's own rates fully price its spend and no
// model row claims any of it -> ProviderBudget::estCents(), verbatim.
bool fastPathApplies(const ProviderBudget& e, bool anyRow) {
  const bool noTokens = periodTokens(e).empty();
  const bool tokensPriced = (e.centsPerMIn && e.centsPerMOut) || noTokens;
  const bool callsPriced = e.calls == 0 || e.centsPerKCalls != 0;
  return !anyRow && tokensPriced && callsPriced && legacyFits(e);
}

void fillFastPath(const ProviderBudget& e, PriceSummary& s) {
  s.fastPath = true;
  s.cents = e.estCents();
  s.callCents = (uint64_t)e.calls * e.centsPerKCalls / 1000ULL;
  s.resCents = s.cents - s.callCents;
  const TokenDelta t = periodTokens(e);
  s.resIn = t.in;
  s.resOut = t.out;
  s.resCr = t.cacheR;
  s.resCw = t.cacheW;
  s.resRate.in = (uint64_t)e.centsPerMIn * 10000ULL;
  s.resRate.out = (uint64_t)e.centsPerMOut * 10000ULL;
  s.resRate.srcIn = e.centsPerMIn ? RateSrc::Provider : RateSrc::None;
  s.resRate.srcOut = e.centsPerMOut ? RateSrc::Provider : RateSrc::None;
  s.anyPriced = !t.empty() || (e.calls && e.centsPerKCalls);
}

}  // namespace

uint64_t costMicro(uint64_t tokens, uint64_t microPerM) {
  bool sat = false;
  const uint64_t v = costMicroSat(tokens, microPerM, sat);
  return sat ? UINT64_MAX : v;
}

EffRate UsageLedger::effectiveRate(const std::string& prov, const char* model) const {
  EffRate r;
  if (!model) model = "";
  const ProviderBudget* e = find(prov);
  const char* p = prov.c_str();
  if (const ModelRate* o = models_.ownerRate(p, model)) {
    pickField(r.in, r.srcIn, o->microIn, RateSrc::Owner);
    pickField(r.out, r.srcOut, o->microOut, RateSrc::Owner);
  }
  if (prov == "cumulo") {
    if (const ModelRate* rt = models_.routerRate(p, model)) {
      pickField(r.in, r.srcIn, rt->microIn, RateSrc::Router);
      pickField(r.out, r.srcOut, rt->microOut, RateSrc::Router);
    }
  }
  if (e) {
    pickField(r.in, r.srcIn, (uint64_t)e->centsPerMIn * 10000ULL, RateSrc::Provider);
    pickField(r.out, r.srcOut, (uint64_t)e->centsPerMOut * 10000ULL, RateSrc::Provider);
  }
  if (const BuiltinPrice* b = builtinModelPrice(p, model)) {
    pickField(r.in, r.srcIn, b->microIn, RateSrc::BuiltinModel);
    pickField(r.out, r.srcOut, b->microOut, RateSrc::BuiltinModel);
  }
  if (const BuiltinPrice* b = builtinDefaultPrice(p, model)) {
    pickField(r.in, r.srcIn, b->microIn, RateSrc::BuiltinProv);
    pickField(r.out, r.srcOut, b->microOut, RateSrc::BuiltinProv);
  }
  return r;
}

EffRate UsageLedger::residualRate(const std::string& prov) const {
  EffRate r;
  const char* p = prov.c_str();
  if (const ProviderBudget* e = find(prov)) {
    pickField(r.in, r.srcIn, (uint64_t)e->centsPerMIn * 10000ULL, RateSrc::Provider);
    pickField(r.out, r.srcOut, (uint64_t)e->centsPerMOut * 10000ULL, RateSrc::Provider);
  }
  // Fail-closed: the HIGHEST model price the owner (or the router) set for this
  // provider, per field, so earlier usage is never priced below one of those.
  uint64_t hiIn = 0, hiOut = 0;
  RateSrc hiInSrc = RateSrc::None, hiOutSrc = RateSrc::None;
  auto scan = [&](const std::vector<ModelRate>& v, RateSrc src) {
    for (const ModelRate& m : v) {
      if (std::strcmp(m.prov, p) != 0) continue;
      if (m.microIn > hiIn) { hiIn = m.microIn; hiInSrc = src; }
      if (m.microOut > hiOut) { hiOut = m.microOut; hiOutSrc = src; }
    }
  };
  scan(models_.ownerRates(), RateSrc::Owner);
  if (prov == "cumulo") scan(models_.routerRates(), RateSrc::Router);
  pickField(r.in, r.srcIn, hiIn, hiInSrc);
  pickField(r.out, r.srcOut, hiOut, hiOutSrc);
  if (const BuiltinPrice* b = builtinDefaultPrice(p, "")) {
    pickField(r.in, r.srcIn, b->microIn, RateSrc::BuiltinProv);
    pickField(r.out, r.srcOut, b->microOut, RateSrc::BuiltinProv);
  }
  return r;
}

EffRate UsageLedger::otherRate(const std::string& prov) const {
  EffRate r;
  const char* p = prov.c_str();
  auto raise = [&r](uint64_t in, uint64_t out, RateSrc s) {
    raiseField(r.in, r.srcIn, in, s);
    raiseField(r.out, r.srcOut, out, s);
  };
  for (const ModelRate& m : models_.ownerRates())
    if (std::strcmp(m.prov, p) == 0) raise(m.microIn, m.microOut, RateSrc::Owner);
  if (prov == "cumulo")
    for (const ModelRate& m : models_.routerRates()) raise(m.microIn, m.microOut, RateSrc::Router);
  if (const ProviderBudget* e = find(prov))
    raise((uint64_t)e->centsPerMIn * 10000ULL, (uint64_t)e->centsPerMOut * 10000ULL,
          RateSrc::Provider);
  uint32_t bIn = 0, bOut = 0;
  builtinHighestPrice(p, bIn, bOut);
  raise(bIn, bOut, RateSrc::BuiltinModel);
  if (const BuiltinPrice* b = builtinDefaultPrice(p, ""))
    raise(b->microIn, b->microOut, RateSrc::BuiltinProv);
  return r;
}

EffRate UsageLedger::rowRate(const std::string& prov, const char* model) const {
  if (model && std::strcmp(model, kOtherModel) == 0) return otherRate(prov);
  return effectiveRate(prov, model);
}

void UsageLedger::startTracking() {
  models_.markTracking();
  for (const auto& e : entries_) {
    const TokenDelta t = periodTokens(e);
    if (t.empty() && !e.totalIn && !e.totalOut) continue;
    ProvTrack* tr = models_.trackFor(e.name.c_str());
    if (!tr) continue;
    tr->periodKey = e.periodKey;
    tr->pre = t;
    tr->lost = TokenDelta{};
    tr->preTotIn = e.totalIn;
    tr->preTotOut = e.totalOut;
  }
}

TokenDelta UsageLedger::earlierUsage(const ProviderBudget& e) const {
  const TokenDelta all = periodTokens(e);
  if (!models_.tracking()) return all;
  TokenDelta pre;
  const ProvTrack* t = models_.track(e.name.c_str());
  if (!t || t->periodKey != e.periodKey) return pre;
  pre.in = minU(t->pre.in, all.in);
  pre.out = minU(t->pre.out, all.out);
  pre.cacheR = minU(t->pre.cacheR, all.cacheR);
  pre.cacheW = minU(t->pre.cacheW, all.cacheW);
  return pre;
}

uint64_t UsageLedger::callRate(const std::string& prov, RateSrc* src) const {
  uint64_t v = 0;
  RateSrc s = RateSrc::None;
  if (const ProviderBudget* e = find(prov))
    pickField(v, s, (uint64_t)e->centsPerKCalls * 10000ULL, RateSrc::Provider);
  if (const BuiltinPrice* b = builtinSlotDefault(prov.c_str()))
    pickField(v, s, b->microPerKCalls, RateSrc::BuiltinProv);
  if (src) *src = s;
  return v;
}

namespace {

bool rowOf(const ModelSpend& r, const ProviderBudget& e) {
  return std::strcmp(r.prov, e.name.c_str()) == 0 && UsageLedger::inPeriod(e, r);
}

bool anyInPeriodRow(const UsageLedger& L, const ProviderBudget& e) {
  for (const auto& r : L.models().rows())
    if (rowOf(r, e) && (r.tokIn || r.tokOut || r.cacheR || r.cacheW)) return true;
  return false;
}

// The in-period model rows of one provider, each at its own price, clamped to what
// the provider recorded since tracking began (a row can never claim more). `rem`
// leaves with what no row claimed: spend whose model was lost.
void priceRows(const UsageLedger& L, const ProviderBudget& e, MicroSum& m, TokenDelta& rem,
               PriceSummary& s, RowPrice* out) {
  const auto& rows = L.models().rows();
  for (size_t i = 0; i < rows.size() && i < ModelLedger::kMaxRows; ++i) {
    if (!rowOf(rows[i], e)) continue;
    const TokenDelta t = claim(rows[i], rem);
    if (t.empty()) continue;
    const EffRate rate = L.rowRate(e.name, rows[i].model);
    m.addTokens(t, rate);
    notePriced(s, t, rate);
    const uint64_t part = m.take();
    if (out) {
      out[i].take = t;
      out[i].rate = rate;
      out[i].cents = part;
    }
  }
}

// One part of the spend no row prices, at one rate; its cents are the running floor's step.
uint64_t pricePart(MicroSum& m, PriceSummary& s, const TokenDelta& t, const EffRate& r) {
  m.addTokens(t, r);
  notePriced(s, t, r);
  return m.take();
}

void priceCalls(MicroSum& m, PriceSummary& s) {
  if (s.calls) {
    m.addCalls(s.calls, s.callRate);
    if (s.callRate) {
      s.anyPriced = true;
      if (rateSrcIsEstimate(s.callSrc)) s.approx = true;
    }
  }
  s.callCents = m.take();
}

}  // namespace

uint64_t UsageLedger::priceProvider(const std::string& name, PriceSummary* out,
                                    RowPrice* rows) const {
  PriceSummary s;
  if (rows)
    for (size_t i = 0; i < ModelLedger::kMaxRows; ++i) rows[i] = RowPrice{};
  const ProviderBudget* e = find(name);
  if (e) {
    s.calls = e->calls;
    s.callRate = callRate(name, &s.callSrc);
    const TokenDelta pre = earlierUsage(*e);
    TokenDelta rem = minus(periodTokens(*e), pre);   // spend since tracking began
    if (rem.empty() && fastPathApplies(*e, anyInPeriodRow(*this, *e))) {
      fillFastPath(*e, s);
    } else {
      MicroSum m;
      priceRows(*this, *e, m, rem, s, rows);
      // What no row accounts for lost its model (gate G1-G3): any model's price.
      s.othIn = rem.in;
      s.othOut = rem.out;
      s.othCr = rem.cacheR;
      s.othCw = rem.cacheW;
      if (!rem.empty()) s.othRate = otherRate(name);
      s.othCents = pricePart(m, s, rem, s.othRate);
      s.resIn = pre.in;
      s.resOut = pre.out;
      s.resCr = pre.cacheR;
      s.resCw = pre.cacheW;
      s.resRate = residualRate(name);
      s.resCents = pricePart(m, s, pre, s.resRate);
      priceCalls(m, s);
      s.cents = m.cents;
    }
  }
  if (out) *out = s;
  return s.cents;
}

bool UsageLedger::anyPriced(const std::string& name) const {
  PriceSummary s;
  priceProvider(name, &s);
  return s.anyPriced;
}

namespace {
// Whether the in-period rows of `e` add up exactly to its spend since tracking began
// that is not already lost spend: every record files under a row once tracking has
// started, so they always do unless a store failed.
// How a provider's in-period rows compare with its spend since tracking began that is
// not already lost spend (every record files under a row once tracking has started,
// so they match unless the stores diverged).
enum class RowsVs : uint8_t {
  Match,    // the same, field by field
  Behind,   // no field above it (`gap` = what the rows miss): a stale spend file
  Ahead,    // some field above it (or saturated): the counters lack spend the rows hold
};
RowsVs rowsVsCounters(const UsageLedger& L, const ProviderBudget& e, TokenDelta& gap) {
  bool sat = false;
  TokenDelta have;
  for (const auto& r : L.models().rows()) {
    if (!rowOf(r, e)) continue;
    have.in = addSat(have.in, r.tokIn, sat);
    have.out = addSat(have.out, r.tokOut, sat);
    have.cacheR = addSat(have.cacheR, r.cacheR, sat);
    have.cacheW = addSat(have.cacheW, r.cacheW, sat);
  }
  TokenDelta want = minus(periodTokens(e), L.earlierUsage(e));
  const ProvTrack* t = L.models().track(e.name.c_str());
  if (t && t->periodKey == e.periodKey) want = minus(want, t->lost);
  if (!sat && sameDelta(have, want)) return RowsVs::Match;
  if (sat || !minus(have, want).empty()) return RowsVs::Ahead;
  gap = minus(want, have);
  return RowsVs::Behind;
}

// Earlier usage a track keeps above what the stored counters hold is spend the
// counters never kept (a usgLedger write that did not land before tracking started)
// or counters that are not the ones it described (a settings-only reset, then new
// spend): either way none of it can be trusted, and taking later spend for it would
// price that spend low. It is dropped (that spend then reads as lost: fail-closed).
void dropStaleEarlier(ProvTrack& t, const ProviderBudget& e) {
  const bool stale = t.pre.in > e.tokensIn || t.pre.out > e.tokensOut ||
                     t.pre.cacheR > e.cacheRead || t.pre.cacheW > e.cacheWrite;
  if (stale) t.pre = TokenDelta{};
  if (stale || t.preTotIn > e.totalIn || t.preTotOut > e.totalOut) t.preTotIn = t.preTotOut = 0;
}

// A track goes with its provider; its period parts count only in that period (a
// pre-sync track cannot tell which period its spend was carried into: dropped).
// Returns whether a track of a provider with no entry was dropped (a settings-only
// reset): that must not outlive this boot (the caller stores the result).
bool rebaseTracks(UsageLedger& L) {
  ModelLedger& M = L.models();
  bool orphaned = false;
  for (size_t i = M.tracks().size(); i-- > 0;) {
    const std::string prov = M.tracks()[i].prov;   // load time only
    const ProviderBudget* e = L.find(prov);
    if (!e) {
      M.dropTrack(prov.c_str());
      orphaned = true;
      continue;
    }
    ProvTrack* t = M.trackFor(prov.c_str());   // present: found, never made
    if (t->periodKey != e->periodKey) {
      t->periodKey = e->periodKey;
      t->pre = t->lost = TokenDelta{};
    }
    dropStaleEarlier(*t, *e);
  }
  return orphaned;
}
}  // namespace

// The rows may hold spend the stored counters lack (gate G1, G3): no row's
// attribution can be trusted, so all of the provider's spend since tracking began is
// lost spend, priced at otherRate.
void UsageLedger::loseAttribution(const ProviderBudget& e) {
  const auto& rows = models_.rows();
  for (size_t i = 0; i < rows.size(); ++i)
    if (rowOf(rows[i], e)) models_.dropAttribution(i);
  const TokenDelta lost = minus(periodTokens(e), earlierUsage(e));
  ProvTrack* t = models_.trackFor(e.name.c_str());
  if (!t) return;   // a provider id the ledger cannot carry: priced the same anyway
  t->periodKey = e.periodKey;
  t->lost = lost;
}

// The rows are only behind the counters (gate G3: a spend-file write that did not
// land, or spend an earlier release recorded after a downgrade): every row is still
// true, so the rows stay and only what they miss is lost spend.
void UsageLedger::addLost(const ProviderBudget& e, const TokenDelta& gap) {
  ProvTrack* t = models_.trackFor(e.name.c_str());
  if (!t) return;   // a provider id the ledger cannot carry: priced the same anyway
  t->periodKey = e.periodKey;   // tracks are already aligned (rebaseTracks); a new one starts here
  bool sat = false;
  t->lost.in = addSat(t->lost.in, gap.in, sat);
  t->lost.out = addSat(t->lost.out, gap.out, sat);
  t->lost.cacheR = addSat(t->lost.cacheR, gap.cacheR, sat);
  t->lost.cacheW = addSat(t->lost.cacheW, gap.cacheW, sat);
}

bool UsageLedger::rebaseModelRows() {
  const auto& rows = models_.rows();
  for (size_t i = 0; i < rows.size(); ++i) {
    const ProviderBudget* e = find(std::string(rows[i].prov));   // load time only
    if (e && inPeriod(*e, rows[i])) continue;
    models_.clearPeriodSpend(i);
    // A row of an existing provider follows that provider's period (its next record
    // then lands in the period the provider counts in, even before the clock syncs).
    if (e) models_.setRowPeriod(i, e->periodKey);
  }
  const bool orphaned = rebaseTracks(*this);
  if (!models_.tracking()) return orphaned;
  for (const auto& e : entries_) {
    TokenDelta gap;
    const RowsVs v = rowsVsCounters(*this, e, gap);
    if (v == RowsVs::Match) continue;
    // Behind is safe to keep only when the counters' latest store landed: then they
    // hold every record the rows hold (a later landed write is a superset).
    if (v == RowsVs::Behind && !models_.countersBehind()) addLost(e, gap);
    else loseAttribution(e);
  }
  return orphaned;
}

// ---- serialization -----------------------------------------------------------
// Fields are fixed-order and numeric except the leading name, so a truncated/garbled
// blob (NVS wear, older firmware) parses defensively: a short group is skipped, never
// crashes. name is assumed free of '|' and ';' (provider ids are [a-z0-9_-]).

static void appendU64(std::string& out, uint64_t v) {
  char buf[24];
  int i = 0;
  if (v == 0) buf[i++] = '0';
  while (v) { buf[i++] = char('0' + (v % 10)); v /= 10; }
  while (i > 0) out.push_back(buf[--i]);
}

// Shared '|' splitter: fills up to maxParts, returns the count. Extra bars beyond
// maxParts are folded into the last part (callers never read past their count).
static int splitBars(const std::string& group, std::string* parts, int maxParts) {
  int np = 0;
  size_t p = 0;
  while (np < maxParts) {
    size_t bar = group.find('|', p);
    if (bar == std::string::npos) { parts[np++] = group.substr(p); break; }
    parts[np++] = group.substr(p, bar - p);
    p = bar + 1;
  }
  return np;
}

std::string UsageLedger::serialize() const {
  std::string out;
  for (const auto& e : entries_) {
    if (!out.empty()) out.push_back(';');
    out += e.name;
    out.push_back('|'); appendU64(out, e.periodKey);
    out.push_back('|'); appendU64(out, e.tokens);
    out.push_back('|'); appendU64(out, e.calls);
    out.push_back('|'); appendU64(out, e.tokenLimit);
    out.push_back('|'); appendU64(out, e.callLimit);
    out.push_back('|'); appendU64(out, (uint64_t)e.resetDay);
    // Cost-extension tail (2026-07-16): absent in old blobs, reads as 0.
    out.push_back('|'); appendU64(out, e.tokensIn);
    out.push_back('|'); appendU64(out, e.tokensOut);
    out.push_back('|'); appendU64(out, e.totalIn);
    out.push_back('|'); appendU64(out, e.totalOut);
    out.push_back('|'); appendU64(out, e.totalCalls);
    out.push_back('|'); appendU64(out, e.centsPerMIn);
    out.push_back('|'); appendU64(out, e.centsPerMOut);
    out.push_back('|'); appendU64(out, e.centsPerKCalls);
    // W16 tail: the $ ceiling (cents). Absent in older blobs -> reads 0 (no
    // limit); an OLDER deserializer's fixed parts[] simply ignores it.
    out.push_back('|'); appendU64(out, e.centsLimit);
    // v4.1.3 tail: period prompt-cache counters (same append-only compat rules).
    out.push_back('|'); appendU64(out, e.cacheRead);
    out.push_back('|'); appendU64(out, e.cacheWrite);
  }
  // Attribution tail: '#'-prefixed 4-field groups. tag is [a-z0-9_:-] by
  // construction ("turn"/"synthesis"/"loop:<id>"/"spawn:<backend>") - no '|'/';'.
  for (const auto& t : tags_) {
    if (!out.empty()) out.push_back(';');
    out.push_back('#');
    out += t.prov;
    out.push_back('|'); out += t.tag;
    out.push_back('|'); appendU64(out, t.tokIn);
    out.push_back('|'); appendU64(out, t.tokOut);
  }
  return out;
}

void UsageLedger::deserialize(const std::string& s) {
  entries_.clear();
  tags_.clear();
  size_t i = 0, n = s.size();
  while (i < n) {
    size_t semi = s.find(';', i);
    if (semi == std::string::npos) semi = n;
    std::string group = s.substr(i, semi - i);
    i = semi + 1;
    if (group.empty()) continue;
    if (group[0] == '#') {   // attribution tag group: #prov|tag|tokIn|tokOut
      std::string tp[4];
      int tn = splitBars(group.substr(1), tp, 4);
      if (tn < 4 || tp[0].empty() || tp[1].empty()) continue;
      if (tags_.size() >= kMaxTags) continue;
      TagUsage t;
      t.prov   = tp[0];
      t.tag    = tp[1];
      t.tokIn  = (uint64_t)strtoull(tp[2].c_str(), nullptr, 10);
      t.tokOut = (uint64_t)strtoull(tp[3].c_str(), nullptr, 10);
      tags_.push_back(std::move(t));
      continue;
    }
    std::string parts[18];
    int np = splitBars(group, parts, 18);
    if (np < 7 || parts[0].empty()) continue;   // malformed group -> skip
    if (entries_.size() >= kMaxProviders) break;
    ProviderBudget e;
    e.name       = parts[0];
    e.periodKey  = (uint32_t)strtoul(parts[1].c_str(), nullptr, 10);
    e.tokens     = (uint64_t)strtoull(parts[2].c_str(), nullptr, 10);
    e.calls      = (uint32_t)strtoul(parts[3].c_str(), nullptr, 10);
    e.tokenLimit = (uint64_t)strtoull(parts[4].c_str(), nullptr, 10);
    e.callLimit  = (uint32_t)strtoul(parts[5].c_str(), nullptr, 10);
    unsigned rd  = (unsigned)strtoul(parts[6].c_str(), nullptr, 10);
    e.resetDay   = (uint8_t)(rd < 1 ? 1 : (rd > 28 ? 28 : rd));
    // Cost-extension tail - tolerate its absence (an old 7-field blob) so the
    // upgrade needs no NVS migration; missing fields read 0. EXCEPT the period
    // split: a legacy blob's `tokens` is real period spend, so attribute it to
    // the input side - otherwise the "This period" tile reads 0 while the budget
    // bar on the same page shows the true total (review: contradictory surfaces).
    if (np == 7) e.tokensIn = e.tokens;
    if (np > 7)  e.tokensIn       = (uint64_t)strtoull(parts[7].c_str(),  nullptr, 10);
    if (np > 8)  e.tokensOut      = (uint64_t)strtoull(parts[8].c_str(),  nullptr, 10);
    if (np > 9)  e.totalIn        = (uint64_t)strtoull(parts[9].c_str(),  nullptr, 10);
    if (np > 10) e.totalOut       = (uint64_t)strtoull(parts[10].c_str(), nullptr, 10);
    if (np > 11) e.totalCalls     = (uint64_t)strtoull(parts[11].c_str(), nullptr, 10);
    if (np > 12) e.centsPerMIn    = (uint32_t)strtoul(parts[12].c_str(),  nullptr, 10);
    if (np > 13) e.centsPerMOut   = (uint32_t)strtoul(parts[13].c_str(),  nullptr, 10);
    if (np > 14) e.centsPerKCalls = (uint32_t)strtoul(parts[14].c_str(),  nullptr, 10);
    if (np > 15) e.centsLimit     = (uint64_t)strtoull(parts[15].c_str(), nullptr, 10);
    if (np > 16) e.cacheRead      = (uint64_t)strtoull(parts[16].c_str(), nullptr, 10);
    if (np > 17) e.cacheWrite     = (uint64_t)strtoull(parts[17].c_str(), nullptr, 10);
    entries_.push_back(std::move(e));
  }
}

// ---- UsageHistory --------------------------------------------------------------

namespace {
bool histModelOk(const char* m) {
  return m && (!*m || std::strcmp(m, kOtherModel) == 0 || modelKeySafe(m));
}
uint64_t bucketTokens(const DayUsage& e) { return e.tokIn + e.tokOut + e.cacheR + e.cacheW; }
void mergeInto(DayUsage& dst, const DayUsage& src) {
  dst.tokIn += src.tokIn;
  dst.tokOut += src.tokOut;
  dst.calls += src.calls;
  dst.cacheR += src.cacheR;
  dst.cacheW += src.cacheW;
}
void setModel(DayUsage& e, const char* m) {
  const size_t n = std::strlen(m);
  const size_t k = n < kModelKeyCap - 1 ? n : kModelKeyCap - 1;
  std::memcpy(e.model, m, k);
  e.model[k] = 0;
}
}  // namespace

DayUsage* UsageHistory::find(uint32_t dayKey, const std::string& prov, const char* model) {
  for (auto& e : entries_)
    if (e.dayKey == dayKey && e.prov == prov && std::strcmp(e.model, model) == 0) return &e;
  return nullptr;
}

size_t UsageHistory::countDay(uint32_t dayKey) const {
  size_t n = 0;
  for (const auto& e : entries_)
    if (e.dayKey == dayKey) ++n;
  return n;
}

DayUsage* UsageHistory::push(const std::string& prov, const char* model, uint32_t dayKey) {
  if (entries_.size() >= kMaxEntries) {
    // Evict the OLDEST bucket to make room (the graphs only show recent windows).
    size_t oldest = 0;
    for (size_t i = 1; i < entries_.size(); ++i)
      if (entries_[i].dayKey < entries_[oldest].dayKey) oldest = i;
    entries_.erase(entries_.begin() + (std::ptrdiff_t)oldest);
  }
  entries_.emplace_back();
  DayUsage& d = entries_.back();
  d.dayKey = dayKey;
  d.prov = prov;   // a provider id fits the string's inline buffer: no allocation
  setModel(d, model);
  return &d;
}

// Free one bucket on a full day: the provider with the most buckets that day folds
// its smallest named bucket into its ~other bucket (or, with no ~other bucket yet,
// its two smallest named buckets become one ~other bucket). Provider totals per day
// are unchanged; only model attribution is coarsened.
size_t UsageHistory::countDayProv(uint32_t dayKey, const std::string& prov) const {
  size_t n = 0;
  for (const auto& e : entries_)
    if (e.dayKey == dayKey && e.prov == prov) ++n;
  return n;
}

namespace {
bool isOtherBucket(const DayUsage& e) { return std::strcmp(e.model, kOtherModel) == 0; }

// The two smallest named buckets of (day, prov), as indexes (n = none).
void twoSmallest(const std::vector<DayUsage>& v, uint32_t day, const std::string& prov,
                 size_t& a, size_t& b) {
  const size_t n = v.size();
  a = b = n;
  for (size_t i = 0; i < n; ++i) {
    if (v[i].dayKey != day || v[i].prov != prov || isOtherBucket(v[i])) continue;
    if (a == n || bucketTokens(v[i]) < bucketTokens(v[a])) { b = a; a = i; }
    else if (b == n || bucketTokens(v[i]) < bucketTokens(v[b])) b = i;
  }
}
}  // namespace

bool UsageHistory::foldOnePair(uint32_t dayKey) {
  const DayUsage* busiest = nullptr;   // the provider with the most buckets that day
  size_t most = 0;
  for (const auto& e : entries_) {
    if (e.dayKey != dayKey) continue;
    const size_t n = countDayProv(dayKey, e.prov);
    if (n > most) { most = n; busiest = &e; }
  }
  if (most < 2) return false;
  const std::string prov = busiest->prov;   // a provider id: inline, no allocation
  size_t a, b;
  twoSmallest(entries_, dayKey, prov, a, b);
  if (a == entries_.size()) return false;
  if (DayUsage* other = find(dayKey, prov, kOtherModel)) {
    mergeInto(*other, entries_[a]);
    entries_.erase(entries_.begin() + (std::ptrdiff_t)a);
    return true;
  }
  if (b == entries_.size()) return false;
  mergeInto(entries_[a], entries_[b]);
  setModel(entries_[a], kOtherModel);
  entries_.erase(entries_.begin() + (std::ptrdiff_t)b);
  return true;
}

// The bucket a (day, prov, model) record lands in, creating it within the per-day
// cap: a 13th distinct (prov, model) on a day goes to (day, prov, ~other).
DayUsage* UsageHistory::bucketFor(const std::string& prov, const char* model, uint32_t dayKey) {
  if (DayUsage* e = find(dayKey, prov, model)) return e;
  if (countDay(dayKey) < kMaxPerDay) return push(prov, model, dayKey);
  if (DayUsage* o = find(dayKey, prov, kOtherModel)) return o;
  // No ~other bucket for this provider yet and no room: its smallest named bucket
  // that day becomes it (spend kept, attribution coarsened).
  DayUsage* smallest = nullptr;
  for (auto& e : entries_) {
    if (e.dayKey != dayKey || e.prov != prov) continue;
    if (!smallest || bucketTokens(e) < bucketTokens(*smallest)) smallest = &e;
  }
  if (smallest) {
    setModel(*smallest, kOtherModel);
    return smallest;
  }
  // The provider has no bucket that day at all: fold another provider's pair to
  // make room (only impossible with more than kMaxPerDay providers in one day, when
  // the bucket is created anyway - spend is never dropped).
  foldOnePair(dayKey);
  return push(prov, model, dayKey);
}

void UsageHistory::record(const std::string& prov, const char* model, uint32_t dayKey,
                          const TokenDelta& d, uint32_t calls) {
  if (prov.empty() || dayKey == 0) return;   // no sane clock -> no dated bucket
  if (!histModelOk(model)) model = "";       // an unsafe key records unattributed
  if (entries_.capacity() < kMaxEntries) entries_.reserve(kMaxEntries);   // once
  DayUsage* e = bucketFor(prov, model, dayKey);
  e->tokIn += d.in;
  e->tokOut += d.out;
  e->calls += calls;
  e->cacheR += d.cacheR;
  e->cacheW += d.cacheW;
}

int UsageHistory::prune(uint32_t todayKey, uint32_t keepDays) {
  if (keepDays == 0) keepDays = 1;
  const uint32_t cutoff = (todayKey > keepDays) ? (todayKey - keepDays) : 0;
  int removed = 0;
  for (size_t i = entries_.size(); i-- > 0;) {
    if (entries_[i].dayKey < cutoff) {
      entries_.erase(entries_.begin() + i);
      ++removed;
    }
  }
  return removed;
}

std::string UsageHistory::serialize() const {
  std::string out;
  for (const auto& e : entries_) {
    if (!out.empty()) out.push_back(';');
    appendU64(out, e.dayKey);
    out.push_back('|'); out += e.prov;
    out.push_back('|'); appendU64(out, e.tokIn);
    out.push_back('|'); appendU64(out, e.tokOut);
    out.push_back('|'); appendU64(out, e.calls);
    // 2026-10 tail (fields 6-8), each present only when it or a later one is set.
    const int tail = e.cacheW ? 3 : e.cacheR ? 2 : e.model[0] ? 1 : 0;
    if (tail >= 1) { out.push_back('|'); out += e.model; }
    if (tail >= 2) { out.push_back('|'); appendU64(out, e.cacheR); }
    if (tail >= 3) { out.push_back('|'); appendU64(out, e.cacheW); }
  }
  return out;
}

void UsageHistory::deserialize(const std::string& s) {
  entries_.clear();
  if (entries_.capacity() < kMaxEntries) entries_.reserve(kMaxEntries);   // once
  size_t i = 0, n = s.size();
  while (i < n) {
    size_t semi = s.find(';', i);
    if (semi == std::string::npos) semi = n;
    std::string group = s.substr(i, semi - i);
    i = semi + 1;
    if (group.empty()) continue;
    std::string parts[8];
    int np = splitBars(group, parts, 8);
    if (np < 5 || parts[1].empty()) continue;   // malformed group -> skip
    if (entries_.size() >= kMaxEntries) break;
    DayUsage d;
    d.dayKey = (uint32_t)strtoul(parts[0].c_str(), nullptr, 10);
    d.prov   = parts[1];
    d.tokIn  = (uint64_t)strtoull(parts[2].c_str(), nullptr, 10);
    d.tokOut = (uint64_t)strtoull(parts[3].c_str(), nullptr, 10);
    d.calls  = (uint32_t)strtoul(parts[4].c_str(), nullptr, 10);
    if (np > 5 && histModelOk(parts[5].c_str())) setModel(d, parts[5].c_str());
    if (np > 6) d.cacheR = (uint64_t)strtoull(parts[6].c_str(), nullptr, 10);
    if (np > 7) d.cacheW = (uint64_t)strtoull(parts[7].c_str(), nullptr, 10);
    if (d.dayKey == 0) continue;
    entries_.push_back(std::move(d));
  }
}

size_t historyEntryJson(const DayUsage& e, bool first, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  int n = snprintf(out, cap, "%s{\"d\":%lu,\"prov\":\"%.15s\",\"in\":%llu,\"out\":%llu,\"calls\":%lu",
                   first ? "" : ",", (unsigned long)e.dayKey, e.prov.c_str(),
                   (unsigned long long)e.tokIn, (unsigned long long)e.tokOut,
                   (unsigned long)e.calls);
  if (n < 0 || (size_t)n >= cap) return 0;
  size_t off = (size_t)n;
  auto more = [&](int k) {
    if (k < 0 || off + (size_t)k >= cap) return false;
    off += (size_t)k;
    return true;
  };
  if (e.model[0] && !more(snprintf(out + off, cap - off, ",\"m\":\"%s\"", e.model))) return 0;
  if (e.cacheR && !more(snprintf(out + off, cap - off, ",\"cr\":%llu", (unsigned long long)e.cacheR)))
    return 0;
  if (e.cacheW && !more(snprintf(out + off, cap - off, ",\"cw\":%llu", (unsigned long long)e.cacheW)))
    return 0;
  if (!more(snprintf(out + off, cap - off, "}"))) return 0;
  return off;
}

}  // namespace orch
}  // namespace nimbus
