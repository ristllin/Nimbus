#pragma once
#include <string>
#include <vector>

#include "nimbus/orch/tool_registry.h"

// food_portions - the arithmetic of a meal plan or a "how much to use up what is
// left" answer, done by the device instead of the model (lane CTX).
//
// The food-coach eval (tests/evals/food_coach) on a Mistral head, 27 Oct: plans that
// stopped 20 to 35 percent under the budget they were asked to fill, "3 slices
// (150 g)" of 25 g slices, and a "Total: 1,001 kcal" over lines that added up to 401.
// The model chooses the foods and their proportions; this sizes them: the amounts
// the owner fixed stay as they are, the other foods keep the model's proportions and
// are scaled together so the total lands on the kcal left, rounded to whole units (a
// slice, a piece) or to 5 g, then nudged one step at a time back inside the band
// (95-100 percent: "max out" never means going over).
//
// Portable, Arduino-free, host-tested (test_food_portions). Registered as the
// food.portions tool on the device and in the host lab.
namespace nimbus::orch {

struct PortionFood {
  std::string name;
  double kcal100 = 0;      // kcal per 100 g (required, > 0)
  double protein100 = 0;   // protein per 100 g
  double unitG = 0;        // one slice or piece in grams (0 = weighed in grams)
  double fixedG = -1;      // an amount the owner fixed (< 0 = free)
  double proposedG = 0;    // the model's own amount: the proportions it wants (required when free)
};

struct PortionLine {
  std::string name;
  double grams = 0;
  int units = 0;           // whole units when the food has a unit size
  double kcal = 0, protein = 0;
  bool fixed = false;
};

struct PortionPlan {
  bool ok = false;
  std::string err;
  std::vector<PortionLine> lines;
  double kcal = 0, protein = 0;
  double leftKcal = 0, leftProtein = -1;   // < 0 = no protein goal given
};

// Size `foods` to fill `leftKcal`: fixed amounts first, then the free foods scaled
// together with their proposed proportions kept, so the total lands within 95-100
// percent of it whenever whole units allow, and never over it unless the fixed
// amounts alone are. A free food with no proposed amount is
// refused (equal-kcal shares made piles of salad). `leftProtein` < 0 = no protein
// goal (reported only).
PortionPlan sizePortions(const std::vector<PortionFood>& foods, double leftKcal, double leftProtein = -1);

// The tool text: one line per food, then the total against what is left.
std::string renderPortions(const PortionPlan& p);

// food.portions over the registry (read-only: it writes nothing, logs nothing).
void registerFoodTools(ToolRegistry& reg);

}  // namespace nimbus::orch
