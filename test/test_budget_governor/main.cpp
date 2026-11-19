// test_budget_governor - the history profile's step-down governor (lane RB-BG,
// budget_governor.h) on a virtual clock: simulated months of spending, the owner's pinned
// profile and ceilings, the file it keeps, and the class rules (it never moves a pinned
// profile, never flaps, and every step it takes has a line and a record).
#include <unity.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../support/recorded_models.h"   // readFixture
#include "nimbus/orch/budget_governor.h"
#include "nimbus/orch/day_start.h"
#include "nimbus/orch/loops.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint64_t kUsd = 1000000ull;
constexpr uint64_t kCeiling = 20 * kUsd;   // the default ceiling

// A device on a virtual clock: one night per day at the day start (04:00, UTC), the spend
// store's month count, and a spending rate that follows the profile in force.
struct Sim {
  BudgetFile file;
  int64_t day = 0;              // calendar day number of the current day
  uint64_t spent = 0;           // the month's spend
  uint32_t month = 0;
  bool ceilingActive = true;
  uint64_t ceiling = kCeiling;
  double baseUsdPerDay = 0.75;  // the spending at standard
  double noise = 0;             // +- share of the day's spend
  uint32_t lcg = 1;
  std::vector<GovernorStep> steps;   // every run that said or changed something
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
  GovernorStep live() {
    const double usd = baseUsdPerDay * profileCostPermille(file.effective()) / 1000.0 * (1 + jitter());
    spent += (uint64_t)std::llround(std::max(0.0, usd) * kUsd);
    day++;
    if (monthOf(day) != month) {   // the spend store's month rolls at local midnight
      month = monthOf(day);
      spent = 0;
    }
    return night();
  }
  GovernorStep night() {
    const uint64_t epoch = (uint64_t)day * 86400u + 4 * 3600u + 60u;   // 04:01, just after the day start
    GovernorFacts g;
    TEST_ASSERT_TRUE(governorFactsAt(epoch, 0, 240, month, spent, ceilingActive, ceiling, g));
    const GovernorStep st = governorNight(file, g);
    if (!st.line.empty() || st.changed()) steps.push_back(st);
    byNight.push_back(file.effective());
    return st;
  }
  void run(int days) {
    for (int i = 0; i < days; i++) live();
  }
};

bool hasEmDash(const std::string& s) { return s.find("\xE2\x80\x94") != std::string::npos; }

void assertVisible(const GovernorStep& st) {
  // Every step it takes is visible: a line for the owner and a record for the Usage page.
  if (st.changed()) {
    TEST_ASSERT_FALSE_MESSAGE(st.line.empty(), "a change without a line");
    TEST_ASSERT_FALSE_MESSAGE(st.record.empty(), "a change without a record");
    TEST_ASSERT_TRUE(st.record.size() <= 111);   // the change record's row width
  }
  TEST_ASSERT_FALSE(hasEmDash(st.line));
  for (char c : st.line) TEST_ASSERT_TRUE_MESSAGE((unsigned char)c < 128, "ASCII only");
}

// ---- the simulated months --------------------------------------------------------------

// The plan's month: spending on course for 21 USD against the 20 USD default ceiling steps
// down to lean (the largest that fits 0.9 of it) once three nights are observed, holds there,
// and after the owner spends less the next month returns to standard after exactly seven
// quiet nights.
void test_an_overspend_month_steps_down_and_returns_after_seven_quiet_nights() {
  Sim s(2026, 11, 1);
  s.baseUsdPerDay = 0.7;
  s.night();   // the first night only notes where the month stands
  s.run(29);
  TEST_ASSERT_EQUAL(1, (int)s.steps.size());
  const GovernorStep down = s.steps[0];
  TEST_ASSERT_TRUE(down.kind == GovernorStep::Kind::Down);
  TEST_ASSERT_TRUE(down.from == HistoryProfile::Standard && down.to == HistoryProfile::Lean);
  assertVisible(down);
  TEST_ASSERT_TRUE(down.line.find("lean, was standard") != std::string::npos);
  TEST_ASSERT_TRUE(down.line.find("/budget profile standard") != std::string::npos);
  TEST_ASSERT_TRUE(s.byNight[3] == HistoryProfile::Lean);   // nights 1 to 3 observed, the 4th decided
  TEST_ASSERT_TRUE(s.byNight[2] == HistoryProfile::Standard);
  // December: the owner spends less (9 USD a month at standard).
  s.baseUsdPerDay = 0.3;
  const size_t before = s.byNight.size();
  s.run(31);
  TEST_ASSERT_EQUAL(2, (int)s.steps.size());
  const GovernorStep up = s.steps[1];
  TEST_ASSERT_TRUE(up.kind == GovernorStep::Kind::Up && up.to == HistoryProfile::Standard);
  assertVisible(up);
  TEST_ASSERT_TRUE(up.line.find("7 nights") != std::string::npos);
  // Seven quiet observed nights: the move into December observes nothing, three nights
  // observe, then seven more nights in a row project standard under 0.7 of the ceiling.
  size_t upAt = before;
  while (s.byNight[upAt] != HistoryProfile::Standard) upAt++;
  TEST_ASSERT_TRUE_MESSAGE(upAt - before >= 7 + 2, std::to_string(upAt - before).c_str());
  TEST_ASSERT_TRUE_MESSAGE(upAt - before <= 7 + 2 + 3, std::to_string(upAt - before).c_str());
}

// Nothing fits: spending on course past the ceiling even on tight steps down to tight at once
// and says so in one plain line naming the date the ceiling is reached; at tight it says it
// once a month, not every night.
void test_when_nothing_fits_it_names_the_clamp_date() {
  Sim s(2026, 11, 1);
  s.baseUsdPerDay = 1.6;   // tight would still spend about 27.70 USD this month
  s.night();
  s.run(4);
  TEST_ASSERT_EQUAL(1, (int)s.steps.size());
  const GovernorStep st = s.steps[0];
  TEST_ASSERT_TRUE(st.kind == GovernorStep::Kind::NothingFits);
  TEST_ASSERT_TRUE(st.from == HistoryProfile::Standard && st.to == HistoryProfile::Tight);
  assertVisible(st);
  TEST_ASSERT_TRUE(st.clampDay > 0);
  // The date by the governor's own arithmetic: the spend so far, then tight's rate a day.
  const std::string date = dayMonthText(st.clampDay);
  TEST_ASSERT_TRUE_MESSAGE(st.line.find("reached on " + date) != std::string::npos, st.line.c_str());
  TEST_ASSERT_TRUE(st.line.find("tight, was standard") != std::string::npos);
  const CivilTime c = civilFromDays((uint32_t)st.clampDay);
  TEST_ASSERT_EQUAL(11, c.month);
  // Decided on the night of 4 November with 1 to 3 November spent (4.80 USD): tight would
  // spend 1.60 x 0.578 = 0.9248 USD a day, so the other 15.20 USD take 17 days counting the
  // 4th: 20 November.
  TEST_ASSERT_EQUAL_STRING("20 November", date.c_str());
  s.run(20);   // still in November: no second line
  TEST_ASSERT_EQUAL(1, (int)s.steps.size());
  s.run(10);   // December: one more line
  TEST_ASSERT_EQUAL(2, (int)s.steps.size());
  TEST_ASSERT_TRUE(s.steps[1].kind == GovernorStep::Kind::NothingFits && !s.steps[1].changed());
}

// A pinned profile is never moved, through any spending; the governor warns once a month with
// the projected date, and only while the ceiling is in sight.
void test_a_pinned_profile_is_never_moved() {
  for (const HistoryProfile p : {HistoryProfile::Tight, HistoryProfile::Lean, HistoryProfile::Standard,
                                 HistoryProfile::Wide})
    for (const double rate : {0.05, 0.75, 1.6, 5.0}) {
      Sim s(2026, 10, 3);
      pinProfile(s.file, p, s.day);
      s.baseUsdPerDay = rate;
      s.night();
      s.run(120);
      for (const HistoryProfile at : s.byNight) TEST_ASSERT_TRUE(at == p);
      int warnings = 0;
      for (const GovernorStep& st : s.steps) {
        TEST_ASSERT_FALSE(st.changed());
        TEST_ASSERT_TRUE(st.kind == GovernorStep::Kind::PinnedOver);
        TEST_ASSERT_TRUE(st.line.find("/budget profile auto") != std::string::npos);
        assertVisible(st);
        warnings++;
      }
      TEST_ASSERT_TRUE(warnings <= 5);   // at most one a month (October to February)
      const double month = rate * 30 * profileCostPermille(p) / 1000.0;
      if (month > 20) TEST_ASSERT_TRUE(warnings >= 3);
      if (month < 15) TEST_ASSERT_EQUAL(0, warnings);
    }
}

// The owner's ceiling is only read. A ceiling the owner set higher is the one projected
// against; one the owner turned off (or only provider limits) is no ceiling: the governor
// never steps down then, and a stepped-down profile comes back up, one step per 7 nights.
void test_the_owner_s_ceiling_decides_and_is_never_overridden() {
  Sim high(2026, 11, 1);
  high.ceiling = 50 * kUsd;
  high.night();
  high.run(60);
  TEST_ASSERT_EQUAL(0, (int)high.steps.size());   // 22.50 USD is well under 0.9 of 50

  Sim off(2026, 11, 1);
  off.file.governed = HistoryProfile::Tight;
  off.ceilingActive = false;
  off.baseUsdPerDay = 5.0;   // 150 USD a month: no ceiling to protect
  off.night();
  off.run(30);
  TEST_ASSERT_EQUAL(2, (int)off.steps.size());
  TEST_ASSERT_TRUE(off.steps[0].to == HistoryProfile::Lean && off.steps[1].to == HistoryProfile::Standard);
  for (const GovernorStep& st : off.steps) assertVisible(st);
  for (size_t i = 1; i < off.byNight.size(); i++) TEST_ASSERT_TRUE(off.byNight[i] >= off.byNight[i - 1]);
  TEST_ASSERT_TRUE(off.file.effective() == HistoryProfile::Standard);   // never wide by itself
}

// The class: at any constant spending rate the profile moves in one direction only (no
// down then up, no up then down), over five months from any day of the month, at every
// ceiling state. 0.9 / 0.7 exceeds 31 / 28.
void test_a_constant_rate_never_flaps() {
  int moved = 0;
  for (double rate = 0.05; rate < 3.0; rate += 0.0175)
    for (const unsigned startDay : {1u, 9u, 17u, 28u}) {
      Sim s(2026, 12, startDay);   // through February's 28 days
      s.baseUsdPerDay = rate;
      s.night();
      s.run(150);
      int dir = 0;
      for (size_t i = 1; i < s.byNight.size(); i++) {
        if (s.byNight[i] == s.byNight[i - 1]) continue;
        const int d = s.byNight[i] > s.byNight[i - 1] ? 1 : -1;
        moved++;
        char what[96];
        std::snprintf(what, sizeof what, "rate %.4f from day %u: reversed on night %zu", rate, startDay, i);
        TEST_ASSERT_TRUE_MESSAGE(dir == 0 || dir == d, what);
        dir = d;
      }
    }
  TEST_ASSERT_TRUE(moved > 50);
}

// The class with noise: daily spending +-40% around a rate. Whatever it does, a step down is
// never undone in under nine nights (three observed, the third the first of seven quiet) and a
// step up never in under three (observed): the hysteresis is in the rule, not in luck.
void test_a_noisy_rate_never_reverses_quickly() {
  int reversals = 0;
  for (uint32_t seed = 1; seed <= 60; seed++)
    for (const double rate : {0.55, 0.62, 0.7, 0.85}) {
      Sim s(2026, 11, 5);
      s.lcg = seed;
      s.noise = 0.4;
      s.baseUsdPerDay = rate;
      s.night();
      s.run(120);
      size_t lastAt = 0;
      int lastDir = 0;
      for (size_t i = 1; i < s.byNight.size(); i++) {
        if (s.byNight[i] == s.byNight[i - 1]) continue;
        const int d = s.byNight[i] > s.byNight[i - 1] ? 1 : -1;
        if (lastDir && d != lastDir) {
          reversals++;
          const size_t gap = i - lastAt;
          TEST_ASSERT_TRUE_MESSAGE(gap >= (lastDir < 0 ? 9u : 3u), std::to_string(gap).c_str());
        }
        lastAt = i;
        lastDir = d;
      }
      for (const GovernorStep& st : s.steps) assertVisible(st);
    }
  std::printf("noisy months: %d reversals in 240 runs of 120 nights\n", reversals);
}

// The clock: one run per running day; a clock set back runs nothing; nights the device slept
// share the spend between them; a night that crosses into a new month observes nothing.
void test_the_virtual_clock_edges() {
  Sim s(2026, 11, 10);
  s.night();
  const BudgetFile was = s.file;
  TEST_ASSERT_TRUE(s.night().kind == GovernorStep::Kind::None);   // the same running day again
  s.day -= 2;
  TEST_ASSERT_TRUE(s.night().kind == GovernorStep::Kind::None);   // the clock went back
  TEST_ASSERT_EQUAL(was.night, s.file.night);
  s.day += 2;
  // Five days off, 1 USD spent meanwhile: five nights of 0.20 USD (quiet: nothing moves).
  s.day += 5;
  s.spent += kUsd;
  s.night();
  TEST_ASSERT_EQUAL(5, s.file.nObs);
  for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_UINT64(kUsd / 5, s.file.obs[i]);
  // Thirty days off: never more than seven nights kept.
  s.day += 30;
  s.month = Sim::monthOf(s.day);
  s.spent = 0;
  s.night();   // a new month: nothing observed
  TEST_ASSERT_EQUAL(5, s.file.nObs);
  s.run(1);
  TEST_ASSERT_EQUAL(6, s.file.nObs);
  // No clock: no facts.
  GovernorFacts g;
  TEST_ASSERT_FALSE(governorFactsAt(1000, 0, 240, 0, 0, true, kCeiling, g));
  // Between midnight and the day start the running day is still yesterday's.
  TEST_ASSERT_TRUE(governorFactsAt((uint64_t)s.day * 86400u + 3600u, 0, 240, s.month, 0, true, kCeiling, g));
  TEST_ASSERT_EQUAL(s.day - 1, g.runningDay);
  TEST_ASSERT_EQUAL(s.day, g.calendarDay);
}

// The file: a round trip keeps every field; a missing or unreadable file reads as the default
// (standard, unpinned) and the owner's status says which.
void test_the_file_round_trips_and_a_bad_file_reads_as_the_default() {
  BudgetFile f;
  pinProfile(f, HistoryProfile::Wide, 20777);
  f.governed = HistoryProfile::Lean;
  f.night = 20777;
  f.month = 202611;
  f.snapMicro = 123456789;
  f.nObs = 3;
  f.obs[0] = 1;
  f.obs[1] = 2;
  f.obs[2] = 3;
  f.quiet = 2;
  f.warnedMonth = 202610;
  BudgetFile back;
  TEST_ASSERT_TRUE(parseBudgetFile(budgetFileJson(f), back) == BudgetFileRead::Ok);
  TEST_ASSERT_EQUAL_STRING(budgetFileJson(f).c_str(), budgetFileJson(back).c_str());
  TEST_ASSERT_TRUE(back.effective() == HistoryProfile::Wide);

  BudgetFile d;
  TEST_ASSERT_TRUE(parseBudgetFile("", d) == BudgetFileRead::Missing);
  TEST_ASSERT_TRUE(d.effective() == HistoryProfile::Standard && !d.pinned);
  TEST_ASSERT_TRUE(profileStatusText(d, BudgetFileRead::Missing).find("default") != std::string::npos);
  const char* const kBad[] = {
      "{",
      "[]",
      "nonsense",
      "{\"v\":2,\"pinned\":\"\",\"governed\":\"lean\",\"by\":\"device\"}",
      "{\"v\":1,\"pinned\":\"huge\",\"governed\":\"lean\",\"by\":\"device\"}",
      "{\"v\":1,\"pinned\":\"\",\"governed\":\"medium\",\"by\":\"device\"}",
      "{\"v\":1,\"pinned\":\"\",\"governed\":\"lean\",\"by\":\"someone\"}",
      "{\"v\":1,\"pinned\":\"\",\"governed\":\"lean\",\"by\":\"device\",\"obs\":[1,2,3,4,5,6,7,8]}",
      "{\"v\":1,\"pinned\":\"\",\"governed\":\"lean\",\"by\":\"device\",\"obs\":[\"x\"]}",
      "{\"v\":1,\"pinned\":\"\",\"governed\":\"lean\",\"by\":\"device\",\"quiet\":99}",
      "{\"v\":1,\"pinned\":\"\",\"governed\":\"lean\",\"by\":\"device\",\"night\":-4}",
  };
  for (const char* bad : kBad) {
    BudgetFile x;
    x.pinned = true;   // whatever it held before
    TEST_ASSERT_TRUE_MESSAGE(parseBudgetFile(bad, x) == BudgetFileRead::Corrupt, bad);
    TEST_ASSERT_TRUE_MESSAGE(x.effective() == HistoryProfile::Standard && !x.pinned, bad);
    TEST_ASSERT_TRUE(profileStatusText(x, BudgetFileRead::Corrupt).find("could not be read") != std::string::npos);
  }
}

// The cost ratios are the measured ones (tests/evals/measured/cost.json, RB-M0b).
void test_the_cost_ratios_are_the_measured_ones() {
  JsonDocument d;
  TEST_ASSERT_FALSE(deserializeJson(d, recorded::readFixture("tests/evals/measured/cost.json")));
  JsonObjectConst e = d["engine2_sonnet"];
  const double standard = e["standard"]["blended_turn_usd_at_measured_cold_share"] | 0.0;
  TEST_ASSERT_TRUE(standard > 0);
  for (const HistoryProfile p : {HistoryProfile::Tight, HistoryProfile::Lean, HistoryProfile::Standard,
                                 HistoryProfile::Wide}) {
    const double v = e[historyProfileSlug(p)]["blended_turn_usd_at_measured_cold_share"] | 0.0;
    TEST_ASSERT_EQUAL_MESSAGE((int)std::lround(v / standard * 1000), (int)profileCostPermille(p), historyProfileSlug(p));
  }
}

// The owner's commands on the file: a pin starts the observations again and holds; auto hands
// the profile back to the governor at standard; the words name what applies.
void test_pin_and_auto() {
  Sim s(2026, 11, 1);
  s.baseUsdPerDay = 0.7;
  s.night();
  s.run(10);   // stepped down to lean
  TEST_ASSERT_TRUE(s.file.effective() == HistoryProfile::Lean);
  pinProfile(s.file, HistoryProfile::Standard, s.day);
  TEST_ASSERT_TRUE(s.file.effective() == HistoryProfile::Standard && s.file.nObs == 0);
  TEST_ASSERT_TRUE(profileStatusText(s.file, BudgetFileRead::Ok).find("chosen by you") != std::string::npos);
  TEST_ASSERT_TRUE(profileReplyText(s.file, "standard", true).find("kept there") != std::string::npos);
  s.run(15);
  TEST_ASSERT_TRUE(s.file.effective() == HistoryProfile::Standard);
  unpinProfile(s.file, s.day);
  TEST_ASSERT_TRUE(s.file.effective() == HistoryProfile::Standard && !s.file.pinned);
  TEST_ASSERT_TRUE(profileReplyText(s.file, "auto", true).find("chooses") != std::string::npos);
  s.run(9);   // the governor judges again from standard: down once it has observed three nights
  TEST_ASSERT_TRUE(s.file.effective() < HistoryProfile::Standard);
  TEST_ASSERT_TRUE(profileStatusText(s.file, BudgetFileRead::Ok).find("set by the device") != std::string::npos);
  TEST_ASSERT_TRUE(profileReplyText(s.file, "x", false).find("Unknown profile") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("$23.10", usdText(23100000).c_str());
  TEST_ASSERT_EQUAL_STRING("$0.01", usdText(5000).c_str());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_overspend_month_steps_down_and_returns_after_seven_quiet_nights);
  RUN_TEST(test_when_nothing_fits_it_names_the_clamp_date);
  RUN_TEST(test_a_pinned_profile_is_never_moved);
  RUN_TEST(test_the_owner_s_ceiling_decides_and_is_never_overridden);
  RUN_TEST(test_a_constant_rate_never_flaps);
  RUN_TEST(test_a_noisy_rate_never_reverses_quickly);
  RUN_TEST(test_the_virtual_clock_edges);
  RUN_TEST(test_the_file_round_trips_and_a_bad_file_reads_as_the_default);
  RUN_TEST(test_the_cost_ratios_are_the_measured_ones);
  RUN_TEST(test_pin_and_auto);
  return UNITY_END();
}
