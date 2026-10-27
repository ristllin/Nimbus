#include <unity.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "nimbus/orch/food_portions.h"

// The portion calculator (food_portions.h): the device does a plan's arithmetic.
// The class: whatever the foods, the fixed amounts stay exact, units stay whole,
// weighed foods move in 5 g steps, and the total lands within 95-100 percent of the
// kcal left whenever a step allows it (a Mistral head stopped 20-35 percent under),
// and never over it unless the fixed amounts alone are (a Sonnet answer to "max out
// the day" went 1 kcal over, 27 Oct).

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

namespace {
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
bool inBand(const PortionPlan& p) { return p.kcal >= 0.95 * p.leftKcal && p.kcal <= p.leftKcal + 1e-6; }

// The integer after `label` in `t` (the first one), or -1.
long numberAfter(const std::string& t, const std::string& label, size_t from = 0) {
  const size_t at = t.find(label, from);
  return at == std::string::npos ? -1 : std::strtol(t.c_str() + at + label.size(), nullptr, 10);
}
}  // namespace

// "How much pasta and how many 25 g slices to use up the 800 kcal left?"
void test_pasta_and_slices_fill_what_is_left() {
  const PortionPlan p = sizePortions({{"pasta salad", 140, 5, 0, -1, 300}, {"mozzarella", 250, 22, 25, -1, 50}}, 800);
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_TRUE_MESSAGE(inBand(p), renderPortions(p).c_str());
  TEST_ASSERT_TRUE(p.lines[1].units > 0 && std::fabs(p.lines[1].grams - 25.0 * p.lines[1].units) < 1e-9);
  TEST_ASSERT_TRUE(std::fmod(p.lines[0].grams, 5.0) == 0);
  const std::string t = renderPortions(p);
  TEST_ASSERT_TRUE_MESSAGE(has(t, "- mozzarella: " + std::to_string(p.lines[1].units) + " x 25 g"), t.c_str());
  TEST_ASSERT_TRUE(has(t, "kcal of 800 left"));
}

// A plan with three amounts the owner fixed: they stay exact, the rest fills the budget.
void test_fixed_amounts_stay_and_the_rest_fills() {
  const std::vector<PortionFood> foods = {
      {"beef fillet", 190, 28, 0, 100, 0}, {"skyr 5%", 110, 9, 0, 200, 0},  {"berries", 50, 1, 0, 150, 0},
      {"chicken breast", 165, 31, 0, -1, 100}, {"tofu", 145, 15, 0, -1, 100}, {"bread", 250, 9, 30, -1, 30},
      {"salad", 20, 1, 0, -1, 150}};
  const PortionPlan p = sizePortions(foods, 1000, 60);
  TEST_ASSERT_TRUE_MESSAGE(inBand(p), renderPortions(p).c_str());
  TEST_ASSERT_TRUE(p.lines[0].grams == 100 && p.lines[1].grams == 200 && p.lines[2].grams == 150);
  TEST_ASSERT_TRUE(p.lines[0].fixed && !p.lines[3].fixed);
  const std::string t = renderPortions(p);
  TEST_ASSERT_TRUE(has(t, "- beef fillet: 100 g = 190 kcal, 28.0 g protein (fixed)"));
  TEST_ASSERT_TRUE_MESSAGE(has(t, "(meets the 60 g left)"), t.c_str());
}

// The class over random foods: fixed exact, whole units, 5 g steps, never negative,
// never over the kcal left, the total in the band or (when no step fits) within one
// step under it, and the shown Total the sum of the shown lines.
void test_property_over_random_foods() {
  srand(2610);
  for (int run = 0; run < 2000; run++) {
    std::vector<PortionFood> foods;
    const int n = 1 + rand() % 6;
    double fixedKcal = 0;
    const double left = 200 + rand() % 1800;
    for (int i = 0; i < n; i++) {
      PortionFood f;
      f.name = "food" + std::to_string(i);
      f.kcal100 = 15 + rand() % 900;
      f.protein100 = rand() % 35;
      f.unitG = rand() % 3 == 0 ? 10 + rand() % 60 : 0;
      if (rand() % 4 == 0 && fixedKcal < left * 0.5) {
        f.fixedG = 5.0 * (rand() % 30);
        fixedKcal += f.fixedG * f.kcal100 / 100;
      }
      f.proposedG = 10 + rand() % 300;
      foods.push_back(f);
    }
    const PortionPlan p = sizePortions(foods, left);
    TEST_ASSERT_TRUE(p.ok);
    double maxStep = 0;
    bool anyFree = false;
    for (size_t i = 0; i < foods.size(); i++) {
      const PortionLine& l = p.lines[i];
      TEST_ASSERT_TRUE(l.grams >= 0);
      if (foods[i].fixedG >= 0) {
        TEST_ASSERT_TRUE(l.grams == foods[i].fixedG);
        continue;
      }
      anyFree = true;
      const double step = foods[i].unitG > 0 ? foods[i].unitG : 5;
      maxStep = std::max(maxStep, step * foods[i].kcal100 / 100);
      TEST_ASSERT_TRUE(std::fabs(std::fmod(l.grams, step)) < 1e-6 || std::fabs(std::fmod(l.grams, step) - step) < 1e-6);
    }
    const std::string t = renderPortions(p);
    if (fixedKcal <= left) TEST_ASSERT_TRUE_MESSAGE(p.kcal <= left + 1e-6, t.c_str());
    if (anyFree && fixedKcal <= 0.95 * left)
      TEST_ASSERT_TRUE_MESSAGE(inBand(p) || left - p.kcal <= maxStep + 1e-6, t.c_str());
    long lines = 0;
    for (size_t at = t.find("\n- "); at != std::string::npos; at = t.find("\n- ", at + 1))
      lines += numberAfter(t, " = ", at);
    TEST_ASSERT_EQUAL_MESSAGE(lines, numberAfter(t, "Total: "), t.c_str());
  }
}

void test_fixed_amounts_over_the_budget_leave_the_rest_at_zero() {
  const PortionPlan p = sizePortions({{"cake", 400, 5, 0, 300, 0}, {"salad", 20, 1, 0, -1, 100}}, 800);
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_TRUE(p.lines[1].grams == 0);
  TEST_ASSERT_TRUE(has(renderPortions(p), "The fixed amounts alone pass the kcal left"));
}

// Whole units that cannot land in the band: the nearest under the kcal left, said so.
void test_a_unit_too_big_for_the_band_is_named() {
  const PortionPlan p = sizePortions({{"pizza slice", 270, 11, 120, -1, 120}}, 500);
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_EQUAL(1, p.lines[0].units);   // 2 slices (648 kcal) would pass the 500 left
  TEST_ASSERT_TRUE_MESSAGE(has(renderPortions(p), "could not land within 95-100 percent"), renderPortions(p).c_str());
  TEST_ASSERT_FALSE(has(renderPortions(p), "fixed amounts alone"));
  // In the band: no note.
  TEST_ASSERT_FALSE(has(renderPortions(sizePortions({{"pasta", 140, 5, 0, -1, 300}}, 800)), "could not land"));
}

void test_refusals_say_what_is_missing() {
  TEST_ASSERT_TRUE(has(sizePortions({{"pasta", 140, 5, 0, -1, 100}}, 0).err, "left_kcal must be a number between 0"));
  // Figures no food has are refused, not sized (a stray digit is not a meal).
  TEST_ASSERT_TRUE(has(sizePortions({{"pasta", 140, 5, 0, -1, 100}}, 25000).err, "left_kcal must be"));
  TEST_ASSERT_TRUE(has(sizePortions({{"pasta", 1400, 5, 0, -1, 100}}, 500).err, "between 0 and 1000"));
  TEST_ASSERT_TRUE(has(sizePortions({{"pasta", 140, 150, 0, -1, 100}}, 500).err, "protein_100g between"));
  TEST_ASSERT_TRUE(has(sizePortions({{"pasta", 140, 5, 0, -1, 9000}}, 500).err, "over 5000 g"));
  TEST_ASSERT_TRUE(has(sizePortions({{"pasta", 140, 5, 0, 9000, 0}}, 500).err, "over 5000 g"));
  TEST_ASSERT_TRUE(has(sizePortions({}, 500).err, "at least one food"));
  TEST_ASSERT_TRUE(has(sizePortions({{"pasta", 0, 5, 0, -1, 100}}, 500).err, "'pasta' needs kcal_100g"));
  TEST_ASSERT_TRUE(has(sizePortions(std::vector<PortionFood>(13, {"x", 100, 1, 0, -1, 0}), 500).err, "at most 12"));
  // A free food with no proposed portion is refused, naming it (equal-kcal shares
  // gave a Mistral head 640 g of eggplant and 500 g of strawberries).
  const std::string e = sizePortions({{"eggplant", 25, 1, 0, -1, 0}, {"beef", 190, 28, 0, 100, 0}}, 800).err;
  TEST_ASSERT_TRUE_MESSAGE(has(e, "give amount_g") && has(e, "for: eggplant"), e.c_str());
  // A food counted in slices may come without an amount: it starts from one slice.
  const PortionPlan slices = sizePortions({{"pasta", 140, 5, 0, -1, 300}, {"cheese", 280, 22, 25, -1, 0}}, 800);
  TEST_ASSERT_TRUE(slices.ok && slices.lines[1].units >= 1);
}

void test_the_tool_answers_through_the_registry() {
  ToolRegistry reg;
  registerFoodTools(reg);
  JsonDocument d;
  d["left_kcal"] = 800;
  JsonArray foods = d["foods"].to<JsonArray>();
  JsonObject pasta = foods.add<JsonObject>();
  pasta["name"] = "pasta salad";
  pasta["kcal_100g"] = 140;
  pasta["amount_g"] = 300;
  JsonObject cheese = foods.add<JsonObject>();
  cheese["name"] = "mozzarella";
  cheese["kcal_100g"] = 250;
  cheese["unit_g"] = 25;   // no amount_g: a food counted in slices starts from one slice
  const ToolResult r = reg.dispatch("food.portions", d.as<ArduinoJson::JsonObjectConst>(), principalForRole("c", Role::Admin));
  TEST_ASSERT_TRUE_MESSAGE(r.success, r.error.c_str());
  TEST_ASSERT_TRUE(has(r.output, "Total: ") && has(r.output, "of 800 left"));
  TEST_ASSERT_TRUE(has(r.output, "Answer with exactly these amounts, every line"));
  TEST_ASSERT_TRUE(has(r.output, "This total counts only: pasta salad, mozzarella."));
  TEST_ASSERT_TRUE(has(r.output, "A food here the owner never named is your suggestion"));
  TEST_ASSERT_TRUE_MESSAGE(has(r.output, "The answer in one sentence: ") && has(r.output, " g pasta salad and "), r.output.c_str());
  // A fixed 0 g is read as no amount: the food is sized like any free one.
  cheese["fixed_g"] = 0;
  const ToolResult z = reg.dispatch("food.portions", d.as<ArduinoJson::JsonObjectConst>(), principalForRole("c", Role::Admin));
  TEST_ASSERT_TRUE(z.success && !has(z.output, "(fixed)") && !has(z.output, "mozzarella: 0 g"));
  JsonDocument bad;
  bad["left_kcal"] = 800;
  TEST_ASSERT_FALSE(reg.dispatch("food.portions", bad.as<ArduinoJson::JsonObjectConst>(), principalForRole("c", Role::Admin)).success);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pasta_and_slices_fill_what_is_left);
  RUN_TEST(test_fixed_amounts_stay_and_the_rest_fills);
  RUN_TEST(test_property_over_random_foods);
  RUN_TEST(test_fixed_amounts_over_the_budget_leave_the_rest_at_zero);
  RUN_TEST(test_a_unit_too_big_for_the_band_is_named);
  RUN_TEST(test_refusals_say_what_is_missing);
  RUN_TEST(test_the_tool_answers_through_the_registry);
  return UNITY_END();
}
