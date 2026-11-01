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
  // Up to the first figure: "you're short by 102 g, more if ..." still states it.
  for (const Tok& t : toks) {
    if (t.number && t.unit != Unit::None) return false;
    if (!t.number && oneOf(t.word, {"if", "would", "could", "might", "may", "suppose", "unless", "should", "will", "i'll",
                                    "once", "want"}))
      return true;
  }
  return false;
}

// A figure the sentence takes back ("the 250 g figure was my error", "not the 250 g").
bool retracted(const std::vector<Tok>& toks) {
  return hasWord(toks, {"not", "no", "never", "isn't", "wasn't", "error", "mistake", "mistaken", "wrong", "incorrect",
                        "invented", "hallucinated", "sorry"});
}

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
  return skipBefore(t) || (!t.number && oneOf(t.word, {"of", "is", "at", "was", "to", "for", "set", "a", "the", "your", "my",
                                                       "as", "it", "that", "plan's"}));
}

struct Stated {
  double value = 0;
  Unit unit = Unit::None;
};

// The figure just before a target word ("2,400 kcal target", "150-160 g band").
std::vector<Stated> figureBefore(const std::vector<Tok>& toks, size_t i, bool macros) {
  std::vector<Stated> out;
  for (size_t back = i; back > 0 && i - back <= 5; back--) {
    const Tok& t = toks[back - 1];
    if (!t.number) {
      if (!skipBefore(t)) break;
      continue;
    }
    if (foodUnit(t, macros)) {
      out.push_back({t.value, t.unit});
      if (back >= 2 && toks[back - 2].number && toks[back - 2].unit == Unit::None)
        out.push_back({toks[back - 2].value, t.unit});
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
    if (t.unit == Unit::None && fwd + 1 < toks.size() && toks[fwd + 1].number && foodUnit(toks[fwd + 1], macros)) {
      out.push_back({t.value, toks[fwd + 1].unit});
      out.push_back({toks[fwd + 1].value, toks[fwd + 1].unit});
    } else if (foodUnit(t, macros)) {
      out.push_back({t.value, t.unit});
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

void targetGaps(const std::string& sentence, const std::vector<Tok>& toks, const std::vector<double>& sources,
                std::vector<FoodGap>& out) {
  const bool macros = macroSentence(toks);
  for (size_t i = 0; i < toks.size(); i++) {
    if (toks[i].number) continue;
    bool word = oneOf(toks[i].word, {"target", "targets", "goal", "goals", "budget", "allowance", "aim", "minimum", "limit", "floor"});
    // "dinner budget is ~1,036 kcal" is what is left for a meal, "remaining to target: 90 kcal" what is left.
    if (word && i > 0 && oneOf(toks[i - 1].word, {"breakfast", "lunch", "dinner", "snack", "meal", "remaining", "to", "left"})) word = false;
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
    if (!toks[f].number && oneOf(toks[f].word, {"target", "goal", "minimum", "budget", "limit", "allowance", "floor"})) return false;
  for (size_t f = i + 1; f < toks.size() && f <= i + 4; f++) {
    const Tok& t = toks[f];
    if (t.number) break;
    if (oneOf(t.word, {"left", "remaining", "spare", "more", "remain", "remains", "go", "short", "below", "under"})) return true;
    if (!oneOf(t.word, {"g", "gm", "gram", "grams", "kcal", "kcals", "cal", "cals", "calorie", "calories", "protein", "of", "to", "still"})) break;
  }
  for (size_t b = i; b > 0 && i - b < 4; b--) {
    const Tok& t = toks[b - 1];
    if (t.number) break;
    if (oneOf(t.word, {"leaves", "leaving", "remaining", "need", "needs", "needed", "short"})) return true;
  }
  return false;
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

// One figure said to be left: a firm need over an unknown row, or a target nothing holds.
void leftGapOf(const LeftCheck& c, const Tok& t, std::vector<FoodGap>& out) {
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
  if (c.taken || (!isProtein && !c.facts.kcalKnown)) return;   // taken back, or the total line covers it
  const double implied = t.value + (isProtein ? c.facts.protein : c.facts.kcal);
  const double tolerance = t.approx ? std::fmax(implied * 0.03, 2.0) : 2.0;
  if (nearSource(implied, c.sources, tolerance)) return;
  g.kind = FoodGapKind::UnsourcedTarget;
  g.implied = true;
  g.stated = implied;
  out.push_back(g);
}

void leftGaps(const LeftCheck& c, const std::vector<Tok>& toks, std::vector<FoodGap>& out) {
  const bool protein = hasWord(toks, {"protein", "floor"});
  for (size_t i = 0; i < toks.size(); i++) {
    const Tok& t = toks[i];
    if (t.number && t.unit != Unit::None && (t.unit != Unit::Gram || protein) && leftAt(toks, i)) leftGapOf(c, t, out);
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

// The kcal figure of a sentence about the whole of today, against the ledger. False = fine.
bool kcalTotalOff(const Tok& t, const FoodFacts& facts, const std::vector<double>& sources) {
  const double tol = t.approx ? std::fmax(facts.kcal * 0.02, 2.0) : 1.0;
  if (facts.kcalKnown && std::fabs(t.value - facts.kcal) <= tol) return false;
  // No rows today: only a figure the owner or a record gave may stand (30 Oct: yesterday's
  // 2,524 carried into a day with nothing logged yet).
  return !(facts.rows == 0 && facts.kcalKnown && nearSource(t.value, sources, 1.0));
}

bool proteinTotalOff(const Tok& t, const FoodFacts& facts) {
  const double tol = t.approx ? std::fmax(facts.protein * 0.05, 1.0) : 0.5;
  return std::fabs(t.value - facts.protein) > tol;
}

// The figures a whole-day sentence states: the first kcal and the first "g protein".
void dayFigures(const std::vector<Tok>& toks, const Tok*& kcal, const Tok*& protein) {
  for (size_t i = 0; i < toks.size(); i++) {
    const Tok& t = toks[i];
    if (!t.number || leftAt(toks, i)) continue;
    if (t.unit == Unit::Kcal && !kcal) kcal = &t;
    if (t.unit == Unit::Gram && !protein && i + 2 < toks.size() && toks[i + 2].word == "protein") protein = &t;
  }
}

void dayTotalGaps(const std::string& sentence, const std::vector<Tok>& toks, const FoodFacts& facts,
                  const std::vector<double>& sources, std::vector<FoodGap>& out) {
  const std::string low = folded(sentence);
  if (!dayTotalCue(low) || mealSentence(low) ||
      hasWord(toks, {"yesterday", "tomorrow", "last", "after", "plus", "then", "before", "drafted", "was"}))
    return;
  const Tok* kcal = nullptr;
  const Tok* protein = nullptr;
  dayFigures(toks, kcal, protein);
  FoodGap g;
  g.kind = FoodGapKind::WrongDayTotal;
  g.excerpt = excerptOf(sentence);
  if (kcal && kcalTotalOff(*kcal, facts, sources)) {
    g.stated = kcal->value;
    g.truth = facts.kcal;
    out.push_back(g);
  } else if (protein && facts.rows > 0 && proteinTotalOff(*protein, facts)) {
    g.protein = true;
    g.stated = protein->value;
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

int monthOf(const std::string& w) {
  static const char* const kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
  if (w.size() < 3) return 0;
  for (int i = 0; i < 12; i++)
    if (w.compare(0, 3, kMonths[i]) == 0) return i + 1;
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

// Every date in a sentence, as days since 1970: 2026-10-14, 14 Oct, Oct 14, 14 October 2026.
std::vector<int> datesIn(const std::vector<Tok>& toks, const std::string& low, int todayYear) {
  std::vector<int> out;
  isoDatesIn(low, out);
  for (size_t i = 0; i < toks.size(); i++) {
    const int month = toks[i].number ? 0 : monthOf(toks[i].word);
    if (!month || toks[i].word.size() > 9 || (toks[i].word == "may" && !(i > 0 && toks[i - 1].number))) continue;
    if (const Tok* day = dayOfMonthAt(toks, i)) out.push_back(daysOf(todayYear, month, (int)day->value));
  }
  return out;
}

}  // namespace

std::string staleStatusIn(const std::string& reply, const std::string& today, int maxAgeDays, int* ageDays) {
  if (today.size() < 10) return std::string();
  const int year = std::atoi(today.c_str());
  const int now = daysOf(year, std::atoi(today.c_str() + 5), std::atoi(today.c_str() + 8));
  for (const std::string& sentence : sentencesOf(reply)) {
    const std::string low = folded(sentence);
    const std::vector<Tok> toks = tokensOf(low);
    if (low.find('?') != std::string::npos) continue;
    const bool status = hasWord(toks, {"streak", "consecutive", "currently", "latest", "running", "still", "current"}) ||
                        low.find("in a row") != std::string::npos || low.find("so far") != std::string::npos ||
                        low.find("as of") != std::string::npos || low.find("you've been") != std::string::npos;
    if (!status) continue;
    for (int d : datesIn(toks, low, year)) {
      if (d > now) d = d - 365;   // "14 Dec" read in January is last year's
      if (now - d <= maxAgeDays) continue;
      if (ageDays) *ageDays = now - d;
      return excerptOf(sentence);
    }
  }
  return std::string();
}

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
    if (out.size() == before) leftGaps(LeftCheck{sentence, facts, sources, taken}, toks, out);
    dayTotalGaps(sentence, toks, facts, sources, out);
  }
  return out;
}

std::string ownerStatementsOf(const std::string& text) {
  std::string out;
  for (const std::string& sentence : sentencesOf(text)) {
    const std::string low = folded(sentence);
    const std::vector<Tok> toks = tokensOf(low);
    if (low.find('?') != std::string::npos || retracted(toks) || hasWord(toks, {"why", "where", "how", "hallucinate", "hallucinating", "invent"}))
      continue;
    out += sentence + "\n";
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
      out += "\n\n(Correction: the device has no source for a daily target of " + statedTarget(g) +
             ". Check it against your plan.)";
    } else if (g.kind == FoodGapKind::WrongDayTotal && !total) {
      total = true;
      if (!facts.kcalKnown && !g.protein)
        out += "\n\n(Correction: a row of today's food log cannot be read, so today's total is not known yet.)";
      else if (facts.rows == 0)
        out += "\n\n(Correction: the food log has no rows for today yet.)";
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
