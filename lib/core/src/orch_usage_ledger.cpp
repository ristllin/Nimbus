#include "nimbus/orch/usage_ledger.h"

#include <cstdlib>

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
  // No rates -> estimate is structurally $0 -> honest answer is "cannot tell",
  // which must gate NOTHING (the UI nudges the owner to set prices instead).
  if (e->centsPerMIn == 0 && e->centsPerMOut == 0 && e->centsPerKCalls == 0)
    return false;
  return e->estCents() >= e->centsLimit;
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

void UsageHistory::record(const std::string& prov, uint32_t dayKey, uint64_t tokIn,
                          uint64_t tokOut, uint32_t calls) {
  if (prov.empty() || dayKey == 0) return;   // no sane clock -> no dated bucket
  for (auto& e : entries_) {
    if (e.dayKey == dayKey && e.prov == prov) {
      e.tokIn += tokIn;
      e.tokOut += tokOut;
      e.calls += calls;
      return;
    }
  }
  if (entries_.size() >= kMaxEntries) {
    // Evict the OLDEST bucket to make room (the graphs only show recent windows).
    size_t oldest = 0;
    for (size_t i = 1; i < entries_.size(); ++i)
      if (entries_[i].dayKey < entries_[oldest].dayKey) oldest = i;
    entries_.erase(entries_.begin() + oldest);
  }
  DayUsage d;
  d.dayKey = dayKey;
  d.prov = prov;
  d.tokIn = tokIn;
  d.tokOut = tokOut;
  d.calls = calls;
  entries_.push_back(std::move(d));
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
  }
  return out;
}

void UsageHistory::deserialize(const std::string& s) {
  entries_.clear();
  size_t i = 0, n = s.size();
  while (i < n) {
    size_t semi = s.find(';', i);
    if (semi == std::string::npos) semi = n;
    std::string group = s.substr(i, semi - i);
    i = semi + 1;
    if (group.empty()) continue;
    std::string parts[5];
    int np = splitBars(group, parts, 5);
    if (np < 5 || parts[1].empty()) continue;   // malformed group -> skip
    if (entries_.size() >= kMaxEntries) break;
    DayUsage d;
    d.dayKey = (uint32_t)strtoul(parts[0].c_str(), nullptr, 10);
    d.prov   = parts[1];
    d.tokIn  = (uint64_t)strtoull(parts[2].c_str(), nullptr, 10);
    d.tokOut = (uint64_t)strtoull(parts[3].c_str(), nullptr, 10);
    d.calls  = (uint32_t)strtoul(parts[4].c_str(), nullptr, 10);
    if (d.dayKey == 0) continue;
    entries_.push_back(std::move(d));
  }
}

}  // namespace orch
}  // namespace nimbus
