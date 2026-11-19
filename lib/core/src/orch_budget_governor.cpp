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

HistoryProfile smaller(HistoryProfile p) { return p == HistoryProfile::Tight ? p : (HistoryProfile)((uint8_t)p - 1); }
HistoryProfile larger(HistoryProfile p) { return p == HistoryProfile::Wide ? p : (HistoryProfile)((uint8_t)p + 1); }
const char* slug(HistoryProfile p) { return historyProfileSlug(p); }

const char* const kSetBy[] = {"default", "owner", "device"};   // ProfileSetBy, in order

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
bool over(uint64_t projected, uint64_t ceiling, uint32_t permille) {
  return capped(projected) * 1000 > capped(ceiling) * permille;
}
bool under(uint64_t projected, uint64_t ceiling, uint32_t permille) {
  return capped(projected) * 1000 < capped(ceiling) * permille;
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

void change(BudgetFile& f, GovernorStep& st, HistoryProfile to, const GovernorFacts& g) {
  st.to = to;
  f.governed = to;
  f.setBy = ProfileSetBy::Governor;
  f.setDay = g.calendarDay;
  f.nObs = 0;
  f.quiet = 0;
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
    return " At this rate the ceiling is reached on " + dayMonthText(clampDay) +
           " and paid replies pause until it resets.";
  return fmt(" At this rate the month ends at about %s, just under it.", Usd(projected).s);
}

std::string recordLine(const GovernorStep& st, const GovernorFacts& g) {
  return fmt("History profile %s (was %s), by the device: on course for %s of %s", slug(st.to), slug(st.from),
             Usd(st.projectedMicro).s, Usd(g.ceilingMicro).s);
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
  static const uint16_t kPermille[] = {578, 793, 1000, 1448};   // tight, lean, standard, wide
  return (uint8_t)p <= (uint8_t)HistoryProfile::Wide ? kPermille[(uint8_t)p] : 1000;
}

// ---- the file -------------------------------------------------------------------------

namespace {

// The object's braces around the text.
bool wholeObject(const std::string& text) {
  const size_t first = text.find_first_not_of(" \n\r\t"), last = text.find_last_not_of(" \n\r\t");
  return first != std::string::npos && text[first] == '{' && text[last] == '}';
}

bool setByField(const std::string& text, ProfileSetBy& out) {
  std::string by;
  if (!stringField(text, "by", by)) return false;
  for (uint8_t i = 0; i < 3; i++)
    if (by == kSetBy[i]) {
      out = (ProfileSetBy)i;
      return true;
    }
  return false;
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
  int64_t v = 0, set = 0, night = 0, month = 0, snap = 0, quiet = 0, warned = 0;
  if (!(numberField(text, "v", 1, 1, v) && v == 1 && numberField(text, "set", 0, INT32_MAX, set) &&
        numberField(text, "night", 0, INT32_MAX, night) && numberField(text, "month", 0, 999912, month) &&
        numberField(text, "snap", 0, INT64_MAX, snap) && numberField(text, "quiet", 0, kGovernorQuietNights, quiet) &&
        numberField(text, "warned", 0, 999912, warned)))
    return false;
  f.setDay = set;
  f.night = night;
  f.month = (uint32_t)month;
  f.snapMicro = (uint64_t)snap;
  f.quiet = (uint8_t)quiet;
  f.warnedMonth = (uint32_t)warned;
  return true;
}

}  // namespace

BudgetFileRead parseBudgetFile(const std::string& text, BudgetFile& out) {
  out = BudgetFile();
  if (text.empty()) return BudgetFileRead::Missing;
  // The owner's choice is kept even when the rest does not read: a pin is never lost to a
  // damaged field (the governor stays off while the file reads as corrupt).
  std::string pinned;
  const bool pinRead = stringField(text, "pinned", pinned);
  if (pinRead && parseHistoryProfile(pinned, out.pinnedProfile)) out.pinned = true;
  BudgetFile f = out;
  const bool ok = wholeObject(text) && pinRead && (pinned.empty() || f.pinned) &&
                  profileField(text, "governed", f.governed) && setByField(text, f.setBy) && numberFields(text, f) &&
                  obsField(text, f);
  if (!ok) return BudgetFileRead::Corrupt;
  out = f;
  return BudgetFileRead::Ok;
}

std::string budgetFileJson(const BudgetFile& f) {
  std::string obs;
  for (uint8_t i = 0; i < f.nObs && i < kGovernorObsNights; i++)
    obs += (i ? "," : "") + std::to_string((unsigned long long)f.obs[i]);
  return fmt("{\"v\":1,\"pinned\":\"%s\",\"governed\":\"%s\",\"by\":\"%s\",\"set\":%lld,\"night\":%lld,\"month\":%lu,"
             "\"snap\":%llu,\"obs\":[%s],\"quiet\":%u,\"warned\":%lu}",
             f.pinned ? slug(f.pinnedProfile) : "", slug(f.governed), kSetBy[(uint8_t)f.setBy % 3],
             (long long)f.setDay, (long long)f.night, (unsigned long)f.month, (unsigned long long)f.snapMicro,
             obs.c_str(), (unsigned)f.quiet, (unsigned long)f.warnedMonth);
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

namespace {

// The owner's pinned profile: never moved, warned about once a month.
void pinnedNight(BudgetFile& f, const GovernorFacts& g, bool ceiling, GovernorStep& st) {
  f.quiet = 0;
  if (!ceiling || f.nObs < kGovernorMinObs) return;
  const HistoryProfile cur = f.effective();
  st.projectedMicro = projectedMonthMicro(f, g, cur);
  if (!over(st.projectedMicro, g.ceilingMicro, kGovernorDownPermille) || f.warnedMonth == g.monthKey) return;
  f.warnedMonth = g.monthKey;
  st.kind = GovernorStep::Kind::PinnedOver;
  st.clampDay = clampDayAt(g, rateUnder(f, cur));
  st.line = fmt("Your chosen conversation history (%s) is on course for %s this month against the %s ceiling.",
                slug(cur), Usd(st.projectedMicro).s, Usd(g.ceilingMicro).s) +
            clampClause(st.clampDay, st.projectedMicro, g) + " Send /budget profile auto to let the device shorten it.";
}

// No ceiling to protect: a quiet night, one step back up after seven.
void noCeilingNight(BudgetFile& f, const GovernorFacts& g, GovernorStep& st) {
  const HistoryProfile cur = f.effective();
  if (cur >= kDefaultHistoryProfile) {
    f.quiet = 0;
    return;
  }
  if (++f.quiet < kGovernorQuietNights) return;
  change(f, st, larger(cur), g);
  st.kind = GovernorStep::Kind::Up;
  st.line = fmt("Replies carry a longer conversation history again (%s, was %s): no spending ceiling applies now.",
                slug(st.to), slug(st.from));
  st.record = fmt("History profile %s (was %s), by the device: no ceiling applies", slug(st.to), slug(st.from));
}

// Over 0.9 of the ceiling: the largest smaller profile that fits, else tight and the clamp date.
void stepDown(BudgetFile& f, const GovernorFacts& g, uint64_t projCur, GovernorStep& st) {
  const HistoryProfile cur = f.effective();
  f.quiet = 0;
  for (HistoryProfile q = cur; q != HistoryProfile::Tight;) {
    q = smaller(q);
    const uint64_t p = projectedMonthMicro(f, g, q);
    if (over(p, g.ceilingMicro, kGovernorDownPermille)) continue;
    st.projectedMicro = p;
    change(f, st, q, g);
    st.kind = GovernorStep::Kind::Down;
    st.line = fmt("Spending this month is on course for %s against the %s ceiling, so replies now carry a shorter "
                  "conversation history (%s, was %s). Send /budget profile %s to keep the longer one.",
                  Usd(projCur).s, Usd(g.ceilingMicro).s, slug(q), slug(st.from), slug(st.from));
    st.record = recordLine(st, g);
    return;
  }
  st.projectedMicro = projectedMonthMicro(f, g, HistoryProfile::Tight);
  st.clampDay = clampDayAt(g, rateUnder(f, HistoryProfile::Tight));
  if (cur == HistoryProfile::Tight && f.warnedMonth == g.monthKey) return;
  f.warnedMonth = g.monthKey;
  if (cur != HistoryProfile::Tight) change(f, st, HistoryProfile::Tight, g);
  st.kind = GovernorStep::Kind::NothingFits;
  st.line = fmt("Spending this month is on course for %s against the %s ceiling even with the shortest "
                "conversation history (tight%s%s).",
                Usd(st.projectedMicro).s, Usd(g.ceilingMicro).s, st.changed() ? ", was " : "",
                st.changed() ? slug(st.from) : "") +
            clampClause(st.clampDay, st.projectedMicro, g);
  if (st.changed()) st.record = recordLine(st, g);
}

// Under standard and quiet: one step up after seven nights. The step up asks what a whole month
// at the larger profile's rate would cost, not what is left of this one: near a month's end the
// spend so far (under the smaller profile) would make any profile look quiet.
void stepUp(BudgetFile& f, const GovernorFacts& g, GovernorStep& st) {
  const HistoryProfile cur = f.effective();
  if (cur >= kDefaultHistoryProfile) {   // never above standard by itself
    f.quiet = 0;
    return;
  }
  const HistoryProfile next = larger(cur);
  const uint64_t projNext = capped(rateUnder(f, next)) * g.daysInMonth;
  f.quiet = under(projNext, g.ceilingMicro, kGovernorUpPermille) ? (uint8_t)(f.quiet + 1) : 0;
  if (f.quiet < kGovernorQuietNights) return;
  st.projectedMicro = projNext;
  change(f, st, next, g);
  st.kind = GovernorStep::Kind::Up;
  st.line = fmt("Spending has stayed well under the %s ceiling for 7 nights, so replies carry a longer conversation "
                "history again (%s, was %s).",
                Usd(g.ceilingMicro).s, slug(next), slug(st.from));
  st.record = recordLine(st, g);
}

}  // namespace

GovernorStep governorNight(BudgetFile& f, const GovernorFacts& g) {
  GovernorStep st;
  st.from = st.to = f.effective();
  if (g.runningDay <= 0) return st;
  if (f.night && g.runningDay + kGovernorObsNights < f.night) {
    // The clock was far ahead when the last night ran (a bad time sync, then the right one):
    // start again from tonight rather than wait for that day to come round.
    f.night = 0;
    f.nObs = 0;
    f.quiet = 0;
  }
  if (f.night && g.runningDay <= f.night) return st;   // ran tonight, or the clock went back a little
  observe(f, g);
  f.night = g.runningDay;
  f.month = g.monthKey;
  f.snapMicro = g.spentMicro;
  st.kind = GovernorStep::Kind::Observed;
  const bool ceiling = g.ceilingActive && g.ceilingMicro > 0;
  if (f.pinned) pinnedNight(f, g, ceiling, st);
  else if (!ceiling) noCeilingNight(f, g, st);
  else if (f.nObs < kGovernorMinObs) {}   // still observing the profile in force
  else if (const uint64_t projCur = projectedMonthMicro(f, g, f.effective());
           over(projCur, g.ceilingMicro, kGovernorDownPermille))
    stepDown(f, g, projCur, st);
  else stepUp(f, g, st);
  return st;
}

void pinProfile(BudgetFile& f, HistoryProfile p, int64_t day) {
  f.pinned = true;
  f.pinnedProfile = p;
  f.setBy = ProfileSetBy::Owner;
  f.setDay = day;
  f.nObs = 0;
  f.quiet = 0;
  f.warnedMonth = 0;
}

void unpinProfile(BudgetFile& f, int64_t day) {
  f.pinned = false;
  f.governed = kDefaultHistoryProfile;
  f.setBy = ProfileSetBy::Owner;
  f.setDay = day;
  f.nObs = 0;
  f.quiet = 0;
  f.warnedMonth = 0;
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
  const char* now = slug(f.effective());
  if (read == BudgetFileRead::Corrupt)
    return fmt("Conversation history: %s. The budget settings file could not be read, so %s applies and the device "
               "does not adjust it; send /budget profile <name> or /budget profile auto to write it again.",
               now, f.pinned ? "your chosen profile" : "the default");
  if (f.pinned)
    return fmt("Conversation history: %s, chosen by you. The device does not change it; send /budget profile auto "
               "to let it adjust for spending.", now);
  if (f.setBy == ProfileSetBy::Governor) {
    const std::string when = f.setDay ? " on " + dayMonthText(f.setDay) : std::string();
    if (f.governed < kDefaultHistoryProfile)
      return fmt("Conversation history: %s, set by the device%s because the month's spending was on course past the "
                 "ceiling. Send /budget profile %s to keep the longer one.",
                 now, when.c_str(), slug(kDefaultHistoryProfile));
    return fmt("Conversation history: %s, set back by the device%s.", now, when.c_str());
  }
  return fmt("Conversation history: %s (the default%s). The device shortens it if a month's spending heads past the "
             "ceiling.", now, read == BudgetFileRead::Missing ? "; no budget settings saved yet" : "");
}

std::string profileReplyText(const BudgetFile& f, const std::string& arg, bool ok) {
  if (!ok) return "Unknown profile. Use /budget profile tight, lean, standard, wide or auto.";
  if (arg.empty())
    return profileStatusText(f, BudgetFileRead::Ok) +
           "\nChoose one with /budget profile tight, lean, standard or wide, or /budget profile auto.";
  if (arg == "auto")
    return fmt("The device now chooses the conversation history: %s for now, shorter if a month's spending heads "
               "past the ceiling.", slug(f.effective()));
  return fmt("Conversation history set to %s and kept there: the device will not change it. Send /budget profile "
             "auto to let it adjust for spending again.", slug(f.effective()));
}

bool profileCommandChanges(const BudgetFile& f, const std::string& arg) {
  if (arg == "auto") return f.pinned;
  HistoryProfile p;
  return parseHistoryProfile(arg, p) && !(f.pinned && f.pinnedProfile == p);
}

}  // namespace orch
}  // namespace nimbus
