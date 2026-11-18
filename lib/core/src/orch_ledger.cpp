// orch_ledger.cpp - the food ledger store (lane RB-L1a). See nimbus/orch/ledger.h for the
// storage layout, the commit point and the recovery rule.
#include "nimbus/orch/ledger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "nimbus/utf8.h"

namespace nimbus {
namespace orch {
namespace ledger {

namespace {

const char kLogName[] = "changes.log";
const char kOldLogName[] = "changes.old.log";
const char kMonthHead[] = "# nimbus ledger v1 seq=";
const char kIdAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";   // Crockford base 32
const uint64_t kDamaged = UINT64_MAX;   // monthSeq_ of a month file that cannot be read
const int kMaxIntDigits = 12;

int64_t pow10i(int dp) {
  int64_t p = 1;
  while (dp-- > 0) p *= 10;
  return p;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// ---- calendar (days since 1970-01-01, proleptic Gregorian) ----
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

std::string civilFromDays(int64_t z) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t y = (int64_t)yoe + era * 400 + (m <= 2);
  char buf[16];
  snprintf(buf, sizeof buf, "%04lld-%02u-%02u", (long long)y, m, d);
  return buf;
}

int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

bool dayNumber(const std::string& day, int64_t& out) {
  if (day.size() != 10 || day[4] != '-' || day[7] != '-') return false;
  for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
    if (!isDigit(day[i])) return false;
  const int y = atoi(day.substr(0, 4).c_str()), m = atoi(day.substr(5, 2).c_str()), d = atoi(day.substr(8, 2).c_str());
  if (m < 1 || m > 12 || d < 1 || d > 31) return false;
  out = daysFromCivil(y, (unsigned)m, (unsigned)d);
  return civilFromDays(out) == day;   // rejects 30 Feb and friends
}

int minutesOf(const std::string& hhmm) { return atoi(hhmm.substr(0, 2).c_str()) * 60 + atoi(hhmm.substr(3, 2).c_str()); }

std::string hhmmOf(int64_t minuteOfDay) {
  char buf[8];
  snprintf(buf, sizeof buf, "%02d:%02d", (int)(minuteOfDay / 60), (int)(minuteOfDay % 60));
  return buf;
}

// The local time of `clock` in minutes since 1970-01-01 local.
int64_t localMinutes(const Clock& c) { return floorDiv(c.epoch + c.utcOffsetSec, 60); }

std::string ledgerDayOfMinutes(int64_t localMin) {
  return civilFromDays(floorDiv(localMin - kDayStartMinute, 1440));
}

// ---- text ----
std::string clean(std::string s) {
  for (char& c : s)
    if ((unsigned char)c < 0x20 || c == 0x7F) c = ' ';
  utf8Repair(s);
  return s;
}

std::string escapeToken(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\\') out += "\\\\";
    else if (c == '\t') out += "\\t";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

std::string unescapeToken(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 == s.size()) {
      out += s[i];
      continue;
    }
    const char n = s[++i];
    out += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
  }
  return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out(1);
  for (char c : s) {
    if (c == sep) out.emplace_back();
    else out.back() += c;
  }
  return out;
}

std::string csvField(const std::string& s) {
  if (s.find_first_of(",\"") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

bool csvSplit(const std::string& line, std::vector<std::string>& out) {
  out.assign(1, std::string());
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c != '"') out.back() += c;
      else if (i + 1 < line.size() && line[i + 1] == '"') out.back() += '"', ++i;
      else quoted = false;
    } else if (c == '"' && out.back().empty()) {
      quoted = true;
    } else if (c == ',') {
      out.emplace_back();
    } else {
      out.back() += c;
    }
  }
  return !quoted;
}

std::string fixedCsv(const Fixed& f, int dp) { return f.set ? formatFixed(f, dp, false) : std::string(); }

bool fixedFromCsv(const std::string& s, int dp, Fixed& out) {
  out = Fixed();
  return s.empty() || parseFixed(s, dp, out);
}

std::string plural(size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

bool readAll(tx::TxFiles& fs, const std::string& path, std::string& out, bool& exists) {
  out.clear();
  size_t n = 0;
  exists = fs.size(path, n);
  if (!exists) return true;
  if (n > kMaxFileBytes) return false;   // never trust a size the card reports past this
  out.resize(n);
  size_t off = 0;
  while (off < n) {
    size_t got = 0;
    if (!fs.readAt(path, off, &out[off], std::min<size_t>(4096, n - off), got) || got == 0) return false;
    off += got;
  }
  return true;
}

bool monthFileName(const std::string& name) {
  if (name.size() != 11 || name.compare(7, 4, ".csv") != 0 || name[4] != '-') return false;
  for (size_t i : {0, 1, 2, 3, 5, 6})
    if (!isDigit(name[i])) return false;
  return true;
}

bool endsWith(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::string monthOf(const std::string& day) { return day.substr(0, 7); }

// Rows without a time sort after the timed ones of their day.
std::string sortKey(const Row& r) { return r.day + (r.time.empty() ? "~" : r.time); }

}  // namespace

// ---- figures ----------------------------------------------------------------------------
namespace {

// The digits of a decimal number, read once: the whole part, the first `dp` decimals and
// the one after them (for rounding).
struct Digits {
  int64_t whole = 0, frac = 0;
  int intDigits = 0, fracDigits = 0, roundDigit = 0;
};

// Reads [digits][.digits] from `i`; false past kMaxIntDigits whole digits.
bool readDigits(const std::string& text, size_t& i, int dp, Digits& d) {
  for (; i < text.size() && isDigit(text[i]); ++i, ++d.intDigits) {
    if (d.intDigits >= kMaxIntDigits) return false;
    d.whole = d.whole * 10 + (text[i] - '0');
  }
  if (i >= text.size() || text[i] != '.') return true;
  for (++i; i < text.size() && isDigit(text[i]); ++i, ++d.fracDigits) {
    if (d.fracDigits < dp) d.frac = d.frac * 10 + (text[i] - '0');
    else if (d.fracDigits == dp) d.roundDigit = text[i] - '0';
  }
  return true;
}

}  // namespace

bool parseFixed(const std::string& text, int dp, Fixed& out) {
  size_t i = 0;
  bool neg = false;
  if (!text.empty() && (text[0] == '-' || text[0] == '+')) neg = text[i++] == '-';
  Digits d;
  if (!readDigits(text, i, dp, d) || i != text.size() || d.intDigits + d.fracDigits == 0) return false;
  for (int k = d.fracDigits; k < dp; ++k) d.frac *= 10;
  const int64_t v = d.whole * pow10i(dp) + d.frac + (d.roundDigit >= 5 ? 1 : 0);
  out = Fixed::of(neg ? -v : v);
  return true;
}

bool fixedFromDouble(double value, int dp, Fixed& out) {
  if (!std::isfinite(value) || std::fabs(value) >= 1e12) return false;
  char buf[48];
  snprintf(buf, sizeof buf, "%.9f", value);
  return parseFixed(buf, dp, out);
}

std::string formatFixed(const Fixed& f, int dp, bool thousands) {
  if (!f.set) return std::string();
  const int64_t p = pow10i(dp);
  const uint64_t mag = f.v < 0 ? (uint64_t)(-(f.v + 1)) + 1 : (uint64_t)f.v;
  std::string whole = std::to_string(mag / (uint64_t)p);
  if (thousands)
    for (int k = (int)whole.size() - 3; k > 0; k -= 3) whole.insert((size_t)k, ",");
  std::string frac;
  if (dp > 0) {
    char buf[24];
    snprintf(buf, sizeof buf, "%0*llu", dp, (unsigned long long)(mag % (uint64_t)p));
    frac = buf;
    while (!frac.empty() && frac.back() == '0') frac.pop_back();
  }
  return (f.v < 0 ? "-" : "") + whole + (frac.empty() ? "" : "." + frac);
}

const char* basisSlug(Basis b) {
  switch (b) {
    case Basis::Label: return "label";
    case Basis::Stated: return "stated";
    case Basis::Estimate: return "estimate";
    case Basis::Legacy: return "legacy";
    case Basis::None: break;
  }
  return "";
}

bool parseBasis(const std::string& slug, Basis& out) {
  for (Basis b : {Basis::None, Basis::Label, Basis::Stated, Basis::Estimate, Basis::Legacy})
    if (slug == basisSlug(b)) {
      out = b;
      return true;
    }
  return false;
}

// Two rows are equal when every column encodes the same (the CSV form is exact).
bool Row::operator==(const Row& o) const { return encodeRow(*this) == encodeRow(o); }

uint32_t fieldByName(const std::string& name) {
  static const struct {
    const char* name;
    uint32_t bit;
  } kNames[] = {{"day", kFDay},         {"time", kFTime},       {"meal", kFMeal},       {"item", kFItem},
                {"qty", kFQty},         {"unit", kFUnit},       {"kcal", kFKcal},       {"protein_g", kFProtein},
                {"protein", kFProtein}, {"carb_g", kFCarb},     {"carb", kFCarb},       {"fat_g", kFFat},
                {"fat", kFFat},         {"fibre_g", kFFibre},   {"fibre", kFFibre},     {"polyol_g", kFPolyol},
                {"polyol", kFPolyol},   {"alcohol_g", kFAlcohol}, {"alcohol", kFAlcohol}, {"per100", kFPer100},
                {"basis", kFBasis},     {"src", kFSrc},         {"note", kFNote}};
  for (const auto& n : kNames)
    if (name == n.name) return n.bit;
  return 0;
}

// ---- the ledger day -----------------------------------------------------------------------
void localDateTime(int64_t epoch, int32_t utcOffsetSec, std::string& date, std::string& hhmm) {
  const int64_t m = floorDiv(epoch + utcOffsetSec, 60);
  date = civilFromDays(floorDiv(m, 1440));
  hhmm = hhmmOf(m - floorDiv(m, 1440) * 1440);
}

std::string ledgerDayOf(const std::string& date, const std::string& hhmm) {
  return minutesOf(hhmm) < kDayStartMinute ? addDays(date, -1) : date;
}

std::string ledgerToday(const Clock& clock) {
  return clock.valid() ? ledgerDayOfMinutes(localMinutes(clock)) : std::string();
}

std::string ledgerDayForTime(const Clock& clock, const std::string& hhmm) {
  if (!clock.valid() || !validTime(hhmm)) return std::string();
  const int64_t now = localMinutes(clock);
  int64_t cand = floorDiv(now, 1440) * 1440 + minutesOf(hhmm);
  const int64_t delta = cand - now;
  if (delta >= 720) cand -= 1440;          // 12 h or more ahead: yesterday's (a tie goes to the past)
  else if (delta < -720) cand += 1440;     // more than 12 h back: tomorrow's is nearer
  return ledgerDayOfMinutes(cand);
}

bool validDay(const std::string& day) {
  int64_t n = 0;
  return dayNumber(day, n);
}

bool validTime(const std::string& t) {
  return t.size() == 5 && isDigit(t[0]) && isDigit(t[1]) && t[2] == ':' && isDigit(t[3]) && isDigit(t[4]) &&
         minutesOf(t) / 60 < 24 && atoi(t.substr(3).c_str()) < 60;
}

std::string addDays(const std::string& day, int delta) {
  int64_t n = 0;
  return dayNumber(day, n) ? civilFromDays(n + delta) : std::string();
}

// ---- rendering ---------------------------------------------------------------------------
std::string renderRow(const Row& r) {
  std::string figs = formatFixed(r.kcal.set ? r.kcal : Fixed::of(0), kKcalDp) + " kcal";
  const struct {
    const char* label;
    const Fixed* f;
  } grams[] = {{"P", &r.protein}, {"C", &r.carb},       {"F", &r.fat},
               {"fibre", &r.fibre}, {"polyol", &r.polyol}, {"alcohol", &r.alcohol}};
  for (const auto& g : grams)
    if (g.f->set) figs += std::string(", ") + g.label + " " + formatFixed(*g.f, kGramDp) + " g";
  if (r.per100.set) figs += ", " + formatFixed(r.per100, kKcalDp) + " kcal per 100";
  std::string what = r.item;
  if (r.qty.set) what = formatFixed(r.qty, kQtyDp) + (r.unit.empty() ? "" : " " + r.unit) + " " + r.item;
  std::string when = r.day + (r.time.empty() ? "" : " " + r.time) + (r.meal.empty() ? "" : " " + r.meal);
  std::string out = r.id + " | " + figs + " | " + what + " | " + when;
  if (r.basis != Basis::None) out += std::string(" | ") + basisSlug(r.basis);
  if (!r.src.empty()) out += " | src " + r.src;
  if (!r.note.empty()) out += " | note " + r.note;
  return out;
}

DayTotals totalsOf(const std::string& day, const std::vector<Row>& rows) {
  DayTotals t;
  t.day = day;
  for (const Row& r : rows) {
    if (r.day != day) continue;
    ++t.rows;
    t.kcal += r.kcal.set ? r.kcal.v : 0;
    if (r.protein.set) t.protein += r.protein.v;
    else ++t.noProtein;
    t.carb += r.carb.set ? r.carb.v : 0;
    t.fat += r.fat.set ? r.fat.v : 0;
    t.fibre += r.fibre.set ? r.fibre.v : 0;
    t.polyol += r.polyol.set ? r.polyol.v : 0;
    t.alcohol += r.alcohol.set ? r.alcohol.v : 0;
  }
  return t;
}

std::string renderDayLine(const DayTotals& t, const Targets* targets) {
  std::string out = "day " + t.day + " (ledger day, 04:00 to 04:00): " + plural(t.rows, "row", "rows") + ", " +
                    formatFixed(Fixed::of(t.kcal), kKcalDp) + " kcal, P " +
                    formatFixed(Fixed::of(t.protein), kGramDp) + " g";
  if (t.noProtein && t.rows) out += " (P missing on " + plural(t.noProtein, "row", "rows") + ")";
  if (!targets || (!targets->kcal.set && !targets->protein.set)) return out + "; no target set";
  const Fixed& k = targets->kcal;
  const Fixed& p = targets->protein;
  out += "; target " + (k.set ? formatFixed(k, kKcalDp) : "-") + " / " + (p.set ? formatFixed(p, kGramDp) : "-");
  out += " -> left " + (k.set ? formatFixed(Fixed::of(k.v - t.kcal), kKcalDp) : "-") + " kcal, " +
         (p.set ? formatFixed(Fixed::of(p.v - t.protein), kGramDp) : "-") + " g";
  return out;
}

// ---- row checks --------------------------------------------------------------------------
namespace {

// Grams (or ml) per unit of qty for the units the density rule reads; 0 for any other unit.
int64_t gramsPerUnit(const std::string& unit) {
  const std::string u = foldCase(unit);
  if (u == "g" || u == "ml") return 1;
  if (u == "kg" || u == "l") return 1000;
  return 0;
}

std::string figureError(const char* name, const Fixed& f, int64_t max, int dp) {
  if (!f.set) return std::string();
  if (f.v < 0) return std::string(name) + " is negative";
  if (f.v > max * pow10i(dp)) return std::string(name) + " is more than " + formatFixed(Fixed::of(max), 0);
  return std::string();
}

std::string textError(const char* name, const std::string& s, size_t max) {
  if (s.size() > max) return std::string(name) + " is longer than " + std::to_string(max) + " bytes";
  return std::string();
}

std::string densityError(const Row& r) {
  const char* fact = " kcal per 100 g is more than 900 kcal per 100 g; no food has more (fat is 900, and 1 kcal is 4.184 kJ)";
  if (r.per100.set && r.per100.v > 9000) return formatFixed(r.per100, kKcalDp) + fact;
  const int64_t scale = gramsPerUnit(r.unit);
  if (!r.qty.set || r.qty.v <= 0 || !scale) return std::string();
  const int64_t grams = r.qty.v * scale;   // 0.001 g
  // kcal (0.1) * 100 > 9 * grams (0.001) is the same as kcal per 100 g above 900.
  if (r.kcal.v * 100 <= 9 * grams) return std::string();
  const int64_t per100 = r.kcal.v * 100000 / grams;   // 0.1 kcal per 100 g
  return formatFixed(Fixed::of(per100), kKcalDp) + fact;
}

}  // namespace

std::string rowError(const Row& r) {
  if (!validDay(r.day)) return "day '" + r.day + "' is not a YYYY-MM-DD date";
  if (!r.time.empty() && !validTime(r.time)) return "time '" + r.time + "' is not HH:MM from 00:00 to 23:59";
  if (r.item.empty()) return "item is empty";
  if (!r.kcal.set) return "kcal is missing";
  std::string e;
  const struct {
    const char* name;
    const std::string* s;
    size_t max;
  } texts[] = {{"item", &r.item, kMaxItem}, {"meal", &r.meal, kMaxMeal}, {"unit", &r.unit, kMaxUnit},
               {"src", &r.src, kMaxSrc},    {"note", &r.note, kMaxNote}};
  for (const auto& t : texts)
    if (!(e = textError(t.name, *t.s, t.max)).empty()) return e;
  const struct {
    const char* name;
    const Fixed* f;
    int64_t max;
    int dp;
  } figs[] = {{"kcal", &r.kcal, 100000, kKcalDp},        {"protein_g", &r.protein, 100000, kGramDp},
              {"carb_g", &r.carb, 100000, kGramDp},      {"fat_g", &r.fat, 100000, kGramDp},
              {"fibre_g", &r.fibre, 100000, kGramDp},    {"polyol_g", &r.polyol, 100000, kGramDp},
              {"alcohol_g", &r.alcohol, 100000, kGramDp}, {"per100", &r.per100, 100000, kKcalDp},
              {"qty", &r.qty, 1000000, kQtyDp}};
  for (const auto& f : figs)
    if (!(e = figureError(f.name, *f.f, f.max, f.dp)).empty()) return e;
  return densityError(r);
}

std::vector<std::string> figureWarnings(const Row& r) {
  std::vector<std::string> out;
  if (r.basis != Basis::Estimate && r.basis != Basis::Stated) return out;   // never for label (Ruling 11)
  if (!r.protein.set || !r.carb.set || !r.fat.set || !r.kcal.set) return out;
  auto g = [](const Fixed& f) { return f.set ? f.v : 0; };
  // Macro energy in 0.001 kcal: grams are in 0.01 g, so 4 kcal/g is 40 per unit.
  const int64_t macro = 40 * r.protein.v + 40 * r.carb.v + 90 * r.fat.v + 20 * g(r.fibre) + 24 * g(r.polyol) +
                        70 * g(r.alcohol);
  if (macro <= 0) return out;
  const int64_t kcal = r.kcal.v * 100;   // 0.001 kcal
  const std::string macroText =
      "4P + 4C + 9F + 2 fibre + 2.4 polyol + 7 alcohol = " + formatFixed(Fixed::of((macro + 50) / 100), kKcalDp) + " kcal";
  const std::string kcalText = formatFixed(r.kcal, kKcalDp) + " kcal";
  const int64_t kj = 4184 * macro;   // the macro energy in kJ, 0.000001 units
  const int64_t offKj = kcal * 1000 - kj;
  if (100 * std::llabs(offKj) <= 15 * kj) {
    out.push_back("warning: " + r.id + ": " + kcalText + " is the size of a kJ figure for its macros (" + macroText +
                  "; 1 kcal is 4.184 kJ)");
    return out;
  }
  const int64_t off = kcal - macro;
  if (100 * std::llabs(off) <= 15 * macro) return out;
  const int64_t pct = 100 * std::llabs(off) / macro;
  out.push_back("warning: " + r.id + ": " + kcalText + " is " + std::to_string(pct) + "% " +
                (off > 0 ? "above" : "below") + " " + macroText);
  return out;
}

std::string foldCase(const std::string& s) {
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i) {
    unsigned char c = (unsigned char)out[i];
    if (c >= 'A' && c <= 'Z') {
      out[i] = (char)(c + 32);
    } else if (c == 0xC3 && i + 1 < out.size()) {
      const unsigned char n = (unsigned char)out[i + 1];
      if (n >= 0x80 && n <= 0x9E && n != 0x97) out[i + 1] = (char)(n + 0x20);
      ++i;
    }
  }
  return out;
}

bool identicalRows(const Row& a, const Row& b) {
  return a.day == b.day && a.qty == b.qty && a.kcal == b.kcal && foldCase(a.unit) == foldCase(b.unit) &&
         foldCase(a.item) == foldCase(b.item);
}

// ---- the CSV and the ids -----------------------------------------------------------------
std::string encodeRow(const Row& r) {
  const std::string f[] = {r.id,
                           r.day,
                           r.time,
                           r.meal,
                           r.item,
                           fixedCsv(r.qty, kQtyDp),
                           r.unit,
                           fixedCsv(r.kcal, kKcalDp),
                           fixedCsv(r.protein, kGramDp),
                           fixedCsv(r.carb, kGramDp),
                           fixedCsv(r.fat, kGramDp),
                           fixedCsv(r.fibre, kGramDp),
                           fixedCsv(r.polyol, kGramDp),
                           fixedCsv(r.alcohol, kGramDp),
                           fixedCsv(r.per100, kKcalDp),
                           basisSlug(r.basis),
                           r.src,
                           r.note,
                           r.call};
  std::string out;
  for (size_t i = 0; i < sizeof f / sizeof f[0]; ++i) out += (i ? "," : "") + csvField(f[i]);
  return out;
}

bool decodeRow(const std::string& line, Row& out) {
  std::vector<std::string> f;
  if (!csvSplit(line, f) || f.size() != 19) return false;
  Row r;
  r.id = f[0];
  r.day = f[1];
  r.time = f[2];
  r.meal = f[3];
  r.item = f[4];
  r.unit = f[6];
  r.src = f[16];
  r.note = f[17];
  r.call = f[18];
  const bool ok = fixedFromCsv(f[5], kQtyDp, r.qty) && fixedFromCsv(f[7], kKcalDp, r.kcal) &&
                  fixedFromCsv(f[8], kGramDp, r.protein) && fixedFromCsv(f[9], kGramDp, r.carb) &&
                  fixedFromCsv(f[10], kGramDp, r.fat) && fixedFromCsv(f[11], kGramDp, r.fibre) &&
                  fixedFromCsv(f[12], kGramDp, r.polyol) && fixedFromCsv(f[13], kGramDp, r.alcohol) &&
                  fixedFromCsv(f[14], kKcalDp, r.per100) && parseBasis(f[15], r.basis);
  if (!ok) return false;
  out = r;
  return true;
}

std::string rowId(uint64_t seq, unsigned index) {
  const uint64_t mask = (1ull << 25) - 1;
  uint64_t x = ((seq << 4) | (index & 15u)) & mask;
  // A bijection on 25 bits (odd multiplies and xor-shifts), so distinct inputs never collide
  // and neighbours spread over every character.
  x = (x * 0x079D681ull) & mask;
  x ^= x >> 13;
  x = (x * 0x12F6EA3ull) & mask;
  x ^= x >> 11;
  x = (x * 0x116A537ull) & mask;
  x ^= x >> 12;
  std::string id = "L";
  for (int shift = 20; shift >= 0; shift -= 5) id += kIdAlphabet[(x >> shift) & 31];
  return id;
}

std::string normalizeId(const std::string& id) {
  size_t a = id.find_first_not_of(" \t"), b = id.find_last_not_of(" \t");
  std::string out = a == std::string::npos ? std::string() : id.substr(a, b - a + 1);
  for (char& c : out)
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  return out;
}

bool validPerson(const std::string& p) {
  if (p.empty() || p.size() > 32) return false;
  for (char c : p)
    if (!(isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return false;
  return true;
}

uint32_t crc32(const std::string& data) {
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : data) {
    c ^= b;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// ---- the store ---------------------------------------------------------------------------
struct LedgerStore::Month {
  std::string ym;
  uint64_t seq = 0;
  std::vector<Row> rows;
};

// One change of a record: 'A' a new row, 'P' a changed row (both put by id), 'D' a removal.
struct Entry {
  char kind;
  Row row;   // A and P: the whole row; D: id and day
};

struct LedgerStore::Record {
  uint64_t seq = 0;
  std::string call;
  int64_t at = 0;
  std::string receipt;
  std::vector<Entry> entries;
};

struct LedgerStore::Work {
  uint64_t seq = 0;
  Clock now;
  std::string call;
  std::map<std::string, Month> months;   // loaded and changed months, by YYYY-MM
  std::set<std::string> touched;          // months the write changes
  std::vector<Entry> entries;
  std::vector<std::string> lines;         // the changed rows, receipt order
  std::vector<Row> changed;
  std::set<std::string> days;
  std::vector<std::string> warnings;
  std::map<std::string, int64_t> addedNow;   // ids added by this write
};

namespace {

void putRow(std::vector<Row>& rows, const Row& r) {
  for (Row& x : rows)
    if (x.id == r.id) {
      x = r;
      return;
    }
  rows.push_back(r);
}

void dropRow(std::vector<Row>& rows, const std::string& id) {
  rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& x) { return x.id == id; }), rows.end());
}

void applyEntry(std::vector<Row>& rows, const Entry& e) {
  if (e.kind == 'D') dropRow(rows, e.row.id);
  else putRow(rows, e.row);
}

void sortRows(std::vector<Row>& rows) {
  std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return sortKey(a) < sortKey(b); });
}

std::string encodeEntry(const Entry& e) {
  if (e.kind == 'D') return "D" + e.row.day + "," + e.row.id;
  return std::string(1, e.kind) + encodeRow(e.row);
}

bool decodeEntry(const std::string& tok, Entry& e) {
  if (tok.empty()) return false;
  e.kind = tok[0];
  e.row = Row();
  if (e.kind == 'D') {
    const size_t comma = tok.find(',');
    if (comma == std::string::npos) return false;
    e.row.day = tok.substr(1, comma - 1);
    e.row.id = tok.substr(comma + 1);
    return validDay(e.row.day);
  }
  return (e.kind == 'A' || e.kind == 'P') && decodeRow(tok.substr(1), e.row) && validDay(e.row.day);
}

std::string encodeRecord(const LedgerStore::Record& r) {
  std::string body = escapeToken(r.call) + "\t" + std::to_string(r.at) + "\t" + escapeToken(r.receipt);
  for (const Entry& e : r.entries) body += "\t" + escapeToken(encodeEntry(e));
  char head[64];
  snprintf(head, sizeof head, "L1 %llu %zu %08x ", (unsigned long long)r.seq, body.size(), (unsigned)crc32(body));
  return head + body + "\n";
}

bool allOf(const std::string& s, const char* set, size_t maxLen) {
  return !s.empty() && s.size() <= maxLen && s.find_first_not_of(set) == std::string::npos;
}

// "L1 <seq> <len> <crc8> <body>" -> seq and body, when the body has the stated length and
// crc. Splits on the first four spaces only, so a body that starts with a space keeps it.
bool splitRecordLine(const std::string& line, uint64_t& seq, std::string& body) {
  if (line.compare(0, 3, "L1 ") != 0) return false;
  const size_t a = line.find(' ', 3), b = a == std::string::npos ? a : line.find(' ', a + 1);
  if (b == std::string::npos || line.size() < b + 10 || line[b + 9] != ' ') return false;
  const std::string seqText = line.substr(3, a - 3), lenText = line.substr(a + 1, b - a - 1);
  const std::string crcText = line.substr(b + 1, 8);
  if (!allOf(seqText, "0123456789", 19) || !allOf(lenText, "0123456789", 19) || !allOf(crcText, "0123456789abcdef", 8))
    return false;
  seq = strtoull(seqText.c_str(), nullptr, 10);
  body = line.substr(b + 10);
  return seq != 0 && body.size() == strtoull(lenText.c_str(), nullptr, 10) &&
         crc32(body) == strtoul(crcText.c_str(), nullptr, 16);
}

// One log line (without its newline) -> a record. False for a torn or damaged line.
bool decodeRecord(const std::string& line, LedgerStore::Record& r) {
  uint64_t seq = 0;
  std::string body;
  if (!splitRecordLine(line, seq, body)) return false;
  const std::vector<std::string> tok = split(body, '\t');
  if (tok.size() < 3) return false;
  r = LedgerStore::Record();
  r.seq = seq;
  r.call = unescapeToken(tok[0]);
  r.at = atoll(tok[1].c_str());
  r.receipt = unescapeToken(tok[2]);
  for (size_t i = 3; i < tok.size(); ++i) {
    Entry e;
    if (!decodeEntry(unescapeToken(tok[i]), e)) return false;
    r.entries.push_back(e);
  }
  return true;
}

std::string encodeMonth(const LedgerStore::Month& m) {
  std::string out = kMonthHead + std::to_string(m.seq) + "\n" + kColumns + "\n";
  for (const Row& r : m.rows) out += encodeRow(r) + "\n";
  return out;
}

bool parseMonthHead(const std::string& firstLine, uint64_t& seq) {
  const size_t n = sizeof kMonthHead - 1;
  if (firstLine.compare(0, n, kMonthHead) != 0 || firstLine.size() == n) return false;
  for (size_t i = n; i < firstLine.size(); ++i)
    if (!isDigit(firstLine[i])) return false;
  seq = strtoull(firstLine.c_str() + n, nullptr, 10);
  return true;
}

}  // namespace

LedgerStore::LedgerStore(tx::TxFiles& files, const std::string& person, size_t logRollBytes)
    : files_(files), person_(person), dir_(std::string(kLedgerRoot) + "/" + person), rollBytes_(logRollBytes) {}

std::vector<std::string> LedgerStore::monthNames() {
  std::vector<std::string> names, out;
  files_.list(dir_, names);
  for (const std::string& n : names)
    if (monthFileName(n)) out.push_back(n.substr(0, 7));
  std::sort(out.begin(), out.end());
  return out;
}

// Finish or undo every month swap a power cut interrupted.
bool LedgerStore::finishSwaps(std::vector<std::string>& months, std::string& report) {
  std::vector<std::string> names;
  if (!files_.list(dir_, names)) return true;   // no folder yet: nothing to finish
  bool ok = true;
  for (const std::string& n : names) {
    if (endsWith(n, ".csv.part")) {
      ok = files_.remove(path(n)) && ok;
      report += "dropped " + n + "; ";
    } else if (endsWith(n, ".csv.bak")) {
      const std::string dst = n.substr(0, n.size() - 4);
      size_t sz = 0;
      const bool hasDst = files_.size(path(dst), sz);
      ok = (hasDst ? files_.remove(path(n)) : files_.rename(path(n), path(dst))) && ok;
      report += (hasDst ? "dropped " : "restored ") + n + "; ";
    }
  }
  months = monthNames();
  return ok;
}

bool LedgerStore::open(std::string* report) {
  std::string rep;
  const uint64_t seenBefore = lastSeq_;
  opened_ = false;
  dirty_ = true;
  tornTail_ = false;
  lastSeq_ = 0;
  calls_.clear();
  addedAt_.clear();
  monthSeq_.clear();
  if (!validPerson(person_)) {
    if (report) *report = "person '" + person_ + "' is not a valid ledger name";
    return false;
  }
  std::vector<std::string> months;
  bool ok = finishSwaps(months, rep);
  for (const std::string& ym : months) {
    char buf[64];
    size_t got = 0;
    uint64_t seq = kDamaged;
    if (files_.readAt(path(ym + ".csv"), 0, buf, sizeof buf, got)) {
      const std::string head(buf, got);
      if (!parseMonthHead(head.substr(0, head.find('\n')), seq)) seq = kDamaged;
    } else {
      ok = false;
    }
    monthSeq_[ym] = seq;
    if (seq == kDamaged) rep += ym + ".csv is damaged; ";
    else lastSeq_ = std::max(lastSeq_, seq);
  }
  std::vector<Record> recs;
  ok = ok && readLogs(recs);
  if (ok) scanLogs(recs);
  ok = ok && replay(recs, rep);
  // The storage interface answers "missing" for a file it cannot reach, so a card that went
  // away reads as an empty ledger. A ledger never goes back: older than this instance last
  // saw means the storage is not readable, not that the changes are gone.
  if (ok && lastSeq_ < seenBefore) {
    ok = false;
    lastSeq_ = seenBefore;
    rep += "the ledger reads back older than change " + std::to_string(seenBefore) + "; the storage is not readable";
  }
  if (report) *report = rep;
  opened_ = true;
  dirty_ = !ok;
  return ok;
}

namespace {

// The records of one log file, in file order. False when the file cannot be read.
bool readRecords(tx::TxFiles& fs, const std::string& path, std::vector<LedgerStore::Record>& out, bool* tornTail) {
  std::string text;
  bool exists = false;
  if (!readAll(fs, path, text, exists)) return false;
  if (tornTail) *tornTail = !text.empty() && text.back() != '\n';
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    LedgerStore::Record r;
    if (decodeRecord(text.substr(start, end - start), r)) out.push_back(r);
    start = end + 1;
  }
  return true;
}

}  // namespace

bool LedgerStore::readLogs(std::vector<Record>& recs) {
  if (!readRecords(files_, path(kOldLogName), recs, nullptr) || !readRecords(files_, path(kLogName), recs, &tornTail_))
    return false;
  logSize_ = 0;
  files_.size(path(kLogName), logSize_);
  return true;
}

// After a failed append: is record `seq` in the log? `known` is false when the log cannot be
// read back. A log that cannot be found counts as unreadable, because the storage answers
// "missing" for a file it cannot reach, and the append may have created it.
bool LedgerStore::recordLanded(uint64_t seq, bool& known) {
  std::vector<Record> recs;
  size_t sz = 0;
  known = files_.size(path(kLogName), sz) && readRecords(files_, path(kLogName), recs, nullptr);
  if (!known) return false;
  for (const Record& r : recs)
    if (r.seq == seq) return true;
  return false;
}

// Did something other than this instance append to (or roll) the log since it last looked?
bool LedgerStore::logChanged() {
  size_t sz = 0;
  if (!files_.size(path(kLogName), sz)) sz = 0;
  return sz != logSize_;
}

void LedgerStore::scanLogs(const std::vector<Record>& recs) {
  int64_t newest = 0;
  for (const Record& r : recs) {
    lastSeq_ = std::max(lastSeq_, r.seq);
    if (!r.call.empty()) calls_.emplace(r.call, r.seq);
    newest = std::max(newest, r.at);
    for (const Entry& e : r.entries)
      if (e.kind == 'A' && r.at > 0) addedAt_[e.row.id] = r.at;
  }
  pruneAdds(newest);
}

// Only adds inside the identical-row window are kept.
void LedgerStore::pruneAdds(int64_t newest) {
  for (auto it = addedAt_.begin(); it != addedAt_.end();)
    it = it->second < newest - kIdenticalWindowSec ? addedAt_.erase(it) : std::next(it);
}

// Apply one record to the months that lack it (loading each on first need). False only
// when a month could not be read at all; a damaged month is marked and skipped.
bool LedgerStore::replayRecord(const Record& r, std::map<std::string, Month>& pending, std::string& report) {
  std::set<std::string> hit;
  for (const Entry& e : r.entries) {
    const std::string ym = monthOf(e.row.day);
    const auto ms = monthSeq_.find(ym);
    const uint64_t have = ms == monthSeq_.end() ? 0 : ms->second;
    if (have == kDamaged || r.seq <= have) continue;
    bool io = false;
    if (!pending.count(ym) && !loadMonth(ym, pending[ym], nullptr, &io)) {
      if (io) return false;
      monthSeq_[ym] = kDamaged;   // kept as it is; named by every read or write that needs it
      pending.erase(ym);
      report += ym + ".csv is damaged; ";
      continue;
    }
    if (r.seq <= pending[ym].seq) continue;
    applyEntry(pending[ym].rows, e);
    hit.insert(ym);
  }
  for (const std::string& ym : hit) {   // one sort per record, as the write did
    sortRows(pending[ym].rows);
    pending[ym].seq = r.seq;
  }
  return true;
}

// Apply every logged record a month file lacks, in seq order, then swap each month in.
bool LedgerStore::replay(std::vector<Record>& recs, std::string& report) {
  std::stable_sort(recs.begin(), recs.end(), [](const Record& a, const Record& b) { return a.seq < b.seq; });
  std::map<std::string, Month> pending;
  for (const Record& r : recs)
    if (!replayRecord(r, pending, report)) return false;
  bool ok = true;
  for (auto& kv : pending) {
    if (monthSeq_[kv.first] == kDamaged || kv.second.seq <= monthSeq_[kv.first]) continue;
    if (storeMonth(kv.second)) {
      monthSeq_[kv.first] = kv.second.seq;
      report += "replayed " + kv.first + " to " + std::to_string(kv.second.seq) + "; ";
    } else {
      ok = false;
    }
  }
  return ok;
}

bool LedgerStore::loadMonth(const std::string& ym, Month& out, std::string* err, bool* io) {
  out = Month();
  out.ym = ym;
  const std::string name = ym + ".csv";
  std::string text;
  bool exists = false;
  if (!readAll(files_, path(name), text, exists)) {
    if (io) *io = true;
    if (err) *err = "the ledger file " + name + " could not be read";
    return false;
  }
  if (!exists) return true;
  std::vector<std::string> lines = split(text, '\n');
  if (!lines.empty() && lines.back().empty()) lines.pop_back();
  bool ok = lines.size() >= 2 && parseMonthHead(lines[0], out.seq) && lines[1] == kColumns;
  for (size_t i = 2; ok && i < lines.size(); ++i) {
    Row r;
    ok = decodeRow(lines[i], r) && monthOf(r.day) == ym;
    if (ok) out.rows.push_back(r);
  }
  if (!ok && err) *err = "the ledger file " + name + " is damaged (it is kept as it is)";
  return ok;
}

// The month swap: new copy to .part, old to .bak, .part into place, .bak away.
bool LedgerStore::storeMonth(const Month& m) {
  const std::string dst = path(m.ym + ".csv"), part = dst + ".part", bak = dst + ".bak";
  const std::string text = encodeMonth(m);
  files_.remove(part);
  size_t sz = 0;
  if (!files_.writeWhole(part, text) || !files_.size(part, sz) || sz != text.size()) {
    files_.remove(part);
    return false;
  }
  const bool had = files_.size(dst, sz);
  if (had) {
    files_.remove(bak);
    if (!files_.rename(dst, bak)) {
      files_.remove(part);
      return false;
    }
  }
  if (!files_.rename(part, dst)) {
    if (had) files_.rename(bak, dst);
    return false;
  }
  if (had) files_.remove(bak);   // a leftover .bak beside its file is dropped at open
  return true;
}

bool LedgerStore::ensureOpen(std::string* err) {
  if (opened_ && !dirty_) return true;
  std::string report;
  if (open(&report)) return true;
  if (err) *err = report.empty() ? "the ledger storage could not be read" : report;
  return false;
}

bool LedgerStore::findReceipt(uint64_t seq, std::string& receipt) {
  std::vector<Record> recs;
  if (!readRecords(files_, path(kOldLogName), recs, nullptr) || !readRecords(files_, path(kLogName), recs, nullptr))
    return false;
  for (const Record& r : recs)
    if (r.seq == seq) {
      receipt = r.receipt;
      return true;
    }
  return false;
}

bool LedgerStore::appendRecord(const std::string& line) {
  const std::string data = tornTail_ ? "\n" + line : line;
  return files_.append(path(kLogName), data.data(), data.size());
}

// ---- applying ops ----
bool LedgerStore::locate(Work& w, const std::string& id, std::string& ym, size_t& pos, std::string& err) {
  auto search = [&](Month& m) {
    for (size_t i = 0; i < m.rows.size(); ++i)
      if (m.rows[i].id == id) {
        ym = m.ym;
        pos = i;
        return true;
      }
    return false;
  };
  for (auto& kv : w.months)
    if (search(kv.second)) return true;
  std::vector<std::string> names = monthNames();
  for (auto it = names.rbegin(); it != names.rend(); ++it) {
    if (w.months.count(*it)) continue;
    Month m;   // only the month that holds the id stays loaded
    if (!loadMonth(*it, m, &err)) return false;
    if (search(m)) {
      w.months[*it] = std::move(m);
      return true;
    }
  }
  err = "no row " + id + " in the ledger";
  return false;
}

namespace {

Row cleaned(Row r) {
  for (std::string* s : {&r.meal, &r.item, &r.unit, &r.src, &r.note, &r.time, &r.day}) *s = clean(*s);
  return r;
}

void applySet(Row& r, uint32_t mask, const Row& v) {
  const struct {
    uint32_t bit;
    std::string Row::*s;
  } texts[] = {{kFDay, &Row::day}, {kFTime, &Row::time}, {kFMeal, &Row::meal}, {kFItem, &Row::item},
               {kFUnit, &Row::unit}, {kFSrc, &Row::src},  {kFNote, &Row::note}};
  for (const auto& t : texts)
    if (mask & t.bit) r.*t.s = clean(v.*t.s);
  const struct {
    uint32_t bit;
    Fixed Row::*f;
  } figs[] = {{kFQty, &Row::qty},       {kFKcal, &Row::kcal},     {kFProtein, &Row::protein},
              {kFCarb, &Row::carb},     {kFFat, &Row::fat},       {kFFibre, &Row::fibre},
              {kFPolyol, &Row::polyol}, {kFAlcohol, &Row::alcohol}, {kFPer100, &Row::per100}};
  for (const auto& f : figs)
    if (mask & f.bit) r.*f.f = v.*f.f;
  if (mask & kFBasis) r.basis = v.basis;
}

}  // namespace

void LedgerStore::identicalWarnings(Work& w, const Row& r) {
  if (!w.now.valid()) return;
  for (const Row& x : w.months[monthOf(r.day)].rows) {
    if (x.id == r.id || !identicalRows(x, r)) continue;
    auto now = w.addedNow.find(x.id);
    auto old = addedAt_.find(x.id);
    const int64_t when = now != w.addedNow.end() ? now->second : old != addedAt_.end() ? old->second : 0;
    if (when <= 0 || w.now.epoch - when > kIdenticalWindowSec || w.now.epoch < when) continue;
    w.warnings.push_back("warning: " + r.id + " is identical to " + x.id +
                         " (same item, quantity, unit, kcal and ledger day), added " +
                         std::to_string((w.now.epoch - when) / 60) + " min earlier");
  }
}

bool LedgerStore::applyAdd(Work& w, const Op& op, size_t index, std::string& err) {
  Row r = cleaned(op.row);
  r.id = rowId(w.seq, (unsigned)index);
  r.call = w.call;
  if (!r.time.empty() && !validTime(r.time)) {
    err = "time '" + r.time + "' is not HH:MM from 00:00 to 23:59";
    return false;
  }
  if (r.day.empty()) {
    if (!r.time.empty()) {
      r.day = ledgerDayForTime(w.now, r.time);
    } else if (w.now.valid()) {
      std::string date;
      localDateTime(w.now.epoch, w.now.utcOffsetSec, date, r.time);
      r.day = ledgerDayOf(date, r.time);
    }
    if (r.day.empty()) {
      err = "no day derivable: the clock is not set and the add gives no day";
      return false;
    }
  }
  if (!(err = rowError(r)).empty()) return false;
  const std::string ym = monthOf(r.day);
  if (!w.months.count(ym) && !loadMonth(ym, w.months[ym], &err)) return false;
  identicalWarnings(w, r);
  for (const std::string& x : figureWarnings(r)) w.warnings.push_back(x);
  w.entries.push_back(Entry{'A', r});
  applyEntry(w.months[ym].rows, w.entries.back());
  w.addedNow[r.id] = w.now.valid() ? w.now.epoch : 0;
  w.touched.insert(ym);
  w.days.insert(r.day);
  w.changed.push_back(r);
  w.lines.push_back("added " + renderRow(r));
  return true;
}

namespace {

// The errors an update, remove or move has before any row is looked up.
std::string changeOpError(const Op& op) {
  if (op.ids.empty()) return "no id given (send id or ids)";
  if (op.ids.size() > kMaxIdsPerOp) return "an op names at most 16 ids";
  if (op.kind == OpKind::Update && op.setMask == 0) return "set names no column";
  return std::string();
}

}  // namespace

bool LedgerStore::applyChange(Work& w, const Op& op, std::string& err) {
  if (!(err = changeOpError(op)).empty()) return false;
  std::set<std::string> seen;
  for (const std::string& raw : op.ids) {
    const std::string id = normalizeId(raw);
    if (seen.insert(id).second && !changeOne(w, op, id, err)) return false;
  }
  return true;
}

// One row of an update, remove or move. A row that changes month is removed from the old
// month and put in the new one.
bool LedgerStore::changeOne(Work& w, const Op& op, const std::string& id, std::string& err) {
  std::string ym;
  size_t pos = 0;
  if (!locate(w, id, ym, pos, err)) return false;
  const Row old = w.months[ym].rows[pos];
  w.days.insert(old.day);
  w.touched.insert(ym);
  if (op.kind == OpKind::Remove) {
    w.entries.push_back(Entry{'D', old});
    applyEntry(w.months[ym].rows, w.entries.back());
    w.changed.push_back(old);
    w.lines.push_back("removed " + renderRow(old));
    return true;
  }
  Row nr = old;
  if (op.kind == OpKind::Move) nr.day = clean(op.day);
  else applySet(nr, op.setMask, op.set);
  nr.call = w.call;
  if (!(err = rowError(nr)).empty()) return false;
  const std::string nym = monthOf(nr.day);
  if (nym != ym) {
    if (!w.months.count(nym) && !loadMonth(nym, w.months[nym], &err)) return false;
    w.entries.push_back(Entry{'D', old});
    applyEntry(w.months[ym].rows, w.entries.back());
  }
  w.entries.push_back(Entry{'P', nr});
  applyEntry(w.months[nym].rows, w.entries.back());
  for (const std::string& x : figureWarnings(nr)) w.warnings.push_back(x);
  w.touched.insert(nym);
  w.days.insert(nr.day);
  w.changed.push_back(nr);
  w.lines.push_back(op.kind == OpKind::Move ? "moved from " + old.day + ": " + renderRow(nr)
                                            : "updated " + renderRow(nr));
  return true;
}

bool LedgerStore::applyOp(Work& w, const Op& op, size_t index, std::string& err) {
  if (op.kind == OpKind::Add) return applyAdd(w, op, index, err);
  return applyChange(w, op, err);
}

namespace {

const char* opName(OpKind k) {
  switch (k) {
    case OpKind::Add: return "add";
    case OpKind::Update: return "update";
    case OpKind::Remove: return "remove";
    case OpKind::Move: return "move";
  }
  return "op";
}

// The errors of a write's arguments, before storage is read.
std::string argumentError(const std::string& person, const std::string& callId, size_t ops) {
  if (!validPerson(person)) return "person '" + person + "' is not a valid ledger name";
  if (callId.size() > kMaxCall) return "the call id is longer than 80 bytes";
  if (clean(callId) != callId) return "the call id holds a control character";   // kept exactly, never cleaned
  if (ops == 0 || ops > kMaxOps) return "a write takes 1 to 16 ops";
  return std::string();
}

WriteResult refusal(WriteStatus st, const std::string& why) {
  WriteResult r;
  r.status = st;
  r.text = "error: " + why + "; nothing was written";
  return r;
}

}  // namespace

WriteResult LedgerStore::alreadyApplied(const std::string& call, uint64_t seq) {
  std::string first;
  if (!findReceipt(seq, first)) return refusal(WriteStatus::NotSaved, "the change log could not be read");
  WriteResult r;
  r.status = WriteStatus::AlreadyApplied;
  r.seq = seq;
  r.text = "already applied: call " + call + " was written as change " + std::to_string(seq) +
           "; its receipt follows\n" + first;
  return r;
}

// The receipt: the changed rows, one line per day touched, then the warnings.
std::string LedgerStore::receiptOf(Work& w, const Targets* targets) {
  std::string receipt;
  for (const std::string& l : w.lines) receipt += l + "\n";
  for (const std::string& d : w.days) receipt += renderDayLine(totalsOf(d, w.months[monthOf(d)].rows), targets) + "\n";
  for (const std::string& x : w.warnings) receipt += x + "\n";
  receipt.pop_back();
  return receipt;
}

// Append the record (the commit point), then swap each month in. False: not committed.
bool LedgerStore::commit(Work& w, const std::string& receipt, bool& known) {
  Record rec;
  rec.seq = w.seq;
  rec.call = w.call;
  rec.at = w.now.valid() ? w.now.epoch : 0;
  rec.receipt = receipt;
  rec.entries = w.entries;
  const std::string line = encodeRecord(rec);
  const size_t encodedSize = line.size() + (tornTail_ ? 1 : 0);
  if (!appendRecord(line)) {
    dirty_ = true;
    if (!recordLanded(w.seq, known)) return false;
    open();   // committed after all: recovery swaps the months in (now, or before the next step)
    return true;
  }
  tornTail_ = false;
  logSize_ += encodedSize;
  lastSeq_ = w.seq;
  if (!w.call.empty()) calls_.emplace(w.call, w.seq);
  for (const auto& kv : w.addedNow)
    if (kv.second > 0) addedAt_[kv.first] = kv.second;
  if (w.now.valid()) pruneAdds(w.now.epoch);
  for (const std::string& ym : w.touched) {
    if (storeMonth(w.months[ym])) monthSeq_[ym] = w.seq;
    else dirty_ = true;   // committed: recovery replays it before the next read or write
  }
  maybeRoll();
  return true;
}

WriteResult LedgerStore::write(const std::vector<Op>& ops, const std::string& callId, const Clock& now,
                               const Targets* targets) {
  std::string err = argumentError(person_, callId, ops.size());
  if (!err.empty()) return refusal(WriteStatus::Refused, err);
  if (!ensureOpen(&err) || (logChanged() && !open(&err)))
    return refusal(WriteStatus::NotSaved, "the ledger could not be read: " + err);
  const std::string& call = callId;
  const auto seen = call.empty() ? calls_.end() : calls_.find(call);
  if (seen != calls_.end()) return alreadyApplied(call, seen->second);
  if (lastSeq_ >= kMaxSeq) return refusal(WriteStatus::Refused, "the ledger has used every change number");
  Work w;
  w.seq = lastSeq_ + 1;
  w.now = now;
  w.call = call;
  for (size_t i = 0; i < ops.size(); ++i)
    if (!applyOp(w, ops[i], i, err))
      return refusal(WriteStatus::Refused, "op " + std::to_string(i + 1) + " (" + opName(ops[i].kind) + "): " + err);
  for (const std::string& ym : w.touched) {
    sortRows(w.months[ym].rows);
    w.months[ym].seq = w.seq;
  }
  const std::string receipt = receiptOf(w, targets);
  bool known = true;
  if (!commit(w, receipt, known)) {
    if (known) return refusal(WriteStatus::NotSaved, "the change log could not be written");
    WriteResult r;
    r.status = WriteStatus::Uncertain;
    r.seq = w.seq;
    r.text = "error: the change log write failed part way and could not be read back, so whether this "
             "change is stored is not known; the same call id again applies it once or says already applied";
    return r;
  }
  WriteResult res;
  res.status = WriteStatus::Ok;
  res.text = receipt;
  res.changed = w.changed;
  res.days.assign(w.days.begin(), w.days.end());
  res.warnings = w.warnings;
  res.seq = w.seq;
  return res;
}

// Keep the log bounded: past the roll size (kLogRollBytes on a device), and only with every month in step, the log
// becomes changes.old.log (the previous old log goes). The next open reads both.
void LedgerStore::maybeRoll() {
  size_t sz = 0;
  if (dirty_ || !files_.size(path(kLogName), sz) || sz <= rollBytes_) return;
  // A damaged month still lacks the records in the logs: keep them until it is repaired.
  for (const auto& kv : monthSeq_)
    if (kv.second == kDamaged) return;
  files_.remove(path(kOldLogName));
  files_.rename(path(kLogName), path(kOldLogName));
  opened_ = false;   // re-read the call ids and recent adds from the logs that remain
}

bool LedgerStore::day(const std::string& d, DayView& out, std::string* err) {
  out = DayView();
  if (!validDay(d)) {
    if (err) *err = "day '" + d + "' is not a YYYY-MM-DD date";
    return false;
  }
  if (!ensureOpen(err)) return false;
  Month m;
  if (!loadMonth(monthOf(d), m, err)) return false;
  for (const Row& r : m.rows)
    if (r.day == d) out.rows.push_back(r);
  out.totals = totalsOf(d, out.rows);
  return true;
}

bool LedgerStore::allRows(std::vector<Row>& out, std::string* err) {
  out.clear();
  if (!ensureOpen(err)) return false;
  for (const std::string& ym : monthNames()) {
    Month m;
    if (!loadMonth(ym, m, err)) return false;
    out.insert(out.end(), m.rows.begin(), m.rows.end());
  }
  return true;
}

}  // namespace ledger
}  // namespace orch
}  // namespace nimbus
