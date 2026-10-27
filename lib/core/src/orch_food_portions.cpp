#include "nimbus/orch/food_portions.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>

namespace nimbus::orch {

namespace {

constexpr size_t kMaxFoods = 12;
// The band the total is brought into: at or under the kcal left (using it up, as
// the owner asks, never going over), and no more than 5 percent under it.
constexpr double kLow = 0.95, kHigh = 1.0;
// Argument bounds: past these a figure is not a food (and a rounding stays exact).
constexpr double kMaxLeftKcal = 20000, kMaxKcal100 = 1000, kMaxGrams = 5000;
constexpr double kGramStep = 5;                // a weighed food moves 5 g at a time
constexpr int kMaxNudges = 400;            // a 12-food plan sizes in well under a millisecond

double stepGrams(const PortionFood& f) { return f.unitG > 0 ? f.unitG : kGramStep; }

void setGrams(PortionLine& l, const PortionFood& f, double grams) {
  l.grams = grams < 0 ? 0 : grams;
  l.units = f.unitG > 0 ? (int)std::lround(l.grams / f.unitG) : 0;
  l.kcal = l.grams * f.kcal100 / 100.0;
  l.protein = l.grams * f.protein100 / 100.0;
}

// Whole units, or the nearest 5 g.
double roundedGrams(const PortionFood& f, double grams) {
  const double step = stepGrams(f);
  return std::round(grams / step) * step;
}

double totalKcal(const std::vector<PortionLine>& lines) {
  double t = 0;
  for (const PortionLine& l : lines) t += l.kcal;
  return t;
}

bool inRange(double v, double hi) { return v >= 0 && v <= hi; }

// Why one food's figures are not a food's, or "".
std::string foodRefusal(const PortionFood& f) {
  if (f.name.empty()) return "every food needs a name";
  if (!(f.kcal100 > 0) || f.kcal100 > kMaxKcal100) return "'" + f.name + "' needs kcal_100g between 0 and 1000";
  if (!inRange(f.protein100, 100)) return "'" + f.name + "' needs protein_100g between 0 and 100";
  if (!inRange(f.unitG, kMaxGrams) || f.fixedG > kMaxGrams || !inRange(f.proposedG, kMaxGrams))
    return "'" + f.name + "' has an amount over 5000 g";
  return std::string();
}

std::string validate(const std::vector<PortionFood>& foods, double leftKcal) {
  if (!(leftKcal > 0) || leftKcal > kMaxLeftKcal)
    return "left_kcal must be a number between 0 and 20000 (the kcal left for this meal or the day)";
  if (foods.empty()) return "give at least one food";
  if (foods.size() > kMaxFoods) return "at most " + std::to_string(kMaxFoods) + " foods";
  for (const PortionFood& f : foods) {
    const std::string why = foodRefusal(f);
    if (!why.empty()) return why;
  }
  std::string missing;
  for (const PortionFood& f : foods)
    if (f.fixedG < 0 && !(f.proposedG > 0) && !(f.unitG > 0)) missing += (missing.empty() ? "" : ", ") + f.name;
  if (!missing.empty())
    return "give amount_g (the portion you would choose in grams; the proportions are kept) or fixed_g "
           "(an amount the owner fixed) for: " + missing;
  return std::string();
}

// A free food's proposed grams: the model's amount, else one unit of a food counted
// in slices or pieces (validate() refuses a weighed free food with neither).
double proposedGrams(const PortionFood& f) { return f.proposedG > 0 ? f.proposedG : f.unitG; }

// The free foods' kcal shares: their proposed amounts.
std::vector<double> shares(const std::vector<PortionFood>& foods) {
  std::vector<double> w(foods.size(), 0.0);
  for (size_t i = 0; i < foods.size(); i++)
    if (foods[i].fixedG < 0) w[i] = proposedGrams(foods[i]) * foods[i].kcal100 / 100;
  return w;
}

// The free food whose one step moves the total toward the band, or -1. Up: the
// largest step that stays at or under the top (never over the kcal left). Down: the
// largest step that stays at or over the bottom, else the smallest step (under the
// band beats over the kcal left).
int nudgePick(const std::vector<PortionLine>& lines, const std::vector<PortionFood>& foods, double left, bool up) {
  const double total = totalKcal(lines);
  int pick = -1, fallback = -1;
  double best = 0, smallest = 0;
  for (size_t i = 0; i < foods.size(); i++) {
    if (foods[i].fixedG >= 0 || (!up && lines[i].grams <= 0)) continue;
    const double step = stepGrams(foods[i]) * foods[i].kcal100 / 100;
    const bool fits = up ? total + step <= kHigh * left : total - step >= kLow * left;
    if (fits && step > best) {
      pick = (int)i;
      best = step;
    }
    if (fallback < 0 || step < smallest) {
      fallback = (int)i;
      smallest = step;
    }
  }
  return pick >= 0 || up ? pick : fallback;
}

// One step toward the band; false when the total is in it or no step helps.
bool nudge(std::vector<PortionLine>& lines, const std::vector<PortionFood>& foods, double left) {
  const double total = totalKcal(lines);
  if (total >= kLow * left && total <= kHigh * left) return false;
  const bool up = total < kLow * left;
  const int pick = nudgePick(lines, foods, left, up);
  if (pick < 0) return false;
  const size_t k = (size_t)pick;
  setGrams(lines[k], foods[k], lines[k].grams + (up ? 1 : -1) * stepGrams(foods[k]));
  return true;
}

std::string num(double v, int decimals) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return buf;
}

// "2 x 25 g" for a food counted in units, "485 g" for a weighed one.
std::string amountText(const PortionLine& l) {
  return l.units > 0 ? std::to_string(l.units) + " x " + num(l.grams / l.units, 0) + " g" : num(l.grams, 0) + " g";
}

}  // namespace

PortionPlan sizePortions(const std::vector<PortionFood>& foods, double leftKcal, double leftProtein) {
  PortionPlan p;
  p.leftKcal = leftKcal;
  p.leftProtein = leftProtein;
  p.err = validate(foods, leftKcal);
  if (!p.err.empty()) return p;
  p.lines.resize(foods.size());
  double fixedKcal = 0;
  for (size_t i = 0; i < foods.size(); i++) {
    p.lines[i].name = foods[i].name;
    p.lines[i].fixed = foods[i].fixedG >= 0;
    if (p.lines[i].fixed) {
      setGrams(p.lines[i], foods[i], foods[i].fixedG);
      fixedKcal += p.lines[i].kcal;
    }
  }
  const double room = leftKcal - fixedKcal;
  const std::vector<double> w = shares(foods);
  double wsum = 0;
  for (double x : w) wsum += x;
  for (size_t i = 0; i < foods.size(); i++) {
    if (p.lines[i].fixed) continue;
    const double kcal = room > 0 && wsum > 0 ? room * w[i] / wsum : 0;
    setGrams(p.lines[i], foods[i], roundedGrams(foods[i], kcal / foods[i].kcal100 * 100));
  }
  for (int n = 0; n < kMaxNudges; n++)
    if (!nudge(p.lines, foods, leftKcal)) break;
  for (const PortionLine& l : p.lines) {
    p.kcal += l.kcal;
    p.protein += l.protein;
  }
  p.ok = true;
  return p;
}

std::string renderPortions(const PortionPlan& p) {
  if (!p.ok) return p.err;
  std::string out = "Portions sized to the " + num(p.leftKcal, 0) + " kcal left:\n";
  long kcal = 0;   // the lines' own rounded figures, so the total matches them exactly
  double fixedKcal = 0;
  std::string names, opening;
  for (size_t i = 0; i < p.lines.size(); i++) {
    const PortionLine& l = p.lines[i];
    const long lineKcal = std::lround(l.kcal);
    out += "- " + l.name + ": " + amountText(l) + (l.units > 0 ? " (" + num(l.grams, 0) + " g)" : "") + " = " +
           std::to_string(lineKcal) + " kcal, " + num(l.protein, 1) + " g protein" + (l.fixed ? " (fixed)" : "") +
           "\n";
    kcal += lineKcal;
    if (l.fixed) fixedKcal += l.kcal;
    names += (names.empty() ? "" : ", ") + l.name;
    opening += (i == 0 ? "" : i + 1 == p.lines.size() ? " and " : ", ") + amountText(l) + " " + l.name;
  }
  out += "Total: " + std::to_string(kcal) + " kcal of " + num(p.leftKcal, 0) + " left, " + num(p.protein, 1) +
         " g protein";
  if (p.leftProtein >= 0)
    out += p.protein + 0.05 >= p.leftProtein ? " (meets the " + num(p.leftProtein, 0) + " g left)"
                                            : " (" + num(p.leftProtein - p.protein, 1) + " g short of the " +
                                                  num(p.leftProtein, 0) + " g left)";
  out += ".\n";
  if (fixedKcal > p.leftKcal) out += "The fixed amounts alone pass the kcal left: the other foods are at 0 g.\n";
  else if (p.kcal < kLow * p.leftKcal || p.kcal > kHigh * p.leftKcal)
    out += "Whole units could not land within 95-100 percent of the kcal left: this is the nearest.\n";
  // The answer's sentence, ready to use (a weaker head opened with a heading).
  out += "The answer in one sentence: " + opening + ", " + std::to_string(kcal) +
         " kcal. Lead with it (after the one sentence owning a mistake, when [THIS MESSAGE] asks for one).\n";
  // Weaker heads dropped the fixed lines, or left the owner's fixed foods out of the
  // call and added them on top.
  out += "This total counts only: " + names + ". A food of the meal the owner named that is missing here is "
         "not counted: call again with it (fixed_g for an amount the owner fixed). A food here the owner never "
         "named is your suggestion: say so, or ask what they have instead. Answer with exactly these "
         "amounts, every line with its kcal (the fixed ones too) and this total.\n";
  return out;
}

namespace {

double numArg(ArduinoJson::JsonObjectConst a, const char* k, double def) {
  return a[k].is<double>() ? a[k].as<double>() : def;
}

ToolResult portionsTool(ArduinoJson::JsonObjectConst a) {
  std::vector<PortionFood> foods;
  for (ArduinoJson::JsonObjectConst f : a["foods"].as<ArduinoJson::JsonArrayConst>()) {
    PortionFood pf;
    pf.name = f["name"].is<const char*>() ? std::string(f["name"].as<const char*>()) : std::string();
    pf.kcal100 = numArg(f, "kcal_100g", 0);
    pf.protein100 = numArg(f, "protein_100g", 0);
    pf.unitG = numArg(f, "unit_g", 0);
    pf.fixedG = numArg(f, "fixed_g", -1);
    if (pf.fixedG <= 0) pf.fixedG = -1;   // a fixed 0 g is no amount at all: the food is free
    pf.proposedG = numArg(f, "amount_g", 0);
    foods.push_back(pf);
  }
  const PortionPlan p = sizePortions(foods, numArg(a, "left_kcal", 0), numArg(a, "left_protein", -1));
  return p.ok ? ToolResult::ok(renderPortions(p)) : ToolResult::fail(p.err);
}

}  // namespace

void registerFoodTools(ToolRegistry& reg) {
  reg.add("food.portions",
          "Exact amounts for a meal plan, or for using up what is left of the day: give left_kcal (and "
          "left_protein), and EVERY food of the meal, the owner's fixed ones included, from the foods the "
          "owner said they have (none named: ask what they have instead of calling): its kcal and "
          "protein per 100 g, then fixed_g for an amount the owner gave (an approximate one too), else "
          "amount_g (required) for the sensible portion you would choose (the proportions are kept and "
          "scaled together to fill the kcal left), and unit_g for slices or pieces. Returns each food's "
          "amount, kcal and protein and the total. Use it for every plan and every how-much answer "
          "instead of adding up yourself, and answer with exactly its amounts; it writes nothing.",
          [](ArduinoJson::JsonObjectConst a, const Principal&) { return portionsTool(a); },
          R"({"type":"object","properties":{"left_kcal":{"type":"number"},"left_protein":{"type":"number"},)"
          R"("foods":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},)"
          R"("kcal_100g":{"type":"number"},"protein_100g":{"type":"number"},"amount_g":{"type":"number"},)"
          R"("unit_g":{"type":"number"},"fixed_g":{"type":"number"}},"required":["name","kcal_100g"]}}},)"
          R"("required":["left_kcal","foods"]})");
}

}  // namespace nimbus::orch
