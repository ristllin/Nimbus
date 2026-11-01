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
      p = readNumber(sentence, p, t.value);
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
  return hasWord(toks, {"if", "would", "could", "might", "may", "suppose", "unless", "should", "will", "i'll", "once", "want"});
}

// A figure the sentence takes back ("the 250 g figure was my error", "not the 250 g").
bool retracted(const std::vector<Tok>& toks) {
  return hasWord(toks, {"not", "no", "never", "isn't", "wasn't", "error", "mistake", "mistaken", "wrong", "incorrect",
                        "invented", "hallucinated", "sorry"});
}

bool macroSentence(const std::vector<Tok>& toks) {
  return hasWord(toks, {"protein", "fat", "fibre", "fiber", "carb", "carbs", "macros"});
}

// Is this number a daily figure the sentence can be read about?
bool foodUnit(const Tok& t, bool macros) { return t.unit == Unit::Kcal || (t.unit == Unit::Gram && macros); }

bool skipBetween(const Tok& t) {
  return t.number ? false
                  : oneOf(t.word, {"g", "gm", "gram", "grams", "kcal", "kcals", "cal", "cals", "calorie", "calories",
                                   "daily", "day", "per", "protein", "fat", "fibre", "fiber", "carb", "carbs", "plan's",
                                   "of", "is", "at", "was", "to", "for", "set", "a", "the", "your", "my", "as", "minimum",
                                   "stated", "current", "working", "original", "it", "that"});
}

struct Stated {
  double value = 0;
  Unit unit = Unit::None;
};

// The number a target word at `i` names: before it ("2,400 kcal target"), else after it
// ("protein target: 150 g", "target of 2,400 kcal"). Ranges keep both ends.
std::vector<Stated> figuresOfTarget(const std::vector<Tok>& toks, size_t i, bool macros) {
  std::vector<Stated> out;
  for (size_t back = i; back > 0 && i - back <= 5; back--) {
    const Tok& t = toks[back - 1];
    if (t.number) {
      if (!foodUnit(t, macros)) break;
      out.push_back({t.value, t.unit});
      if (back >= 2 && toks[back - 2].number && toks[back - 2].unit == Unit::None)
        out.push_back({toks[back - 2].value, t.unit});
      return out;
    }
    if (!skipBetween(t)) break;
  }
  for (size_t fwd = i + 1; fwd < toks.size() && fwd - i <= 5; fwd++) {
    const Tok& t = toks[fwd];
    if (t.number) {
      if (t.unit == Unit::None && fwd + 1 < toks.size() && toks[fwd + 1].number && foodUnit(toks[fwd + 1], macros)) {
        out.push_back({t.value, toks[fwd + 1].unit});
        out.push_back({toks[fwd + 1].value, toks[fwd + 1].unit});
      } else if (foodUnit(t, macros)) {
        out.push_back({t.value, t.unit});
      }
      return out;
    }
    if (!skipBetween(t)) break;
  }
  return out;
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

void targetGaps(const std::string& sentence, const std::vector<Tok>& toks, const std::vector<double>& sources,
                std::vector<FoodGap>& out) {
  const bool macros = macroSentence(toks);
  for (size_t i = 0; i < toks.size(); i++) {
    if (toks[i].number) continue;
    const bool word = oneOf(toks[i].word, {"target", "targets", "goal", "goals", "budget", "allowance", "aim", "minimum", "limit"});
    // "we adjusted it to 2,500", "finalised at 2,500 kcal": a target by another name.
    const bool settled = i + 1 < toks.size() && oneOf(toks[i].word, {"adjusted", "raised", "lowered", "changed"});
    if (!word && !settled) continue;
    for (const Stated& f : figuresOfTarget(toks, i, macros)) {
      if (nearSource(f.value, sources, f.unit == Unit::Kcal ? 0.5 : 0.05)) continue;
      FoodGap g;
      g.kind = FoodGapKind::UnsourcedTarget;
      g.excerpt = excerptOf(sentence);
      g.stated = f.value;
      g.protein = f.unit == Unit::Gram;
      out.push_back(g);
      return;   // one per sentence
    }
  }
}

// A figure the sentence says is left of the day: "364 kcal left", "leaves 31 kcal", "need ~102 g more".
bool leftAt(const std::vector<Tok>& toks, size_t i) {
  // "Remaining for 2,400 kcal target": the target itself, not what is left of it.
  for (size_t f = i + 1; f < toks.size() && f <= i + 3; f++)
    if (!toks[f].number && oneOf(toks[f].word, {"target", "goal", "minimum", "budget", "limit", "allowance"})) return false;
  for (size_t f = i + 1; f < toks.size() && f <= i + 3; f++) {
    const Tok& t = toks[f];
    if (t.number) break;
    if (oneOf(t.word, {"left", "remaining", "spare", "more", "remain", "remains"})) return true;
    if (!oneOf(t.word, {"g", "gm", "gram", "grams", "kcal", "kcals", "cal", "cals", "calorie", "calories", "protein", "of", "to"})) break;
  }
  for (size_t b = i; b > 0 && i - b < 4; b--) {
    const Tok& t = toks[b - 1];
    if (t.number) break;
    if (oneOf(t.word, {"leaves", "leaving", "remaining", "need", "needs", "needed"})) return true;
  }
  return false;
}

void leftGaps(const std::string& sentence, const std::vector<Tok>& toks, const FoodFacts& facts,
              const std::vector<double>& sources, bool taken, std::vector<FoodGap>& out) {
  const bool protein = hasWord(toks, {"protein"});
  const std::string low = folded(sentence);
  for (size_t i = 0; i < toks.size(); i++) {
    const Tok& t = toks[i];
    if (!t.number || t.unit == Unit::None || (t.unit == Unit::Gram && !protein) || !leftAt(toks, i)) continue;
    const bool isProtein = t.unit == Unit::Gram;
    FoodGap g;
    g.excerpt = excerptOf(sentence);
    g.protein = isProtein;
    if (isProtein && facts.proteinUnknown > 0 && low.find("at most") == std::string::npos &&
        low.find("up to") == std::string::npos && low.find("unknown") == std::string::npos &&
        low.find("not known") == std::string::npos) {
      g.kind = FoodGapKind::ProteinLeftUnknown;
      g.stated = t.value;
      out.push_back(g);
      continue;
    }
    if (taken || (!isProtein && !facts.kcalKnown)) continue;   // taken back, or the total line covers it
    if (isProtein && facts.proteinUnknown > 0) continue;
    const double implied = t.value + (isProtein ? facts.protein : facts.kcal);
    const double tolerance = t.approx ? std::fmax(implied * 0.03, 2.0) : 2.0;
    if (nearSource(implied, sources, tolerance)) continue;
    g.kind = FoodGapKind::UnsourcedTarget;
    g.implied = true;
    g.stated = implied;
    out.push_back(g);
  }
}

bool mealSentence(const std::string& low) {
  return low.find("meal total") != std::string::npos || low.find("estimate") != std::string::npos;
}

// A sentence about the whole of today.
bool dayTotalCue(const std::string& low) {
  for (const char* cue : {"today's total", "total today", "total for today", "totals for today", "today's log", "today now",
                          "today is ", "today (", "today:", "today so far", "so far today", "day so far", "day total",
                          "day's total", "logged today", "total so far", "totals so far", "today you", "today stands"})
    if (low.find(cue) != std::string::npos) return true;
  return false;
}

void dayTotalGaps(const std::string& sentence, const std::vector<Tok>& toks, const FoodFacts& facts,
                  std::vector<FoodGap>& out) {
  const std::string low = folded(sentence);
  if (!dayTotalCue(low) || mealSentence(low) ||
      hasWord(toks, {"yesterday", "tomorrow", "last", "after", "plus", "then", "before", "drafted", "was"}))
    return;
  bool sawKcal = false, sawProtein = false;
  for (size_t i = 0; i < toks.size(); i++) {
    const Tok& t = toks[i];
    if (!t.number) continue;
    if (t.unit == Unit::Kcal && !sawKcal) {
      sawKcal = true;
      if (i + 1 < toks.size() && leftAt(toks, i)) continue;
      const double tol = t.approx ? std::fmax(facts.kcal * 0.02, 2.0) : 1.0;
      if (facts.kcalKnown && std::fabs(t.value - facts.kcal) <= tol) continue;
      FoodGap g;
      g.kind = FoodGapKind::WrongDayTotal;
      g.excerpt = excerptOf(sentence);
      g.stated = t.value;
      g.truth = facts.kcal;
      out.push_back(g);
      return;
    }
    const bool proteinNext = i + 2 < toks.size() && toks[i + 2].word == "protein";
    if (t.unit == Unit::Gram && proteinNext && !sawProtein) {
      sawProtein = true;
      if (leftAt(toks, i)) continue;
      const double tol = t.approx ? std::fmax(facts.protein * 0.05, 1.0) : 0.5;
      if (std::fabs(t.value - facts.protein) <= tol) continue;
      FoodGap g;
      g.kind = FoodGapKind::WrongDayTotal;
      g.protein = true;
      g.excerpt = excerptOf(sentence);
      g.stated = t.value;
      g.truth = facts.protein;
      out.push_back(g);
      return;
    }
  }
}

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.10g", std::round(v * 10) / 10);
  return buf;
}

std::string statedTarget(const FoodGap& g) { return num(g.stated) + (g.protein ? " g protein" : " kcal"); }

}  // namespace

std::vector<FoodGap> unsourcedFoodTargets(const std::string& text, const std::string& sourceNumbers) {
  std::vector<FoodGap> out;
  const std::vector<double> sources = parseNumbers(sourceNumbers);
  for (const std::string& sentence : sentencesOf(text)) {
    const std::string low = folded(sentence);
    const std::vector<Tok> toks = tokensOf(low);
    if (!hypothetical(low, toks) && !retracted(toks)) targetGaps(sentence, toks, sources, out);
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
    const bool taken = retracted(toks);
    const size_t before = out.size();
    if (!taken) targetGaps(sentence, toks, sources, out);
    if (!facts.have) continue;
    if (out.size() == before) leftGaps(sentence, toks, facts, sources, taken, out);
    dayTotalGaps(sentence, toks, facts, out);
  }
  return out;
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
          out += "- \"" + g.excerpt + "\": the ledger has today at " + num(g.truth) + (g.protein ? " g protein" : " kcal") +
                 ", from its rows. Use the ledger's figure; do not add up a day yourself.\n";
        break;
      case FoodGapKind::ProteinLeftUnknown:
        out += "- \"" + g.excerpt + "\": " + std::to_string(facts.proteinUnknown) +
               " of today's rows have no protein figure, so what is left is at most a number, not a need. Say what is "
               "known and that the rest is unknown, or ask for the missing figure.\n";
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
      out += "\n\n(Correction: the device has no source for a daily target of " + statedTarget(g) +
             ". Check it against your plan.)";
    } else if (g.kind == FoodGapKind::WrongDayTotal && !total) {
      total = true;
      if (!facts.kcalKnown && !g.protein)
        out += "\n\n(Correction: a row of today's food log cannot be read, so today's total is not known yet.)";
      else
        out += "\n\n(Correction: the food log has today at " + num(g.truth) + (g.protein ? " g protein.)" : " kcal.)");
    } else if (g.kind == FoodGapKind::ProteinLeftUnknown && !protein) {
      protein = true;
      out += "\n\n(Note: a meal logged today has no protein figure, so what is left is not known exactly.)";
    }
  }
  return out;
}

}  // namespace nimbus::orch
