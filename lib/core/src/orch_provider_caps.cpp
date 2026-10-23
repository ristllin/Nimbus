#include "nimbus/orch/provider_caps.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "nimbus/orch/provider_slots.h"
#include "nimbus/orch/usage_ledger.h"   // usagePeriodKey - the budget's own period

namespace nimbus {
namespace orch {

namespace {
uint8_t bitOf(NoticeChannel c) { return (uint8_t)(1u << (uint8_t)c); }
bool chatChannel(NoticeChannel c) { return c == NoticeChannel::Telegram || c == NoticeChannel::Web; }
}  // namespace

LocalDate localDateOf(time_t now) {
  LocalDate d;
  struct tm tmv;
  if (now < kCeilingClockValid || !localtime_r(&now, &tmv)) return d;
  d.year = tmv.tm_year + 1900;
  d.month = tmv.tm_mon + 1;
  d.day = tmv.tm_mday;
  return d;
}

// ---- the provider caps -----------------------------------------------------------

int ProviderCaps::find(const char* provider) const {
  if (!provider || !*provider) return -1;
  for (int i = 0; i < (int)n; ++i)
    if (std::strncmp(rows[i].provider, provider, sizeof(rows[i].provider) - 1) == 0) return i;
  return -1;
}

int ProviderCaps::ensure(const char* provider) {
  const int at = find(provider);
  if (at >= 0 || !provider || !*provider) return at;
  if (n >= kMaxRows) return -1;   // more providers than rows: never a cap we lost (setRules first)
  Row& r = rows[n];
  r = Row();
  std::strncpy(r.provider, provider, sizeof(r.provider) - 1);
  return n++;
}

int ProviderCaps::ensureRuled(const char* provider, const std::vector<CapRule>& rules) {
  const int at = ensure(provider);
  if (at >= 0 || !provider || !*provider) return at;
  for (int i = 0; i < (int)n; ++i) {
    if (rows[i].frozen) continue;
    bool ruled = false;
    for (const CapRule& c : rules) ruled = ruled || (c.capMicro && c.provider == rows[i].provider);
    if (ruled) continue;
    rows[i] = Row();
    std::strncpy(rows[i].provider, provider, sizeof(rows[i].provider) - 1);
    return i;
  }
  return -1;
}

bool ProviderCaps::anyCap() const {
  for (int i = 0; i < (int)n; ++i)
    if (rows[i].capMicro) return true;
  return false;
}

uint32_t ProviderCaps::periodOf(const Row& r, const LocalDate& today) {
  return today.valid() ? usagePeriodKey(today.year, today.month, today.day, r.resetDay) : 0;
}

bool ProviderCaps::settle(Row& r) {
  const bool over = r.capMicro && r.spentMicro >= r.capMicro;
  if (over == r.frozen) return false;
  r.frozen = over;
  r.noticesSent = 0;   // a new episode owes every notice; a closed one owes none
  return true;
}

bool ProviderCaps::setRules(const std::vector<CapRule>& rules, const LocalDate& today) {
  bool changed = false;
  for (int i = 0; i < (int)n; ++i) rows[i].capMicro = 0;   // a cap removed: uncapped
  for (const CapRule& c : rules) {
    if (!c.capMicro) continue;
    const int at = ensureRuled(c.provider.c_str(), rules);
    if (at < 0) continue;
    Row& r = rows[at];
    const uint64_t before = r.capMicro;
    r.capMicro = c.capMicro;
    r.resetDay = c.resetDay ? c.resetDay : 1;
    // A cap moved above the warning line owes a fresh warning when it is reached.
    if (before != r.capMicro && r.spentMicro < r.capMicro / 100 * kWarnPercent && r.warned) {
      r.warned = false;
      changed = true;
    }
  }
  changed = roll(today) || changed;
  for (int i = 0; i < (int)n; ++i) changed = settle(rows[i]) || changed;
  return changed;
}

bool ProviderCaps::roll(const LocalDate& today) {
  if (!today.valid()) return false;
  bool changed = false;
  for (int i = 0; i < (int)n; ++i) {
    Row& r = rows[i];
    const uint32_t now = periodOf(r, today);
    if (now == r.periodKey) continue;
    changed = true;
    if (r.periodKey == 0 || now < r.periodKey) {   // the clock's first read, or set back
      r.periodKey = now;
      continue;
    }
    r.periodKey = now;   // a later period: a new count, quietly
    r.spentMicro = 0;
    r.frozen = false;
    r.noticesSent = 0;
    r.warned = false;
  }
  return changed;
}

bool ProviderCaps::add(const char* provider, uint64_t micro, const LocalDate& today) {
  if (micro == 0) return false;
  const int at = ensure(provider);
  if (at < 0) return false;
  roll(today);
  Row& r = rows[at];
  if (r.periodKey == 0) r.periodKey = periodOf(r, today);
  r.spentMicro = addSatMicro(r.spentMicro, micro);
  settle(r);
  return true;
}

bool ProviderCaps::seed(const char* provider, uint64_t micro, const LocalDate& today) {
  const int at = ensure(provider);
  if (at < 0) return false;
  Row& r = rows[at];
  r.periodKey = periodOf(r, today);
  r.spentMicro = micro;
  settle(r);
  return true;
}

bool ProviderCaps::frozenWith(const char* provider, uint64_t pendingMicro) const {
  const int at = find(provider);
  if (at < 0) return false;
  const Row& r = rows[at];
  if (!r.capMicro) return false;
  return r.frozen || addSatMicro(r.spentMicro, pendingMicro) >= r.capMicro;
}

ProviderCaps::Owed ProviderCaps::nextOwed(NoticeChannel ch) const {
  Owed o;
  if (!chatChannel(ch)) return o;
  for (int i = 0; i < (int)n; ++i)
    if (rows[i].frozen && rows[i].capMicro && !(rows[i].noticesSent & bitOf(ch))) {
      o.kind = Owed::Freeze;
      o.row = i;
      return o;
    }
  for (int i = 0; i < (int)n; ++i) {
    const Row& r = rows[i];
    if (r.capMicro && !r.frozen && !r.warned && r.spentMicro >= r.capMicro / 100 * kWarnPercent) {
      o.kind = Owed::Warn;
      o.row = i;
      return o;
    }
  }
  return o;
}

bool ProviderCaps::markDelivered(NoticeChannel ch, const Owed& o) {
  if (o.row < 0 || o.row >= (int)n) return false;
  Row& r = rows[o.row];
  if (o.kind == Owed::Freeze) {
    if (r.noticesSent & bitOf(ch)) return false;
    r.noticesSent |= bitOf(ch);
    return true;
  }
  if (o.kind == Owed::Warn && !r.warned) {
    r.warned = true;
    return true;
  }
  return false;
}

// ---- the day ---------------------------------------------------------------------

bool DaySpend::roll(uint32_t today) {
  if (today == 0 || today == dayKey) return false;
  if (dayKey == 0 || today > dayKey) {   // a new day (a clock set back keeps the count)
    if (dayKey != 0) {
      spentMicro = 0;
      told = 0;
    }
    dayKey = today;
    return true;
  }
  dayKey = today;
  return true;
}

bool DaySpend::add(uint64_t micro, uint32_t today) {
  const bool changed = roll(today);
  if (micro == 0) return changed;
  spentMicro = addSatMicro(spentMicro, micro);
  return true;
}

bool DaySpend::owed(NoticeChannel ch) const {
  return chatChannel(ch) && thresholdMicro && spentMicro >= thresholdMicro && !(told & bitOf(ch));
}

bool DaySpend::mark(NoticeChannel ch) {
  if (told & bitOf(ch)) return false;
  told |= bitOf(ch);
  return true;
}

bool DaySpend::setThreshold(uint64_t micro) {
  if (micro == thresholdMicro) return false;
  thresholdMicro = micro;
  if (spentMicro < thresholdMicro) told = 0;   // a raised threshold warns again when reached
  return true;
}

// ---- the file --------------------------------------------------------------------

std::string serializeCeilingFile(const SpendCeiling& c, const DaySpend& d, const ProviderCaps& p) {
  std::string s = c.serialize();
  char buf[160];
  snprintf(buf, sizeof(buf), "|%llu|%lu|%llu|%u", (unsigned long long)d.thresholdMicro,
           (unsigned long)d.dayKey, (unsigned long long)d.spentMicro, (unsigned)d.told);
  s += buf;
  for (int i = 0; i < (int)p.n; ++i) {
    const ProviderCaps::Row& r = p.rows[i];
    snprintf(buf, sizeof(buf), ";p|%s|%lu|%llu|%d|%u|%d", r.provider, (unsigned long)r.periodKey,
             (unsigned long long)r.spentMicro, r.frozen ? 1 : 0, (unsigned)r.noticesSent,
             r.warned ? 1 : 0);
    s += buf;
  }
  return s;
}

namespace {
// The '|'-separated fields of one group (no empty-field rejection: a missing tail is "").
std::vector<std::string> splitFields(const std::string& g) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t bar = g.find('|', start);
    out.push_back(g.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  return out;
}

// A whole unsigned decimal; false on anything else (an empty field reads 0).
bool parseU64(const std::string& f, uint64_t& out) {
  out = 0;
  for (char ch : f) {
    if (ch < '0' || ch > '9') return false;
    const uint64_t d = (uint64_t)(ch - '0');
    if (out > (UINT64_MAX - d) / 10) return false;
    out = out * 10 + d;
  }
  return true;
}

bool validProvider(const std::string& s) {
  if (s.empty() || s.size() >= sizeof(ProviderCaps::Row().provider)) return false;
  for (char ch : s)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
  return true;
}

void parseDay(const std::vector<std::string>& main, DaySpend& d) {
  uint64_t v[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; ++i)
    if ((size_t)(9 + i) < main.size() && !parseU64(main[9 + i], v[i])) return;   // a bad tail: defaults
  d.thresholdMicro = v[0] > SpendCeiling::kMaxLimitMicro ? SpendCeiling::kMaxLimitMicro : v[0];
  d.dayKey = (uint32_t)v[1];
  d.spentMicro = v[2];
  d.told = (uint8_t)(v[3] & 0x07);
}

void parseRow(const std::string& g, ProviderCaps& p) {
  const std::vector<std::string> f = splitFields(g);
  if (f.size() < 7 || f[0] != "p" || !validProvider(f[1]) || p.n >= ProviderCaps::kMaxRows) return;
  if (p.find(f[1].c_str()) >= 0) return;   // one row per provider
  uint64_t v[5];
  for (int i = 0; i < 5; ++i)
    if (!parseU64(f[2 + i], v[i])) return;
  ProviderCaps::Row& r = p.rows[p.n++];
  r = ProviderCaps::Row();
  std::strncpy(r.provider, f[1].c_str(), sizeof(r.provider) - 1);
  r.periodKey = (uint32_t)v[0];
  r.spentMicro = v[1];
  r.frozen = v[2] != 0;   // re-decided against the cap once the cap table is known
  r.noticesSent = (uint8_t)(v[3] & 0x07);
  r.warned = v[4] != 0;
}
}  // namespace

bool deserializeCeilingFile(const std::string& text, SpendCeiling& c, DaySpend& d, ProviderCaps& p,
                            bool* earlier) {
  d = DaySpend();
  p = ProviderCaps();
  if (earlier) *earlier = false;
  if (!c.deserialize(text)) return false;
  std::string body = text;
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
  std::vector<std::string> groups;
  size_t start = 0;
  for (;;) {
    const size_t semi = body.find(';', start);
    groups.push_back(body.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
    if (semi == std::string::npos) break;
    start = semi + 1;
  }
  if (groups.size() >= 2) {
    const std::vector<std::string> main = splitFields(groups[1]);
    if (earlier) *earlier = main.size() <= 9;
    parseDay(main, d);
  }
  for (size_t i = 2; i < groups.size(); ++i) parseRow(groups[i], p);
  return true;
}

// ---- copy ------------------------------------------------------------------------

std::string providerName(const std::string& slug) {
  const ProviderSlot* s = findProviderSlot(slug.c_str());
  if (s) return s->label;
  if (slug == "custom") return "your custom endpoint";
  if (slug == "tavily") return "Tavily";
  return slug;
}

std::string capResetText(uint32_t periodKey, uint8_t resetDay) {
  static const char* kMonths[] = {"January", "February", "March",     "April",
                                  "May",     "June",     "July",      "August",
                                  "September", "October", "November", "December"};
  if (periodKey == 0) return std::string();
  const uint32_t next = periodKey + 1;   // months since year 0
  return std::to_string(resetDay ? resetDay : 1) + " " + kMonths[next % 12];
}

namespace {
constexpr const char* kCapWhere = "open the device's web page, go to Assistant > Usage, and raise "
                                  "its monthly limit";
}  // namespace

std::string providerFreezeText(const CapFacts& f) {
  const std::string name = providerName(f.provider);
  std::string s = "Paid AI calls to " + name + " are paused. This month's estimated " + name +
                  " spend (" + formatUsd(f.spentMicro) + ") reached the " + formatUsd(f.capMicro) +
                  " monthly limit you set, so replies, helpers, memory, voice, photos and images "
                  "stop using " + name + "; other providers keep working. To raise it, " + kCapWhere +
                  ".";
  const std::string d = capResetText(f.periodKey, f.resetDay);
  s += d.empty() ? " Otherwise it resumes when the next period starts."
                 : " Otherwise it resumes on " + d + ".";
  return s;
}

std::string providerWarnText(const CapFacts& f) {
  const std::string name = providerName(f.provider);
  return "This month's estimated " + name + " spend is " + formatUsd(f.spentMicro) + ", " +
         std::to_string(ProviderCaps::kWarnPercent) + "% of the " + formatUsd(f.capMicro) +
         " monthly limit you set. At the limit, paid calls to " + name +
         " pause until it is raised or the period ends. To change it, " + kCapWhere + ".";
}

std::string dailyWarnText(uint64_t spentMicro, uint64_t thresholdMicro) {
  return "About " + formatUsd(spentMicro) + " spent on AI today, past your daily warning of " +
         formatUsd(thresholdMicro) +
         ". Spending continues; this is a heads-up. Change the warning on Assistant > Usage.";
}

std::string capsMigrationText(const std::vector<CapRule>& rules, bool overallActive,
                              uint64_t overallLimitMicro) {
  std::string caps;
  for (const CapRule& c : rules) {
    if (!c.capMicro) continue;
    if (!caps.empty()) caps += ", ";
    caps += providerName(c.provider) + " " + formatUsd(c.capMicro);
  }
  std::string s = "New in this update: the monthly limits you set per provider now stop paid AI "
                  "calls. At its limit a provider pauses for replies, helpers, memory, voice, "
                  "photos and images, and the others keep working. Limits that apply: " + caps +
                  ". Every other provider has no limit.";
  if (overallActive)
    s += " An overall monthly ceiling of " + formatUsd(overallLimitMicro) + " also applies.";
  else
    s += " The overall monthly ceiling is off because you set provider limits; you can turn it "
         "on under Assistant > Usage.";
  s += " To change a limit, open the device's web page and go to Assistant > Usage.";
  return s;
}

}  // namespace orch
}  // namespace nimbus
