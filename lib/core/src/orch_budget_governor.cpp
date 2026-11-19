#include "nimbus/orch/budget_governor.h"

#include <ArduinoJson.h>

#include <cstdio>

#include "nimbus/orch/day_start.h"   // runningDay, calendarDay, clockSet
#include "nimbus/orch/loops.h"       // civilFromDays, daysFromCivil
#include "nimbus/orch/spend_ceiling.h"   // addSatMicro

namespace nimbus {
namespace orch {

namespace {

constexpr HistoryProfile kOrder[] = {HistoryProfile::Tight, HistoryProfile::Lean, HistoryProfile::Standard,
                                     HistoryProfile::Wide};

HistoryProfile smaller(HistoryProfile p) { return p == HistoryProfile::Tight ? p : (HistoryProfile)((uint8_t)p - 1); }
HistoryProfile larger(HistoryProfile p) { return p == HistoryProfile::Wide ? p : (HistoryProfile)((uint8_t)p + 1); }

const char* setBySlug(ProfileSetBy b) {
  switch (b) {
    case ProfileSetBy::Owner: return "owner";
    case ProfileSetBy::Governor: return "device";
    case ProfileSetBy::Default: break;
  }
  return "default";
}

bool setByFrom(const std::string& s, ProfileSetBy& out) {
  if (s == "owner") out = ProfileSetBy::Owner;
  else if (s == "device") out = ProfileSetBy::Governor;
  else if (s == "default") out = ProfileSetBy::Default;
  else return false;
  return true;
}

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
  const uint64_t r = meanNight(f);
  const uint32_t from = profileCostPermille(f.effective());
  return from ? capped(r) * profileCostPermille(p) / from : 0;
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
      for (uint8_t k = 1; k < kGovernorObsNights; k++) f.obs[k - 1] = f.obs[k];
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

std::string clampClause(int64_t clampDay, uint64_t projected, const GovernorFacts& g) {
  if (g.spentMicro >= g.ceilingMicro) return " The ceiling is already reached: paid replies pause until it resets.";
  if (clampDay)
    return " At this rate the ceiling is reached on " + dayMonthText(clampDay) + " and paid replies pause until it resets.";
  return " At this rate the month ends at about " + usdText(projected) + ", just under it.";
}

std::string recordLine(const GovernorStep& st, const GovernorFacts& g) {
  return std::string("History profile ") + historyProfileSlug(st.to) + " (was " + historyProfileSlug(st.from) +
         "), by the device: on course for " + usdText(st.projectedMicro) + " of " + usdText(g.ceilingMicro);
}

}  // namespace

uint32_t profileCostPermille(HistoryProfile p) {
  switch (p) {
    case HistoryProfile::Tight: return 578;
    case HistoryProfile::Lean: return 793;
    case HistoryProfile::Wide: return 1448;
    case HistoryProfile::Standard: break;
  }
  return 1000;
}

// ---- the file -------------------------------------------------------------------------

BudgetFileRead parseBudgetFile(const std::string& text, BudgetFile& out) {
  out = BudgetFile();
  if (text.empty()) return BudgetFileRead::Missing;
  JsonDocument d;
  if (deserializeJson(d, text) != DeserializationError::Ok || !d.is<JsonObject>() || (d["v"] | 0) != 1)
    return BudgetFileRead::Corrupt;
  BudgetFile f;
  const std::string pinned = d["pinned"] | "";
  if (!pinned.empty()) {
    if (!parseHistoryProfile(pinned, f.pinnedProfile)) return BudgetFileRead::Corrupt;
    f.pinned = true;
  }
  if (!parseHistoryProfile(d["governed"] | "", f.governed) || !setByFrom(d["by"] | "", f.setBy))
    return BudgetFileRead::Corrupt;
  f.setDay = d["set"] | (int64_t)0;
  f.night = d["night"] | (int64_t)0;
  f.month = d["month"] | 0u;
  f.snapMicro = d["snap"] | (uint64_t)0;
  f.quiet = (uint8_t)(d["quiet"] | 0u);
  f.warnedMonth = d["warned"] | 0u;
  JsonArrayConst obs = d["obs"].as<JsonArrayConst>();
  if (obs.size() > kGovernorObsNights || f.quiet > kGovernorQuietNights || f.night < 0 || f.setDay < 0)
    return BudgetFileRead::Corrupt;
  for (JsonVariantConst v : obs) {
    if (!v.is<uint64_t>()) return BudgetFileRead::Corrupt;
    f.obs[f.nObs++] = v.as<uint64_t>();
  }
  out = f;
  return BudgetFileRead::Ok;
}

std::string budgetFileJson(const BudgetFile& f) {
  JsonDocument d;
  d["v"] = 1;
  d["pinned"] = f.pinned ? historyProfileSlug(f.pinnedProfile) : "";
  d["governed"] = historyProfileSlug(f.governed);
  d["by"] = setBySlug(f.setBy);
  d["set"] = f.setDay;
  d["night"] = f.night;
  d["month"] = f.month;
  d["snap"] = f.snapMicro;
  JsonArray obs = d["obs"].to<JsonArray>();
  for (uint8_t i = 0; i < f.nObs && i < kGovernorObsNights; i++) obs.add(f.obs[i]);
  d["quiet"] = f.quiet;
  d["warned"] = f.warnedMonth;
  std::string out;
  serializeJson(d, out);
  return out;
}

// ---- the facts and the night ----------------------------------------------------------

bool governorFactsAt(uint64_t epoch, int32_t offsetMin, uint16_t startMin, uint32_t monthKey, uint64_t spentMicro,
                     bool ceilingActive, uint64_t ceilingMicro, GovernorFacts& out) {
  if (!day::clockSet(epoch)) return false;
  GovernorFacts g;
  g.runningDay = day::runningDay(epoch, offsetMin, startMin);
  g.calendarDay = day::calendarDay(epoch, offsetMin);
  if (g.runningDay <= 0 || g.calendarDay <= 0) return false;
  const CivilTime c = civilFromDays((uint32_t)g.calendarDay);
  g.dayOfMonth = (uint8_t)c.day;
  g.daysInMonth = daysIn(c.year, c.month);
  g.monthKey = monthKey;
  g.spentMicro = spentMicro;
  g.ceilingActive = ceilingActive && ceilingMicro > 0;
  g.ceilingMicro = ceilingMicro;
  out = g;
  return true;
}

uint64_t projectedMonthMicro(const BudgetFile& f, const GovernorFacts& g, HistoryProfile p) {
  if (f.nObs < kGovernorMinObs) return 0;
  return addSatMicro(g.spentMicro, capped(rateUnder(f, p)) * daysLeft(g));
}

GovernorStep governorNight(BudgetFile& f, const GovernorFacts& g) {
  GovernorStep st;
  st.from = st.to = f.effective();
  if (g.runningDay <= 0 || (f.night && g.runningDay <= f.night)) return st;   // ran tonight, or the clock went back
  observe(f, g);
  f.night = g.runningDay;
  f.month = g.monthKey;
  f.snapMicro = g.spentMicro;
  st.kind = GovernorStep::Kind::Observed;
  const HistoryProfile cur = f.effective();
  const bool ceiling = g.ceilingActive && g.ceilingMicro > 0;

  if (f.pinned) {   // the owner's choice: never moved, warned about once a month
    f.quiet = 0;
    if (!ceiling || f.nObs < kGovernorMinObs) return st;
    st.projectedMicro = projectedMonthMicro(f, g, cur);
    if (!over(st.projectedMicro, g.ceilingMicro, kGovernorDownPermille) || f.warnedMonth == g.monthKey) return st;
    f.warnedMonth = g.monthKey;
    st.kind = GovernorStep::Kind::PinnedOver;
    st.clampDay = clampDayAt(g, rateUnder(f, cur));
    st.line = std::string("Your chosen conversation history (") + historyProfileSlug(cur) + ") is on course for " +
              usdText(st.projectedMicro) + " this month against the " + usdText(g.ceilingMicro) + " ceiling." +
              clampClause(st.clampDay, st.projectedMicro, g) +
              " Send /budget profile auto to let the device shorten it.";
    return st;
  }

  if (!ceiling) {   // nothing to protect: a quiet night
    if (cur >= kDefaultHistoryProfile) {
      f.quiet = 0;
      return st;
    }
    if (++f.quiet < kGovernorQuietNights) return st;
    change(f, st, larger(cur), g);
    st.kind = GovernorStep::Kind::Up;
    st.line = std::string("Replies carry a longer conversation history again (") + historyProfileSlug(st.to) +
              ", was " + historyProfileSlug(st.from) + "): no spending ceiling applies now.";
    st.record = std::string("History profile ") + historyProfileSlug(st.to) + " (was " + historyProfileSlug(st.from) +
                "), by the device: no ceiling applies";
    return st;
  }
  if (f.nObs < kGovernorMinObs) return st;   // still observing the profile in force

  const uint64_t projCur = projectedMonthMicro(f, g, cur);
  if (over(projCur, g.ceilingMicro, kGovernorDownPermille)) {
    f.quiet = 0;
    for (HistoryProfile q = cur; q != HistoryProfile::Tight;) {
      q = smaller(q);
      const uint64_t p = projectedMonthMicro(f, g, q);
      if (over(p, g.ceilingMicro, kGovernorDownPermille)) continue;
      st.projectedMicro = p;
      change(f, st, q, g);
      st.kind = GovernorStep::Kind::Down;
      st.line = "Spending this month is on course for " + usdText(projCur) + " against the " +
                usdText(g.ceilingMicro) + " ceiling, so replies now carry a shorter conversation history (" +
                historyProfileSlug(q) + ", was " + historyProfileSlug(st.from) + "). Send /budget profile " +
                historyProfileSlug(st.from) + " to keep the longer one.";
      st.record = recordLine(st, g);
      return st;
    }
    // Nothing fits: tight, and the date the ceiling is reached at that rate.
    const uint64_t tightRate = rateUnder(f, HistoryProfile::Tight);
    st.projectedMicro = projectedMonthMicro(f, g, HistoryProfile::Tight);
    st.clampDay = clampDayAt(g, tightRate);
    if (cur == HistoryProfile::Tight && f.warnedMonth == g.monthKey) return st;
    f.warnedMonth = g.monthKey;
    if (cur != HistoryProfile::Tight) change(f, st, HistoryProfile::Tight, g);
    st.kind = GovernorStep::Kind::NothingFits;
    st.line = "Spending this month is on course for " + usdText(st.projectedMicro) + " against the " +
              usdText(g.ceilingMicro) + " ceiling even with the shortest conversation history (tight" +
              (st.changed() ? std::string(", was ") + historyProfileSlug(st.from) : std::string()) + ")." +
              clampClause(st.clampDay, st.projectedMicro, g);
    if (st.changed()) st.record = recordLine(st, g);
    return st;
  }
  if (cur >= kDefaultHistoryProfile) {   // never above standard by itself
    f.quiet = 0;
    return st;
  }
  // The step up asks what a whole month at the larger profile's rate would cost, not what is
  // left of this one: near a month's end the spend so far (under the smaller profile) would
  // make any profile look quiet.
  const HistoryProfile next = larger(cur);
  const uint64_t projNext = capped(rateUnder(f, next)) * g.daysInMonth;
  f.quiet = under(projNext, g.ceilingMicro, kGovernorUpPermille) ? (uint8_t)(f.quiet + 1) : 0;
  if (f.quiet < kGovernorQuietNights) return st;
  st.projectedMicro = projNext;
  change(f, st, next, g);
  st.kind = GovernorStep::Kind::Up;
  st.line = "Spending has stayed well under the " + usdText(g.ceilingMicro) +
            " ceiling for 7 nights, so replies carry a longer conversation history again (" +
            historyProfileSlug(next) + ", was " + historyProfileSlug(st.from) + ").";
  st.record = recordLine(st, g);
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

std::string usdText(uint64_t micro) {
  const uint64_t cents = (micro + 5000) / 10000;
  char b[32];
  std::snprintf(b, sizeof b, "$%llu.%02llu", (unsigned long long)(cents / 100), (unsigned long long)(cents % 100));
  return b;
}

std::string dayMonthText(int64_t calendarDay) {
  static const char* const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                        "July",    "August",   "September", "October", "November", "December"};
  if (calendarDay <= 0 || calendarDay > 0xFFFFFFFFll) return std::string();
  const CivilTime c = civilFromDays((uint32_t)calendarDay);
  if (c.month < 1 || c.month > 12) return std::string();
  return std::to_string(c.day) + " " + kMonths[c.month - 1];
}

std::string profileStatusText(const BudgetFile& f, BudgetFileRead read) {
  const std::string head = std::string("Conversation history: ") + historyProfileSlug(f.effective());
  if (read == BudgetFileRead::Corrupt)
    return head + ". The budget settings file could not be read, so the default applies; it is written again at "
                  "the next change.";
  if (f.pinned)
    return head + ", chosen by you. The device does not change it; send /budget profile auto to let it adjust "
                  "for spending.";
  if (f.setBy == ProfileSetBy::Governor) {
    const std::string when = f.setDay ? " on " + dayMonthText(f.setDay) : std::string();
    if (f.governed < kDefaultHistoryProfile)
      return head + ", set by the device" + when +
             " because the month's spending was on course past the ceiling. Send /budget profile " +
             historyProfileSlug(kDefaultHistoryProfile) + " to keep the longer one.";
    return head + ", set back by the device" + when + ".";
  }
  return head + (read == BudgetFileRead::Missing ? " (the default; no budget settings saved yet)." : " (the default).") +
         " The device shortens it if a month's spending heads past the ceiling.";
}

std::string profileReplyText(const BudgetFile& f, const std::string& arg, bool ok) {
  if (!ok) return "Unknown profile. Use /budget profile tight, lean, standard, wide or auto.";
  if (arg.empty())
    return profileStatusText(f, BudgetFileRead::Ok) +
           "\nChoose one with /budget profile tight, lean, standard or wide, or /budget profile auto.";
  if (arg == "auto")
    return std::string("The device now chooses the conversation history: ") +
           historyProfileSlug(kDefaultHistoryProfile) +
           " for now, shorter if a month's spending heads past the ceiling.";
  return std::string("Conversation history set to ") + historyProfileSlug(f.effective()) +
         " and kept there: the device will not change it. Send /budget profile auto to let it adjust for spending "
         "again.";
}

}  // namespace orch
}  // namespace nimbus
