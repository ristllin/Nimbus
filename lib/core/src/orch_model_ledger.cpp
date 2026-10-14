#include "nimbus/orch/model_ledger.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace nimbus {
namespace orch {

namespace {

bool isOther(const char* model) { return std::strcmp(model, kOtherModel) == 0; }

// A model the ledger accepts: a safe key, or the reserved fold row.
bool acceptModel(const char* model) { return model && (isOther(model) || modelKeySafe(model)); }

void copyField(char* dst, size_t cap, const char* src) {
  const size_t n = std::strlen(src);
  const size_t k = n < cap - 1 ? n : cap - 1;
  std::memcpy(dst, src, k);
  dst[k] = 0;
}

bool same(const char* aProv, const char* aModel, const char* prov, const char* model) {
  return std::strcmp(aProv, prov) == 0 && std::strcmp(aModel, model) == 0;
}

uint64_t periodSpend(const ModelSpend& r) { return r.tokIn + r.tokOut + r.cacheR + r.cacheW; }

// Spend that counts in the current period. Only a row of the recording provider
// `prov` can be judged by `periodKey` (each provider has its own reset day, so its
// own period key): an incoming 0 (clock unsynced) keeps every stored period current,
// a real key makes that provider's row from another period stale. Another
// provider's row with period spend counts as current (fail-closed): UsageLedger
// rolls every row to its own provider's period first (syncPeriods), so a row whose
// provider moved on already reads 0 here.
bool hasCurrentSpend(const ModelSpend& r, const char* prov, uint32_t periodKey) {
  if (periodSpend(r) == 0) return false;
  if (std::strcmp(r.prov, prov) != 0) return true;
  return periodKey == 0 || r.periodKey == 0 || r.periodKey == periodKey;
}

// The UsageLedger roll rule, applied to one model row.
void rollTo(ModelSpend& r, uint32_t periodKey) {
  if (r.periodKey == periodKey || periodKey == 0) return;
  if (r.periodKey == 0) { r.periodKey = periodKey; return; }
  r.periodKey = periodKey;
  r.tokIn = r.tokOut = r.cacheR = r.cacheW = 0;
}

uint64_t satAdd(uint64_t a, uint64_t b) { return (UINT64_MAX - a < b) ? UINT64_MAX : a + b; }

// A retired row's all-time totals move into its provider's ~other row.
void foldTotals(ModelSpend& into, const ModelSpend& from) {
  into.totIn = satAdd(into.totIn, from.totIn);
  into.totOut = satAdd(into.totOut, from.totOut);
  if (from.lastDay > into.lastDay) into.lastDay = from.lastDay;
}

// A whole row (period spend and totals) moves into its provider's ~other row. Rows
// are synced to their provider's period before a record, so both share it; a row of
// an older period (never counted) brings its totals only.
void mergeRow(ModelSpend& into, const ModelSpend& from) {
  const uint32_t k = into.periodKey > from.periodKey ? into.periodKey : from.periodKey;
  rollTo(into, k);
  if (from.periodKey == k || from.periodKey == 0) {
    into.tokIn = satAdd(into.tokIn, from.tokIn);
    into.tokOut = satAdd(into.tokOut, from.tokOut);
    into.cacheR = satAdd(into.cacheR, from.cacheR);
    into.cacheW = satAdd(into.cacheW, from.cacheW);
  }
  foldTotals(into, from);
}

// The track's period parts follow its provider like a row's (rollTo rules).
void rollTrack(ProvTrack& t, uint32_t periodKey) {
  if (t.periodKey == periodKey || periodKey == 0) return;
  if (t.periodKey != 0) {
    t.pre = TokenDelta{};
    t.lost = TokenDelta{};
  }
  t.periodKey = periodKey;
}

// The fold victim order (gate G2): the least spend this period, then the oldest lastDay.
bool foldsBefore(const ModelSpend& a, const ModelSpend& b) {
  if (periodSpend(a) != periodSpend(b)) return periodSpend(a) < periodSpend(b);
  return a.lastDay < b.lastDay;
}

void appendU64(std::string& out, uint64_t v) {
  char buf[24];
  int i = 0;
  if (v == 0) buf[i++] = '0';
  while (v) { buf[i++] = char('0' + (v % 10)); v /= 10; }
  while (i > 0) out.push_back(buf[--i]);
}

// A decimal field, saturating at `max` (an over-range number reads as the max, never
// a wrapped small one). Non-digits end the number.
uint64_t parseSat(const std::string& s, uint64_t max) {
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') break;
    const uint64_t d = (uint64_t)(c - '0');
    if (v > (max - d) / 10) return max;
    v = v * 10 + d;
  }
  return v;
}

// Split one ';'-group on '|' into at most maxParts parts; text after the maxParts-th
// bar is ignored (fields are append-only: a newer build's extra fields are skipped).
int splitBars(const std::string& group, std::string* parts, int maxParts) {
  int np = 0;
  size_t p = 0;
  while (np < maxParts) {
    const size_t bar = group.find('|', p);
    if (bar == std::string::npos) { parts[np++] = group.substr(p); break; }
    parts[np++] = group.substr(p, bar - p);
    p = bar + 1;
  }
  return np;
}

// Walk the non-empty ';'-groups of a blob. '!' groups (the "!v|N" header, the spend
// file's tracking groups) reach `fn` too: a reader skips the ones it does not know.
template <typename F>
void forEachGroup(const std::string& s, F fn) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    size_t semi = s.find(';', i);
    if (semi == std::string::npos) semi = n;
    const std::string group = s.substr(i, semi - i);
    i = semi + 1;
    if (group.empty()) continue;
    if (!fn(group)) break;
  }
}

constexpr const char* kHeader = "!v|1";

}  // namespace

void ModelLedger::reserveAll() {
  // Once, on first use (never at static init, before PSRAM placement is enabled).
  if (rows_.capacity() < kMaxRows) rows_.reserve(kMaxRows);
  if (owner_.capacity() < kMaxOwnerRates) owner_.reserve(kMaxOwnerRates);
  if (router_.capacity() < kMaxRouterRates) router_.reserve(kMaxRouterRates);
  if (tracks_.capacity() < kMaxTracks) tracks_.reserve(kMaxTracks);
}

const char* ModelLedger::spendKey(const char* model) const {
  if (model && *model) return model;
  return tracking_ ? kOtherModel : "";
}

const ProvTrack* ModelLedger::track(const char* prov) const {
  for (const auto& t : tracks_)
    if (std::strcmp(t.prov, prov) == 0) return &t;
  return nullptr;
}

ProvTrack* ModelLedger::trackFor(const char* prov) {
  for (auto& t : tracks_)
    if (std::strcmp(t.prov, prov) == 0) return &t;
  if (!provKeySafe(prov) || tracks_.size() >= kMaxTracks) return nullptr;
  reserveAll();
  tracks_.emplace_back();
  copyField(tracks_.back().prov, sizeof(tracks_.back().prov), prov);
  return &tracks_.back();
}

void ModelLedger::dropTrack(const char* prov) {
  for (size_t i = 0; i < tracks_.size(); ++i)
    if (std::strcmp(tracks_[i].prov, prov) == 0) {
      tracks_.erase(tracks_.begin() + (std::ptrdiff_t)i);
      return;
    }
}

bool ModelLedger::hasRow(const char* prov) const {
  for (const auto& r : rows_)
    if (std::strcmp(r.prov, prov) == 0) return true;
  return false;
}

const ModelSpend* ModelLedger::findRow(const char* prov, const char* model) const {
  for (const auto& r : rows_)
    if (same(r.prov, r.model, prov, model)) return &r;
  return nullptr;
}

ModelSpend* ModelLedger::findMut(const char* prov, const char* model) {
  for (auto& r : rows_)
    if (same(r.prov, r.model, prov, model)) return &r;
  return nullptr;
}

size_t ModelLedger::namedCount(const char* prov) const {
  size_t n = 0;
  for (const auto& r : rows_)
    if (std::strcmp(r.prov, prov) == 0 && !isOther(r.model)) ++n;
  return n;
}

ModelSpend* ModelLedger::pushRow(const char* prov, const char* model) {
  if (rows_.size() >= kMaxRows) return nullptr;
  rows_.emplace_back();
  ModelSpend& r = rows_.back();
  copyField(r.prov, sizeof(r.prov), prov);
  copyField(r.model, sizeof(r.model), model);
  return &r;
}

// Free one row: the named row with the oldest lastDay and no spend this period
// folds its all-time totals into its provider's ~other row. When that provider has
// no ~other row yet, the victim BECOMES it (totals kept) and the search goes on.
bool ModelLedger::evictOne(const char* prov, uint32_t periodKey) {
  for (size_t guard = 0; guard <= kMaxRows; ++guard) {
    size_t victim = rows_.size();
    for (size_t i = 0; i < rows_.size(); ++i) {
      const ModelSpend& r = rows_[i];
      if (isOther(r.model) || hasCurrentSpend(r, prov, periodKey)) continue;
      if (victim == rows_.size() || r.lastDay < rows_[victim].lastDay) victim = i;
    }
    if (victim == rows_.size()) return false;
    ModelSpend* other = findMut(rows_[victim].prov, kOtherModel);
    if (!other) {
      copyField(rows_[victim].model, sizeof(rows_[victim].model), kOtherModel);
      continue;
    }
    foldTotals(*other, rows_[victim]);
    rows_.erase(rows_.begin() + (std::ptrdiff_t)victim);
    return true;
  }
  return false;
}

// A provider at its named cap: its named row with the oldest lastDay and no spend
// this period takes the new model, after the old model's all-time totals fold into
// the provider's ~other row (made from free room when it has none). With no ~other
// row and no room, the old row itself becomes ~other (a named slot frees up) and
// nullptr tells the caller to go on.
ModelSpend* ModelLedger::recycleStale(const char* prov, const char* model, uint32_t periodKey) {
  size_t v = rows_.size();
  for (size_t i = 0; i < rows_.size(); ++i) {
    const ModelSpend& r = rows_[i];
    if (std::strcmp(r.prov, prov) != 0 || isOther(r.model) || hasCurrentSpend(r, prov, periodKey))
      continue;
    if (v == rows_.size() || r.lastDay < rows_[v].lastDay) v = i;
  }
  if (v == rows_.size()) return nullptr;
  ModelSpend* other = findMut(prov, kOtherModel);
  if (!other) other = pushRow(prov, kOtherModel);   // reserved: rows_[v] never moves
  if (!other) {
    copyField(rows_[v].model, sizeof(rows_[v].model), kOtherModel);
    return nullptr;
  }
  foldTotals(*other, rows_[v]);
  ModelSpend& r = rows_[v];
  r = ModelSpend{};
  copyField(r.prov, sizeof(r.prov), prov);
  copyField(r.model, sizeof(r.model), model);
  return &r;
}

void ModelLedger::syncPeriods(PeriodOf periodOf, const void* ctx) {
  if (!periodOf) return;
  for (auto& r : rows_) rollTo(r, periodOf(ctx, r.prov));
  for (auto& t : tracks_) rollTrack(t, periodOf(ctx, t.prov));
}

void ModelLedger::clearPeriodSpend(size_t i) {
  if (i >= rows_.size()) return;
  ModelSpend& r = rows_[i];
  r.tokIn = r.tokOut = r.cacheR = r.cacheW = 0;
}

void ModelLedger::dropAttribution(size_t i) {
  if (i >= rows_.size()) return;
  ModelSpend& r = rows_[i];
  r.totIn = r.totIn > r.tokIn ? r.totIn - r.tokIn : 0;
  r.totOut = r.totOut > r.tokOut ? r.totOut - r.tokOut : 0;
  clearPeriodSpend(i);
}

// Gate G2: every row holds spend this period and `prov` has no row at all, so no
// slot can be freed the usual way. The smallest named row of another provider
// (foldsBefore) moves whole into its provider's ~other row - priced at that
// provider's highest known price, so nothing it held reads cheaper - and its room is
// `prov`'s. A victim whose provider has no ~other row yet becomes it (no room freed)
// and the search goes on, bounded: with at most 12 providers holding counters
// (UsageLedger::kMaxProviders) over 32 rows some provider holds two, so a fold comes.
// It fails only when no other provider holds a named row (a hand-made file).
bool ModelLedger::foldSmallestElsewhere(const char* prov) {
  for (size_t guard = 0; guard <= kMaxRows; ++guard) {
    size_t v = rows_.size();
    for (size_t i = 0; i < rows_.size(); ++i) {
      const ModelSpend& r = rows_[i];
      if (isOther(r.model) || std::strcmp(r.prov, prov) == 0) continue;
      if (v == rows_.size() || foldsBefore(r, rows_[v])) v = i;
    }
    if (v == rows_.size()) return false;
    ModelSpend* other = findMut(rows_[v].prov, kOtherModel);
    if (!other) {
      copyField(rows_[v].model, sizeof(rows_[v].model), kOtherModel);
      continue;
    }
    mergeRow(*other, rows_[v]);
    rows_.erase(rows_.begin() + (std::ptrdiff_t)v);
    return true;
  }
  return false;
}

// Last resort when no row can be freed: the provider's named row with the least
// spend this period becomes its ~other row (its counters stay, now unattributed).
ModelSpend* ModelLedger::relabelSmallest(const char* prov) {
  ModelSpend* best = nullptr;
  for (auto& r : rows_) {
    if (std::strcmp(r.prov, prov) != 0 || isOther(r.model)) continue;
    if (!best || periodSpend(r) < periodSpend(*best)) best = &r;
  }
  if (best) copyField(best->model, sizeof(best->model), kOtherModel);
  return best;
}

ModelSpend* ModelLedger::otherRowFor(const char* prov, uint32_t periodKey) {
  if (ModelSpend* o = findMut(prov, kOtherModel)) return o;
  if (rows_.size() < kMaxRows || evictOne(prov, periodKey)) {
    if (ModelSpend* o = findMut(prov, kOtherModel)) return o;   // eviction may have made it
    return pushRow(prov, kOtherModel);
  }
  if (ModelSpend* r = relabelSmallest(prov)) return r;
  if (foldSmallestElsewhere(prov)) return pushRow(prov, kOtherModel);   // gate G2
  return nullptr;
}

ModelSpend* ModelLedger::rowFor(const char* prov, const char* model, uint32_t periodKey) {
  if (ModelSpend* r = findMut(prov, model)) return r;
  if (isOther(model)) return otherRowFor(prov, periodKey);
  if (namedCount(prov) >= kMaxNamedPerProv) {
    if (ModelSpend* r = recycleStale(prov, model, periodKey)) return r;
    if (namedCount(prov) >= kMaxNamedPerProv) return otherRowFor(prov, periodKey);
  }
  if (rows_.size() < kMaxRows || evictOne(prov, periodKey)) return pushRow(prov, model);
  // Gate G2: a provider with no row at all still gets one for its live spend.
  if (!hasRow(prov) && foldSmallestElsewhere(prov)) return pushRow(prov, model);
  return otherRowFor(prov, periodKey);
}

void ModelLedger::record(const char* prov, const char* model, const TokenDelta& d,
                         uint32_t periodKey, uint32_t dayKey) {
  if (!provKeySafe(prov) || !acceptModel(model) || d.empty()) return;
  reserveAll();
  ModelSpend* r = rowFor(prov, model, periodKey);
  // No row found or made (only a hand-made file of 32 fold rows of 32 providers):
  // the provider counter keeps the spend, priced at the provider's highest price.
  if (!r) return;
  tracking_ = true;
  rollTo(*r, periodKey);
  r->tokIn = satAdd(r->tokIn, d.in);
  r->tokOut = satAdd(r->tokOut, d.out);
  r->cacheR = satAdd(r->cacheR, d.cacheR);
  r->cacheW = satAdd(r->cacheW, d.cacheW);
  r->totIn = satAdd(r->totIn, d.in);
  r->totOut = satAdd(r->totOut, d.out);
  if (dayKey) r->lastDay = dayKey;
}

// ---- owner prices --------------------------------------------------------------

const ModelRate* ModelLedger::ownerRate(const char* prov, const char* model) const {
  for (const auto& r : owner_)
    if (same(r.prov, r.model, prov, model)) return &r;
  return nullptr;
}

RateEdit ModelLedger::setOwnerRate(const char* prov, const char* model, uint32_t microIn,
                                   uint32_t microOut) {
  if (!provKeySafe(prov) || !modelKeySafe(model)) return RateEdit::Invalid;
  reserveAll();
  for (size_t i = 0; i < owner_.size(); ++i) {
    if (!same(owner_[i].prov, owner_[i].model, prov, model)) continue;
    if (microIn == 0 && microOut == 0) {
      owner_.erase(owner_.begin() + (std::ptrdiff_t)i);
    } else {
      owner_[i].microIn = microIn;
      owner_[i].microOut = microOut;
    }
    return RateEdit::Ok;
  }
  if (microIn == 0 && microOut == 0) return RateEdit::Ok;   // clearing an unset row
  if (owner_.size() >= kMaxOwnerRates) return RateEdit::Full;
  owner_.emplace_back();
  ModelRate& r = owner_.back();
  copyField(r.prov, sizeof(r.prov), prov);
  copyField(r.model, sizeof(r.model), model);
  r.microIn = microIn;
  r.microOut = microOut;
  return RateEdit::Ok;
}

RateEdit ModelLedger::setOwnerRateDurable(const char* prov, const char* model, uint32_t microIn,
                                          uint32_t microOut,
                                          const std::function<bool(const std::string&)>& persist) {
  reserveAll();
  const std::vector<ModelRate> before = owner_;
  const RateEdit e = setOwnerRate(prov, model, microIn, microOut);
  if (e != RateEdit::Ok) return e;
  if (persist && persist(serializeOwnerRates())) return RateEdit::Ok;
  owner_ = before;   // capacity is kept: no reallocation
  return RateEdit::WriteFailed;
}

// ---- router prices ---------------------------------------------------------------

const ModelRate* ModelLedger::routerRate(const char* prov, const char* model) const {
  for (const auto& r : router_)
    if (same(r.prov, r.model, prov, model)) return &r;
  return nullptr;
}

void ModelLedger::replaceRouterRates(const std::vector<ModelRate>& rows) {
  reserveAll();
  router_.clear();
  for (const ModelRate& r : rows) {
    if (router_.size() >= kMaxRouterRates) break;
    if (!provKeySafe(r.prov) || !modelKeySafe(r.model)) continue;
    if (routerRate(r.prov, r.model)) continue;   // first row of a key wins
    router_.push_back(r);
  }
}

// ---- text forms ------------------------------------------------------------------
// spend row:  prov|model|periodKey|tokIn|tokOut|cacheR|cacheW|totIn|totOut|lastDay
// tracking:   !t   (tracking started)
// behind:     !n   (the provider counters' latest store did not land)
// track:      !p|prov|periodKey|preIn|preOut|preCr|preCw|lostIn|lostOut|lostCr|lostCw|
//             preTotIn|preTotOut|storedTotIn|storedTotOut|storedBeforeIn|storedBeforeOut
// price row:  prov|model|microIn|microOut

std::string ModelLedger::serializeSpend() const {
  std::string out = kHeader;
  if (tracking_) out += ";!t";
  if (countersBehind_) out += ";!n";
  for (const auto& t : tracks_) {
    out += ";!p|";
    out += t.prov;
    const uint64_t f[] = {t.periodKey, t.pre.in,  t.pre.out,  t.pre.cacheR,  t.pre.cacheW,
                          t.lost.in,   t.lost.out, t.lost.cacheR, t.lost.cacheW,
                          t.preTotIn,  t.preTotOut, t.storedTotIn, t.storedTotOut,
                          t.storedBeforeIn, t.storedBeforeOut};
    for (uint64_t v : f) {
      out.push_back('|');
      appendU64(out, v);
    }
  }
  for (const auto& r : rows_) {
    out.push_back(';');
    out += r.prov;
    out.push_back('|'); out += r.model;
    out.push_back('|'); appendU64(out, r.periodKey);
    out.push_back('|'); appendU64(out, r.tokIn);
    out.push_back('|'); appendU64(out, r.tokOut);
    out.push_back('|'); appendU64(out, r.cacheR);
    out.push_back('|'); appendU64(out, r.cacheW);
    out.push_back('|'); appendU64(out, r.totIn);
    out.push_back('|'); appendU64(out, r.totOut);
    out.push_back('|'); appendU64(out, r.lastDay);
  }
  return out;
}

void ModelLedger::parseTrack(const std::string& group) {
  tracking_ = true;
  std::string p[17];
  const int np = splitBars(group, p, 17);
  if (np < 2 || !provKeySafe(p[1].c_str()) || track(p[1].c_str())) return;
  ProvTrack* t = trackFor(p[1].c_str());
  if (!t) return;
  uint64_t* u64[] = {&t->pre.in,   &t->pre.out,   &t->pre.cacheR,  &t->pre.cacheW,
                     &t->lost.in,  &t->lost.out,  &t->lost.cacheR, &t->lost.cacheW,
                     &t->preTotIn, &t->preTotOut, &t->storedTotIn, &t->storedTotOut,
                     &t->storedBeforeIn, &t->storedBeforeOut};
  if (np > 2) t->periodKey = (uint32_t)parseSat(p[2], UINT32_MAX);
  for (int k = 0; k < 14; ++k)
    if (np > 3 + k) *u64[k] = parseSat(p[3 + k], UINT64_MAX);
}

void ModelLedger::deserializeSpend(const std::string& s) {
  reserveAll();
  rows_.clear();
  tracks_.clear();
  tracking_ = false;
  countersBehind_ = false;
  forEachGroup(s, [&](const std::string& group) {
    if (group[0] == '!') {
      if (group == "!t") tracking_ = true;
      else if (group == "!n") countersBehind_ = true;
      else if (group.compare(0, 3, "!p|") == 0) parseTrack(group);
      return true;   // the header, or a group a newer build added
    }
    if (rows_.size() >= kMaxRows) return false;   // the cap (tracks are written first)
    std::string p[10];
    const int np = splitBars(group, p, 10);
    if (np < 2 || !provKeySafe(p[0].c_str()) || !acceptModel(p[1].c_str())) return true;
    if (findRow(p[0].c_str(), p[1].c_str())) return true;   // a duplicate key: first wins
    ModelSpend r;
    copyField(r.prov, sizeof(r.prov), p[0].c_str());
    copyField(r.model, sizeof(r.model), p[1].c_str());
    uint64_t* u64[] = {&r.tokIn, &r.tokOut, &r.cacheR, &r.cacheW, &r.totIn, &r.totOut};
    if (np > 2) r.periodKey = (uint32_t)parseSat(p[2], UINT32_MAX);
    for (int k = 0; k < 6; ++k)
      if (np > 3 + k) *u64[k] = parseSat(p[3 + k], UINT64_MAX);
    if (np > 9) r.lastDay = (uint32_t)parseSat(p[9], UINT32_MAX);
    rows_.push_back(r);
    tracking_ = true;   // a row means tracking had started (a file without "!t")
    return true;
  });
}

std::string ModelLedger::serializeRates(const std::vector<ModelRate>& v) {
  std::string out = kHeader;
  for (const auto& r : v) {
    out.push_back(';');
    out += r.prov;
    out.push_back('|'); out += r.model;
    out.push_back('|'); appendU64(out, r.microIn);
    out.push_back('|'); appendU64(out, r.microOut);
  }
  return out;
}

void ModelLedger::deserializeRates(const std::string& s, std::vector<ModelRate>& v, size_t cap) {
  if (v.capacity() < cap) v.reserve(cap);
  v.clear();
  forEachGroup(s, [&](const std::string& group) {
    if (group[0] == '!') return true;   // the header
    if (v.size() >= cap) return false;
    std::string p[4];
    const int np = splitBars(group, p, 4);
    if (np < 2 || !provKeySafe(p[0].c_str()) || !modelKeySafe(p[1].c_str())) return true;
    for (const auto& r : v)
      if (same(r.prov, r.model, p[0].c_str(), p[1].c_str())) return true;
    ModelRate r;
    copyField(r.prov, sizeof(r.prov), p[0].c_str());
    copyField(r.model, sizeof(r.model), p[1].c_str());
    if (np > 2) r.microIn = (uint32_t)parseSat(p[2], UINT32_MAX);
    if (np > 3) r.microOut = (uint32_t)parseSat(p[3], UINT32_MAX);
    if (r.microIn == 0 && r.microOut == 0) return true;   // 0:0 is "unset": no row
    v.push_back(r);
    return true;
  });
}

// ---- the editable list -----------------------------------------------------------

bool editableHas(const std::string& csv, const char* key) {
  const size_t kl = key ? std::strlen(key) : 0;
  if (!kl) return false;
  for (size_t p = csv.find(key); p != std::string::npos; p = csv.find(key, p + 1)) {
    const bool startOk = p == 0 || csv[p - 1] == ',';
    const bool endOk = p + kl == csv.size() || csv[p + kl] == ',';
    if (startOk && endOk) return true;
  }
  return false;
}

namespace {
struct KeyList {
  std::string csv;
  size_t      n = 0;
  void add(const char* key) {   // a key, verbatim, once
    if (n >= kMaxEditable || !modelKeySafe(key) || editableHas(csv, key)) return;
    if (n++) csv.push_back(',');
    csv += key;
  }
};
}  // namespace

std::string editableModelKeys(const char* prov, const std::string& choicesCsv,
                              const std::string& orchSel, const std::string& subSel,
                              const std::string& extra, const ModelLedger& L) {
  KeyList out;
  if (!provKeySafe(prov)) return out.csv;
  out.csv.reserve(kMaxEditable * kModelKeyCap);   // once: no regrowth
  auto addId = [&](const char* id, size_t len) {   // a configured id: as its key
    char raw[kModelKeyCap], key[kModelKeyCap];
    if (len == 0 || len >= kModelKeyCap) return;
    std::memcpy(raw, id, len);
    raw[len] = 0;
    if (usageModelKey(prov, raw, "", key)) out.add(key);
  };
  size_t p = 0;
  while (p < choicesCsv.size()) {
    size_t c = choicesCsv.find(',', p);
    if (c == std::string::npos) c = choicesCsv.size();
    addId(choicesCsv.data() + p, c - p);
    p = c + 1;
  }
  for (const std::string* s : {&orchSel, &subSel, &extra}) addId(s->data(), s->size());
  // Spend and price rows already hold keys: added verbatim (a key is not
  // re-normalized, so a model that reached the ledger stays priceable).
  for (const auto& r : L.rows())
    if (std::strcmp(r.prov, prov) == 0) out.add(r.model);
  for (const auto& r : L.ownerRates())
    if (std::strcmp(r.prov, prov) == 0) out.add(r.model);
  return out.csv;
}

// ---- the web field ------------------------------------------------------------------

bool parseModelRateField(const std::string& value, ModelRateField& out) {
  const size_t c1 = value.find(':');
  if (c1 == std::string::npos || c1 == 0) return false;
  const size_t c2 = value.find(':', c1 + 1);
  if (c2 == std::string::npos) return false;
  const size_t c3 = value.find(':', c2 + 1);
  if (c3 == std::string::npos || c3 + 1 >= value.size()) return false;
  auto u32 = [](const std::string& s, uint32_t& v) {
    if (s.empty() || s.size() > 10) return false;
    uint64_t x = 0;
    for (char c : s) {
      if (c < '0' || c > '9') return false;
      x = x * 10 + (uint64_t)(c - '0');
    }
    if (x > UINT32_MAX) return false;
    v = (uint32_t)x;
    return true;
  };
  ModelRateField f;
  f.prov = value.substr(0, c1);
  if (!u32(value.substr(c1 + 1, c2 - c1 - 1), f.microIn)) return false;
  if (!u32(value.substr(c2 + 1, c3 - c2 - 1), f.microOut)) return false;
  f.model = value.substr(c3 + 1);
  out = f;
  return true;
}

}  // namespace orch
}  // namespace nimbus
