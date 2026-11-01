#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/orch/claims.h"        // figureNumbers
#include "nimbus/orch/food_figures.h"

// The coach's figures against the device's records (food_figures.h). The replies are the
// week of 28 Oct as delivered, food words and names changed where they identify anyone.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static FoodFacts ledger(double kcal, double protein, int unknown = 0) {
  FoodFacts f;
  f.have = true;
  f.kcal = kcal;
  f.rows = 5;
  f.protein = protein;
  f.proteinUnknown = unknown;
  return f;
}

static const char kPlan[] = "Calories: 2,400 kcal/day. Protein: 150-160 g/day, with 150 g as the minimum.";

static bool hasKind(const std::vector<FoodGap>& g, FoodGapKind k) {
  for (const FoodGap& x : g)
    if (x.kind == k) return true;
  return false;
}

// ---- the hallucinated targets: impossible to state without a source -------------------
static void test_the_31_oct_250_g_target() {
  const FoodFacts f = ledger(2036, 211.7);
  const std::string rules = figureNumbers("Standing rule: 2,400 kcal a day.");
  for (const char* reply : {
           "You have 38.3 g protein left from your **250 g daily protein target**.",
           "Remaining for 2,400 kcal target: 364 kcal, 38.3 g protein left.",
           "Your protein goal is 250 g.",
           "Protein target: 250 g.",
       }) {
    const auto gaps = foodFigureGaps(reply, f, rules);
    TEST_ASSERT_TRUE_MESSAGE(hasKind(gaps, FoodGapKind::UnsourcedTarget), reply);
    const std::string tail = foodGapTail(gaps, f);
    TEST_ASSERT_TRUE_MESSAGE(tail.find("(Correction: the device has no source for a daily target of 250 g protein.") !=
                                 std::string::npos, tail.c_str());
  }
  // The same words, the plan read: nothing to correct.
  const std::string read = figureNumbers(kPlan);
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("Your protein goal is 150 g.", f, read).size());
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("Protein: 150-160 g/day is the target.", f, read).size());
}

static void test_the_29_oct_2500_kcal_target() {
  const FoodFacts f = ledger(1779, 153);
  const std::string src = figureNumbers("Your original target was 2,400 kcal. " + std::string(kPlan));
  const auto gaps = foodFigureGaps("The 2,500 kcal target is from the plan we built together.", f, src);
  TEST_ASSERT_TRUE(hasKind(gaps, FoodGapKind::UnsourcedTarget));
  TEST_ASSERT_EQUAL_FLOAT(2500, (float)gaps[0].stated);
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("Your original target was 2,400 kcal, but we adjusted it to **2,500 kcal**.", f, src),
                           FoodGapKind::UnsourcedTarget));
  // No source at all: even the right number is unsourced until it is read.
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("That leaves 621 kcal against your 2,400 kcal target.", f, ""), FoodGapKind::UnsourcedTarget));
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("That leaves 621 kcal against your 2,400 kcal target.", f, src).size());
}

// A "left" figure is a target in disguise.
static void test_a_left_figure_implies_a_target() {
  const FoodFacts f = ledger(2036, 211.7);
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("Protein: 38.3 g left.", f, ""), FoodGapKind::UnsourcedTarget));
  const auto gaps = foodFigureGaps("You have 38.3 g protein left today.", f, figureNumbers(kPlan));
  TEST_ASSERT_EQUAL_INT(1, (int)gaps.size());
  TEST_ASSERT_TRUE(gaps[0].implied);
  TEST_ASSERT_EQUAL_FLOAT(250, (float)gaps[0].stated);
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("That leaves 500 kcal for dinner.", f, figureNumbers(kPlan)), FoodGapKind::UnsourcedTarget));
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("364 kcal left for today.", f, figureNumbers(kPlan)).size());
  // A real reply on mistral: the figure before "to the kcal target" is what is left, not the target.
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("That leaves 364 kcal to the kcal target.", f, figureNumbers(kPlan)).size());
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("That leaves ~365 kcal for dinner.", f, figureNumbers(kPlan)).size());
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("Remaining to target: ~364 kcal.", f, figureNumbers(kPlan)).size());
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("Remaining to target: ~90 kcal.", f, figureNumbers(kPlan)), FoodGapKind::UnsourcedTarget));
}

// The owner disputing a figure does not source it; the owner stating one does.
static void test_a_question_about_a_figure_is_no_source() {
  const FoodFacts f = ledger(1779, 153);
  for (const char* disputed : {"Why 2500? Cant you verify the plan we built?", "2500 is the daily goal?? Are you sure?",
                               "You hallucinate the 2500 out of the blue."}) {
    const auto gaps = foodFigureGaps("The 2,500 kcal target is from the plan.", f, figureNumbers(ownerStatementsOf(disputed)));
    TEST_ASSERT_TRUE_MESSAGE(hasKind(gaps, FoodGapKind::UnsourcedTarget), disputed);
  }
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("The 2,500 kcal target stands.", f, figureNumbers(ownerStatementsOf("Make my target 2500 kcal."))).size());
}

// Precision: what these replies must never be corrected for.
static void test_not_claims() {
  const FoodFacts f = ledger(2036, 211.7);
  for (const char* reply : {
           "What is your daily protein target?",
           "If your daily target is 2,500 kcal, the drafted total leaves 721 kcal remaining.",
           "The 250 g figure was my error; it is not in the plan.",
           "One flag: the plan's protein goal is 150-160 g/day, not the 250 g mentioned earlier.",
           "You're right, there is no 250 g target, and I'm sorry.",
           "A draft model identical to the target does not give an infinite speedup.",
           "Goal weight is 93.6 kg.",
           "Plausible range 1,050-1,400 kcal.",
           "Total central estimate: ~1,200 kcal / ~85 g protein.",
           "This meal is 1,038 kcal and 100.2 g protein.",
           "Want me to set a 2,000 kcal target?",
           "Yesterday's total was 2,524 kcal.",
           "I would aim for a 300 kcal snack.",
           "Sized to close tonight's gap: dinner budget is ~1,036-1,066 kcal.",
       }) {
    const auto gaps = foodFigureGaps(reply, f, "");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)gaps.size(), reply);
  }
}

// ---- the day total is the ledger's -----------------------------------------------------
static void test_the_day_total_is_the_ledgers() {
  const FoodFacts f = ledger(2369, 175.2);
  for (const char* wrong : {
           "Today's total is now **3,989 kcal** and 211 g protein.",
           "Today (Sat 31 Oct) so far: 2,036 kcal / 211.7 g protein.",
           "Today's total so far: 256 kcal.",
           "Totals so far: 1,461 kcal | 136.8 g protein.",
           "Day so far: 66 + 196 + 517 = 779 kcal / 47.9 g protein.",
       }) {
    const auto gaps = foodFigureGaps(wrong, f, "");
    TEST_ASSERT_TRUE_MESSAGE(hasKind(gaps, FoodGapKind::WrongDayTotal), wrong);
    TEST_ASSERT_TRUE_MESSAGE(foodGapTail(gaps, f).find("the food log has today at 2369 kcal") != std::string::npos, wrong);
  }
  for (const char* right : {
           "Today now totals 2,369 kcal / 175.2 g protein.",
           "Today (Sat 31 Oct), straight from the log (6 rows): 2,369 kcal and 175.2 g protein.",
           "Today's log stands at ~2,370 kcal.",
           "Today's total is 2369 kcal.",
           "Dinner logged: 1,038 kcal, 100.2 g protein.",
       }) {
    TEST_ASSERT_FALSE_MESSAGE(hasKind(foodFigureGaps(right, f, ""), FoodGapKind::WrongDayTotal), right);
  }
  // A protein figure off by a row is caught too; a total nobody can read is no figure.
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("Today's total is 2,369 kcal and 211.7 g protein.", f, ""), FoodGapKind::WrongDayTotal));
  FoodFacts unread = f;
  unread.kcalKnown = false;
  const auto gaps = foodFigureGaps("Today's total is 2,369 kcal.", unread, "");
  TEST_ASSERT_TRUE(hasKind(gaps, FoodGapKind::WrongDayTotal));
  TEST_ASSERT_TRUE(foodGapTail(gaps, unread).find("not known yet") != std::string::npos);
  // No rows today (30 Oct, 06:00: yesterday's 2,524 carried in), unless the figure is the owner's.
  FoodFacts empty;
  empty.have = true;
  const auto carried = foodFigureGaps("Today's total is 2,524 kcal and 194.1 g protein.", empty, "");
  TEST_ASSERT_TRUE(hasKind(carried, FoodGapKind::WrongDayTotal));
  TEST_ASSERT_TRUE(foodGapTail(carried, empty).find("no rows for today") != std::string::npos);
  TEST_ASSERT_FALSE(hasKind(foodFigureGaps("Today's total is 1,500 kcal.", empty, "1500"), FoodGapKind::WrongDayTotal));
  // With no ledger read, nothing is held against the reply but the targets.
  FoodFacts none;
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("Today's total is 3,989 kcal.", none, "").size());
}

// ---- known and unknown protein ---------------------------------------------------------
static void test_a_need_over_an_unknown_row_is_no_figure() {
  const FoodFacts f = ledger(1792, 48.4, 1);
  const std::string src = figureNumbers(kPlan);
  const auto gaps = foodFigureGaps(
      "Protein-wise you need at least ~102 g more to clear the 150 g minimum, since the Flat Iron row has no protein figure.", f, src);
  TEST_ASSERT_TRUE(hasKind(gaps, FoodGapKind::ProteinLeftUnknown));
  TEST_ASSERT_TRUE(foodGapTail(gaps, f).find("not known exactly") != std::string::npos);
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("Protein: at most 101.6 g left of the 150 g minimum, one meal is unknown.", f, src).size());
  // The same mistake in other words, real replies on the eval's models.
  for (const char* need : {"You'd need about 107 g more to reach the 155 g floor.", "Your floor is 150 g, so you still need at least 106.6 g more.",
                           "Against your 150 g floor, you'd need roughly 107 g more from what's left of the day.",
                           "Against your 150 g floor, that's at least 106.6 g still to go."})
    TEST_ASSERT_TRUE_MESSAGE(hasKind(foodFigureGaps(need, f, src + " 155"), FoodGapKind::ProteinLeftUnknown), need);
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("You're short by at least ~102 g of protein, more if the steak row gets a number.", f, src),
                           FoodGapKind::ProteinLeftUnknown));
  // All rows known: the same need is a sum against a source.
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("You need ~102 g more protein to clear the 150 g minimum.", ledger(1792, 48.4), src).size());
}

// The class: every target word, every phrasing of a figure, sourced and not.
static void test_property_every_target_word() {
  const FoodFacts f = ledger(1000, 50);
  for (const char* word : {"target", "goal", "budget", "allowance", "limit"}) {
    for (const std::string& shape : {std::string("Your 1,850 kcal daily ") + word + " stands.",
                                     std::string("The daily ") + word + " is 1,850 kcal.",
                                     std::string("Calorie ") + word + ": 1,850 kcal."}) {
      TEST_ASSERT_TRUE_MESSAGE(hasKind(foodFigureGaps(shape, f, "2400 150"), FoodGapKind::UnsourcedTarget), shape.c_str());
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)foodFigureGaps(shape, f, "1850").size(), shape.c_str());
    }
  }
}

// A memory about to be written is held to the same rule: a made-up target must not become a source.
static void test_a_target_in_a_memory_needs_a_source() {
  TEST_ASSERT_EQUAL_INT(1, (int)unsourcedFoodTargets("Roy's daily protein target is 250 g (stable preference).", "2400 150 160").size());
  TEST_ASSERT_EQUAL_INT(0, (int)unsourcedFoodTargets("Roy's daily protein target is 150 g.", "2400 150 160").size());
  TEST_ASSERT_EQUAL_INT(0, (int)unsourcedFoodTargets("Roy had a boba tea at 225 kcal.", "").size());
}

static void test_the_correction_round_names_the_way_out() {
  const FoodFacts f = ledger(2036, 211.7);
  const std::string fix = foodGapCorrection(foodFigureGaps("Your 250 g protein target stands.", f, "2400"), f);
  TEST_ASSERT_TRUE_MESSAGE(fix.find("your own earlier replies are not a source") != std::string::npos, fix.c_str());
  TEST_ASSERT_TRUE_MESSAGE(fix.find("files.read") != std::string::npos, fix.c_str());
}

// ---- a routine citing old data as the state of things (1 Nov, the 14 Oct streak) ----------
static void test_a_stale_status_in_a_routine() {
  int age = 0;
  const char* real = "You've been doing one lesson daily. That's 7 consecutive days as of 2026-10-14; I don't yet have an update.";
  TEST_ASSERT_TRUE(staleStatusIn(real, "2026-11-01", kStaleStatusDays, &age).find("consecutive") != std::string::npos);
  TEST_ASSERT_EQUAL_INT(18, age);
  for (const char* stale : {"Your streak was 7 days on 14 Oct.", "As of Oct 14 you are running well.", "Currently at 80 kg (14 October).",
                            "Your latest figure, from 2026-10-01, is 5 km."}) {
    TEST_ASSERT_FALSE_MESSAGE(staleStatusIn(stale, "2026-11-01", kStaleStatusDays, &age).empty(), stale);
  }
  for (const char* fine : {"Your streak was 7 days on 30 Oct.", "That's 7 consecutive days as of 2026-10-29.",
                           "Did you do your lesson today?", "Your streak on 14 Oct, is it still going?",
                           "The treaty of 14 Oct 1066 changed the map.", "Here is a nugget about verbs.", "May your day be good, still."}) {
    TEST_ASSERT_TRUE_MESSAGE(staleStatusIn(fine, "2026-11-01", kStaleStatusDays, &age).empty(), fine);
  }
  // Across a year boundary.
  TEST_ASSERT_TRUE(staleStatusIn("Your streak was 7 days on 30 Dec.", "2027-01-02", kStaleStatusDays, &age).empty());
  TEST_ASSERT_FALSE(staleStatusIn("Your streak was 7 days on 1 Dec.", "2027-01-02", kStaleStatusDays, &age).empty());
  TEST_ASSERT_TRUE(staleStatusIn("Your streak was 7 days on 1 Dec.", "", kStaleStatusDays, &age).empty());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_31_oct_250_g_target);
  RUN_TEST(test_the_29_oct_2500_kcal_target);
  RUN_TEST(test_a_left_figure_implies_a_target);
  RUN_TEST(test_a_question_about_a_figure_is_no_source);
  RUN_TEST(test_not_claims);
  RUN_TEST(test_the_day_total_is_the_ledgers);
  RUN_TEST(test_a_need_over_an_unknown_row_is_no_figure);
  RUN_TEST(test_property_every_target_word);
  RUN_TEST(test_a_target_in_a_memory_needs_a_source);
  RUN_TEST(test_the_correction_round_names_the_way_out);
  RUN_TEST(test_a_stale_status_in_a_routine);
  return UNITY_END();
}
