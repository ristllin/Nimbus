#include "nimbus/orch/ledger_migrate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <vector>

#include "nimbus/orch/file_append.h"   // csvHeaderOf, csvStoredRow, csvFigure: read as the day total reads

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
  bool proteinKnown = true;   // false: the row's protein cell is blank, not 0 g
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

// One quoted cell, `at` just past its opening quote, left on the separator after it.
// False when the quote never closes or text follows it: the row's columns are unknowable.
bool readQuotedCell(const std::string& line, size_t& at, std::string& cell) {
  for (; at < line.size(); at++) {
    if (line[at] != '"') {
      cell += line[at];
    } else if (at + 1 < line.size() && line[at + 1] == '"') {
      cell += '"';   // "" inside quotes is one quote
      at++;
    } else {
      at = line.find_first_not_of(' ', at + 1);
      if (at == std::string::npos) at = line.size();
      return at == line.size() || line[at] == ',';
    }
  }
  return false;
}

// One plain cell, left on the separator after it. A comma INSIDE parentheses does not
// split: the real file's item names carry them ("Boba tea (no sugar, honey tapioca)") - a
// blind split truncates the item and misreads the columns after it. Blanks stay: a cell
// that starts with one is the tail of an item split at its comma (quantityCell).
void readPlainCell(const std::string& line, size_t& at, std::string& cell) {
  int depth = 0;
  for (; at < line.size(); at++) {
    const char c = line[at];
    if (c == ',' && depth == 0) return;
    if (c == '(') depth++;
    if (c == ')' && depth > 0) depth--;
    cell += c;
  }
}

// Split one legacy row. A cell in double quotes may hold commas (a model writing standard
// CSV quotes such an item). Empty when a quote leaves the row's columns unknowable, which
// no schema maps.
std::vector<std::string> splitCsv(const std::string& line) {
  std::vector<std::string> out;
  size_t at = 0;
  for (;;) {
    std::string cell;
    const size_t text = line.find_first_not_of(' ', at);
    if (text != std::string::npos && line[text] == '"') {
      at = text + 1;
      if (!readQuotedCell(line, at, cell)) return {};
    } else {
      readPlainCell(line, at, cell);
    }
    out.push_back(cell);
    if (at >= line.size()) return out;
    at++;   // the separator
  }
}

// A quantity column ("150", "150 g", "1 cup"). An item split at a comma it did not quote
// shifts every column after it, and the figures would still read as numbers (grams as kcal):
// this cell is where the shift shows, and the row then drops. The schemas have open tails
// (fixture: nine columns), so a column count cannot show it.
bool quantityCell(const std::string& f) {
  return !f.empty() && ((f[0] >= '0' && f[0] <= '9') || f[0] == '.');
}

// <stamp, meal, item, grams, kcal, protein[, ...]>
bool foldMealRow(const std::vector<std::string>& f, FoldRow& r) {
  if (f.size() < 6 || !quantityCell(f[3])) return false;
  r.meal = f[1];
  r.item = f[2];
  return isNum(f[4], r.kcal) && isNum(f[5], r.protein);
}

// <stamp, item, kcal, protein> or <stamp, item, amount, kcal, protein[, ...]>: the meal
// comes from the time of day.
bool foldTimedRow(const std::vector<std::string>& f, const std::string& time, FoldRow& r) {
  const size_t figures = f.size() == 4 ? 2 : 3;
  if (figures == 3 && !quantityCell(f[2])) return false;
  r.item = f[1];
  if (!isNum(f[figures], r.kcal) || !isNum(f[figures + 1], r.protein)) return false;
  r.meal = mealOfTime(time);
  return !r.meal.empty();
}

// One legacy line mapped, or why it cannot be.
bool foldRow(const std::string& line, FoldRow& r) {
  const std::vector<std::string> f = splitCsv(line);
  if (f.size() < 4) return false;
  if (f[0].find_first_of("0123456789") == std::string::npos) return false;   // a header or prose
  std::string time;
  splitStamp(f[0], r.day, time);
  if (r.day.size() != 10 || r.day[4] != '-') return false;                  // no real date
  r.ts = f[0];
  return mealWord(f[1]) ? foldMealRow(f, r) : foldTimedRow(f, time, r);
}

// A header row holds no figures: dropping one loses nothing.
bool headerRow(const std::string& line) {
  const std::vector<std::string> f = splitCsv(line);
  return !f.empty() && headerWord(f[0]);
}

// ---- a legacy file with a header row (31 Oct: the head rebuilt it keyed by hand) ---------
struct LegacyHeader {
  std::vector<std::string> names;
  int date = -1, key = -1, item = -1, kcal = -1, protein = -1;
  bool usable() const { return kcal >= 0 && protein >= 0 && (date >= 0 || key >= 0) && (key >= 0 || item >= 0); }
};

bool nameHolds(const std::string& name, std::initializer_list<const char*> parts) {
  for (const char* p : parts)
    if (name.find(p) != std::string::npos) return true;
  return false;
}

std::string lowerText(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

LegacyHeader readLegacyHeader(const std::string& line) {
  LegacyHeader h;
  h.names = csvHeaderOf(line);
  for (size_t i = 0; i < h.names.size(); i++) {
    const std::string n = lowerText(h.names[i]);
    const int c = (int)i;
    if (h.date < 0 && nameHolds(n, {"date", "day", "time"})) h.date = c;
    else if (h.key < 0 && nameHolds(n, {"meal", "key", "entry", "slot"})) h.key = c;
    else if (h.item < 0 && nameHolds(n, {"item", "food", "desc", "what", "dish"})) h.item = c;
    else if (h.kcal < 0 && nameHolds(n, {"kcal", "calorie"})) h.kcal = c;
    else if (h.protein < 0 && nameHolds(n, {"protein"})) h.protein = c;
  }
  return h;
}

// "2026-11-01-drink" names the day and the meal; a plain "drink" only the meal.
void splitDatedKey(const std::string& key, std::string& day, std::string& meal) {
  const bool dated = key.size() > 11 && isoDayOf(key.substr(0, 10)) == key.substr(0, 10) && key[10] == '-';
  if (dated && day.empty()) day = key.substr(0, 10);
  meal = dated ? key.substr(11) : key;
}

// One row by its header's names. A total row and a row whose figures cannot be read map
// to nothing; a blank protein stays unknown.
bool foldHeaderedRow(const LegacyHeader& h, const std::string& line, size_t pos, FoldRow& r) {
  const std::vector<std::string> f = csvStoredRow(line, h.names);
  if (f.size() != h.names.size() || csvStatesTotal(f)) return false;
  if (h.date >= 0) r.day = isoDayOf(f[(size_t)h.date]);
  splitDatedKey(h.key >= 0 ? f[(size_t)h.key] : std::string(), r.day, r.meal);
  if (r.day.empty() || !csvFigure(f[(size_t)h.kcal], r.kcal)) return false;
  r.item = h.item >= 0 ? f[(size_t)h.item] : r.meal;
  if (r.item.empty()) r.item = r.meal;
  std::replace(r.item.begin(), r.item.end(), ',', ';');   // the ledger is comma-separated
  if (r.meal.empty()) {
    std::string day, time;
    if (h.date >= 0) splitStamp(f[(size_t)h.date], day, time);
    r.meal = mealOfTime(time);
    if (r.meal.empty()) return false;
  }
  r.proteinKnown = h.protein >= 0 && csvFigure(f[(size_t)h.protein], r.protein);
  r.ts = std::to_string(pos);   // keyed by hand: the last row of a meal is its last word
  return true;
}

// A row that is no meal: a header, or a total the rows already sum to. Dropping one loses
// no figure, so it never keeps the source.
bool holdsNoMeal(const LegacyHeader& h, const std::string& line) {
  return headerRow(line) || (h.usable() && csvStatesTotal(csvStoredRow(line, h.names)));
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
  bool proteinKnown = true;
};
using FinalMap = std::map<LedgerKey, Final>;

// Parse and group the legacy rows: (day, meal) -> timestamp (one append's
// generation) -> item -> the row that wins (rows arrive in file order, so the
// later row replaces the earlier). Unmappable rows are counted, never guessed.
GenMap groupLegacy(const std::string& legacyText, LedgerMigration& m) {
  GenMap keyed;
  size_t pos = 0;
  size_t at = 0;
  LegacyHeader header;
  while (at < legacyText.size()) {
    const size_t nl = legacyText.find('\n', at);
    std::string line =
        legacyText.substr(at, (nl == std::string::npos ? legacyText.size() : nl) - at);
    at = nl == std::string::npos ? legacyText.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t thisPos = pos++;
    if (line.empty()) continue;
    if (thisPos == 0 && line.find_first_of("0123456789") == std::string::npos) {
      header = readLegacyHeader(line);   // the first row names its columns
      if (header.usable()) continue;
    }
    FoldRow r;
    const bool read = header.usable() ? foldHeaderedRow(header, line, thisPos, r) : foldRow(line, r);
    if (!read) {
      m.dropped++;
      if (!holdsNoMeal(header, line)) m.droppedData++;
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
      out.proteinKnown = out.proteinKnown && it.second.proteinKnown;
    }
    merged[kv.first] = out;
  }
  return merged;
}

// The designed ledger's own rows stay exactly as stored and win their keys: a legacy row of the
// same day and meal is older than the row written to the ledger. Only legacy rows of other keys
// are added. (Rewriting the ledger from a parse would lose what the parse cannot hold: a quoted
// comma, two rows of one meal.)
void dropLedgerKeys(const std::string& existingLedger, FinalMap& merged) {
  std::vector<std::string> header;
  size_t at = 0;
  while (at < existingLedger.size()) {
    const size_t nl = existingLedger.find('\n', at);
    std::string line = existingLedger.substr(at, (nl == std::string::npos ? existingLedger.size() : nl) - at);
    at = nl == std::string::npos ? existingLedger.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (header.empty()) {
      header = csvHeaderOf(line);
      continue;
    }
    const std::vector<std::string> f = csvStoredRow(line, header);
    if (f.size() >= 2) merged.erase({isoDayOf(f[0]), f[1]});
  }
}

// The items cell of a ledger row. It stays plain when every reader takes it as one field: no
// quote, and any comma shielded by parentheses that close. Otherwise it goes in quotes.
std::string itemsCell(const std::string& items) {
  int depth = 0;
  bool plain = items.find('"') == std::string::npos;
  bool comma = false;
  for (char c : items) {
    if (c == ',') {
      comma = true;
      plain = plain && depth > 0;
    }
    if (c == '(') depth++;
    if (c == ')' && depth > 0) depth--;
  }
  if (plain && (!comma || depth == 0)) return items;
  std::string out = "\"";
  for (char c : items) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

// Sort by day, then by the eating order of meals, and render the whole file.
std::string renderLedger(const FinalMap& merged, int& folded, const std::string& existingLedger) {
  std::vector<std::pair<LedgerKey, Final>> rows(merged.begin(), merged.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    if (a.first.first != b.first.first) return a.first.first < b.first.first;
    const int ra = mealRank(a.first.second), rb = mealRank(b.first.second);
    if (ra != rb) return ra < rb;
    return a.first.second < b.first.second;
  });
  std::string out = existingLedger.empty() ? std::string(kLedgerHeader) + "\n" : existingLedger;
  if (!out.empty() && out.back() != '\n') out += '\n';
  for (const auto& kv : rows) {
    out += kv.first.first + "," + kv.first.second + "," + itemsCell(kv.second.items) + "," +
           numText(kv.second.kcal) + "," + (kv.second.proteinKnown ? numText(kv.second.protein) : std::string()) + "\n";
    folded++;
  }
  return out;
}

}  // namespace

LedgerMigration planLedgerMigration(const std::string& legacyText,
                                    const std::string& existingLedger) {
  LedgerMigration m;
  if (legacyFoldedAlready(legacyText)) {
    m.ok = true;   // its rows had their one fold: what the ledger did to them since stands
    return m;
  }
  const GenMap keyed = groupLegacy(legacyText, m);
  FinalMap merged = foldWinners(keyed);
  if (!existingLedger.empty()) {
    // A ledger in other columns cannot take these rows: nothing is folded, nothing is lost.
    const size_t eol = existingLedger.find('\n');
    std::string first = existingLedger.substr(0, eol);
    if (!first.empty() && first.back() == '\r') first.pop_back();
    if (first != kLedgerHeader) {
      m.ok = true;
      return m;
    }
    dropLedgerKeys(existingLedger, merged);
  }
  m.out = renderLedger(merged, m.folded, existingLedger);
  m.ok = true;
  return m;
}

bool legacyFoldedAlready(const std::string& legacyText) {
  const std::string mark = std::string("\n") + kLegacyFoldedMark;
  return legacyText.find(mark) != std::string::npos;
}

std::string markLegacyFolded(const std::string& legacyText) {
  std::string out = legacyText;
  if (!out.empty() && out.back() != '\n') out += '\n';
  return out + kLegacyFoldedMark + "\n";
}

bool legacyLogFoldable(const FileStore& store, const FileEntry& legacy) {
  return store.ownedBy(legacy, kOwnerNs, false);
}

LedgerMigration planLedgerFold(const std::string& legacyText, size_t legacyOnDisk, bool ledgerStored,
                               const std::string& ledgerText, size_t ledgerOnDisk) {
  LedgerMigration m;
  if (legacyOnDisk > kLedgerFoldCap || (ledgerStored && ledgerOnDisk > kLedgerFoldCap)) {
    m.err = "too large to fold safely";
    return m;
  }
  if (legacyText.size() != legacyOnDisk || (ledgerStored && ledgerText.size() != ledgerOnDisk)) {
    m.err = "a file could not be read whole";
    return m;
  }
  return planLedgerMigration(legacyText, ledgerStored ? ledgerText : std::string());
}

}  // namespace nimbus::orch
