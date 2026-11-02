#include "nimbus/orch/food_figures.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "nimbus/mem_cap.h"   // utf8CapLen - an excerpt is cut on a character boundary

namespace nimbus::orch {

namespace {

enum class Unit : uint8_t { None = 0, Kcal, Gram };

struct Tok {
  std::string word;    // lowercase; empty for a number
  double value = 0;
  bool number = false;
  bool approx = false;   // "~450", "about 450"
  Unit unit = Unit::None;
  size_t at = 0, end = 0;   // the number's bytes in the text it was read from
};

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (unsigned char)c >= 0x80; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool oneOf(const std::string& w, std::initializer_list<const char*> set) {
  for (const char* s : set)
    if (w == s) return true;
  return false;
}

// Lowercase, the curly apostrophe straight, markdown emphasis out.
std::string folded(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s.compare(i, 3, "\xE2\x80\x99") == 0) {
      out += '\'';
      i += 2;
    } else if (s[i] != '*' && s[i] != '`') {
      out += lower(s[i]);
    }
  }
  return out;
}

// A number from `p`: digits with thousands commas and one decimal point.
size_t readNumber(const std::string& s, size_t p, double& v) {
  std::string digits;
  for (; p < s.size(); p++) {
    if (isDigit(s[p])) {
      digits += s[p];
    } else if (s[p] == ',' && p + 3 < s.size() + 0 && isDigit(s[p + 1]) && p + 3 < s.size() && isDigit(s[p + 2]) &&
               isDigit(s[p + 3]) && (p + 4 >= s.size() || !isDigit(s[p + 4]))) {
      continue;
    } else if (s[p] == '.' && p + 1 < s.size() && isDigit(s[p + 1]) && digits.find('.') == std::string::npos) {
      digits += '.';
    } else {
      break;
    }
  }
  v = std::strtod(digits.c_str(), nullptr);
  return p;
}

std::vector<Tok> tokensOf(const std::string& sentence) {
  std::vector<Tok> out;
  bool tilde = false;
  for (size_t p = 0; p < sentence.size();) {
    const char c = sentence[p];
    if (isDigit(c)) {
      Tok t;
      t.number = true;
      t.approx = tilde || (!out.empty() && oneOf(out.back().word, {"about", "around", "approx", "approximately", "roughly"}));
      t.at = p;
      p = readNumber(sentence, p, t.value);
      t.end = p;
      out.push_back(t);
      tilde = false;
    } else if (isLetter(c)) {
      Tok t;
      while (p < sentence.size() && (isLetter(sentence[p]) || sentence[p] == '\'')) t.word += sentence[p++];
      if (!out.empty() && out.back().number && out.back().unit == Unit::None) {
        if (oneOf(t.word, {"kcal", "kcals", "cal", "cals", "calorie", "calories"})) out.back().unit = Unit::Kcal;
        else if (oneOf(t.word, {"g", "gm", "gram", "grams"})) out.back().unit = Unit::Gram;
      }
      out.push_back(t);
      tilde = false;
    } else {
      tilde = c == '~';
      p++;
    }
  }
  return out;
}

// Sentences: a line, or text up to ". ", "! ", "? ". A decimal point never ends one.
std::vector<std::string> sentencesOf(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  const auto flush = [&]() {
    if (cur.find_first_not_of(" \t") != std::string::npos) out.push_back(cur);
    cur.clear();
  };
  for (size_t i = 0; i < text.size(); i++) {
    const char c = text[i];
    if (c == '\n') {
      flush();
      continue;
    }
    cur += c;
    if ((c == '.' || c == '!' || c == '?') && (i + 1 == text.size() || text[i + 1] == ' ' || text[i + 1] == '\n')) flush();
  }
  flush();
  return out;
}

bool hasWord(const std::vector<Tok>& toks, std::initializer_list<const char*> set) {
  for (const Tok& t : toks)
    if (!t.number && oneOf(t.word, set)) return true;
  return false;
}

// A figure in this sentence is not stated as fact: a question, a hypothetical, a future.
bool hypothetical(const std::string& low, const std::vector<Tok>& toks) {
  if (low.find('?') != std::string::npos) return true;
  // Up to the first figure: "you're short by 102 g, more if ..." still states it.
  for (const Tok& t : toks) {
    if (t.number && t.unit != Unit::None) return false;
    if (!t.number && oneOf(t.word, {"if", "would", "could", "might", "may", "suppose", "unless", "will", "i'll", "once", "want"}))
      return true;
  }
  return false;
}

bool negationWord(const Tok& t) {
  return !t.number && oneOf(t.word, {"not", "no", "never", "isn't", "wasn't", "aren't", "didn't", "don't", "doesn't", "without"});
}

// The sentence takes its figures back, whole: "the 250 g figure was my error". Not "no" or "not" or
// "sorry", which are in every other reply ("No worries, your target is 250 g").
bool retracted(const std::string& low, const std::vector<Tok>& toks) {
  return hasWord(toks, {"error", "mistake", "mistaken", "wrong", "incorrect", "invented", "hallucinated"}) ||
         low.find("made up") != std::string::npos;
}

// A negation that binds to this figure: "not the 250 g", "no 250 g target", "no target of 250",
// "never said 2500". "You're not at your 2,500 kcal target" says there is one.
bool negatedBefore(const std::vector<Tok>& toks, size_t i) {
  for (size_t b = i; b > 0 && i - b < 4; b--) {
    const Tok& t = toks[b - 1];
    if (t.number) return false;
    if (negationWord(t)) return true;
    if (!oneOf(t.word, {"the", "a", "an", "any", "that", "this", "about", "around", "such", "actually", "really", "target", "goal",
                        "floor", "minimum", "budget", "limit", "of", "your", "my", "said", "say", "told", "mentioned", "gave", "give"}))
      return false;
  }
  return false;
}

// "the 2,500 kcal target isn't in your plan", "250 g is not your target". "is not met" keeps the target.
bool negatedAfter(const std::vector<Tok>& toks, size_t i) {
  bool verb = false;   // "2,500 kcal, not 2,400" negates what follows it
  for (size_t f = i + 1; f < toks.size() && f <= i + 5; f++) {
    const Tok& t = toks[f];
    if (t.number) return false;
    if (negationWord(t)) {
      if (!verb && oneOf(t.word, {"not", "no", "never", "without"})) return false;
      return f + 1 >= toks.size() || !oneOf(toks[f + 1].word, {"met", "reached", "hit", "there", "yet"});
    }
    verb = verb || oneOf(t.word, {"is", "was", "does", "did"});
    if (!verb && !oneOf(t.word, {"g", "gm", "gram", "grams", "kcal", "kcals", "cal", "cals", "calorie", "calories", "protein", "figure", "number",
                                 "target", "goal", "floor", "minimum", "budget", "limit"}))
      return false;
  }
  return false;
}

bool negatedFigure(const std::vector<Tok>& toks, size_t i) { return negatedBefore(toks, i) || negatedAfter(toks, i); }

bool macroSentence(const std::vector<Tok>& toks) {
  return hasWord(toks, {"protein", "fat", "fibre", "fiber", "carb", "carbs", "macros", "floor"});
}

// Is this number a daily figure the sentence can be read about?
bool foodUnit(const Tok& t, bool macros) { return t.unit == Unit::Kcal || (t.unit == Unit::Gram && macros); }

// What may stand between a figure and the target word it names: "2,400 kcal daily target".
bool skipBefore(const Tok& t) {
  return !t.number && oneOf(t.word, {"g", "gm", "gram", "grams", "kcal", "kcals", "cal", "cals", "calorie", "calories", "daily",
                                     "day", "per", "protein", "fat", "fibre", "fiber", "carb", "carbs", "stated", "current",
                                     "working", "original", "minimum"});
}

// "target of 2,400 kcal", "protein target: 150 g", "adjusted it to 2,500".
bool skipAfter(const Tok& t) {
  return skipBefore(t) || (!t.number && oneOf(t.word, {"of", "is", "are", "at", "was", "to", "for", "set", "a", "the", "your", "my",
                                                       "as", "it", "that", "plan's", "about", "around", "roughly"}));
}

struct Stated {
  double value = 0;
  Unit unit = Unit::None;
  size_t at = 0;   // its token
};

bool leftAt(const std::vector<Tok>& toks, size_t i);

// A bare "2,500" in a sentence that names what it counts: "your calorie target is 2,500".
Unit bareUnit(const Tok& t, const std::vector<Tok>& toks) {
  if (hasWord(toks, {"calorie", "calories", "kcal", "kcals"})) return t.value >= 500 ? Unit::Kcal : Unit::None;
  return hasWord(toks, {"protein"}) && t.value >= 20 ? Unit::Gram : Unit::None;
}

Unit figureUnit(const Tok& t, const std::vector<Tok>& toks, bool macros) {
  if (foodUnit(t, macros)) return t.unit;
  return t.unit == Unit::None ? bareUnit(t, toks) : Unit::None;
}

// The figure just before a target word ("2,400 kcal target", "150-160 g band").
std::vector<Stated> figureBefore(const std::vector<Tok>& toks, size_t i, bool macros) {
  std::vector<Stated> out;
  for (size_t back = i; back > 0 && i - back <= 5; back--) {
    const Tok& t = toks[back - 1];
    if (!t.number) {
      if (!skipBefore(t)) break;
      continue;
    }
    const Unit unit = figureUnit(t, toks, macros);
    if (unit != Unit::None) {
      out.push_back({t.value, unit, back - 1});
      if (back >= 2 && toks[back - 2].number && toks[back - 2].unit == Unit::None)
        out.push_back({toks[back - 2].value, unit, back - 2});
    }
    break;
  }
  return out;
}

// The figure just after one ("protein target: 150 g", "target of 150-160 g").
std::vector<Stated> figureAfter(const std::vector<Tok>& toks, size_t i, bool macros) {
  std::vector<Stated> out;
  for (size_t fwd = i + 1; fwd < toks.size() && fwd - i <= 5; fwd++) {
    const Tok& t = toks[fwd];
    if (!t.number) {
      if (!skipAfter(t)) break;
      continue;
    }
    if (leftAt(toks, fwd)) break;   // "against your targets: 472 kcal left of 2,400"
    if (t.unit == Unit::None && fwd + 1 < toks.size() && toks[fwd + 1].number && foodUnit(toks[fwd + 1], macros)) {
      out.push_back({t.value, toks[fwd + 1].unit, fwd});
      out.push_back({toks[fwd + 1].value, toks[fwd + 1].unit, fwd + 1});
    } else if (figureUnit(t, toks, macros) != Unit::None) {
      out.push_back({t.value, figureUnit(t, toks, macros), fwd});
    }
    break;
  }
  return out;
}

std::vector<Stated> figuresOfTarget(const std::vector<Tok>& toks, size_t i, bool macros) {
  std::vector<Stated> out = figureBefore(toks, i, macros);
  return out.empty() ? figureAfter(toks, i, macros) : out;
}

bool nearSource(double v, const std::vector<double>& sources, double tolerance) {
  for (double s : sources)
    if (std::fabs(s - v) <= tolerance) return true;
  return false;
}

std::vector<double> parseNumbers(const std::string& normalized) {
  std::vector<double> out;
  size_t p = 0;
  while (p < normalized.size()) {
    const size_t e = normalized.find(' ', p);
    const std::string n = normalized.substr(p, e == std::string::npos ? std::string::npos : e - p);
    if (!n.empty()) out.push_back(std::strtod(n.c_str(), nullptr));
    if (e == std::string::npos) break;
    p = e + 1;
  }
  return out;
}

std::string excerptOf(const std::string& sentence) {
  const size_t b = sentence.find_first_not_of(" \t-*");
  const std::string s = b == std::string::npos ? sentence : sentence.substr(b);
  if (s.size() <= 100) return s;
  return s.substr(0, (size_t)nimbus::utf8CapLen(s.c_str(), (int)s.size(), 100));
}

bool mealWord(const std::string& w) {
  return oneOf(w, {"breakfast", "lunch", "dinner", "supper", "brunch", "snack", "snacks", "meal", "meals", "tonight", "shake"});
}

// A meal is named. "meal plan" names a plan.
bool namesAMeal(const std::vector<Tok>& toks) {
  for (size_t i = 0; i < toks.size(); i++)
    if (mealWord(toks[i].word) && !(i + 1 < toks.size() && toks[i + 1].word == "plan")) return true;
  return false;
}

// "... at dinner", "... per meal" right after this token.
bool mealFollows(const std::vector<Tok>& toks, size_t i) {
  size_t j = i + 1;
  while (j < toks.size() && oneOf(toks[j].word, {"g", "gm", "gram", "grams", "kcal", "kcals", "cal", "cals", "calorie", "calories", "protein"})) j++;
  if (j >= toks.size() || !oneOf(toks[j].word, {"at", "for", "per", "with", "in", "each", "by"})) return false;
  for (size_t k = j + 1; k < toks.size() && k <= j + 2; k++) {
    if (mealWord(toks[k].word)) return true;
    if (!oneOf(toks[k].word, {"each", "the", "this", "your", "every"})) return false;
  }
  return false;
}

// Advice for one meal is no daily target ("Aim for 40 g protein at dinner", "Keep dinner to a 600
// kcal limit"). A weak word goes with any meal in its sentence, a firm one only with one right after.
bool perMeal(const std::vector<Tok>& toks, size_t word, size_t figure) {
  if (hasWord(toks, {"daily", "day", "day's"})) return false;
  if (oneOf(toks[word].word, {"target", "targets", "goal", "goals", "floor", "quota"})) return mealFollows(toks, word) || mealFollows(toks, figure);
  return namesAMeal(toks);
}

bool targetWordAt(const std::vector<Tok>& toks, size_t i) {
  const std::string& w = toks[i].word;
  if (oneOf(w, {"adjusted", "raised", "lowered", "changed"})) return i + 1 < toks.size();   // "we adjusted it to 2,500"
  // "Your daily protein is 250 g", "Daily protein: 250 g". "so far" is what happened.
  if (w == "daily")
    return i + 2 < toks.size() && oneOf(toks[i + 1].word, {"protein", "calories", "calorie", "kcal"}) &&
           (toks[i + 2].number || oneOf(toks[i + 2].word, {"is", "are", "of"}));
  if (!oneOf(w, {"target", "targets", "goal", "goals", "budget", "allowance", "aim", "minimum", "limit", "floor", "quota", "ceiling",
                 "cap", "requirement"}))
    return false;
  // "dinner budget is ~1,036 kcal" is what is left for a meal, "remaining to target: 90 kcal" what is left.
  return !(i > 0 && oneOf(toks[i - 1].word, {"breakfast", "lunch", "dinner", "snack", "meal", "remaining", "to", "left"}));
}

// The figures named by a target word.
std::vector<Stated> namedTargets(const std::vector<Tok>& toks, bool macros) {
  std::vector<Stated> out;
  for (size_t i = 0; i < toks.size(); i++) {
    if (toks[i].number || !targetWordAt(toks, i)) continue;
    for (const Stated& f : figuresOfTarget(toks, i, macros))
      if (!perMeal(toks, i, f.at)) out.push_back(f);
  }
  return out;
}

// Under these a figure is a snack or a label, not a day's.
bool dayScale(const Tok& t) { return t.value >= (t.unit == Unit::Kcal ? 800 : 40); }

size_t afterUnit(const std::vector<Tok>& toks, size_t i) {
  size_t j = i + 1;
  while (j < toks.size() && oneOf(toks[j].word, {"g", "gm", "gram", "grams", "kcal", "kcals", "cal", "cals", "calorie", "calories", "protein", "of"})) j++;
  return j;
}

// "2,500 kcal a day", "150 g/day", "250 g protein per day"; with a firm verb, "250 g protein today".
bool rateAfter(const std::vector<Tok>& toks, size_t i, bool firm) {
  const size_t j = afterUnit(toks, i);
  if (j >= toks.size()) return false;
  const std::string& w = toks[j].word;
  if (w == "daily" || w == "day" || (firm && w == "today")) return true;
  return oneOf(w, {"a", "per", "each", "every"}) && j + 1 < toks.size() && toks[j + 1].word == "day";
}

// A daily target by another name: "you should eat 2,500 kcal a day", "you need 250 g protein today".
// What happened ("you've averaged", "a deficit") is no prescription.
std::vector<Stated> prescribedTargets(const std::vector<Tok>& toks, bool macros) {
  std::vector<Stated> out;
  const bool firm = hasWord(toks, {"should", "must", "need"});
  if ((!firm && !hasWord(toks, {"eat", "plan", "plan's", "calls", "recommended", "recommend", "requires"})) || namesAMeal(toks) ||
      hasWord(toks, {"average", "averaged", "averaging", "ate", "eaten", "burn", "burns", "burned", "deficit", "surplus", "maintenance",
                     "tdee", "yesterday"}))
    return out;
  for (size_t i = 0; i < toks.size(); i++)
    if (toks[i].number && foodUnit(toks[i], macros) && dayScale(toks[i]) && !leftAt(toks, i) && rateAfter(toks, i, firm))
      out.push_back({toks[i].value, toks[i].unit, i});
  return out;
}

// "Your plan says 2,100 kcal", "250 g protein is what the plan asks for": the plan's figure for the day.
std::vector<Stated> planTargets(const std::vector<Tok>& toks, bool macros) {
  std::vector<Stated> out;
  if (!hasWord(toks, {"plan", "plan's"}) || !hasWord(toks, {"calls", "asks", "says", "sets", "gives", "wants", "lists"}) || namesAMeal(toks))
    return out;
  for (size_t i = 0; i < toks.size(); i++) {
    if (!toks[i].number || !foodUnit(toks[i], macros) || !dayScale(toks[i]) || leftAt(toks, i)) continue;
    out.push_back({toks[i].value, toks[i].unit, i});
    break;
  }
  return out;
}

void targetGaps(const std::string& sentence, const std::vector<Tok>& toks, const std::vector<double>& sources,
                std::vector<FoodGap>& out) {
  const bool macros = macroSentence(toks);
  std::vector<Stated> stated = namedTargets(toks, macros);
  for (const std::vector<Stated>& more : {prescribedTargets(toks, macros), planTargets(toks, macros)})
    stated.insert(stated.end(), more.begin(), more.end());
  for (const Stated& f : stated) {
    if (nearSource(f.value, sources, f.unit == Unit::Kcal ? 0.5 : 0.05) || negatedFigure(toks, f.at)) continue;
    FoodGap g;
    g.kind = FoodGapKind::UnsourcedTarget;
    g.excerpt = excerptOf(sentence);
    g.stated = f.value;
    g.protein = f.unit == Unit::Gram;
    out.push_back(g);
    return;   // one per sentence
  }
}

bool targetWordWithin(const std::vector<Tok>& toks, size_t from, size_t count) {
  for (size_t f = from; f < toks.size() && f < from + count; f++)
    if (!toks[f].number && oneOf(toks[f].word, {"target", "goal", "minimum", "budget", "limit", "allowance", "floor"})) return true;
  return false;
}

// "you still need 102 g", "need 102 g more", "need 102 g to reach ...". Not "you need 30 g of protein
// at dinner", and not a thing that needs it ("chicken breast needs 30 g").
bool needsMore(const std::vector<Tok>& toks, size_t i, size_t need) {
  if (namesAMeal(toks) && !targetWordWithin(toks, 0, toks.size())) return false;
  if (need > 0 && toks[need - 1].word == "still") return true;
  const size_t j = afterUnit(toks, i);
  if (j < toks.size() && toks[j].word == "more") return true;
  return hasWord(toks, {"reach", "hit", "clear", "close", "meet"}) || targetWordWithin(toks, i + 1, toks.size());
}

// After the figure: "364 kcal left", "38 g short", "106 g below your floor", "102 g to go".
bool leftAfter(const std::vector<Tok>& toks, size_t i) {
  for (size_t f = i + 1; f < toks.size() && f <= i + 4; f++) {
    const Tok& t = toks[f];
    if (t.number) return false;
    if (t.word == "spare") return toks[i].unit == Unit::Kcal;   // "40 g to spare" over a floor is a surplus
    if (oneOf(t.word, {"left", "remaining", "remain", "remains", "short", "needed"})) return !(t.word == "needed" && namesAMeal(toks));
    if (oneOf(t.word, {"below", "under"})) return targetWordWithin(toks, f + 1, 4);
    if (t.word == "go" && toks[f - 1].word == "to") return true;
    if (!oneOf(t.word, {"g", "gm", "gram", "grams", "kcal", "kcals", "cal", "cals", "calorie", "calories", "protein", "of", "to", "still"})) return false;
  }
  return false;
}

// Before it: "leaves you 31 kcal", "short by at least 102 g", "you still need about 102 g".
bool leftBefore(const std::vector<Tok>& toks, size_t i) {
  bool plain = true;   // "short by at least": nothing else between, or "short: today is at 88 g" reads as left
  for (size_t b = i; b > 0 && i - b < 5; b--) {
    const Tok& t = toks[b - 1];
    if (t.number) return false;
    if (t.word == "short") return plain;
    if (oneOf(t.word, {"leaves", "leaving", "remaining", "left"})) return true;
    if (t.word == "need" || t.word == "needed") return needsMore(toks, i, b - 1);
    if (!oneOf(t.word, {"you", "me", "us", "only", "just", "about", "around", "roughly", "approximately", "at", "least", "most", "over",
                        "protein", "up", "to", "still", "with", "a", "an", "the", "is", "today", "for", "by", "another", "of", "target"}))
      return false;
    plain = plain && oneOf(t.word, {"by", "at", "least", "about", "around", "roughly", "approximately"});
  }
  return false;
}

// A figure the sentence says is left of the day: "364 kcal left", "leaves 31 kcal", "need ~102 g more".
bool leftAt(const std::vector<Tok>& toks, size_t i) {
  // "Remaining for 2,400 kcal target": the target itself, not what is left of it.
  for (size_t f = i + 1; f < toks.size() && f <= i + 3; f++) {
    if (!toks[f].number && oneOf(toks[f].word, {"below", "under", "short", "left", "remaining", "over", "above"})) break;
    if (targetWordWithin(toks, f, 1)) return false;
  }
  return leftAfter(toks, i) || leftBefore(toks, i);
}

struct LeftCheck {
  const std::string& sentence;
  const FoodFacts& facts;
  const std::vector<double>& sources;
  bool taken;   // the sentence takes its figure back
};

bool saysAtMost(const std::string& low) {
  return low.find("at most") != std::string::npos || low.find("up to") != std::string::npos ||
         low.find("unknown") != std::string::npos || low.find("not known") != std::string::npos;
}

// The sentence is about a meal it proposes, on top of the ledger: an earlier figure of this kind,
// or a word that points at one ("That leaves ...", "After the pasta ...", "Protein stays ...").
bool projectedLeft(const std::vector<Tok>& toks, size_t i) {
  for (size_t b = 0; b < i; b++) {
    if (toks[b].number ? toks[b].unit == toks[i].unit
                       : oneOf(toks[b].word, {"after", "adding", "add", "plus", "then", "brings", "bringing", "takes", "taking", "puts",
                                              "putting", "gets", "stays", "stay", "this", "that", "which", "with"}))
      return true;
  }
  return false;
}

// A source of this kind above the sum: a target the proposed meal fits under. The sources are a bag
// of every number the turn read, so only numbers of a daily scale count.
bool sourceAbove(double implied, bool protein, const std::vector<double>& sources) {
  for (double s : sources)
    if (s > implied && (protein ? s >= 20 && s < 500 : s >= 800)) return true;
  return false;
}

// Left of a target the sentence itself names (already held to its source), or left after a proposed
// meal. What the ledger says does not settle these: the target stands, and no number was made up.
bool leftOfAKnownTarget(const std::vector<Tok>& toks, size_t i, double implied, const std::vector<double>& sources) {
  const bool protein = toks[i].unit == Unit::Gram;
  for (const Stated& f : namedTargets(toks, macroSentence(toks)))
    if ((f.unit == Unit::Gram) == protein && implied <= f.value + 2) return true;
  return projectedLeft(toks, i) && sourceAbove(implied, protein, sources);
}

// One figure said to be left: a firm need over an unknown row, or a target nothing holds.
void leftGapOf(const LeftCheck& c, const std::vector<Tok>& toks, size_t i, std::vector<FoodGap>& out) {
  const Tok& t = toks[i];
  const bool isProtein = t.unit == Unit::Gram;
  FoodGap g;
  g.excerpt = excerptOf(c.sentence);
  g.protein = isProtein;
  if (isProtein && c.facts.proteinUnknown > 0) {
    if (saysAtMost(folded(c.sentence))) return;
    g.kind = FoodGapKind::ProteinLeftUnknown;
    g.stated = t.value;
    out.push_back(g);
    return;
  }
  if (c.taken || negatedFigure(toks, i) || (!isProtein && !c.facts.kcalKnown)) return;   // taken back, or the total line covers it
  const double implied = t.value + (isProtein ? c.facts.protein : c.facts.kcal);
  const double tolerance = t.approx ? std::fmax(implied * 0.03, 2.0) : 2.0;
  if (nearSource(implied, c.sources, tolerance) || leftOfAKnownTarget(toks, i, implied, c.sources)) return;
  g.kind = FoodGapKind::UnsourcedTarget;
  g.implied = true;
  g.stated = implied;
  g.left = t.value;
  out.push_back(g);
}

// "14 kcal of your 800 left": the 800 is what is left, the 14 is a part of a sum.
bool partOfAnother(const std::vector<Tok>& toks, size_t i) {
  size_t j = i + 1;
  while (j < toks.size() && oneOf(toks[j].word, {"g", "gm", "gram", "grams", "kcal", "kcals", "cal", "cals", "calorie", "calories", "protein"})) j++;
  if (j >= toks.size() || toks[j].word != "of") return false;
  j++;
  if (j < toks.size() && oneOf(toks[j].word, {"your", "the", "my"})) j++;
  return j < toks.size() && toks[j].number;
}

// "38 g short" says protein with no other word for it; "200 g left" may be the rice.
bool gramsAreProtein(const std::vector<Tok>& toks, size_t i) {
  return hasWord(toks, {"protein", "floor"}) || (i + 2 < toks.size() && toks[i + 2].word == "short");
}

void leftGaps(const LeftCheck& c, const std::vector<Tok>& toks, std::vector<FoodGap>& out) {
  for (size_t i = 0; i < toks.size(); i++) {
    const Tok& t = toks[i];
    if (t.number && t.unit != Unit::None && (t.unit != Unit::Gram || gramsAreProtein(toks, i)) && leftAt(toks, i) && !partOfAnother(toks, i))
      leftGapOf(c, toks, i, out);
  }
}

bool mealSentence(const std::string& low) {
  return low.find("meal total") != std::string::npos || low.find("estimate") != std::string::npos;
}

// What a sentence about the whole of today is cued by. The figure it states is the one its cue binds
// to, never just the first in the sentence ("Logged lunch at 650 kcal, which puts today's total at 1,482").
constexpr const char* const kDayCues[] = {"today's total", "total today", "total for today", "totals for today", "today's log", "today now",
                                          "so far today", "today so far", "day so far", "day total", "day's total", "logged today",
                                          "total so far", "totals so far", "today is ", "today (", "today:", "today you", "today stands"};

struct DayFigures {
  const Tok* kcal = nullptr;
  const Tok* protein = nullptr;
  size_t cue = 0;   // the token the cue starts at
};

// May stand between a cue and its figure: "today (Sat 31 Oct), straight from the log (6 rows): 2,369 kcal".
bool totalLink(const Tok& t) {
  return t.number || oneOf(t.word, {"is", "are", "now", "at", "stands", "standing", "totals", "total", "totaling", "comes", "came", "to", "so",
                                    "far", "of", "you", "you're", "we", "we're", "on", "sits", "just", "currently", "about", "around",
                                    "roughly", "approx", "approximately", "exactly", "straight", "from", "the", "log", "ledger", "rows", "row",
                                    "items", "entries", "today", "today's", "day", "logged", "says", "shows", "has", "have", "you've", "it's",
                                    "least", "sat", "sun", "mon", "tue", "wed", "thu", "fri", "saturday", "sunday", "monday", "tuesday",
                                    "wednesday", "thursday", "friday", "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct",
                                    "nov", "dec", "january", "february", "march", "april", "june", "july", "august", "september", "october",
                                    "november", "december"});
}

bool figureLink(const Tok& t) {
  return t.number || oneOf(t.word, {"and", "kcal", "kcals", "cal", "cals", "calorie", "calories", "g", "gm", "gram", "grams", "protein", "so", "far", "today", "logged"});
}

void takeFigure(const std::vector<Tok>& toks, size_t i, DayFigures& d) {
  const Tok& t = toks[i];
  if (!t.number || leftAt(toks, i)) return;
  if (t.unit == Unit::Kcal && !d.kcal) d.kcal = &t;
  if (t.unit == Unit::Gram && !d.protein && i + 2 < toks.size() && toks[i + 2].word == "protein") d.protein = &t;
}

bool boundAfter(const std::vector<Tok>& toks, size_t start, DayFigures& d) {
  size_t i = start;
  while (i < toks.size() && !(toks[i].number && toks[i].unit != Unit::None)) {
    if (!totalLink(toks[i])) return false;
    i++;
  }
  for (; i < toks.size() && figureLink(toks[i]); i++) takeFigure(toks, i, d);
  return d.kcal || d.protein;
}

// "the ledger shows 1400 kcal and 95 g protein so far today"
bool boundBefore(const std::vector<Tok>& toks, size_t start, DayFigures& d) {
  for (size_t i = start; i > 0 && figureLink(toks[i - 1]); i--) takeFigure(toks, i - 1, d);
  return d.kcal || d.protein;
}

bool bindDayFigures(const std::string& low, const std::vector<Tok>& toks, DayFigures& d) {
  for (const char* cue : kDayCues) {
    const size_t pos = low.find(cue);
    if (pos == std::string::npos) continue;
    const size_t start = tokensOf(low.substr(0, pos)).size();
    if (start > 0 && mealWord(toks[start - 1].word)) continue;   // "Lunch today is 650 kcal"
    d.cue = start;
    if (boundAfter(toks, tokensOf(low.substr(0, pos + std::string(cue).size())).size(), d)) return true;
    if (!namesAMeal(toks) && boundBefore(toks, start, d)) return true;
  }
  return false;
}

// "A 700 kcal dinner brings today's total to 2,200 kcal": over a meal the reply proposes. Logged, or
// below what the ledger already holds, it is no projection.
bool projectedTotal(const std::vector<Tok>& toks, const DayFigures& d, const FoodFacts& facts) {
  if (d.kcal->value <= facts.kcal || hasWord(toks, {"logged", "added", "log", "logging"})) return false;
  for (size_t b = d.cue; b > 0 && d.cue - b < 2; b--) {
    if (!oneOf(toks[b - 1].word, {"brings", "bring", "bringing", "takes", "take", "taking", "puts", "put", "putting", "pushes", "push", "gets", "get"})) continue;
    for (size_t k = 0; k + 1 < b; k++)
      if (toks[k].number && toks[k].unit == Unit::Kcal) return true;
  }
  return false;
}

// The kcal figure of a sentence about the whole of today, against the ledger. False = fine.
bool kcalTotalOff(const Tok& t, const FoodFacts& facts, const std::vector<double>& sources) {
  const double tol = t.approx ? std::fmax(facts.kcal * 0.02, 2.0) : 1.0;
  if (facts.kcalKnown && std::fabs(t.value - facts.kcal) <= tol) return false;
  // No rows today: only a figure the owner or a record gave may stand (30 Oct: yesterday's
  // 2,524 carried into a day with nothing logged yet).
  return !(facts.rows == 0 && facts.kcalKnown && nearSource(t.value, sources, 1.0));
}

// Over a row of unknown protein the known part is a floor: more than it is an estimate, less is wrong.
bool proteinTotalOff(const Tok& t, const FoodFacts& facts) {
  const double tol = t.approx ? std::fmax(facts.protein * 0.05, 1.0) : 0.5;
  if (facts.proteinUnknown > 0) return t.value < facts.protein - tol;
  return std::fabs(t.value - facts.protein) > tol;
}

void dayTotalGaps(const std::string& sentence, const std::vector<Tok>& toks, const FoodFacts& facts,
                  const std::vector<double>& sources, std::vector<FoodGap>& out) {
  const std::string low = folded(sentence);
  if (mealSentence(low) || hasWord(toks, {"yesterday", "tomorrow", "last", "after", "plus", "then", "before", "drafted", "was"})) return;
  DayFigures d;
  if (!bindDayFigures(low, toks, d)) return;
  FoodGap g;
  g.kind = FoodGapKind::WrongDayTotal;
  g.excerpt = excerptOf(sentence);
  if (d.kcal && !projectedTotal(toks, d, facts) && kcalTotalOff(*d.kcal, facts, sources)) {
    g.stated = d.kcal->value;
    g.truth = facts.kcal;
    out.push_back(g);
  } else if (d.protein && facts.rows > 0 && proteinTotalOff(*d.protein, facts)) {
    g.protein = true;
    g.stated = d.protein->value;
    g.truth = facts.protein;
    out.push_back(g);
  }
}

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.10g", std::round(v * 10) / 10);
  return buf;
}

std::string statedTarget(const FoodGap& g) { return num(g.stated) + (g.protein ? " g protein" : " kcal"); }

std::string ledgerProtein(const FoodFacts& facts, double known) {
  return (facts.proteinUnknown > 0 ? "at least " : "") + num(known) + " g protein";
}

}  // namespace

namespace {

// Days since 1970 of a civil date (Howard Hinnant's algorithm).
int daysOf(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// The whole word only: "marked 5" and "decided 3" are no dates.
int monthOf(const std::string& w) {
  static const char* const kShort[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
  static const char* const kLong[] = {"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"};
  for (int i = 0; i < 12; i++)
    if (w == kShort[i] || w == kLong[i] || (i == 8 && w == "sept")) return i + 1;
  return 0;
}

void isoDatesIn(const std::string& low, std::vector<int>& out) {
  for (size_t p = 0; p + 10 <= low.size(); p++) {
    bool iso = true;
    for (size_t i = 0; i < 10 && iso; i++) iso = (i == 4 || i == 7) ? low[p + i] == '-' : isDigit(low[p + i]);
    if (iso) out.push_back(daysOf(std::atoi(low.c_str() + p), std::atoi(low.c_str() + p + 5), std::atoi(low.c_str() + p + 8)));
  }
}

// The day number next to a month name at `i` (14 Oct, Oct 14), or null.
const Tok* dayOfMonthAt(const std::vector<Tok>& toks, size_t i) {
  const Tok* day = i > 0 && toks[i - 1].number ? &toks[i - 1] : (i + 1 < toks.size() && toks[i + 1].number ? &toks[i + 1] : nullptr);
  return day && day->value >= 1 && day->value <= 31 && day->value == (int)day->value ? day : nullptr;
}

// An explicit year beside the date: "14 Oct 2026", "Oct 14 2026".
int yearOfDateAt(const std::vector<Tok>& toks, size_t i) {
  for (size_t k = i + 1; k < toks.size() && k <= i + 2; k++)
    if (toks[k].number && toks[k].value >= 1900 && toks[k].value < 2200 && toks[k].value == (int)toks[k].value) return (int)toks[k].value;
  return 0;
}

// Every date in a sentence, as days since 1970: 2026-10-14, 14 Oct, Oct 14, 14 October 2026. With no
// year, a date more than half a year ahead is last year's ("14 Dec" read in January).
std::vector<int> datesIn(const std::vector<Tok>& toks, const std::string& low, int todayYear, int now) {
  std::vector<int> out;
  isoDatesIn(low, out);
  for (size_t i = 0; i < toks.size(); i++) {
    const int month = toks[i].number ? 0 : monthOf(toks[i].word);
    if (!month || (toks[i].word == "may" && !(i > 0 && toks[i - 1].number))) continue;
    const Tok* day = dayOfMonthAt(toks, i);
    if (!day) continue;
    const int year = yearOfDateAt(toks, i);
    const int d = daysOf(year ? year : todayYear, month, (int)day->value);
    out.push_back(!year && d > now + 183 ? d - 365 : d);
  }
  return out;
}

// A sentence that gives something as the state of things. A date that starts or ends a span is not
// the age of its data ("since 2026-10-01", "active until 31 Dec"), unless it also says "as of".
bool statusSentence(const std::string& low, const std::vector<Tok>& toks) {
  const bool status = hasWord(toks, {"streak", "consecutive", "currently", "latest", "running", "still", "current"}) ||
                      low.find("in a row") != std::string::npos || low.find("so far") != std::string::npos ||
                      low.find("as of") != std::string::npos || low.find("you've been") != std::string::npos;
  if (!status) return false;
  return low.find("as of") != std::string::npos || !hasWord(toks, {"since", "started", "began", "begun", "joined", "until", "till"});
}

}  // namespace

std::string staleStatusIn(const std::string& reply, const std::string& today, int maxAgeDays, int* ageDays) {
  if (today.size() < 10) return std::string();
  const int year = std::atoi(today.c_str());
  const int now = daysOf(year, std::atoi(today.c_str() + 5), std::atoi(today.c_str() + 8));
  for (const std::string& sentence : sentencesOf(reply)) {
    const std::string low = folded(sentence);
    const std::vector<Tok> toks = tokensOf(low);
    if (low.find('?') != std::string::npos || !statusSentence(low, toks)) continue;
    for (int d : datesIn(toks, low, year, now)) {
      if (d > now || now - d <= maxAgeDays) continue;   // an upcoming date is not old data
      if (ageDays) *ageDays = now - d;
      return excerptOf(sentence);
    }
  }
  return std::string();
}

std::string staleStatusNote(int ageDays) {
  return "\n\n(Note: that cites a date " + std::to_string(ageDays) + " days old. Check it is still current.)";
}

bool speaksOfFood(const std::string& text) {
  const std::string low = folded(text);
  return low.find("kcal") != std::string::npos || low.find("calorie") != std::string::npos || low.find("protein") != std::string::npos;
}

std::vector<FoodGap> unsourcedFoodTargets(const std::string& text, const std::string& sourceNumbers) {
  std::vector<FoodGap> out;
  const std::vector<double> sources = parseNumbers(sourceNumbers);
  for (const std::string& sentence : sentencesOf(text)) {
    const std::string low = folded(sentence);
    const std::vector<Tok> toks = tokensOf(low);
    if (!hypothetical(low, toks) && !retracted(low, toks)) targetGaps(sentence, toks, sources, out);
  }
  return out;
}

std::vector<FoodGap> foodFigureGaps(const std::string& reply, const FoodFacts& facts, const std::string& sourceNumbers) {
  std::vector<FoodGap> out;
  const std::vector<double> sources = parseNumbers(sourceNumbers);
  for (const std::string& sentence : sentencesOf(reply)) {
    const std::string low = folded(sentence);
    const std::vector<Tok> toks = tokensOf(low);
    if (hypothetical(low, toks)) continue;
    const bool taken = retracted(low, toks);
    const size_t before = out.size();
    if (!taken) targetGaps(sentence, toks, sources, out);
    if (!facts.have) continue;
    if (out.size() == before) leftGaps(LeftCheck{sentence, facts, sources, taken}, toks, out);
    dayTotalGaps(sentence, toks, facts, sources, out);
  }
  return out;
}

namespace {

// Clauses of a sentence: a comma, semicolon or colon that is not inside a figure ("2,200").
std::vector<std::string> clausesOf(const std::string& sentence) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < sentence.size(); i++) {
    const char c = sentence[i];
    const bool split = c == ';' || c == ':' || (c == ',' && !(i > 0 && isDigit(sentence[i - 1]) && i + 1 < sentence.size() && isDigit(sentence[i + 1])));
    if (split) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

// This clause of the owner's disputes or asks, and sources nothing.
bool ownerDisputes(const std::string& low, const std::vector<Tok>& toks, bool asks) {
  if (retracted(low, toks) || hasWord(toks, {"why", "where", "how", "hallucinate", "hallucinating", "invent"})) return true;
  // In a sentence with a question, only a clause that speaks for the owner states anything.
  return asks && (low.find('?') != std::string::npos || !hasWord(toks, {"my", "i", "i'm", "i've"}));
}

// The clause with any figure it negates blanked: "not 250" sources nothing, the rest stands.
std::string withoutNegatedFigures(const std::string& low, const std::vector<Tok>& toks) {
  std::string out;
  size_t from = 0;
  for (size_t i = 0; i < toks.size(); i++) {
    if (!toks[i].number || !negatedFigure(toks, i)) continue;
    out += low.substr(from, toks[i].at - from);
    from = toks[i].end;
  }
  return out + low.substr(from);
}

}  // namespace

std::string ownerStatementsOf(const std::string& text) {
  std::string out;
  for (const std::string& sentence : sentencesOf(text)) {
    const bool asks = sentence.find('?') != std::string::npos;
    for (const std::string& clause : clausesOf(sentence)) {
      const std::string low = folded(clause);
      const std::vector<Tok> toks = tokensOf(low);
      if (!ownerDisputes(low, toks, asks)) out += withoutNegatedFigures(low, toks) + "\n";
    }
  }
  return out;
}

bool unsourcedTargetWrite(const std::string& text, const std::string& sourceNumbers, double* figure) {
  const std::vector<double> sources = parseNumbers(sourceNumbers);
  const auto unsourced = [&](const std::string& field) {
    if (field.empty() || field.find_first_not_of("0123456789.") != std::string::npos) return false;
    const double v = std::strtod(field.c_str(), nullptr);
    if (v < 20 || nearSource(v, sources, 0.5)) return false;
    if (figure) *figure = v;
    return true;
  };
  std::string field;
  bool quoted = false, comment = false, atStart = true;
  for (size_t i = 0; i <= text.size(); i++) {
    const char c = i < text.size() ? text[i] : '\n';
    if (atStart && c == '#') comment = true;
    atStart = c == '\n';
    if (c == '"') quoted = !quoted;
    else if (c == ',' && quoted) continue;   // "2,500"
    else if (c == ',' || c == '\n') {
      if (!comment && unsourced(field)) return true;
      field.clear();
      if (c == '\n') comment = false;
    } else if (c != ' ' && c != '\t' && c != '\r') {
      field += c;
    }
  }
  return false;
}

std::string foodGapCorrection(const std::vector<FoodGap>& gaps, const FoodFacts& facts) {
  std::string out = "Your reply states food figures the device's records do not back. Fix them, then give the reply again:\n";
  for (const FoodGap& g : gaps) {
    switch (g.kind) {
      case FoodGapKind::UnsourcedTarget:
        out += "- \"" + g.excerpt + "\": " + (g.implied ? "that implies a daily target of " : "a daily target of ") +
               statedTarget(g) + ", and nothing this turn holds it (no stored target, standing rule, memory, file or "
               "words of the owner; your own earlier replies are not a source). Read it from the owner's plan with "
               "files.read or memory search, or ask them; until then state no target and no \"left\" figure.\n";
        break;
      case FoodGapKind::WrongDayTotal:
        if (!facts.kcalKnown && !g.protein)
          out += "- \"" + g.excerpt + "\": a row of today's ledger cannot be read, so the day's total is not known. Say "
                 "so, and fix the row with files.edit.\n";
        else
          out += "- \"" + g.excerpt + "\": the ledger has today at " + (g.protein ? ledgerProtein(facts, g.truth) : num(g.truth) + " kcal") +
                 ", from its rows. Use the ledger's figure; do not add up a day yourself.\n";
        break;
      case FoodGapKind::ProteinLeftUnknown:
        out += "- \"" + g.excerpt + "\": " + std::to_string(facts.proteinUnknown) +
               " of today's rows have no protein figure, so the real figure is lower by an unknown amount. Do not write "
               "\"you need\" or \"still to go\": say \"at most\" that many grams, or say only what is known and that the "
               "rest is unknown, and ask for the missing figure.\n";
        break;
    }
  }
  return out;
}

std::string foodGapTail(const std::vector<FoodGap>& gaps, const FoodFacts& facts) {
  std::string out;
  bool target = false, total = false, protein = false;
  for (const FoodGap& g : gaps) {
    if (g.kind == FoodGapKind::UnsourcedTarget && !target) {
      target = true;
      // The owner's line names what the reply said. A sum the device did would be a number nobody stated.
      out += g.implied ? "\n\n(Correction: the device has no source for a daily target behind that " + num(g.left) +
                             (g.protein ? " g protein" : " kcal") + " left. Check it against your plan.)"
                       : "\n\n(Correction: the device has no source for a daily target of " + statedTarget(g) +
                             ". Check it against your plan.)";
    } else if (g.kind == FoodGapKind::WrongDayTotal && !total) {
      total = true;
      if (!facts.kcalKnown && !g.protein)
        out += "\n\n(Correction: a row of today's food log cannot be read, so today's total is not known yet.)";
      else if (facts.rows == 0)
        out += "\n\n(Correction: the food log has no rows for today yet.)";
      else
        out += "\n\n(Correction: the food log has today at " + (g.protein ? ledgerProtein(facts, g.truth) : num(g.truth) + " kcal") + ".)";
    } else if (g.kind == FoodGapKind::ProteinLeftUnknown && !protein) {
      protein = true;
      out += "\n\n(Note: a meal logged today has no protein figure, so what is left is not known exactly.)";
    }
  }
  return out;
}

}  // namespace nimbus::orch
