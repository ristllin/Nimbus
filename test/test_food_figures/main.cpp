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
           "Your protein goal is 250 g.",
           "Protein target: 250 g.",
       }) {
    const auto gaps = foodFigureGaps(reply, f, rules);
    TEST_ASSERT_TRUE_MESSAGE(hasKind(gaps, FoodGapKind::UnsourcedTarget), reply);
    const std::string tail = foodGapTail(gaps, f);
    TEST_ASSERT_TRUE_MESSAGE(tail.find("(Correction: the device has no source for a daily target of 250 g protein.") !=
                                 std::string::npos, tail.c_str());
  }
  // With no target named, the line names what the reply did say; the correction round names the sum.
  const auto implied = foodFigureGaps("Remaining for 2,400 kcal target: 364 kcal, 38.3 g protein left.", f, rules);
  TEST_ASSERT_TRUE(hasKind(implied, FoodGapKind::UnsourcedTarget));
  TEST_ASSERT_TRUE(foodGapTail(implied, f).find("daily target behind that 38.3 g protein left") != std::string::npos);
  TEST_ASSERT_TRUE(foodGapCorrection(implied, f).find("250 g protein") != std::string::npos);
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
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("Against your targets: 364 kcal left of 2,400.", f, figureNumbers(kPlan)).size());
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("The protein floor is met with 40 g to spare.", f, figureNumbers(kPlan)).size());
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps("Just shy: 364 kcal to spare.", f, figureNumbers(kPlan)).size());
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
  // The 250 g is taken back. The 150-160 it stands against is a stated goal and needs its source.
  const char* flag = "One flag: the plan's protein goal is 150-160 g/day, not the 250 g mentioned earlier.";
  TEST_ASSERT_EQUAL_INT(0, (int)foodFigureGaps(flag, f, figureNumbers(kPlan)).size());
  TEST_ASSERT_EQUAL_FLOAT(150, (float)foodFigureGaps(flag, f, "")[0].stated);
  for (const char* reply : {
           "What is your daily protein target?",
           "If your daily target is 2,500 kcal, the drafted total leaves 721 kcal remaining.",
           "The 250 g figure was my error; it is not in the plan.",
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
                           "Against your 150 g floor, that's at least 106.6 g still to go.",
                           "Against your 150 g floor, you're at least 106.6 g short, and the steak will narrow that gap.",
                           "Protein stands at at least 48.4 g, i.e. at least 106.6 g below your 150 g floor."})
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

// ---- review fix pass: what the scanner must not read, and what it must --------------------

static int gapsOf(const char* reply, const FoodFacts& f, const std::string& src) {
  return (int)foodFigureGaps(reply, f, src).size();
}

// The figure a day-total sentence is about is the one its cue binds to, never just the first one.
static void test_a_right_day_total_after_a_meal_figure_is_right() {
  FoodFacts f = ledger(1482, 90);
  f.rows = 3;
  for (const char* right : {
           "Logged lunch at 650 kcal, which puts today's total at 1,482 kcal.",
           "Lunch is 650 kcal, so today is now at 1,482 kcal.",
           "Added the 650 kcal lunch; day total 1,482 kcal.",
           "Lunch 650 kcal, so far today 1,482 kcal and 90 g protein.",
       }) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gapsOf(right, f, "2400 150"), right);
  }
  // The same sentence with the wrong total still draws the correction, against the bound figure.
  const auto wrong = foodFigureGaps("Logged lunch at 650 kcal, which puts today's total at 1,382 kcal.", f, "");
  TEST_ASSERT_TRUE(hasKind(wrong, FoodGapKind::WrongDayTotal));
  TEST_ASSERT_EQUAL_FLOAT(1382, (float)wrong[0].stated);
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("Lunch is 650 kcal, so today is now at 3,989 kcal.", f, ""), FoodGapKind::WrongDayTotal));
}

// A loose cue with a plan or advice after it holds no day total.
static void test_a_plan_or_advice_sentence_about_today_is_no_day_total() {
  const FoodFacts f = ledger(1482, 90);
  FoodFacts empty;
  empty.have = true;
  for (const char* plan : {"Today is a rest day, so your plan says 2,100 kcal.", "For today: dinner of 500 kcal fits.",
                           "Today: your plan gives you 2,100 kcal, you're at 1482.", "Lunch today is 650 kcal.",
                           "Dinner of 500 kcal logged today."}) {
    TEST_ASSERT_FALSE_MESSAGE(hasKind(foodFigureGaps(plan, f, "2100"), FoodGapKind::WrongDayTotal), plan);
  }
  // Real replies, head5-haiku plan-earlier rep1, no-inventory-guard rep1, complaint-recovery rep5.
  const FoodFacts at1400 = ledger(1400, 95);
  for (const char* real : {"Dinner plan, 992 kcal and 88 g protein against the 1,000 kcal and 60 g protein left today:",
                           "Going by your figure of about 1000 kcal and 60 g protein left: the ledger shows 1400 kcal and 95 g protein so far today."}) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)foodFigureGaps(real, at1400, "2400 155").size(), real);
  }
  TEST_ASSERT_FALSE(hasKind(foodFigureGaps("Today is a rest day, so your plan says 2,100 kcal.", empty, "2100"), FoodGapKind::WrongDayTotal));
}

// A day total the reply projects, over a meal it proposes.
static void test_a_projected_day_total_is_not_todays() {
  const FoodFacts f = ledger(1500, 90);
  for (const char* projected : {
           "A 700 kcal dinner brings today's total to 2,200 kcal.",
           "Adding the 700 kcal pasta takes today's total to 2,200 kcal.",
           "Dinner at 700 kcal puts the day total at 2,200 kcal.",
       }) {
    TEST_ASSERT_FALSE_MESSAGE(hasKind(foodFigureGaps(projected, f, "2400 155"), FoodGapKind::WrongDayTotal), projected);
  }
  // A range is an estimate over a dinner still on the plate (24 Oct), not a reading of the ledger.
  TEST_ASSERT_EQUAL_INT(0, gapsOf("Day total: ~2,320-2,360 kcal, under your 2,400 target, and protein well past the floor.", f, "2400"));
  // Below the ledger it is no projection, and a logged meal is no projection at all.
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("Dinner at 700 kcal puts the day total at 1,200 kcal.", f, ""), FoodGapKind::WrongDayTotal));
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("Logged the 700 kcal dinner, which brings today's total to 2,200 kcal.", f, ""), FoodGapKind::WrongDayTotal));
}

// "more", "need" and "under" in ordinary coaching are no statement of what is left.
static void test_comparisons_and_advice_are_not_a_left_figure() {
  const FoodFacts f = ledger(1482, 90);
  const std::string src = "2100 150";
  for (const char* advice : {
           "That's 300 kcal more than yesterday.",
           "A scoop of whey adds 25 g more protein.",
           "Dinner needs 40 g protein to land the day.",
           "Add 200 kcal more at dinner.",
           "Chicken breast needs 30 g of protein.",
           "Add a 25 g protein shake for 25 g more protein.",
           "Stay under 600 kcal at dinner.",
           "That leaves room for a 300 kcal snack.",
       }) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gapsOf(advice, f, src), advice);
  }
  const FoodFacts blank = ledger(1482, 90, 1);
  for (const char* advice : {"Chicken breast needs 30 g of protein.", "Add a 25 g protein shake for 25 g more protein.",
                             "A scoop of whey adds 25 g more protein."}) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gapsOf(advice, blank, src), advice);
  }
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("You need about 102 g more protein, no problem.", blank, src), FoodGapKind::ProteinLeftUnknown));
  // The firm need is still read: the sum against a target nothing holds.
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("You still need 38.3 g protein.", ledger(211.7, 211.7), "150 2400"), FoodGapKind::UnsourcedTarget));
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("Protein stands at 90 g, 38 g under your target.", f, ""), FoodGapKind::UnsourcedTarget));
}

// A meal the reply proposes is on top of the ledger: what is left after it is not a target. The
// replies are real: head5-haiku qty-vague reps 1, 3, 5 and qty-pressed rep 4, ledger 1600 and 88.
static void test_left_after_a_proposed_meal_is_no_target() {
  const FoodFacts f = ledger(1600, 88);
  const std::string src = "2400 155 800 67";
  for (const char* real : {
           "Max out with 320 g cooked pasta and 4 mozzarella slices (25 g each): 786 kcal, leaving 14 kcal of your 800 left.",
           "That brings today to 2,386 kcal against your 2,400 target.",
           "Protein would reach about 129 g, still 26 g short of your 155 g floor.",
           "Protein stays about 22.5 g short of your 155 g floor (you have 67 g left to cover).",
           "That leaves protein about 25 g short of your 155 g floor (67 g still needed today).",
           "Protein stays well short: today is at 88 g, so this brings you to about 119 g against your 155 g floor.",
           "440 g cooked pasta and 1 slice of mozzarella (25 g) gets you to about 765 of your 800 kcal left, so about 2,365 kcal for the day against your 2,400 target.",
           // qty-vague and plan-earlier on haiku and mistral: "left today:" is what is left, then the plan.
           "340 g cooked pasta and 5 x 25 g mozzarella (125 g) uses up the 800 kcal left today: 795 kcal in total.",
           "Dinner fits the 800 kcal left today: 794 kcal and 85.6 g protein, against 67 g left.",
           "Dinner sized to the 800 kcal and 67 g protein you have left today: 170 g salad (tomato, lettuce, cucumber) with protein.",
           "Corrected below, sized to the 800 kcal and 67 g protein left today (2,400 kcal / 155 g protein targets minus the ledger's 1,600 kcal / 88 g so far):",
           "The bread takes half of it. Dropping it leaves about 420 kcal to fill.",
           "- Total: 795 kcal of 800 left, 44.5 g protein (22.5 g short of the 67 g protein left against your 155 g floor)",
           // 22 Oct: a bare figure before "floor" is that floor.
           "Chicken: day becomes 1,921 kcal / 143.2 g protein. That's 12 g short of the 155 floor and about 480 kcal of room.",
           // qty-pressed rep3 and plan-stated: the figure after "left," is the plan's protein, not what is left.
           "Total: 775 kcal of 800 left, 40.1 g protein",
           "Total: 790 kcal of your 800 left, 112.8 g protein.",
           "Here is a dinner plan that uses 790 of the 800 kcal left and gives about 109 g protein, well above the 67 g target.",
       }) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gapsOf(real, f, src), real);
  }
  const FoodFacts g = ledger(1500, 90);
  for (const char* projected : {"After the pasta you'll have 200 kcal left.", "The pasta at 700 kcal leaves you 200 kcal for the rest of the day."}) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gapsOf(projected, g, "2400 155"), projected);
    // No target in anything the turn read: the left figure still has nothing behind it.
    TEST_ASSERT_TRUE_MESSAGE(hasKind(foodFigureGaps(projected, g, ""), FoodGapKind::UnsourcedTarget), projected);
  }
  // More left than the named target allows is a wrong sum, whatever the sentence names.
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("That leaves 1,500 kcal against your 2,400 kcal target.", g, "2400"), FoodGapKind::UnsourcedTarget));
}

// Advice for one meal is not a daily target.
static void test_a_per_meal_figure_is_no_daily_target() {
  const FoodFacts f = ledger(1500, 90);
  for (const char* meal : {"Aim for 40 g protein at dinner.", "Keep dinner to a 600 kcal limit.", "Your lunch goal is 40 g protein.",
                           "A goal of 40 g protein at dinner works.", "Cap breakfast at 400 kcal.", "Minimum 30 g protein per meal."}) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gapsOf(meal, f, "2400 155"), meal);
  }
  for (const char* daily : {"Your 2,500 kcal target leaves room for dinner.", "Your daily limit is 2,500 kcal, so dinner can be big.",
                            "Aim for 250 g protein."}) {
    TEST_ASSERT_TRUE_MESSAGE(hasKind(foodFigureGaps(daily, f, "2400 155"), FoodGapKind::UnsourcedTarget), daily);
  }
}

// A negation takes back the figure it binds to, not every figure in its sentence.
static void test_a_negation_binds_to_its_figure() {
  const FoodFacts f = ledger(1482, 90);
  for (const char* invented : {"No worries, your protein target is 250 g and you have 160 g left.",
                               "Not bad: 1,018 kcal left of your 2,500 kcal budget.", "You're not at your 2,500 kcal target yet.",
                               "Sorry, your target is 2,500 kcal.", "Your target is 2,500 kcal, not 2,400."}) {
    TEST_ASSERT_TRUE_MESSAGE(hasKind(foodFigureGaps(invented, f, "2400"), FoodGapKind::UnsourcedTarget), invented);
    TEST_ASSERT_TRUE_MESSAGE(!unsourcedFoodTargets(invented, "2400").empty(), invented);
  }
  for (const char* back : {"The 250 g protein figure was my error.", "There is no 250 g protein target.", "It is not the 2,500 kcal target.",
                           "The 2,500 kcal target isn't in your plan.", "I never had a source for that 2,500 kcal target, it is wrong."}) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gapsOf(back, f, "2400"), back);
  }
}

// The owner stating a figure is a source whatever else the sentence holds; a figure they dispute is not.
static void test_the_owners_own_figure_survives_a_no_or_a_sorry() {
  struct {
    const char* owner;
    const char* source;
    const char* notSource;
  } cases[] = {
      {"No, my target is 2,200 kcal a day.", "2200", nullptr},
      {"My protein target is 150 g, not 250.", "150", "250"},
      {"I'm trying not to go over 2,000 kcal.", "2000", nullptr},
      {"Sorry, I meant 2,200 kcal is my daily target.", "2200", nullptr},
      {"Remember this: my protein target is 150 g and I have no flexibility on it.", "150", nullptr},
      {"No, that's wrong, my target is 2,200 kcal.", "2200", nullptr},
      {"2500 is wrong, it's 2,200.", "2200", "2500"},
      {"My target is 2,200, why did you say 2500?", "2200", "2500"},
      {"I never said 2500. It's 2,200.", "2200", "2500"},
      {"It's not 2500.", nullptr, "2500"},
      {"You made up 2500.", nullptr, "2500"},
      {"Is it 2500, or 2400?", nullptr, "2500"},
  };
  const FoodFacts f = ledger(1500, 90);
  for (const auto& c : cases) {
    const std::string src = figureNumbers(ownerStatementsOf(c.owner));
    if (c.source) {
      const std::string reply = std::string("Your daily target is ") + c.source + " kcal.";
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)foodFigureGaps(reply, f, src).size(), c.owner);
    }
    if (c.notSource) {
      const std::string reply = std::string("Your daily target is ") + c.notSource + " kcal.";
      TEST_ASSERT_TRUE_MESSAGE(hasKind(foodFigureGaps(reply, f, src), FoodGapKind::UnsourcedTarget), c.owner);
    }
  }
}

// The class of rewordings: a daily target by any name, each both ways.
static void test_rewordings_of_a_target_are_the_same_target() {
  const FoodFacts f = ledger(2036, 211.7);
  const std::string rules = "2400 150 160";
  struct {
    const char* reply;
    const char* sourced;   // the same shape with a figure the plan holds
  } shapes[] = {
      {"Your daily protein is 250 g.", "Your daily protein is 150 g."},
      {"Daily protein: 250 g.", "Daily protein: 150 g."},
      {"You should eat 2,500 kcal a day.", "You should eat 2,400 kcal a day."},
      {"Your daily calories are 2,500.", "Your daily calories are 2,400."},
      {"Your plan calls for 250 g protein per day.", "Your plan calls for 150 g protein per day."},
      {"250 g protein is what the plan asks for.", "150 g protein is what the plan asks for."},
      {"Your quota is 250 g of protein.", "Your quota is 150 g of protein."},
      {"The ceiling is 2,500 kcal.", "The ceiling is 2,400 kcal."},
      {"You need 250 g protein today.", "You need 150 g protein today."},
      {"Your calorie target is 2,500.", "Your calorie target is 2,400."},
  };
  for (const auto& s : shapes) {
    const auto gaps = foodFigureGaps(s.reply, f, rules);
    TEST_ASSERT_TRUE_MESSAGE(hasKind(gaps, FoodGapKind::UnsourcedTarget), s.reply);
    const std::string tail = foodGapTail(gaps, f);
    const bool kcal = std::string(s.reply).find("2,500") != std::string::npos;
    TEST_ASSERT_TRUE_MESSAGE(tail.find(kcal ? "2500 kcal" : "250 g protein") != std::string::npos, tail.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)foodFigureGaps(s.sourced, f, rules).size(), s.sourced);
  }
  TEST_ASSERT_TRUE(hasKind(foodFigureGaps("You're 38.3 g short of 250 g.", f, rules), FoodGapKind::UnsourcedTarget));
  // Not a target: what happened, and a rate with no prescription.
  for (const char* history : {"You've averaged 1,900 kcal a day this week.", "You ate 2,500 kcal a day last week.",
                              "Your daily protein so far is 90 g.", "A 500 kcal a day deficit.", "This shake has 30 g protein."}) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)foodFigureGaps(history, ledger(1000, 90), "").size(), history);
  }
}

// The owner's line names what the reply said, never a sum the device made up.
static void test_the_owners_line_holds_no_number_the_reply_never_said() {
  const FoodFacts f = ledger(2036, 211.7);
  const auto left = foodFigureGaps("You have 38.3 g protein left today.", f, "150");
  const std::string tail = foodGapTail(left, f);
  TEST_ASSERT_TRUE_MESSAGE(tail.find("38.3 g protein") != std::string::npos, tail.c_str());
  TEST_ASSERT_TRUE_MESSAGE(tail.find("250") == std::string::npos, tail.c_str());
  // The model is still told the sum, so it can see what it implied.
  TEST_ASSERT_TRUE(foodGapCorrection(left, f).find("250 g protein") != std::string::npos);
  const std::string need = foodGapTail(foodFigureGaps("You need 250 g protein today.", f, "150"), f);
  TEST_ASSERT_TRUE_MESSAGE(need.find("461.7") == std::string::npos, need.c_str());
}

// A day's protein over a row with no figure: the known part is a floor, not the day.
static void test_a_day_protein_over_an_unknown_row_is_at_least_the_known_part() {
  const FoodFacts f = ledger(1500, 48.4, 1);
  TEST_ASSERT_EQUAL_INT(0, gapsOf("Today so far: 1,500 kcal and about 90 g protein, since the burger has roughly 40 g.", f, ""));
  TEST_ASSERT_EQUAL_INT(0, gapsOf("Today so far: 1,500 kcal and 48.4 g protein.", f, ""));
  const auto low = foodFigureGaps("Today so far: 1,500 kcal and 30 g protein.", f, "");
  TEST_ASSERT_TRUE(hasKind(low, FoodGapKind::WrongDayTotal));
  const std::string tail = foodGapTail(low, f);
  TEST_ASSERT_TRUE_MESSAGE(tail.find("at least 48.4 g protein") != std::string::npos, tail.c_str());
  TEST_ASSERT_TRUE(foodGapCorrection(low, f).find("at least 48.4 g protein") != std::string::npos);
}

// Upcoming dates, start dates and words that begin like a month are not stale data.
static void test_a_stale_status_reads_only_what_is_old() {
  int age = 0;
  for (const char* fine : {"Your current plan runs until 30 Nov.", "You're still on track for the 25 Nov appointment.",
                           "Latest: the review is due on 2026-11-20.", "Still 3 weeks to your 15 Dec holiday.",
                           "Your current streak is 12 days in a row; you marked 5 sessions this week.",
                           "Still decided 3 things, running well.", "You're currently on day 33 of the plan you started on 2026-10-01.",
                           "You've been logging meals since 2026-10-01, nice.", "Your gym membership is currently active until 2026-12-31."}) {
    TEST_ASSERT_TRUE_MESSAGE(staleStatusIn(fine, "2026-11-02", kStaleStatusDays, &age).empty(), fine);
  }
  // Still stale: "as of" outranks a start word, and a plain old date.
  TEST_ASSERT_FALSE(staleStatusIn("7 days in a row since 2026-10-07, as of 2026-10-14.", "2026-11-02", kStaleStatusDays, &age).empty());
  TEST_ASSERT_FALSE(staleStatusIn("Your streak was 7 days on 14 October.", "2026-11-02", kStaleStatusDays, &age).empty());
  TEST_ASSERT_EQUAL_INT(19, age);
  // A date far ahead with no year is last year's.
  TEST_ASSERT_FALSE(staleStatusIn("Your streak was 7 days on 20 Nov.", "2026-01-02", kStaleStatusDays, &age).empty());
  // The owner's line claims only what the device knows.
  TEST_ASSERT_TRUE(staleStatusNote(19).find("19 days old") != std::string::npos);
  TEST_ASSERT_TRUE(staleStatusNote(19).find("nothing newer") == std::string::npos);
}

static void test_speaks_of_food_ignores_case() {
  TEST_ASSERT_TRUE(speaksOfFood("Protein left: 38.3 g (target 250 g)"));
  TEST_ASSERT_TRUE(speaksOfFood("CALORIES: 2,500"));
  TEST_ASSERT_TRUE(speaksOfFood("2,500 Kcal"));
  TEST_ASSERT_FALSE(speaksOfFood("Your dentist is at 3pm."));
}

// A write to the targets file: its figures are checked before anything lands.
static void test_a_targets_write_names_its_unsourced_figure() {
  double figure = 0;
  TEST_ASSERT_TRUE(unsourcedTargetWrite("2026-11-01,protein_g,250,the plan", "2400 150", &figure));
  TEST_ASSERT_EQUAL_FLOAT(250, (float)figure);
  TEST_ASSERT_TRUE(unsourcedTargetWrite("date,metric,target,source\n2026-11-01,kcal,2500,plan\n", "2400", &figure));
  TEST_ASSERT_EQUAL_FLOAT(2500, (float)figure);
  TEST_ASSERT_TRUE(unsourcedTargetWrite("kcal,2500,plan", "2400", &figure));
  TEST_ASSERT_TRUE(unsourcedTargetWrite("2500", "2400", &figure));
  TEST_ASSERT_TRUE(unsourcedTargetWrite("2026-11-01,kcal,\"2,500\",plan", "2400", &figure));
  TEST_ASSERT_FALSE(unsourcedTargetWrite("2026-11-01,kcal,2400,the plan says 2400 kcal in 2026", "2400", &figure));
  TEST_ASSERT_FALSE(unsourcedTargetWrite("2026-11-01,kcal,2400,the plan, 2026", "2400", &figure));
  TEST_ASSERT_FALSE(unsourcedTargetWrite("#superseded: 2026-11-01,kcal,2500,plan", "2400", &figure));
  TEST_ASSERT_FALSE(unsourcedTargetWrite("date,metric,target,source", "", &figure));
  TEST_ASSERT_FALSE(unsourcedTargetWrite("the owner's plan", "", &figure));
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
  RUN_TEST(test_a_right_day_total_after_a_meal_figure_is_right);
  RUN_TEST(test_a_plan_or_advice_sentence_about_today_is_no_day_total);
  RUN_TEST(test_a_projected_day_total_is_not_todays);
  RUN_TEST(test_comparisons_and_advice_are_not_a_left_figure);
  RUN_TEST(test_left_after_a_proposed_meal_is_no_target);
  RUN_TEST(test_a_per_meal_figure_is_no_daily_target);
  RUN_TEST(test_a_negation_binds_to_its_figure);
  RUN_TEST(test_the_owners_own_figure_survives_a_no_or_a_sorry);
  RUN_TEST(test_rewordings_of_a_target_are_the_same_target);
  RUN_TEST(test_the_owners_line_holds_no_number_the_reply_never_said);
  RUN_TEST(test_a_day_protein_over_an_unknown_row_is_at_least_the_known_part);
  RUN_TEST(test_a_stale_status_reads_only_what_is_old);
  RUN_TEST(test_speaks_of_food_ignores_case);
  RUN_TEST(test_a_targets_write_names_its_unsourced_figure);
  return UNITY_END();
}
