#include "nimbus/orch/budget_governor.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nimbus/orch/day_start.h"       // runningDay, calendarDay, clockSet
#include "nimbus/orch/loops.h"           // civilFromDays
#include "nimbus/orch/spend_ceiling.h"   // addSatMicro

// Size matters here (the esp32s3-cyd image has a few KB to spare): the owner's words are
// formatted with snprintf and the file is read by a small parser of the one shape this code
// writes, not through ArduinoJson's templates.

namespace nimbus {
namespace orch {

namespace {

const char* slug(HistoryProfile p) { return historyProfileSlug(p); }

uint8_t daysIn(int year, int month) {
  static const uint8_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 30;
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 2 && leap ? 29 : kDays[month - 1];
}

// Money figures are held under 10^15 micro-USD (a billion dollars) before they are scaled, so
// no product here wraps on a 32-bit target.
constexpr uint64_t kMicroCap = 1000000000000000ull;
uint64_t capped(uint64_t v) { return v < kMicroCap ? v : kMicroCap; }
bool over(uint64_t projected, uint64_t ceiling) {
  return capped(projected) * 1000 > capped(ceiling) * kGovernorAdvicePermille;
}

// The mean nightly spend observed, micro-USD (0 under kGovernorMinObs nights).
uint64_t meanNight(const BudgetFile& f) {
  if (f.nObs < kGovernorMinObs) return 0;
  uint64_t sum = 0;
  for (uint8_t i = 0; i < f.nObs; i++) sum = addSatMicro(sum, f.obs[i]);
  return sum / f.nObs;
}

uint64_t rateUnder(const BudgetFile& f, HistoryProfile p) {
  return capped(meanNight(f)) * profileCostPermille(p) / profileCostPermille(f.effective());
}

uint32_t daysLeft(const GovernorFacts& g) {
  return g.daysInMonth >= g.dayOfMonth ? (uint32_t)(g.daysInMonth - g.dayOfMonth + 1) : 1;
}

// The calendar day the ceiling is reached at `perDay` (0 = not this month).
int64_t clampDayAt(const GovernorFacts& g, uint64_t perDay) {
  if (g.spentMicro >= g.ceilingMicro) return g.calendarDay;
  if (!perDay) return 0;
  const uint64_t k = (g.ceilingMicro - g.spentMicro + perDay - 1) / perDay;   // days, today counted
  return k <= daysLeft(g) ? g.calendarDay + (int64_t)k - 1 : 0;
}

void observe(BudgetFile& f, const GovernorFacts& g) {
  // A night that crosses into a new spend month observes nothing: the old month's last hours
  // are not in the new count (the observations it had stay).
  if (!f.night || f.month != g.monthKey || !g.monthKey) return;
  const int64_t elapsed = g.runningDay - f.night;
  const uint64_t delta = g.spentMicro >= f.snapMicro ? g.spentMicro - f.snapMicro : 0;
  const uint64_t perNight = delta / (uint64_t)elapsed;   // nights the device slept share the spend
  const int64_t pushes = elapsed < kGovernorObsNights ? elapsed : kGovernorObsNights;
  for (int64_t i = 0; i < pushes; i++) {
    if (f.nObs == kGovernorObsNights) {
      std::memmove(f.obs, f.obs + 1, sizeof(f.obs[0]) * (kGovernorObsNights - 1));
      f.nObs--;
    }
    f.obs[f.nObs++] = perNight;
  }
}

std::string fmt(const char* f, ...) {
  char b[480];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

struct Usd {
  char s[24];
  explicit Usd(uint64_t micro) {
    const uint64_t cents = (capped(micro) + 5000) / 10000;
    std::snprintf(s, sizeof s, "$%llu.%02llu", (unsigned long long)(cents / 100), (unsigned long long)(cents % 100));
  }
};

std::string clampClause(int64_t clampDay, uint64_t projected, const GovernorFacts& g) {
  if (g.spentMicro >= g.ceilingMicro) return " The ceiling is already reached: paid replies pause until it resets.";
  if (clampDay)
    return " At this rate it is reached on " + dayMonthText(clampDay) + " and paid replies pause until it resets.";
  return fmt(" At this rate the month ends at about %s, just under it.", Usd(projected).s);
}

// The largest profile smaller than the one in force that keeps the month under 0.9 of the
// ceiling; false when none does.
bool largestFit(const BudgetFile& f, const GovernorFacts& g, HistoryProfile& out) {
  for (int i = (int)f.effective() - 1; i >= 0; i--)
    if (!over(projectedMonthMicro(f, g, (HistoryProfile)i), g.ceilingMicro)) {
      out = (HistoryProfile)i;
      return true;
    }
  return false;
}

std::string fitClause(const BudgetAdvice& a) {
  if (a.fits)
    return fmt(" A shorter conversation history would fit: send /budget profile %s if you want it (it stays until you "
               "change it); nothing changes otherwise.", slug(a.fit));
  if (a.profile == HistoryProfile::Tiny) return " Nothing changes unless you raise the ceiling.";
  return " Even the shortest conversation history (tiny) would not keep it under; nothing changes unless you raise "
         "the ceiling or choose a profile.";
}

// ---- the file's reader: the shape budgetFileJson writes, whitespace allowed ----------------

// Where `key`'s value starts, or nullptr.
const char* valueOf(const std::string& t, const char* key) {
  const std::string k = std::string("\"") + key + "\"";
  const size_t at = t.find(k);
  if (at == std::string::npos) return nullptr;
  const char* p = t.c_str() + at + k.size();
  while (*p == ' ') p++;
  if (*p++ != ':') return nullptr;
  while (*p == ' ') p++;
  return p;
}

// A whole decimal number at `p` (a sign only where `neg` is allowed); false for anything else.
bool numberAt(const char*& p, bool neg, int64_t& out) {
  char* end = nullptr;
  if (!neg && *p == '-') return false;
  if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return false;
  out = std::strtoll(p, &end, 10);
  if (end == p) return false;
  p = end;
  return true;
}

// `key` as a number in [lo, hi]; absent = 0. False for a value of any other shape.
bool numberField(const std::string& t, const char* key, int64_t lo, int64_t hi, int64_t& out) {
  out = 0;
  const char* p = valueOf(t, key);
  if (!p) return true;
  return numberAt(p, lo < 0, out) && out >= lo && out <= hi && (*p == ',' || *p == '}' || *p == ' ');
}

bool stringField(const std::string& t, const char* key, std::string& out) {
  const char* p = valueOf(t, key);
  if (!p || *p != '"') return false;
  const char* end = std::strchr(p + 1, '"');
  if (!end) return false;
  out.assign(p + 1, end);
  return true;
}

bool profileField(const std::string& t, const char* key, HistoryProfile& out) {
  std::string s;
  return stringField(t, key, s) && parseHistoryProfile(s, out);
}

}  // namespace

uint32_t profileCostPermille(HistoryProfile p) {
  static const uint16_t kPermille[] = {578, 793, 1000, 1448, 1448};   // tiny, small, medium, large, full
  return (uint8_t)p <= (uint8_t)HistoryProfile::Full ? kPermille[(uint8_t)p] : 1448;
}

// ---- the file -------------------------------------------------------------------------

namespace {

// The object's braces around the text.
bool wholeObject(const std::string& text) {
  const size_t first = text.find_first_not_of(" \n\r\t"), last = text.find_last_not_of(" \n\r\t");
  return first != std::string::npos && text[first] == '{' && text[last] == '}';
}

// "obs": [n, n, ...], at most seven; absent = none.
bool obsField(const std::string& text, BudgetFile& f) {
  const char* p = valueOf(text, "obs");
  if (!p) return true;
  if (*p++ != '[') return false;
  while (*p == ' ') p++;
  while (*p != ']') {
    int64_t n = 0;
    if (f.nObs >= kGovernorObsNights || !numberAt(p, false, n)) return false;
    f.obs[f.nObs++] = (uint64_t)n;
    while (*p == ' ') p++;
    if (*p == ',') p++;
    while (*p == ' ') p++;
    if (*p == '\0') return false;
  }
  return true;
}

// The numbers, each in its range.
bool numberFields(const std::string& text, BudgetFile& f) {
  int64_t v = 0, set = 0, night = 0, month = 0, snap = 0, warned = 0;
  if (!(numberField(text, "v", 1, 1, v) && v == 1 && numberField(text, "set", 0, INT32_MAX, set) &&
        numberField(text, "night", 0, INT32_MAX, night) && numberField(text, "month", 0, 999912, month) &&
        numberField(text, "snap", 0, INT64_MAX, snap) && numberField(text, "warned", 0, 999912, warned)))
    return false;
  f.setDay = set;
  f.night = night;
  f.month = (uint32_t)month;
  f.snapMicro = (uint64_t)snap;
  f.warnedMonth = (uint32_t)warned;
  return true;
}

}  // namespace

BudgetFileRead parseBudgetFile(const std::string& text, BudgetFile& out) {
  out = BudgetFile();
  if (text.empty()) return BudgetFileRead::Missing;
  // The owner's choice is kept even when the rest does not read: it is never lost to a damaged
  // field (the advice stays off while the file reads as corrupt).
  const bool profileRead = profileField(text, "profile", out.profile);
  BudgetFile f = out;
  const bool ok = wholeObject(text) && profileRead && numberFields(text, f) && obsField(text, f);
  if (!ok) return BudgetFileRead::Corrupt;
  out = f;
  return BudgetFileRead::Ok;
}

std::string budgetFileJson(const BudgetFile& f) {
  std::string obs;
  for (uint8_t i = 0; i < f.nObs && i < kGovernorObsNights; i++)
    obs += (i ? "," : "") + std::to_string((unsigned long long)f.obs[i]);
  return fmt("{\"v\":1,\"profile\":\"%s\",\"set\":%lld,\"night\":%lld,\"month\":%lu,\"snap\":%llu,\"obs\":[%s],"
             "\"warned\":%lu}",
             slug(f.profile), (long long)f.setDay, (long long)f.night, (unsigned long)f.month,
             (unsigned long long)f.snapMicro, obs.c_str(), (unsigned long)f.warnedMonth);
}

// ---- the facts and the night ----------------------------------------------------------

bool governorFactsAt(uint64_t epoch, int32_t offsetMin, uint16_t startMin, const SpendFacts& spend,
                     GovernorFacts& out) {
  if (!day::clockSet(epoch)) return false;
  GovernorFacts g;
  g.runningDay = day::runningDay(epoch, offsetMin, startMin);
  g.calendarDay = day::calendarDay(epoch, offsetMin);
  if (g.runningDay <= 0 || g.calendarDay <= 0) return false;
  const CivilTime c = civilFromDays((uint32_t)g.calendarDay);
  g.dayOfMonth = (uint8_t)c.day;
  g.daysInMonth = daysIn(c.year, c.month);
  g.monthKey = spend.monthKey;
  g.spentMicro = spend.spentMicro;
  g.ceilingActive = spend.ceilingActive && spend.ceilingMicro > 0;
  g.ceilingMicro = spend.ceilingMicro;
  out = g;
  return true;
}

uint64_t projectedMonthMicro(const BudgetFile& f, const GovernorFacts& g, HistoryProfile p) {
  if (f.nObs < kGovernorMinObs) return 0;
  return addSatMicro(g.spentMicro, capped(rateUnder(f, p)) * daysLeft(g));
}

BudgetAdvice governorNight(BudgetFile& f, const GovernorFacts& g) {
  BudgetAdvice a;
  a.profile = f.effective();
  if (g.runningDay <= 0) return a;
  if (f.night && g.runningDay + kGovernorObsNights < f.night) {
    // The clock was far ahead when the last night ran (a bad time sync, then the right one):
    // start again from tonight rather than wait for that day to come round.
    f.night = 0;
    f.nObs = 0;
  }
  if (f.night && g.runningDay <= f.night) return a;   // ran tonight, or the clock went back a little
  observe(f, g);
  f.night = g.runningDay;
  f.month = g.monthKey;
  f.snapMicro = g.spentMicro;
  a.kind = BudgetAdvice::Kind::Observed;
  const bool ceiling = g.ceilingActive && g.ceilingMicro > 0;
  if (!ceiling || f.nObs < kGovernorMinObs || f.warnedMonth == g.monthKey) return a;
  a.projectedMicro = projectedMonthMicro(f, g, a.profile);
  if (!over(a.projectedMicro, g.ceilingMicro)) return a;
  f.warnedMonth = g.monthKey;   // once a month
  a.kind = BudgetAdvice::Kind::Advice;
  a.clampDay = clampDayAt(g, rateUnder(f, a.profile));
  a.fits = largestFit(f, g, a.fit);
  a.line = fmt("Spending this month is on course for %s against the %s ceiling.", Usd(a.projectedMicro).s,
               Usd(g.ceilingMicro).s) +
           clampClause(a.clampDay, a.projectedMicro, g) + fitClause(a);
  a.record = fmt("On course for %s of the %s ceiling: the owner was told (%s%s)", Usd(a.projectedMicro).s,
                 Usd(g.ceilingMicro).s, a.fits ? slug(a.fit) : "no profile", a.fits ? " would fit" : " fits");
  return a;
}

void chooseProfile(BudgetFile& f, HistoryProfile p, int64_t day) {
  f.profile = p;
  f.setDay = day;
  f.nObs = 0;   // the rate under the new profile is observed afresh
}

bool profileCommandChanges(const BudgetFile& f, const std::string& arg) {
  HistoryProfile p;
  return parseHistoryProfile(arg, p) && p != f.profile;
}

// ---- words ----------------------------------------------------------------------------

std::string usdText(uint64_t micro) { return Usd(micro).s; }

std::string dayMonthText(int64_t calendarDay) {
  static const char* const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                        "July",    "August",   "September", "October", "November", "December"};
  if (calendarDay <= 0 || calendarDay > 0xFFFFFFFFll) return std::string();
  const CivilTime c = civilFromDays((uint32_t)calendarDay);
  if (c.month < 1 || c.month > 12) return std::string();
  return fmt("%d %s", c.day, kMonths[c.month - 1]);
}

std::string profileStatusText(const BudgetFile& f, BudgetFileRead read) {
  const bool chosen = f.profile != kDefaultHistoryProfile;
  if (read == BudgetFileRead::Corrupt)
    return fmt("Conversation history: %s. The budget settings file could not be read, so %s applies and the "
               "spending advice is off; send /budget profile <name> to write it again.",
               slug(f.profile), chosen ? "your chosen profile" : "the default");
  if (!chosen) return "Conversation history: full (the default: as much as the model carries).";
  const std::string when = f.setDay ? " on " + dayMonthText(f.setDay) : std::string();
  return fmt("Conversation history: %s, chosen by you%s. Send /budget profile full for the default.", slug(f.profile),
             when.c_str());
}

std::string profileReplyText(const BudgetFile& f, const std::string& arg, bool ok) {
  if (!ok) return "Unknown profile. Use /budget profile tiny, small, medium, large or full (the default).";
  if (arg.empty())
    return profileStatusText(f, BudgetFileRead::Ok) +
           "\nChoose one with /budget profile tiny, small, medium, large or full (the default).";
  if (f.profile == kDefaultHistoryProfile)
    return "Conversation history is back to full, the default: replies carry as much of the conversation as the "
           "model allows.";
  return fmt("Conversation history set to %s: replies carry less of the conversation and cost less. It stays until "
             "you change it; send /budget profile full for the default.", slug(f.profile));
}

}  // namespace orch
}  // namespace nimbus
