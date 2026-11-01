#include "nimbus/orch/ledger_migrate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace nimbus::orch {
namespace {

// A legacy row mapped into the ledger's terms. ts is the row's exact first
// field (the generation marker: one append's rows share it).
struct FoldRow {
  std::string day;
  std::string ts;
  std::string meal;
  std::string item;
  double kcal = 0;
  double protein = 0;
};

bool isNum(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end && *end == '\0';
}

// A header row's first field names a column ("timestamp,meal,item,..."); a
// data row's first field is a date. Anything that is neither is unmappable.
bool headerWord(const std::string& f) {
  for (const char* w : {"date", "datetime", "timestamp", "time", "day"})
    if (f == w) return true;
  return false;
}

// The meal words a legacy row may carry in its second field. "afternoon" and
// "snack" pass through as meals of their own (the owner's own words).
const char* const kMealWords[] = {"breakfast", "brunch",   "lunch", "afternoon",
                                  "snack",      "tea",      "dinner", "supper",
                                  "evening",    "midnight"};
bool mealWord(const std::string& f) {
  for (const char* w : kMealWords)
    if (f == w) return true;
  return false;
}

// A meal inferred from the time of day (no meal word on the row). A row with
// no time at all and no meal word is not mapped - the day is known but the
// slot is not, and a wrong slot would double a meal.
std::string mealOfTime(const std::string& time) {
  int hh = 0, mm = 0;
  if (std::sscanf(time.c_str(), "%d:%d", &hh, &mm) != 2) return std::string();
  if (hh < 11) return "breakfast";
  if (hh < 15 || (hh == 15 && mm < 30)) return "lunch";
  if (hh < 20) return "dinner";
  return "evening";
}

// "2026-10-29T17:28:00" / "2026-10-29 21:56" -> day "2026-10-29" + time "21:56".
void splitStamp(const std::string& f, std::string& day, std::string& time) {
  const size_t t = f.find('T');
  const size_t sp = f.find(' ');
  const size_t at = t == std::string::npos ? sp : t;
  day = at == std::string::npos ? f : f.substr(0, at);
  time = at == std::string::npos || at + 1 >= f.size() ? std::string() : f.substr(at + 1);
}

// Split one legacy row. A comma INSIDE parentheses does not split: the real
// file's item names carry them ("Boba tea (no sugar, honey tapioca)") - a
// blind split truncates the item and misreads the columns after it.
std::vector<std::string> splitCsv(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (char c : line) {
    if (c == ',' && depth == 0) {
      out.push_back(cur);
      cur.clear();
    } else {
      if (c == '(') depth++;
      if (c == ')' && depth > 0) depth--;
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

// One legacy line mapped, or why it cannot be.
bool foldRow(const std::string& line, FoldRow& r) {
  const std::vector<std::string> f = splitCsv(line);
  if (f.size() < 4) return false;
  if (f[0].find_first_of("0123456789") == std::string::npos) return false;   // a header or prose
  if (headerWord(f[0])) return false;
  std::string time;
  splitStamp(f[0], r.day, time);
  if (r.day.size() != 10 || r.day[4] != '-') return false;                  // no real date
  r.ts = f[0];
  if (mealWord(f[1])) {
    // <stamp, meal, item, grams, kcal, protein[, ...]>
    if (f.size() < 6) return false;
    r.meal = f[1];
    r.item = f[2];
    if (!isNum(f[4], r.kcal) || !isNum(f[5], r.protein)) return false;
    return true;
  }
  r.item = f[1];
  if (f.size() == 4) {
    // <stamp, item, kcal, protein>
    if (!isNum(f[2], r.kcal) || !isNum(f[3], r.protein)) return false;
  } else {
    // <stamp, item, amount, kcal, protein[, ...]>
    if (!isNum(f[3], r.kcal) || !isNum(f[4], r.protein)) return false;
  }
  r.meal = mealOfTime(time);
  return !r.meal.empty();
}

// The meals in eating order; an unknown word sorts last, alphabetically.
int mealRank(const std::string& m) {
  for (size_t i = 0; i < sizeof(kMealWords) / sizeof(kMealWords[0]); i++)
    if (m == kMealWords[i]) return (int)i;
  return 100;
}

std::string numText(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

// ---- the fold -----------------------------------------------------------------

using LedgerKey = std::pair<std::string, std::string>;   // (day, meal)
struct Gen {
  std::map<std::string, FoldRow> items;   // item -> the row that wins
  size_t lastPos = 0;                    // the generation's last row's position
};
using GenMap = std::map<LedgerKey, std::map<std::string, Gen>>;
struct Final {
  std::string items;
  double kcal = 0, protein = 0;
};
using FinalMap = std::map<LedgerKey, Final>;

// Parse and group the legacy rows: (day, meal) -> timestamp (one append's
// generation) -> item -> the row that wins (rows arrive in file order, so the
// later row replaces the earlier). Unmappable rows are counted, never guessed.
GenMap groupLegacy(const std::string& legacyText, size_t& dropped) {
  GenMap keyed;
  size_t pos = 0;
  size_t at = 0;
  while (at < legacyText.size()) {
    const size_t nl = legacyText.find('\n', at);
    const std::string line =
        legacyText.substr(at, (nl == std::string::npos ? legacyText.size() : nl) - at);
    at = nl == std::string::npos ? legacyText.size() : nl + 1;
    const size_t thisPos = pos++;
    if (line.empty()) continue;
    FoldRow r;
    if (!foldRow(line, r)) {
      dropped++;
      continue;
    }
    Gen& g = keyed[{r.day, r.meal}][r.ts];
    g.lastPos = thisPos;
    g.items[r.item] = r;
  }
  return keyed;
}

// The winning generation of each key (the one whose last row sits latest in
// the file) joined into one ledger row: the surviving items, summed.
FinalMap foldWinners(const GenMap& keyed) {
  FinalMap merged;
  for (const auto& kv : keyed) {
    const auto& gens = kv.second;
    if (gens.empty()) continue;
    const std::string* winTs = nullptr;
    size_t winPos = 0;
    for (const auto& g : gens)
      if (g.second.lastPos >= winPos) {
        winPos = g.second.lastPos;
        winTs = &g.first;
      }
    Final out;
    for (const auto& it : gens.at(*winTs).items) {
      if (!out.items.empty()) out.items += "; ";
      out.items += it.first;
      out.kcal += it.second.kcal;
      out.protein += it.second.protein;
    }
    merged[kv.first] = out;
  }
  return merged;
}

// The designed ledger's own rows overlay the fold: they are the LATEST
// generation of their keys (written after the legacy file), so a designed row
// is never overwritten by a stale legacy one.
void overlayLedger(const std::string& existingLedger, FinalMap& merged) {
  size_t eat = 0;
  while (eat < existingLedger.size()) {
    const size_t nl = existingLedger.find('\n', eat);
    std::string line =
        existingLedger.substr(eat, (nl == std::string::npos ? existingLedger.size() : nl) - eat);
    eat = nl == std::string::npos ? existingLedger.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || headerWord(line.substr(0, line.find(',')))) continue;
    const std::vector<std::string> f = splitCsv(line);
    if (f.size() < 5) continue;
    double kcal = 0, protein = 0;
    if (!isNum(f[3], kcal) || !isNum(f[4], protein)) continue;
    Final out;
    out.items = f[2];
    out.kcal = kcal;
    out.protein = protein;
    merged[{f[0], f[1]}] = out;
  }
}

// Sort by day, then by the eating order of meals, and render the whole file.
std::string renderLedger(const FinalMap& merged, int& folded) {
  std::vector<std::pair<LedgerKey, Final>> rows(merged.begin(), merged.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    if (a.first.first != b.first.first) return a.first.first < b.first.first;
    const int ra = mealRank(a.first.second), rb = mealRank(b.first.second);
    if (ra != rb) return ra < rb;
    return a.first.second < b.first.second;
  });
  std::string out = std::string(kLedgerHeader) + "\n";
  for (const auto& kv : rows) {
    out += kv.first.first + "," + kv.first.second + "," + kv.second.items + "," +
           numText(kv.second.kcal) + "," + numText(kv.second.protein) + "\n";
    folded++;
  }
  return out;
}

}  // namespace

LedgerMigration planLedgerMigration(const std::string& legacyText,
                                    const std::string& existingLedger) {
  LedgerMigration m;
  size_t dropped = 0;
  const GenMap keyed = groupLegacy(legacyText, dropped);
  FinalMap merged = foldWinners(keyed);
  if (!existingLedger.empty()) overlayLedger(existingLedger, merged);
  m.out = renderLedger(merged, m.folded);
  m.dropped = (int)dropped;
  m.ok = true;
  return m;
}

}  // namespace nimbus::orch
