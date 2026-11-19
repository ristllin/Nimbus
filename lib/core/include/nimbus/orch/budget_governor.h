#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "nimbus/orch/budget.h"   // HistoryProfile

// budget_governor - the history profile a device runs and the nightly step-down governor
// (lane RB-BG, PRD section 2 "Governor"; engine-independent, ships in v5.2.0). Pure: no
// Arduino, no clock of its own, host-tested in test/test_budget_governor (a simulated month
// on a virtual clock). The device (src/agent/budget_profile_store.cpp) and nimbusd
// (nimbusd/src/hosted_budget.h) keep the file and feed it the facts.
//
// The file, /data/budget.json (never NVS): the owner's pinned profile, if any, and the
// governor's own state. A missing or unreadable file reads as the default profile (standard),
// unpinned, and the owner's status says so; the next write replaces it.
//
// The rule, nightly at the device's day start:
//   1. Project the month: what it spent so far plus the mean of the last seven observed nights
//      (at least three, all under the current profile) times the profile's measured cost
//      ratio times the days left.
//   2. Down, at once and as far as needed: the current profile projects over 0.9 of the
//      ceiling -> the largest smaller profile projecting at most 0.9; none -> tight, and one
//      plain line naming the date the ceiling is reached at that rate.
//   3. Up, one step at a time and never above standard (wide is the owner's): seven
//      observed nights in a row in which a whole month at the next larger profile's rate
//      projects under 0.7 of the ceiling. Any other night starts the count again.
//   4. Every change starts the observations again (three nights before the next decision). No
//      ceiling (the owner turned it off, or only provider limits are set) is a quiet night. A
//      pinned profile is never moved: the governor only warns, once a month, when it projects
//      over 0.9 of the ceiling. The governor never writes the ceiling.
//   5. Hysteresis: 0.9 / 0.7 = 1.29 is more than the swing a constant spending rate makes in the
//      projection between months (31 / 28 = 1.11) or between profiles' neighbours, so a
//      constant rate never moves the profile down and then up (or up and then down); and a
//      reversal waits at least nine nights after a step down (the third observed night is the
//      first quiet one) and three after a step up.
namespace nimbus {
namespace orch {

// The measured month cost of each profile against standard, in thousandths (RB-M0b,
// tests/evals/measured/cost.json, engine2_sonnet blended turn USD at the measured cold share:
// tight 0.03737, lean 0.051261, standard 0.064633, wide 0.093597; test_budget_governor reads
// the file and fails when these drift).
uint32_t profileCostPermille(HistoryProfile p);

inline constexpr uint8_t kGovernorObsNights = 7;    // the mean's window
inline constexpr uint8_t kGovernorMinObs = 3;       // nights observed before a decision
inline constexpr uint8_t kGovernorQuietNights = 7;  // quiet nights before a step up
inline constexpr uint32_t kGovernorDownPermille = 900;
inline constexpr uint32_t kGovernorUpPermille = 700;

// Who set the profile in force last.
enum class ProfileSetBy : uint8_t { Default = 0, Owner = 1, Governor = 2 };

struct BudgetFile {
  bool pinned = false;                                // the owner chose (never moved by the governor)
  HistoryProfile pinnedProfile = kDefaultHistoryProfile;
  HistoryProfile governed = kDefaultHistoryProfile;   // the governor's profile (unpinned)
  ProfileSetBy setBy = ProfileSetBy::Default;
  int64_t setDay = 0;                                 // calendar day number of the last change (0 = none)
  int64_t night = 0;                                  // running day of the last nightly run (0 = never)
  uint32_t month = 0;                                 // the spend month (YYYYMM) at that run
  uint64_t snapMicro = 0;                             // the month's spend at that run
  uint8_t nObs = 0;                                   // nights observed under the profile in force
  uint64_t obs[kGovernorObsNights] = {};              // their spend, oldest first
  uint8_t quiet = 0;                                  // quiet nights in a row toward a step up
  uint32_t warnedMonth = 0;                           // the month a "nothing fits" or pinned warning went out
  HistoryProfile effective() const { return pinned ? pinnedProfile : governed; }
};

enum class BudgetFileRead : uint8_t { Ok = 0, Missing, Corrupt };
// Parse /data/budget.json. Missing (empty text) or Corrupt (anything that does not parse into
// a known profile): `out` is the default file. Never throws.
BudgetFileRead parseBudgetFile(const std::string& text, BudgetFile& out);
std::string budgetFileJson(const BudgetFile& f);

// What the governor knows on a night (the device's clock and the spend store).
struct GovernorFacts {
  int64_t runningDay = 0;      // orch::runningDay(now, offset, dayStart): one run per value
  int64_t calendarDay = 0;     // today's local calendar day number
  uint32_t monthKey = 0;       // the spend store's month (YYYYMM)
  uint8_t dayOfMonth = 1;      // today's day of the month (1..31)
  uint8_t daysInMonth = 30;
  uint64_t spentMicro = 0;     // this month's spend, every provider and paid call
  bool ceilingActive = false;  // a ceiling holds (the default 20 USD, or the owner's)
  uint64_t ceilingMicro = 0;
};
// The facts at `epoch` on a device `offsetMin` east of UTC with its day start at `startMin`,
// with the spend store's figures. False when the clock is not set.
bool governorFactsAt(uint64_t epoch, int32_t offsetMin, uint16_t startMin, uint32_t monthKey, uint64_t spentMicro,
                     bool ceilingActive, uint64_t ceilingMicro, GovernorFacts& out);

struct GovernorStep {
  enum class Kind : uint8_t { None = 0, Observed, Down, Up, NothingFits, PinnedOver };
  Kind kind = Kind::None;
  HistoryProfile from = kDefaultHistoryProfile;
  HistoryProfile to = kDefaultHistoryProfile;
  uint64_t projectedMicro = 0;   // the month at this rate under `to`
  int64_t clampDay = 0;          // calendar day the ceiling is reached at that rate (0 = not this month)
  std::string line;              // the one line the owner is told ("" = nothing to say)
  std::string record;            // the change record's line ("" = nothing to record)
  bool changed() const { return from != to; }
};

// The projection of the month under `p`, from what `f` observed (0 when it has not observed
// kGovernorMinObs nights).
uint64_t projectedMonthMicro(const BudgetFile& f, const GovernorFacts& facts, HistoryProfile p);

// One nightly run. Does nothing (Kind::None) unless facts.runningDay is later than the last
// run's; otherwise records the night's observation and decides by the rule above. Changes `f`
// in place; the caller installs f.effective(), stores the file, records and sends the step.
GovernorStep governorNight(BudgetFile& f, const GovernorFacts& facts);

// The owner's /budget profile <name>: pinned, observations started again. `day` is today's
// calendar day number (0 = clock unset).
void pinProfile(BudgetFile& f, HistoryProfile p, int64_t day);
// /budget profile auto: unpinned, the governor starts again from standard.
void unpinProfile(BudgetFile& f, int64_t day);

// ---- the owner's words (plain, ASCII, read well aloud) -------------------------------

// "$23.10".
std::string usdText(uint64_t micro);
// "24 November" for a calendar day number.
std::string dayMonthText(int64_t calendarDay);
// The status line /usage and the device status carry: the profile in force, who set it, and
// when the file could not be read, that the default applies for that reason.
std::string profileStatusText(const BudgetFile& f, BudgetFileRead read);
// The reply to /budget profile (`arg` "" = show, "auto", or a profile slug); `ok` false when
// the word is none of these.
std::string profileReplyText(const BudgetFile& f, const std::string& arg, bool ok);

}  // namespace orch
}  // namespace nimbus
