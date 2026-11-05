#include "nimbus/orch/food_label.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The deterministic label check (food_label.h). Pure string and arithmetic work;
// no I/O, no allocation beyond the working set the caller's vectors use.
namespace nimbus {
namespace orch {

namespace {

// One token of the scan: a run of letters/digits, with '.' kept inside a number,
// '&' kept inside a brand token ("M&S"), and "565/135" kept as one kJ/kcal pair
// (`pair`, the kcal value in `second`). `num` is the token's value when it is a
// plain number.
struct Tok {
  std::string w;
  bool isNum = false;
  double num = 0;
  bool pair = false;    // "565/135": kJ/kcal, the kcal value is `second`
  double second = 0;
};

bool numChar(char c) { return std::isdigit((unsigned char)c) || c == '.'; }

// The token starting at p is a plain number (digits, at most one dot, a digit).
// A second dot ends the number before it ("3.14.1" reads 3.14, then ".1"): the
// scan always advances, so no reply text can stall it.
bool scanNum(const std::string& text, size_t p, size_t& e, double& v) {
  e = p;
  bool dot = false, digit = false;
  while (e < text.size() && numChar(text[e])) {
    if (text[e] == '.') { if (dot) break; dot = true; }
    else digit = true;
    ++e;
  }
  if (!digit || e == p) return false;
  v = std::atof(text.substr(p, e - p).c_str());
  return true;
}

// A number token at p, possibly a kJ/kcal pair ("565/135"), possibly with a
// trailing unit letter ("388g" -> "388" "g"). Pushes the token(s), advances p.
void scanNumberToken(const std::string& text, size_t& p, std::vector<Tok>& out) {
  size_t e = p;
  double v = 0;
  if (!scanNum(text, p, e, v)) return;
  Tok t;
  t.isNum = true;
  t.num = v;
  t.w = text.substr(p, e - p);
  // "565/135": the pair is one token; the kcal value is the second number.
  if (e < text.size() && text[e] == '/' && e + 1 < text.size() && std::isdigit((unsigned char)text[e + 1])) {
    size_t e2 = e + 1;
    double v2 = 0;
    if (scanNum(text, e + 1, e2, v2)) {
      t.pair = true;
      t.second = v2;
      e = e2;
    }
  }
  out.push_back(std::move(t));
  p = e;
  // "388g" / "5.7g" / "100g": the trailing unit letter is its own token.
  if (p < text.size() && text[p] == 'g' &&
      (p + 1 >= text.size() || !std::isalnum((unsigned char)text[p + 1]))) {
    Tok g;
    g.w = "g";
    out.push_back(std::move(g));
    ++p;
  }
}

// A word token at p (letters/digits/'.', '&' inside when alnum follows).
void scanWordToken(const std::string& text, size_t p, std::vector<Tok>& out, size_t& e) {
  e = p;
  while (e < text.size() && (std::isalnum((unsigned char)text[e]) || text[e] == '.' ||
                              (text[e] == '&' && e + 1 < text.size() && std::isalnum((unsigned char)text[e + 1]))))
    ++e;
  if (e == p) ++e;   // a lone '&' ("Fish & chips") is its own token; the scan never stalls
  Tok t;
  t.w = text.substr(p, e - p);
  std::string low;
  for (char ch : t.w) low += (char)std::tolower((unsigned char)ch);
  t.w = low;
  out.push_back(std::move(t));
}

std::vector<Tok> tokenize(const std::string& text) {
  std::vector<Tok> out;
  size_t p = 0;
  while (p < text.size()) {
    const char c = text[p];
    if (!std::isalnum((unsigned char)c) && c != '.' && c != '&') {
      ++p;
      continue;
    }
    if (std::isdigit((unsigned char)c) || (c == '.' && p + 1 < text.size() && std::isdigit((unsigned char)text[p + 1]))) {
      scanNumberToken(text, p, out);
      continue;
    }
    size_t e = p;
    scanWordToken(text, p, out, e);
    p = e;
  }
  return out;
}

bool isWord(const Tok& t, const char* w) { return !t.isNum && t.w == w; }

// The columns of a label table: "per 100 g", "per serving", "per pack",
// "whole pack" open a column. A line that opens none inherits the columns of the
// last line that did (a table's header row), so a row line's values map to the
// header's columns in order: "Fat 5.0g 12g" under "Per 100g Per serving" is
// per-100 g 5.0, per serving 12.
enum class Col { None, Per100, Serving };

// The column markers of one line, with their token positions: "per 100 g",
// "per serving", "per pack", "whole pack" open a column from where they stand.
std::vector<std::pair<size_t, Col>> markersOf(const std::vector<Tok>& ts) {
  std::vector<std::pair<size_t, Col>> out;
  for (size_t p = 0; p + 1 < ts.size(); ++p) {
    if (isWord(ts[p], "per") && ts[p + 1].isNum && !ts[p + 1].pair && ts[p + 1].num == 100)
      out.emplace_back(p, Col::Per100);
    else if (isWord(ts[p], "per") && (isWord(ts[p + 1], "serving") || isWord(ts[p + 1], "pack")))
      out.emplace_back(p, Col::Serving);
    else if (isWord(ts[p], "whole") && isWord(ts[p + 1], "pack"))
      out.emplace_back(p, Col::Serving);
  }
  return out;
}

// What a number token states, from the words around it: "54.9 g protein",
// "protein 14.1 g", "fat 21.0", "611 kcal", "565/135" (a kJ/kcal pair near an
// energy word reads as the kcal value, 135), "net 388 g" / "388 g net" /
// "131g e" (the net weight).
enum class Fig { None, Kcal, Protein, Carb, Fat, Net };

// The words around token i (next two, previous three), for classify's lookups.
struct WordCtx {
  const std::vector<Tok>* ts;
  size_t i;
  const std::string* word(long k) const {
    const long j = (long)i + k;
    if (j < 0 || j >= (long)ts->size() || (*ts)[j].isNum) return nullptr;
    return &(*ts)[j].w;
  }
  bool has(const char* w) const {
    for (long k : {-3, -2, -1, 1, 2})
      if (const std::string* p = word(k)) if (*p == w) return true;
    return false;
  }
};

// A kJ/kcal pair counts as kcal only beside an energy word ("565/135" under
// "Energy kJ/kcal"); "[image 1/2]" is not an energy pair.
Fig pairFig(const Tok& t, const WordCtx& c, double& val) {
  if (!t.pair) return Fig::None;
  if (c.has("kcal") || c.has("kj") || c.has("energy")) {
    val = t.second;
    return Fig::Kcal;
  }
  return Fig::None;
}

// "kcal 235" is kcal, but "235 kcal, 41.0 g protein" is not: a gram-suffixed
// figure after "kcal" belongs to the macro the gram introduces.
Fig kcalFig(const WordCtx& c, double& val, double num) {
  const std::string* n1 = c.word(1);
  const std::string* p1 = c.word(-1);
  if (n1 && *n1 == "kcal") { val = num; return Fig::Kcal; }
  if (p1 && *p1 == "kcal" && !(n1 && *n1 == "g")) { val = num; return Fig::Kcal; }
  return Fig::None;
}

// The macro a word names, Fig::None when it names none. "fat" is refused when it
// belongs to a saturates line ("of which saturates 2.2 g", "saturated fat 3.0").
Fig macroWordFig(const std::string& w, bool nearSaturates, const std::string* before) {
  if (w == "protein") return Fig::Protein;
  if (w == "carb" || w == "carbohydrate" || w == "carbs") return Fig::Carb;
  if (w == "fat" && !nearSaturates && !(before && (*before == "saturates" || *before == "saturated")))
    return Fig::Fat;
  return Fig::None;
}

Fig macroFig(const WordCtx& c, double& val, double num) {
  val = num;
  // The NEAREST macro word wins, not a fixed priority: in a comma list
  // ("54.9 g protein, fat 21.0") the fat figure sits two tokens after "protein",
  // and a priority order would classify it as protein and drop it (first figure
  // wins), leaving the fat unset so Atwater never runs.
  Fig best = Fig::None;
  long bestDist = 0;
  const std::string* p1 = c.word(-1);
  const std::string* p2 = c.word(-2);
  const bool nearSaturates = (p1 && (*p1 == "saturates" || *p1 == "saturated")) ||
                             (p2 && (*p2 == "saturates" || *p2 == "saturated"));
  for (long k : {-3, -2, -1, 1, 2}) {
    const std::string* w = c.word(k);
    if (!w) continue;
    const Fig f = macroWordFig(*w, nearSaturates, c.word(k - 1));
    if (f == Fig::None) continue;
    const long d = k < 0 ? -k : k;
    if (best == Fig::None || d < bestDist) { bestDist = d; best = f; }
  }
  return best;
}

// Net weight: a gram figure with "net"/"weight"/"wt"/"full" nearby ("net 388 g",
// "388 g net", "full 131 g pack"), or the "388g e" form (gram figure, "g", then
// the estimated sign "e").
Fig netFig(const WordCtx& c, double& val, double num) {
  const std::string* n1 = c.word(1);
  const std::string* n2 = c.word(2);
  const std::string* p1 = c.word(-1);
  const bool gram = (n1 && *n1 == "g") || (p1 && *p1 == "g");
  if (!gram) return Fig::None;
  if (c.has("net") || c.has("weight") || c.has("wt") || c.has("full") || (n2 && *n2 == "e")) {
    val = num;
    return Fig::Net;
  }
  return Fig::None;
}

Fig classify(const std::vector<Tok>& ts, size_t i, double& val) {
  const Tok& t = ts[i];
  if (!t.isNum) return Fig::None;
  const WordCtx c{&ts, i};
  if (const Fig f = pairFig(t, c, val); f != Fig::None) return f;
  val = t.num;
  // "565 135 kcal" (spaced): the first number of a pair whose second sits before
  // "kcal" is kJ.
  if (i + 2 < ts.size() && ts[i + 1].isNum && !ts[i + 2].isNum && ts[i + 2].w == "kcal") return Fig::None;
  if (const Fig f = kcalFig(c, val, t.num); f != Fig::None) return f;
  if (const Fig f = macroFig(c, val, t.num); f != Fig::None) return f;
  return netFig(c, val, t.num);
}

// First figure wins per field: a label states each figure once, and a later stray
// number (another row, a prompt line) must not overwrite the label's own.
void setFig(LabelColumn& c, Fig f, double v) {
  switch (f) {
    case Fig::Kcal: if (!c.hasKcal) { c.hasKcal = true; c.kcal = v; } break;
    case Fig::Protein: if (!c.hasProtein) { c.hasProtein = true; c.protein = v; } break;
    case Fig::Carb: if (!c.hasCarb) { c.hasCarb = true; c.carb = v; } break;
    case Fig::Fat: if (!c.hasFat) { c.hasFat = true; c.fat = v; } break;
    default: break;
  }
  c.has = c.has || f == Fig::Kcal || f == Fig::Protein || f == Fig::Carb || f == Fig::Fat;
}

bool mentionsLabel(const std::string& text) {
  std::string low;
  for (char c : text) low += (char)std::tolower((unsigned char)c);
  return low.find("label") != std::string::npos || low.find("per 100") != std::string::npos ||
         low.find("per serving") != std::string::npos || low.find("nutrition") != std::string::npos;
}

bool near(double a, double b, double rel, double absTol) {
  const double tol = std::max(rel * std::max(std::fabs(a), std::fabs(b)), absTol);
  return std::fabs(a - b) <= tol;
}

// Atwater needs all three macros: with one missing the sum is not the label's
// arithmetic, and a doubt from it would be a false one.
void atwaterDoubt(const LabelColumn& c, const char* colName, std::vector<LabelDoubt>& out) {
  if (!c.hasKcal || !c.hasProtein || !c.hasCarb || !c.hasFat) return;
  const double computed = 4 * c.protein + 4 * c.carb + 9 * c.fat;
  if (!near(c.kcal, computed, 0.12, 10.0)) {
    LabelDoubt d;
    d.figure = std::string("kcal ") + colName;
    d.stated = c.kcal;
    d.computed = computed;
    d.why = "4P+4C+9F";
    out.push_back(std::move(d));
  }
}

// One kcal/protein pair of the cross-column check: per-100 g x net weight against
// per-serving. kcal and protein only: the ledger stores those two, and the
// per-100 g fat/carb are rounded to whole grams on real labels, which the
// net-weight scaling amplifies past any fair tolerance (5 g x 3.88 = 19.4 vs a
// stated 22.3 g).
// One kcal/protein pair of the cross-column check.
struct CrossPair {
  const char* name;
  bool hasPer100;
  bool hasServing;
  double per100;
  double serving;
  double absTol;
};

void crossPairDoubt(const LabelRead& r, const CrossPair& p, std::vector<LabelDoubt>& out) {
  if (!p.hasPer100 || !p.hasServing) return;
  const double computed = p.per100 * (r.netG / 100.0);
  if (!near(p.serving, computed, 0.12, p.absTol)) {
    LabelDoubt d;
    d.figure = std::string(p.name) + " per serving vs per 100 g";
    d.stated = p.serving;
    d.computed = computed;
    d.why = "per-100 g x net weight";
    out.push_back(std::move(d));
  }
}

void crossDoubt(const LabelRead& r, std::vector<LabelDoubt>& out) {
  // The per-100 g column must carry kcal and protein both: a stray kcal alone (a
  // prompt line that says "per-100 g values") is not a label read.
  if (!r.hasPer100 || !r.per100.hasKcal || !r.per100.hasProtein || !r.hasPerServing || !r.netKnown || r.netG <= 0)
    return;
  // kcal and protein only: the ledger stores those two, and the per-100 g
  // fat/carb are rounded to whole grams on real labels, which the net-weight
  // scaling amplifies past any fair tolerance (5 g x 3.88 = 19.4 vs 22.3 g).
  crossPairDoubt(r, {"kcal", r.per100.hasKcal, r.perServing.hasKcal, r.per100.kcal, r.perServing.kcal, 10.0}, out);
  crossPairDoubt(r, {"protein", r.per100.hasProtein, r.perServing.hasProtein, r.per100.protein, r.perServing.protein, 1.0}, out);
}

// Fill the figures of one column the row opened but did not state, from the
// source read's same column.
void mergeColumn(LabelColumn& dst, const LabelColumn& src) {
  if (!dst.hasFat && src.hasFat) { dst.fat = src.fat; dst.hasFat = true; }
  if (!dst.hasCarb && src.hasCarb) { dst.carb = src.carb; dst.hasCarb = true; }
  if (!dst.hasProtein && src.hasProtein) { dst.protein = src.protein; dst.hasProtein = true; }
  if (!dst.hasKcal && src.hasKcal) { dst.kcal = src.kcal; dst.hasKcal = true; }
  dst.has = true;
}

// The row's figures win where both state them (that is what is being stored); the
// turn's label read fills the columns the row does not carry.
LabelRead mergeReads(const LabelRead& row, const LabelRead& src) {
  LabelRead m = row;
  if (!m.hasPer100 && src.hasPer100) { m.per100 = src.per100; m.hasPer100 = true; }
  if (!m.hasPerServing && src.hasPerServing) { m.perServing = src.perServing; m.hasPerServing = true; }
  if (!m.netKnown && src.netKnown) { m.netKnown = true; m.netG = src.netG; }
  if (m.hasPerServing && src.hasPerServing) mergeColumn(m.perServing, src.perServing);
  if (m.hasPer100 && src.hasPer100) mergeColumn(m.per100, src.per100);
  return m;
}

std::string fmt(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}

// The fields a read states, for the two-read comparison.
struct Field { const char* name; bool has; double v; };
std::vector<Field> fieldsOf(const LabelRead& r) {
  std::vector<Field> out;
  auto add = [&out](const char* n, bool h, double v) { out.push_back({n, h, v}); };
  add("kcal per 100 g", r.hasPer100 && r.per100.hasKcal, r.per100.kcal);
  add("protein per 100 g", r.hasPer100 && r.per100.hasProtein, r.per100.protein);
  add("carb per 100 g", r.hasPer100 && r.per100.hasCarb, r.per100.carb);
  add("fat per 100 g", r.hasPer100 && r.per100.hasFat, r.per100.fat);
  add("kcal per serving", r.hasPerServing && r.perServing.hasKcal, r.perServing.kcal);
  add("protein per serving", r.hasPerServing && r.perServing.hasProtein, r.perServing.protein);
  add("carb per serving", r.hasPerServing && r.perServing.hasCarb, r.perServing.carb);
  add("fat per serving", r.hasPerServing && r.perServing.hasFat, r.perServing.fat);
  add("net weight", r.netKnown, r.netG);
  return out;
}

// The column a figure on a line belongs to: the last marker standing before it
// (the line's first column before the first marker), or the k-th inherited
// column on a line that opens none.
Col columnOf(const std::vector<std::pair<size_t, Col>>& marks, const std::vector<Col>& inherited,
             size_t i, size_t k) {
  if (!marks.empty()) {
    Col c = marks.front().second;
    for (const auto& m : marks)
      if (m.first < i) c = m.second;
    return c;
  }
  return k < inherited.size() ? inherited[k] : inherited.back();
}

// One line of a label transcription into the read being built.
void parseLine(const std::string& line, LabelRead& r, std::vector<Col>& inherited) {
  const std::vector<Tok> ts = tokenize(line);
  const std::vector<std::pair<size_t, Col>> marks = markersOf(ts);
  if (!marks.empty()) {
    inherited.clear();
    for (const auto& m : marks) inherited.push_back(m.second);
  }
  if (inherited.empty()) inherited.push_back(Col::Serving);   // no marker anywhere: the row's own figures
  // The "100" of a "per 100 g" marker opens the column; it is not itself a figure
  // ("(per 100 g: 179 kcal)" must not read 100 as a protein).
  std::vector<bool> markerNum(ts.size(), false);
  for (const auto& m : marks)
    if (m.second == Col::Per100 && m.first + 1 < ts.size()) markerNum[m.first + 1] = true;
  size_t k = 0;   // a line with no markers of its own: the k-th figure maps to inherited[min(k, size-1)]
  for (size_t i = 0; i < ts.size(); ++i) {
    if (!ts[i].isNum || markerNum[i]) continue;
    double val = 0;
    const Fig f = classify(ts, i, val);
    if (f == Fig::None) continue;
    if (f == Fig::Net) {
      if (!r.netKnown || val > r.netG) { r.netKnown = true; r.netG = val; }
      continue;
    }
    const Col c = columnOf(marks, inherited, i, k);
    ++k;
    if (c == Col::Per100) { setFig(r.per100, f, val); r.hasPer100 = true; }
    else { setFig(r.perServing, f, val); r.hasPerServing = true; }
  }
}

}  // namespace

LabelRead parseLabelRead(const std::string& text) {
  LabelRead r;
  std::vector<Col> inherited;   // the columns of the last line that opened any (a table's header)
  for (size_t b = 0, e; b <= text.size(); b = e + 1) {
    e = text.find('\n', b);
    if (e == std::string::npos) e = text.size();
    const std::string line = text.substr(b, e - b);
    if (e == b && e != text.size()) continue;
    parseLine(line, r, inherited);
    if (e == text.size()) break;
  }
  return r;
}

std::vector<LabelDoubt> checkLabelRead(const LabelRead& r) {
  std::vector<LabelDoubt> out;
  if (r.hasPer100) atwaterDoubt(r.per100, "per 100 g", out);
  if (r.hasPerServing) atwaterDoubt(r.perServing, "per serving", out);
  crossDoubt(r, out);
  return out;
}

// Does the row's own text state the figure a doubt names? A doubt about a figure
// only the turn's sources carry is not the row's to refuse.
bool rowStates(const LabelRead& row, const LabelDoubt& d) {
  auto colHas = [](const LabelColumn& c, const std::string& name) {
    if (name == "kcal") return c.hasKcal;
    if (name == "protein") return c.hasProtein;
    if (name == "fat") return c.hasFat;
    if (name == "carb") return c.hasCarb;
    return false;
  };
  const std::string& f = d.figure;
  const std::string vs = " per serving vs per 100 g";
  if (f.size() > vs.size() && f.compare(f.size() - vs.size(), vs.size(), vs) == 0)
    return row.hasPerServing && colHas(row.perServing, f.substr(0, f.size() - vs.size()));
  const std::string ps = " per serving";
  if (f.size() > ps.size() && f.compare(f.size() - ps.size(), ps.size(), ps) == 0)
    return row.hasPerServing && colHas(row.perServing, f.substr(0, f.size() - ps.size()));
  const std::string p1 = " per 100 g";
  if (f.size() > p1.size() && f.compare(f.size() - p1.size(), p1.size(), p1) == 0)
    return row.hasPer100 && colHas(row.per100, f.substr(0, f.size() - p1.size()));
  return true;
}

// A block of a turn's sources starts at a blank line or a bracketed tag like
// "[image 1/2 ref=...]" or "[TODAY'S FOOD LEDGER]": a turn with several photos
// must not merge their figures into one read.
bool startsBlock(const std::string& line) {
  return line.empty() || (line[0] == '[' && line.size() > 1 && std::isalpha((unsigned char)line[1]));
}

// A source read counts when it carries a label's figures or a net weight.
bool countsAsRead(const LabelRead& r) {
  return (r.hasPer100 && r.per100.hasKcal && r.per100.hasProtein) ||
         (r.hasPerServing && r.perServing.hasKcal && r.perServing.hasProtein) || r.netKnown;
}

// The blocks of a turn's sources, one per blank line or bracketed tag: each holds
// one photo's read (or one plain message), never merged with another's figures.
std::vector<std::string> sourceBlocks(const std::string& turnSources) {
  std::vector<std::string> out;
  std::string block;
  auto flush = [&]() {
    if (!block.empty()) { out.push_back(block); block.clear(); }
  };
  for (size_t b = 0, e; b <= turnSources.size(); b = e + 1) {
    e = turnSources.find('\n', b);
    if (e == std::string::npos) e = turnSources.size();
    const std::string line = turnSources.substr(b, e - b);
    if (startsBlock(line)) flush();
    block += line;
    block += '\n';
    if (e == turnSources.size()) break;
  }
  flush();
  return out;
}

// The label reads a turn's sources hold, one per block.
std::vector<LabelRead> sourceReads(const std::string& turnSources) {
  std::vector<LabelRead> out;
  for (const std::string& block : sourceBlocks(turnSources)) {
    const LabelRead r = parseLabelRead(block);
    if (countsAsRead(r)) out.push_back(r);
  }
  return out;
}

// The source read that belongs to this row: the one whose net weight matches the
// row's own ("full 131 g pack"), else the fullest read.
const LabelRead* pickSource(const std::vector<LabelRead>& srcs, const LabelRead& row) {
  if (row.netKnown) {
    for (const LabelRead& s : srcs)
      if (s.netKnown && std::fabs(s.netG - row.netG) <= 1.0) return &s;
  }
  const LabelRead* best = nullptr;
  size_t bestScore = 0;
  for (const LabelRead& s : srcs) {
    size_t score = 0;
    if (s.hasPer100) score += 2;
    if (s.hasPerServing) score += 2;
    if (s.netKnown) ++score;
    if (score > bestScore) { bestScore = score; best = &s; }
  }
  return best;
}

// One row line of a files.append against the turn's source reads: its label
// figures are checked (merged with the matching source read) and the doubts the
// row itself states are collected.
void gateLine(const std::string& line, const std::vector<LabelRead>& srcs, std::vector<LabelDoubt>& doubts) {
  std::string low;
  for (char c : line) low += (char)std::tolower((unsigned char)c);
  if (low.find("[unverified") != std::string::npos) return;   // the re-read-then-store path
  if (!mentionsLabel(line)) return;
  const LabelRead row = parseLabelRead(line);
  if (!row.hasPer100 && !row.hasPerServing) return;
  const LabelRead empty;
  const LabelRead* srcp = pickSource(srcs, row);
  const LabelRead& src = srcp ? *srcp : empty;
  const LabelRead m = mergeReads(row, src);
  for (const LabelDoubt& x : checkLabelRead(m))
    if (rowStates(row, x)) doubts.push_back(x);
}

std::string labelRowGate(const std::string& rowText, const std::string& turnSources) {
  const std::vector<LabelRead> srcs = sourceReads(turnSources);
  std::vector<LabelDoubt> doubts;
  // One line at a time: a files.append carries the header plus rows, and a column
  // marker of one row must not open a column for the next.
  for (size_t b = 0, e; b <= rowText.size(); b = e + 1) {
    e = rowText.find('\n', b);
    if (e == std::string::npos) e = rowText.size();
    const std::string line = rowText.substr(b, e - b);
    if (e == b) continue;
    gateLine(line, srcs, doubts);
    if (e == rowText.size()) break;
  }
  if (doubts.empty()) return std::string();
  std::string out = "the label figures in this row do not add up: ";
  for (size_t i = 0; i < doubts.size(); ++i) {
    const LabelDoubt& d = doubts[i];
    if (i) out += "; ";
    out += d.figure + " states " + fmt(d.stated) + " but " + d.why + " gives " + fmt(d.computed);
  }
  out += ". Nothing was written. Re-read the label once (image.look on the photo's ref), and if it still "
         "disagrees, store the row marked [unverified: <figure>] and say which figure is in doubt.";
  return out;
}

std::vector<FigureDisagreement> labelReadDisagreements(const std::string& headText,
                                                       const std::string& secondText) {
  const std::vector<Field> a = fieldsOf(parseLabelRead(headText));
  const std::vector<Field> b = fieldsOf(parseLabelRead(secondText));
  std::vector<FigureDisagreement> out;
  for (const Field& fa : a) {
    if (!fa.has) continue;
    for (const Field& fb : b) {
      if (std::strcmp(fa.name, fb.name) != 0 || !fb.has) continue;
      if (!near(fa.v, fb.v, 0.02, 0.05)) {
        FigureDisagreement d;
        d.field = fa.name;
        d.head = fa.v;
        d.second = fb.v;
        out.push_back(std::move(d));
      }
      break;
    }
  }
  return out;
}

std::string secondReadNote(const std::string& headText, const std::string& secondText,
                           const std::string& byProvider) {
  const std::vector<FigureDisagreement> d = labelReadDisagreements(headText, secondText);
  if (d.empty()) return std::string();
  std::string out = "second read";
  if (!byProvider.empty()) out += " (" + byProvider + ")";
  out += " disagrees with the earlier read on: ";
  for (size_t i = 0; i < d.size(); ++i) {
    if (i) out += "; ";
    out += d[i].field + " " + fmt(d[i].head) + " vs " + fmt(d[i].second);
  }
  out += " - both values are shown, never averaged";
  return out;
}

namespace {

// The provider a stored record names in its "(transcribed by X)" tag, "" if none.
std::string providerOfRecord(const std::string& block) {
  std::string low;
  for (char c : block) low += (char)std::tolower((unsigned char)c);
  const size_t at = low.find("(transcribed by ");
  if (at == std::string::npos) return std::string();
  const size_t b = at + 16;   // "(transcribed by " is 16 chars
  const size_t e = low.find(')', b);
  if (e == std::string::npos || e <= b) return std::string();
  return block.substr(b, e - b);
}

// Does a stored record plausibly read the SAME label as the head's reply? A shared
// net weight, or a shared per-serving kcal. A record of a different pack must not
// be "disagreed" with.
bool sameLabelRead(const LabelRead& head, const LabelRead& rec) {
  if (head.netKnown && rec.netKnown && std::fabs(head.netG - rec.netG) <= 1.0) return true;
  return head.hasPerServing && rec.hasPerServing && head.perServing.hasKcal &&
         rec.perServing.hasKcal && near(head.perServing.kcal, rec.perServing.kcal, 0.02, 1.0);
}

}  // namespace

std::string secondReadNoteForReply(const std::string& replyText, const std::string& recordsText) {
  const LabelRead head = parseLabelRead(replyText);
  if (!head.hasPerServing && !head.hasPer100) return std::string();
  std::string out;
  for (const std::string& block : sourceBlocks(recordsText)) {
    const LabelRead rec = parseLabelRead(block);
    if (!countsAsRead(rec) || !sameLabelRead(head, rec)) continue;
    const std::string note = secondReadNote(replyText, block, providerOfRecord(block));
    if (note.empty()) continue;
    if (!out.empty()) out += " ";
    out += note;
  }
  return out;
}

namespace {

// Lowercase, every non-alphanumeric run becomes one space: the one normalization
// both the row's brand candidates and the source text go through, so "M&S" in the
// sources backs "m s" in the row - and "M&s" at the start of its own line, or
// "KFC," with a comma after it, backs them too (punctuation and newlines are word
// separators, not part of a word).
std::string normalizeAmp(const std::string& s) {
  std::string out;
  bool sep = false;
  for (char c : s) {
    if (std::isalnum((unsigned char)c)) {
      out += (char)std::tolower((unsigned char)c);
      sep = false;
    } else if (!out.empty() && !sep) {
      out += ' ';
      sep = true;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// One brand-like token of the row, normalized: holds "&", is all caps, or has an
// internal capital ("M&S", "KFC", "McFlurry").
std::string brandCandidate(const std::string& tok) {
  bool letters = false, upper = false, internalUpper = false, amp = false;
  for (size_t i = 0; i < tok.size(); ++i) {
    const char c = tok[i];
    if (c == '&') { amp = true; continue; }
    if (!std::isalpha((unsigned char)c)) continue;
    letters = true;
    if (std::isupper((unsigned char)c)) { upper = true; if (i > 0) internalUpper = true; }
  }
  if (!letters || tok.size() < 2) return std::string();
  const bool allCaps = upper && tok.find_first_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos;
  if (!(amp || allCaps || internalUpper)) return std::string();
  return normalizeAmp(tok);
}

// The brand/detail tokens a row's items assert.
std::vector<std::string> brandCandidates(const std::string& rowText) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p < rowText.size()) {
    if (!std::isalnum((unsigned char)rowText[p]) && rowText[p] != '&') { ++p; continue; }
    size_t e = p;
    while (e < rowText.size() && (std::isalnum((unsigned char)rowText[e]) ||
                                  (rowText[e] == '&' && e + 1 < rowText.size() && std::isalnum((unsigned char)rowText[e + 1]))))
      ++e;
    if (e == p) ++e;   // a lone '&' is consumed, not a brand; the scan never stalls
    const std::string cand = brandCandidate(rowText.substr(p, e - p));
    p = e;
    if (!cand.empty()) out.push_back(cand);
  }
  return out;
}

}  // namespace

std::string unsupportedDetails(const std::string& rowText, const std::string& sourceText) {
  std::string low;
  for (char c : rowText) low += (char)std::tolower((unsigned char)c);
  if (low.find("[assumed") != std::string::npos) return std::string();
  // Provenance is a label-row property: a row with no label text carries no brand
  // claim to check.
  if (!mentionsLabel(rowText)) return std::string();
  const std::string hay = " " + normalizeAmp(sourceText) + " ";
  std::string out;
  for (const std::string& c : brandCandidates(rowText)) {
    // "m s" (from M&S) matches as a word sequence; a single word as " word ".
    if (hay.find(" " + c + " ") != std::string::npos) continue;
    if (!out.empty()) out += ", ";
    out += c;
  }
  return out;
}

}  // namespace orch
}  // namespace nimbus
