#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "nimbus/orch/budget.h"   // HistoryProfile

// budget_governor - the owner's history profile and the nightly spending advice (lane RB-BG;
// engine-independent, ships in v5.2.0). Pure: no Arduino, no clock of its own, host-tested in
// test/test_budget_governor (simulated months on a virtual clock). The device
// (src/agent/budget_profile_store.cpp) and nimbusd (nimbusd/src/hosted_budget.h) keep the file
// and feed it the facts.
//
// Owner ruling 81: nothing lowers conversation history by itself. The profile in force is the
// owner's choice (default full: each head's own marks, as released v5.1.2 derives them); the
// device never changes it. What the nightly run does instead:
//   1. Project the month: what it spent so far plus the mean of the last seven observed nights
//      (at least three, all under the profile in force) times the days left.
//   2. When that is over 0.9 of the ceiling, tell the owner once that month (Telegram, the web
//      chat, the Usage page's record, the durable log): the projection, the date the ceiling is
//      reached at that rate, and the largest smaller profile that would fit (each profile's
//      measured cost ratio, tests/evals/measured/cost.json), or that none would. The owner
//      decides with /budget profile.
//   3. No ceiling (the owner turned it off, or only provider limits are set) is a quiet night.
//      The advice never writes the ceiling and never changes the profile.
// No step-up was kept either: there is no step down for it to undo.
//
// The file, /data/budget.json (never NVS): the owner's profile and the advice's own state. A
// missing file reads as full. An unreadable one reads as full too, except that a profile whose
// own field still reads is kept; the advice does not run on it and the owner's status says so,
// until the owner's next /budget profile writes the file again.
namespace nimbus {
namespace orch {

// The measured month cost of each profile against medium (60,000/30,000 B), in thousandths
// (RB-M0b, tests/evals/measured/cost.json, engine2_sonnet blended turn USD at the measured cold
// share: tight 0.03737, lean 0.051261, standard 0.064633, wide 0.093597 for tiny, small, medium
// and large; test_budget_governor reads the file and fails when these drift). Full has no
// measurement of its own and is counted at large's ratio.
uint32_t profileCostPermille(HistoryProfile p);

inline constexpr uint8_t kGovernorObsNights = 7;    // the mean's window
inline constexpr uint8_t kGovernorMinObs = 3;       // nights observed before any advice
inline constexpr uint32_t kGovernorAdvicePermille = 900;

struct BudgetFile {
  HistoryProfile profile = kDefaultHistoryProfile;   // the owner's choice (full = the default)
  int64_t setDay = 0;                                // calendar day the owner last chose (0 = never)
  int64_t night = 0;                                 // running day of the last nightly run (0 = never)
  uint32_t month = 0;                                // the spend month (YYYYMM) at that run
  uint64_t snapMicro = 0;                            // the month's spend at that run
  uint8_t nObs = 0;                                  // nights observed under the profile in force
  uint64_t obs[kGovernorObsNights] = {};             // their spend, oldest first
  uint32_t warnedMonth = 0;                          // the month the owner was told (one advice a month)
  HistoryProfile effective() const { return profile; }
};

enum class BudgetFileRead : uint8_t { Ok = 0, Missing, Corrupt };
// Parse /data/budget.json. Missing (empty text) or Corrupt (anything not of the shape this code
// writes): `out` is the default file, keeping a profile whose own field reads. Never throws.
BudgetFileRead parseBudgetFile(const std::string& text, BudgetFile& out);
std::string budgetFileJson(const BudgetFile& f);

// What a night knows (the device's clock and the spend store).
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
// The spend store's figures a night reads.
struct SpendFacts {
  uint32_t monthKey = 0;
  uint64_t spentMicro = 0;
  bool ceilingActive = false;
  uint64_t ceilingMicro = 0;
};
// The facts at `epoch` on a device `offsetMin` east of UTC with its day start at `startMin`,
// with the spend store's figures. False when the clock is not set.
bool governorFactsAt(uint64_t epoch, int32_t offsetMin, uint16_t startMin, const SpendFacts& spend,
                     GovernorFacts& out);

struct BudgetAdvice {
  enum class Kind : uint8_t { None = 0, Observed, Advice };
  Kind kind = Kind::None;
  HistoryProfile profile = kDefaultHistoryProfile;   // the owner's profile, never changed here
  uint64_t projectedMicro = 0;   // the month at this rate under the profile in force
  int64_t clampDay = 0;          // calendar day the ceiling is reached at that rate (0 = not this month)
  bool fits = false;             // a smaller profile would keep the month under 0.9 of the ceiling
  HistoryProfile fit = kDefaultHistoryProfile;   // the largest such profile (when fits)
  std::string line;              // the one line the owner is told ("" = nothing to say)
  std::string record;            // the Usage page's record line ("" = nothing to record)
};

// The projection of the month under `p`, from what `f` observed (0 when it has not observed
// kGovernorMinObs nights).
uint64_t projectedMonthMicro(const BudgetFile& f, const GovernorFacts& facts, HistoryProfile p);

// One nightly run. Does nothing (Kind::None) unless facts.runningDay is later than the last
// run's; otherwise records the night's observation and, over 0.9 of the ceiling and not yet
// this month, gives the advice. Changes only the advice's own state in `f`, never its profile.
BudgetAdvice governorNight(BudgetFile& f, const GovernorFacts& facts);

// The owner's /budget profile <name>: in force from now on, observations started again. `day` is
// today's calendar day number (0 = clock unset).
void chooseProfile(BudgetFile& f, HistoryProfile p, int64_t day);
// Whether /budget profile `arg` (a slug) changes anything: the profile already in force is
// answered with the status and writes nothing.
bool profileCommandChanges(const BudgetFile& f, const std::string& arg);

// ---- the owner's words (plain, ASCII, read well aloud) -------------------------------

std::string usdText(uint64_t micro);           // "$23.10"
std::string dayMonthText(int64_t calendarDay);  // "24 November"
// The status line /usage carries: the profile in force and whether it is the owner's choice;
// when the file could not be read, that it is not adjusted until it is written again.
std::string profileStatusText(const BudgetFile& f, BudgetFileRead read);
// The reply to /budget profile (`arg` "" = show, or a profile slug); `ok` false when the word
// is none of these.
std::string profileReplyText(const BudgetFile& f, const std::string& arg, bool ok);

}  // namespace orch
}  // namespace nimbus
