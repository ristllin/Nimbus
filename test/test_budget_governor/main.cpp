// test_budget_governor - the owner's history profile and the nightly spending advice (lane
// RB-BG, budget_governor.h; owner ruling 81) on a virtual clock: simulated months of spending,
// the owner's choices and ceilings, the file it keeps, and the class rules: no profile ever moves
// without the owner, he is told exactly once a month over the line, a smaller profile he chose
// is respected and kept, and every word is plain.
#include <unity.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../support/recorded_models.h"   // readFixture
#include "nimbus/orch/budget_command.h"   // parseBudgetArgs: /budget profile
#include "nimbus/orch/budget_governor.h"
#include "nimbus/orch/day_start.h"
#include "nimbus/orch/loops.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint64_t kUsd = 1000000ull;
constexpr uint64_t kCeiling = 20 * kUsd;   // the default ceiling
const HistoryProfile kAll[] = {HistoryProfile::Tiny, HistoryProfile::Small, HistoryProfile::Medium,
                               HistoryProfile::Large, HistoryProfile::Full};

// A device on a virtual clock: one night per day at the day start (04:00, UTC), the spend
// store's month count, and a spending rate that follows the profile in force.
struct Sim {
  BudgetFile file;
  int64_t day = 0;              // calendar day number of the current day
  uint64_t spent = 0;           // the month's spend
  uint32_t month = 0;
  bool ceilingActive = true;
  uint64_t ceiling = kCeiling;
  double baseUsdPerDay = 0.5;   // the spending at medium (cost ratio 1000)
  double noise = 0;             // +- share of the day's spend
  uint32_t lcg = 1;
  std::vector<BudgetAdvice> told;            // every advice given
  std::vector<size_t> toldAt;                // the night index of each
  std::vector<std::pair<uint32_t, int>> perMonth;   // month, advices that month
  std::vector<HistoryProfile> byNight;

  explicit Sim(int y, unsigned m, unsigned d) { day = daysFromCivil(y, m, d); month = monthOf(day); }

  static uint32_t monthOf(int64_t calDay) {
    const CivilTime c = civilFromDays((uint32_t)calDay);
    return (uint32_t)(c.year * 100 + c.month);
  }
  double jitter() {
    lcg = lcg * 1103515245u + 12345u;
    return ((double)((lcg >> 8) % 10001) / 10000.0 * 2 - 1) * noise;
  }
  // One whole day of spending, then the night at the next day's start.
  BudgetAdvice live() {
    const double usd = baseUsdPerDay * profileCostPermille(file.effective()) / 1000.0 * (1 + jitter());
    spent += (uint64_t)std::llround(std::max(0.0, usd) * kUsd);
    day++;
    if (monthOf(day) != month) {   // the spend store's month rolls at local midnight
      month = monthOf(day);
      spent = 0;
    }
    return night();
  }
  BudgetAdvice night() {
    const uint64_t epoch = (uint64_t)day * 86400u + 4 * 3600u + 60u;   // 04:01, just after the day start
    GovernorFacts g;
    TEST_ASSERT_TRUE(governorFactsAt(epoch, 0, 240, SpendFacts{month, spent, ceilingActive, ceiling}, g));
    const BudgetAdvice a = governorNight(file, g);
    if (perMonth.empty() || perMonth.back().first != month) perMonth.push_back({month, 0});
    if (a.kind == BudgetAdvice::Kind::Advice) {
      told.push_back(a);
      toldAt.push_back(byNight.size());
      perMonth.back().second++;
    }
    byNight.push_back(file.effective());
    return a;
  }
  void run(int days) {
    for (int i = 0; i < days; i++) live();
  }
};

bool hasEmDash(const std::string& s) { return s.find("\xE2\x80\x94") != std::string::npos; } // em-dash-ok: the test looks for the banned bytes

void assertPlain(const BudgetAdvice& a) {
  TEST_ASSERT_FALSE(a.line.empty());
  TEST_ASSERT_FALSE(a.record.empty());   // the Usage page's record
  TEST_ASSERT_TRUE(a.record.size() <= 111);
  TEST_ASSERT_FALSE(hasEmDash(a.line));
  for (char c : a.line) TEST_ASSERT_TRUE_MESSAGE((unsigned char)c < 128, "ASCII only");
}

// ---- the class rules -----------------------------------------------------------------

// The class: over five months, at every spending rate, every profile the owner may hold, with
// and without noise, the profile never moves by itself, and in each month the owner is told at
// most once, and exactly once whenever that month's spending crossed the line on the profile in
// force after the nights it needs to observe.
void test_no_profile_ever_moves_without_the_owner() {
  int advices = 0;
  for (const HistoryProfile p : kAll)
    for (double rate = 0.05; rate < 3.0; rate += 0.11)
      for (const double noise : {0.0, 0.4}) {
        Sim s(2026, 12, 9);
        chooseProfile(s.file, p, s.day);
        s.baseUsdPerDay = rate;
        s.noise = noise;
        s.night();
        s.run(150);
        for (const HistoryProfile at : s.byNight) TEST_ASSERT_TRUE(at == p);
        for (const auto& m : s.perMonth) TEST_ASSERT_TRUE_MESSAGE(m.second <= 1, std::to_string(m.first).c_str());
        for (const BudgetAdvice& a : s.told) {
          assertPlain(a);
          TEST_ASSERT_TRUE(a.profile == p);
          TEST_ASSERT_TRUE(!a.fits || a.fit < p);   // only ever a smaller profile, as advice
        }
        advices += (int)s.told.size();
        const double month = rate * 30 * profileCostPermille(p) / 1000.0;
        if (noise == 0 && month > 20) {   // clearly over: told in every whole month
          for (size_t i = 1; i + 1 < s.perMonth.size(); i++)
            TEST_ASSERT_EQUAL_MESSAGE(1, s.perMonth[i].second, std::to_string(s.perMonth[i].first).c_str());
        }
        if (month < 15) TEST_ASSERT_EQUAL(0, (int)s.told.size());   // clearly under: never told
      }
  TEST_ASSERT_TRUE(advices > 100);
}

// The plan's month on the default profile: spending on course past the 20 USD ceiling. The
// owner is told once, with the figure, the date the ceiling is reached and the profile that
// would fit; the profile stays full; nothing is said again that month; the next month over the
// line he is told once more. Under the line, or with the ceiling off, nothing is said.
void test_an_overspend_month_tells_the_owner_once() {
  Sim s(2026, 11, 1);
  s.baseUsdPerDay = 0.5;   // full spends 0.72 USD a day: 21.7 USD in November
  s.night();
  s.run(29);
  TEST_ASSERT_EQUAL(1, (int)s.told.size());
  const BudgetAdvice a = s.told[0];
  assertPlain(a);
  TEST_ASSERT_TRUE(s.file.effective() == HistoryProfile::Full);
  TEST_ASSERT_TRUE(a.fits && a.fit == HistoryProfile::Medium);   // large is counted at full's ratio
  TEST_ASSERT_TRUE(a.line.find("/budget profile medium") != std::string::npos);
  TEST_ASSERT_TRUE(a.line.find("nothing changes otherwise") != std::string::npos);
  TEST_ASSERT_TRUE(a.clampDay > 0);
  TEST_ASSERT_TRUE(a.line.find("reached on " + dayMonthText(a.clampDay)) != std::string::npos);
  TEST_ASSERT_TRUE(a.record.find("medium would fit") != std::string::npos);
  // Told on the night it first knew: three observed nights (2 to 4 November) and over the line.
  TEST_ASSERT_EQUAL(3, (int)s.toldAt[0]);
  s.run(31);   // December, the same spending: once more
  TEST_ASSERT_EQUAL(2, (int)s.told.size());

  Sim under(2026, 11, 1);
  under.baseUsdPerDay = 0.3;   // full 13 USD a month
  under.night();
  under.run(60);
  TEST_ASSERT_EQUAL(0, (int)under.told.size());

  Sim off(2026, 11, 1);
  off.ceilingActive = false;   // the owner turned the ceiling off (or only provider limits)
  off.baseUsdPerDay = 5.0;
  off.night();
  off.run(60);
  TEST_ASSERT_EQUAL(0, (int)off.told.size());

  Sim high(2026, 11, 1);   // a ceiling the owner set higher is the one projected against
  high.ceiling = 50 * kUsd;
  high.night();
  high.run(60);
  TEST_ASSERT_EQUAL(0, (int)high.told.size());
}

// Nothing fits: even tiny would pass the ceiling. The line says so and names the date; when the
// ceiling is already reached it says that; between 0.9 of the ceiling and the ceiling it says
// the month ends just under it.
void test_when_nothing_fits_it_says_so_and_names_the_date() {
  Sim s(2026, 11, 1);
  s.baseUsdPerDay = 3.0;   // tiny would still spend 1.73 USD a day
  s.night();
  s.run(4);
  TEST_ASSERT_EQUAL(1, (int)s.told.size());
  const BudgetAdvice a = s.told[0];
  assertPlain(a);
  TEST_ASSERT_FALSE(a.fits);
  TEST_ASSERT_TRUE(a.line.find("Even the shortest") != std::string::npos);
  // Told on the night of 4 November with 1 to 3 November spent (3 x 4.344 = 13.03 USD): at 4.344
  // USD a day the other 6.97 USD take 2 days counting the 4th: 5 November.
  TEST_ASSERT_EQUAL_STRING("5 November", dayMonthText(a.clampDay).c_str());
  TEST_ASSERT_TRUE(a.line.find("reached on 5 November") != std::string::npos);
  TEST_ASSERT_TRUE(s.file.effective() == HistoryProfile::Full);

  Sim reached(2026, 11, 1);   // spent at or over the ceiling when it first knows
  reached.baseUsdPerDay = 6.0;
  reached.night();
  reached.run(4);
  TEST_ASSERT_TRUE(reached.told[0].line.find("already reached") != std::string::npos);

  Sim tiny(2026, 11, 1);   // an owner already on tiny
  chooseProfile(tiny.file, HistoryProfile::Tiny, tiny.day);
  tiny.baseUsdPerDay = 2.0;
  tiny.night();
  tiny.run(10);
  TEST_ASSERT_EQUAL(1, (int)tiny.told.size());
  TEST_ASSERT_TRUE(tiny.told[0].line.find("raise the ceiling") != std::string::npos);
}

// The owner's smaller profile is respected and kept: in force from his command on, through
// any spending and any number of months, and the advice only ever names a smaller one.
void test_an_owner_chosen_smaller_profile_is_kept() {
  Sim s(2026, 11, 1);
  s.baseUsdPerDay = 0.5;
  s.night();
  s.run(10);   // told once on full
  TEST_ASSERT_EQUAL(1, (int)s.told.size());
  chooseProfile(s.file, HistoryProfile::Medium, s.day);   // the owner follows the advice
  TEST_ASSERT_TRUE(profileStatusText(s.file, BudgetFileRead::Ok).find("chosen by you") != std::string::npos);
  TEST_ASSERT_TRUE(profileReplyText(s.file, "medium", true).find("stays until you change it") != std::string::npos);
  s.run(120);
  for (size_t i = 11; i < s.byNight.size(); i++) TEST_ASSERT_TRUE(s.byNight[i] == HistoryProfile::Medium);
  TEST_ASSERT_EQUAL(1, (int)s.told.size());   // 15 USD a month on medium: quiet from then on
  chooseProfile(s.file, HistoryProfile::Full, s.day);   // and back to the default
  TEST_ASSERT_TRUE(profileReplyText(s.file, "full", true).find("back to full") != std::string::npos);
  TEST_ASSERT_TRUE(profileStatusText(s.file, BudgetFileRead::Ok).find("the default") != std::string::npos);
}

// The clock: one run per running day; a clock set back a little runs nothing; one found far
// behind the last night starts again; nights the device slept share the spend between them; a
// night that crosses into a new month observes nothing.
void test_the_virtual_clock_edges() {
  Sim s(2026, 11, 10);
  s.night();
  const BudgetFile was = s.file;
  TEST_ASSERT_TRUE(s.night().kind == BudgetAdvice::Kind::None);   // the same running day again
  s.day -= 2;
  TEST_ASSERT_TRUE(s.night().kind == BudgetAdvice::Kind::None);   // the clock went back
  TEST_ASSERT_EQUAL(was.night, s.file.night);
  s.day += 2;
  s.day += 5;   // five days off, 1 USD spent meanwhile: five nights of 0.20 USD
  s.spent += kUsd;
  s.night();
  TEST_ASSERT_EQUAL(5, s.file.nObs);
  for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_UINT64(kUsd / 5, s.file.obs[i]);
  s.day += 30;   // thirty days off into a new month: nothing observed, never more than seven kept
  s.month = Sim::monthOf(s.day);
  s.spent = 0;
  s.night();
  TEST_ASSERT_EQUAL(5, s.file.nObs);
  s.run(1);
  TEST_ASSERT_EQUAL(6, s.file.nObs);
  s.file.night = s.day + 4 * 365;   // a night that ran on a clock four years ahead
  s.file.nObs = 6;
  TEST_ASSERT_TRUE(s.night().kind != BudgetAdvice::Kind::None);
  TEST_ASSERT_EQUAL(s.day, s.file.night);
  TEST_ASSERT_EQUAL(0, s.file.nObs);
  GovernorFacts g;
  TEST_ASSERT_FALSE(governorFactsAt(1000, 0, 240, SpendFacts{0, 0, true, kCeiling}, g));   // no clock
  TEST_ASSERT_TRUE(governorFactsAt((uint64_t)s.day * 86400u + 3600u, 0, 240, SpendFacts{s.month, 0, true, kCeiling}, g));
  TEST_ASSERT_EQUAL(s.day - 1, g.runningDay);   // before the day start the running day is yesterday's
  TEST_ASSERT_EQUAL(s.day, g.calendarDay);
}

// The file: a round trip keeps every field; a missing or unreadable file reads as full and the
// owner's status says which; a damaged file whose profile still reads keeps the owner's choice.
void test_the_file_round_trips_and_a_bad_file_reads_as_the_default() {
  BudgetFile f;
  chooseProfile(f, HistoryProfile::Small, 20777);
  f.night = 20777;
  f.month = 202611;
  f.snapMicro = 123456789;
  f.nObs = 3;
  f.obs[0] = 1;
  f.obs[1] = 2;
  f.obs[2] = 3;
  f.warnedMonth = 202610;
  BudgetFile back;
  TEST_ASSERT_TRUE(parseBudgetFile(budgetFileJson(f), back) == BudgetFileRead::Ok);
  TEST_ASSERT_EQUAL_STRING(budgetFileJson(f).c_str(), budgetFileJson(back).c_str());
  TEST_ASSERT_TRUE(back.effective() == HistoryProfile::Small);

  BudgetFile d;
  TEST_ASSERT_TRUE(parseBudgetFile("", d) == BudgetFileRead::Missing);
  TEST_ASSERT_TRUE(d.effective() == HistoryProfile::Full);
  TEST_ASSERT_TRUE(profileStatusText(d, BudgetFileRead::Missing).find("the default") != std::string::npos);
  BudgetFile kept;
  TEST_ASSERT_TRUE(parseBudgetFile("{\"v\":1,\"profile\":\"tiny\",\"night\":-4}", kept) == BudgetFileRead::Corrupt);
  TEST_ASSERT_TRUE(kept.effective() == HistoryProfile::Tiny);
  TEST_ASSERT_TRUE(profileStatusText(kept, BudgetFileRead::Corrupt).find("your chosen profile") != std::string::npos);
  BudgetFile spaced;
  TEST_ASSERT_TRUE(parseBudgetFile(" { \"v\": 1, \"profile\": \"large\", \"obs\": [ 1, 2 ] }\n", spaced) ==
                   BudgetFileRead::Ok);
  TEST_ASSERT_TRUE(spaced.profile == HistoryProfile::Large && spaced.nObs == 2 && spaced.obs[1] == 2);
  const char* const kBad[] = {
      "{",
      "[]",
      "nonsense",
      "{\"v\":2,\"profile\":\"full\"}",
      "{\"v\":1,\"profile\":\"huge\"}",
      "{\"v\":1,\"profile\":\"standard\"}",
      "{\"v\":1}",
      "{\"v\":1,\"profile\":\"full\",\"obs\":[1,2,3,4,5,6,7,8]}",
      "{\"v\":1,\"profile\":\"full\",\"obs\":[\"x\"]}",
      "{\"v\":1,\"profile\":\"full\",\"night\":-4}",
  };
  for (const char* bad : kBad) {
    BudgetFile x;
    x.profile = HistoryProfile::Small;   // whatever it held before
    TEST_ASSERT_TRUE_MESSAGE(parseBudgetFile(bad, x) == BudgetFileRead::Corrupt, bad);
    TEST_ASSERT_TRUE_MESSAGE(x.effective() == HistoryProfile::Full, bad);
    TEST_ASSERT_TRUE(profileStatusText(x, BudgetFileRead::Corrupt).find("could not be read") != std::string::npos);
  }
}

// The cost ratios are the measured ones (tests/evals/measured/cost.json, RB-M0b): tiny, small,
// medium and large are engine 2's tight, lean, standard and wide; full is counted at large's.
void test_the_cost_ratios_are_the_measured_ones() {
  JsonDocument d;
  TEST_ASSERT_FALSE(deserializeJson(d, recorded::readFixture("tests/evals/measured/cost.json")));
  JsonObjectConst e = d["engine2_sonnet"];
  const double medium = e["standard"]["blended_turn_usd_at_measured_cold_share"] | 0.0;
  TEST_ASSERT_TRUE(medium > 0);
  const char* const kMeasured[] = {"tight", "lean", "standard", "wide"};
  for (int i = 0; i < 4; i++) {
    const double v = e[kMeasured[i]]["blended_turn_usd_at_measured_cold_share"] | 0.0;
    TEST_ASSERT_EQUAL_MESSAGE((int)std::lround(v / medium * 1000), (int)profileCostPermille(kAll[i]), kMeasured[i]);
  }
  TEST_ASSERT_EQUAL(profileCostPermille(HistoryProfile::Large), profileCostPermille(HistoryProfile::Full));
}

// /budget profile: show, a profile (any case); anything else, "auto" and the old names
// included, is an error; a profile command never keeps a waiting limit change; a command that
// changes nothing writes nothing.
void test_the_profile_command() {
  BudgetRequest r = parseBudgetArgs("profile");
  TEST_ASSERT_TRUE(r.verb == BudgetVerb::Profile && r.target.empty());
  r = parseBudgetArgs("profile MEDIUM");
  TEST_ASSERT_TRUE(r.verb == BudgetVerb::Profile && r.target == "medium");
  r = parseBudgetArgs("profile full");
  TEST_ASSERT_TRUE(r.verb == BudgetVerb::Profile && r.target == "full");
  for (const char* bad : {"profile huge", "profile auto", "profile standard", "profile lean now", "profile 5"}) {
    r = parseBudgetArgs(bad);
    TEST_ASSERT_TRUE_MESSAGE(r.verb == BudgetVerb::Error, bad);
    TEST_ASSERT_TRUE(r.error.find("/budget profile") != std::string::npos);
  }
  Command c;
  c.isCommand = true;
  c.verb = "budget";
  c.args = "profile small";
  TEST_ASSERT_FALSE(budgetKeepsWaiting(c));
  TEST_ASSERT_TRUE(runsWhilePaused("/budget profile small"));
  BudgetFile f;
  TEST_ASSERT_FALSE(profileCommandChanges(f, "full"));
  TEST_ASSERT_TRUE(profileCommandChanges(f, "small"));
  chooseProfile(f, HistoryProfile::Small, 1);
  TEST_ASSERT_FALSE(profileCommandChanges(f, "small"));
  TEST_ASSERT_TRUE(profileCommandChanges(f, "full"));
  TEST_ASSERT_TRUE(profileReplyText(f, "x", false).find("Unknown profile") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("$23.10", usdText(23100000).c_str());
  TEST_ASSERT_EQUAL_STRING("$0.01", usdText(5000).c_str());
}

// Mutation survivors: the advice waits for three observed nights even when the month is already
// past the line, and a profile the owner chooses is observed afresh (the nights under the old one
// would overstate the new one's spending and tell him again for nothing).
void test_the_advice_waits_and_a_choice_is_observed_afresh() {
  Sim early(2026, 11, 1);
  early.night();
  early.spent = 19 * kUsd;   // a heavy first day
  early.live();
  early.live();
  TEST_ASSERT_EQUAL(0, (int)early.told.size());   // two nights observed: not yet
  early.live();
  TEST_ASSERT_EQUAL(1, (int)early.told.size());
  Sim s(2026, 11, 1);
  s.baseUsdPerDay = 0.5;
  s.night();
  s.run(28);   // told in November on full
  TEST_ASSERT_EQUAL(1, (int)s.told.size());
  chooseProfile(s.file, HistoryProfile::Medium, s.day);   // on 29 November
  TEST_ASSERT_EQUAL(0, s.file.nObs);
  s.run(31);   // December on medium: 15.5 USD, under the line
  TEST_ASSERT_EQUAL(1, (int)s.told.size());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_profile_ever_moves_without_the_owner);
  RUN_TEST(test_an_overspend_month_tells_the_owner_once);
  RUN_TEST(test_when_nothing_fits_it_says_so_and_names_the_date);
  RUN_TEST(test_an_owner_chosen_smaller_profile_is_kept);
  RUN_TEST(test_the_virtual_clock_edges);
  RUN_TEST(test_the_file_round_trips_and_a_bad_file_reads_as_the_default);
  RUN_TEST(test_the_cost_ratios_are_the_measured_ones);
  RUN_TEST(test_the_profile_command);
  RUN_TEST(test_the_advice_waits_and_a_choice_is_observed_afresh);
  return UNITY_END();
}
