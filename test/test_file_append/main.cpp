#include <unity.h>

#include <algorithm>
#include <cstring>

#include <string>
#include <vector>

#include "nimbus/orch/file_append.h"

// files.append and the files.read tail view (file_append.h): the class rules.
//   - an append never rewrites a stored byte: the file before is always a byte
//     prefix of the file after, whatever the text (CR/LF mixes, no final line
//     end, a cut last row);
//   - the tail returns exactly the last N complete rows of whatever window of the
//     file it is given, and says when earlier rows exist;
//   - a cut file (no final line end) is reported, never silently truncated or
//     joined to the next row.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// The device's append: the plan's bytes go on the end of the file, nothing else.
static bool appendTo(std::string& file, const std::string& text, AppendPlan* out = nullptr) {
  const AppendPlan p = planAppend(text, file.size(), file.empty() ? '\0' : file.back());
  if (out) *out = p;
  if (!p.ok) return false;
  file += p.bytes;
  return true;
}

static void test_append_never_rewrites_a_stored_byte() {
  const std::vector<std::string> texts = {
      "date,meal,kcal",          "2026-10-19,lunch,640",       "a\r\nb\r\n",
      "c\rd",                    "e\n",                        "\n\nf,g\n\n",
      "x,y,z\n2026-10-19,tea,5", std::string(4000, 'q'),       "last"};
  for (const std::string& seed : {std::string(), std::string("header"), std::string("row\n"),
                                  std::string("cut row with no end")}) {
    std::string file = seed;
    for (const auto& t : texts) {
      const std::string before = file;
      AppendPlan p;
      TEST_ASSERT_TRUE_MESSAGE(appendTo(file, t, &p), p.err.c_str());
      TEST_ASSERT_TRUE_MESSAGE(file.compare(0, before.size(), before) == 0, "an append rewrote stored bytes");
      TEST_ASSERT_EQUAL_CHAR('\n', file.back());                  // always ends a row
      TEST_ASSERT_TRUE(file.find('\r', before.size()) == std::string::npos);   // normalized
      TEST_ASSERT_TRUE(p.rows >= 1);
    }
  }
}

static void test_append_refuses_empty_and_oversize() {
  std::string file = "a\n";
  AppendPlan p;
  TEST_ASSERT_FALSE(appendTo(file, "", &p));
  TEST_ASSERT_FALSE(appendTo(file, " \n\t", &p));
  TEST_ASSERT_TRUE(p.err.find("need 'text'") == 0);
  TEST_ASSERT_FALSE(appendTo(file, std::string(kAppendMaxBytes + 1, 'z'), &p));
  TEST_ASSERT_TRUE(p.err.find("too large") != std::string::npos);
  // The cap counts the bytes as written, the added final line end included.
  TEST_ASSERT_FALSE(appendTo(file, std::string(kAppendMaxBytes, 'z'), &p));
  TEST_ASSERT_TRUE(appendTo(file, std::string(kAppendMaxBytes - 1, 'z'), &p));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kAppendMaxBytes, (uint32_t)p.bytes.size());
  file = "a\n";
  TEST_ASSERT_FALSE(appendTo(file, "", &p));
  TEST_ASSERT_EQUAL_STRING("a\n", file.c_str());   // a refused append writes nothing
}

static void test_cut_file_is_reported_not_joined() {
  std::string file = "date,kcal\n2026-10-18,1800\n2026-10-19,9";   // power cut mid-row
  const TailView v = tailRows(file, 0, 5);
  TEST_ASSERT_TRUE(v.lastCut);
  TEST_ASSERT_EQUAL_STRING("2026-10-19,9", v.rows.back().c_str());
  const std::string shown = renderTail(v, "log.csv", file.size(), "");
  TEST_ASSERT_TRUE(shown.find("the last row is incomplete") != std::string::npos);
  AppendPlan p;
  TEST_ASSERT_TRUE(appendTo(file, "2026-10-19,640", &p));
  TEST_ASSERT_TRUE(p.repairedCut);
  TEST_ASSERT_EQUAL(1, p.rows);
  // The cut row stays as it was, on its own line; the new row starts a fresh one.
  TEST_ASSERT_EQUAL_STRING("date,kcal\n2026-10-18,1800\n2026-10-19,9\n2026-10-19,640\n", file.c_str());
  TEST_ASSERT_FALSE(tailRows(file, 0, 5).lastCut);
}

// Over every window start and every N: the rows returned are exactly the last N
// complete rows inside the window, and `earlier` is set whenever any row of the
// file is not shown.
static void test_tail_returns_exactly_the_last_n_rows() {
  std::string file;
  std::vector<std::string> rows;
  for (int i = 0; i < 40; i++) {
    rows.push_back("2026-10-" + std::to_string(10 + i % 20) + ",row" + std::to_string(i) +
                   std::string((size_t)(i * 7 % 23), 'x'));
    file += rows.back() + "\n";
  }
  for (size_t start = 0; start < file.size(); start += 13) {
    const std::string window = file.substr(start);
    // The rows the window holds whole: a window that starts mid-file drops
    // everything up to its first line end, so a row counts when the line end
    // before it is inside the window (the device reads one byte early for that).
    size_t firstFull = rows.size(), at = 0;
    for (size_t i = 0; i < rows.size(); i++) {
      if (start == 0 || at > start) { firstFull = i; break; }
      at += rows[i].size() + 1;
    }
    const size_t inWindow = rows.size() - firstFull;
    for (size_t n : {1u, 5u, 20u, 39u, 40u, 41u, 500u}) {
      const TailView v = tailRows(window, start, n);
      const size_t want = std::min(std::min(n, kTailMaxRows), inWindow);
      TEST_ASSERT_EQUAL_UINT32((uint32_t)want, (uint32_t)v.rows.size());
      for (size_t k = 0; k < want; k++)
        TEST_ASSERT_EQUAL_STRING(rows[rows.size() - want + k].c_str(), v.rows[k].c_str());
      TEST_ASSERT_EQUAL(want < rows.size(), v.earlier);
      TEST_ASSERT_FALSE(v.lastCut);
    }
  }
}

static void test_tail_render_names_what_is_shown() {
  const std::string file = "date,meal,kcal\na,b,1\nc,d,2\ne,f,3\n";
  const TailView all = tailRows(file, 0, 10);
  const std::string whole = renderTail(all, "food-log.csv", file.size(), "date,meal,kcal");
  TEST_ASSERT_TRUE(whole.find("last 4 rows of food-log.csv (") == 0);
  TEST_ASSERT_TRUE(whole.find("the whole file") != std::string::npos);
  TEST_ASSERT_TRUE(whole.find("header:") == std::string::npos);   // already in the rows
  const TailView two = tailRows(file, 0, 2);
  const std::string part = renderTail(two, "food-log.csv", file.size(), "date,meal,kcal");
  TEST_ASSERT_TRUE(part.find("earlier rows not shown") != std::string::npos);
  TEST_ASSERT_TRUE(part.find("header: date,meal,kcal\nc,d,2\ne,f,3\n") != std::string::npos);
}

// ---- G1 (rows 32/33): the keyed CSV upsert and the day total ------------------
static void test_g1_keyed_upsert_updates_by_key_and_appends_new() {
  const std::string ex = "date,meal,kcal\n2026-10-23,lunch,450\n2026-10-22,dinner,900\n";
  const std::string nt = "date,meal,kcal\n2026-10-23,lunch,520\n2026-10-23,snack,180\n";
  const nimbus::orch::CsvUpsert up =
      nimbus::orch::planCsvUpsert(ex, nt, "meal", "kcal", "2026-10-23");
  TEST_ASSERT_TRUE(up.ok);
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  TEST_ASSERT_EQUAL_INT(1, up.appended);
  TEST_ASSERT_TRUE(up.changed);
  TEST_ASSERT_TRUE(up.out.find("2026-10-23,lunch,520") != std::string::npos);
  // The replaced row is kept as a #replaced: history row, never as a data row.
  TEST_ASSERT_TRUE(up.out.find("#replaced: 2026-10-23,lunch,450\n") != std::string::npos);
  TEST_ASSERT_TRUE(up.out.find("\n2026-10-23,lunch,450\n") == std::string::npos);
  TEST_ASSERT_TRUE(up.out.find("2026-10-22,dinner,900") != std::string::npos);
  // One data row per key: the header, the #replaced: history row, lunch, the
  // 22nd's dinner and the new snack, and "lunch" appears exactly once as a row.
  TEST_ASSERT_EQUAL_INT(5, (int)std::count(up.out.begin(), up.out.end(), '\n'));
  size_t at = 0, lunches = 0;
  while ((at = up.out.find("\n2026-10-23,lunch,", at)) != std::string::npos) { lunches++; at++; }
  TEST_ASSERT_EQUAL_INT(1, (int)lunches);   // once as a data row; the #replaced: line does not match
  // A new file adopts the text's header as its first row.
  const nimbus::orch::CsvUpsert fresh =
      nimbus::orch::planCsvUpsert("", nt, "meal", "kcal", "2026-10-23");
  TEST_ASSERT_TRUE(fresh.ok);
  TEST_ASSERT_EQUAL_INT(2, fresh.appended);
  TEST_ASSERT_TRUE(fresh.out.rfind("date,meal,kcal\n", 0) == 0);
}

static void test_g1_upsert_reports_the_day_total() {
  const std::string ex = "date,meal,kcal\n2026-10-23,lunch,450\n2026-10-22,dinner,900\n";
  const std::string nt = "date,meal,kcal\n2026-10-23,dinner,700\n";
  const nimbus::orch::CsvUpsert up =
      nimbus::orch::planCsvUpsert(ex, nt, "meal", "kcal", "2026-10-23");
  TEST_ASSERT_TRUE(up.ok && up.hasTotal);
  TEST_ASSERT_TRUE(up.todayTotal > 1149.9 && up.todayTotal < 1150.1);   // 450 lunch + 700 dinner
  // Without a sum column there is no total, and no error.
  const nimbus::orch::CsvUpsert noSum =
      nimbus::orch::planCsvUpsert(ex, nt, "meal", "", "2026-10-23");
  TEST_ASSERT_TRUE(noSum.ok);
  TEST_ASSERT_FALSE(noSum.hasTotal);
  // A "day" column works too, as the key of a one-row-per-day ledger; a number
  // with a unit after it is a plain number.
  const std::string ex2 = "day,kcal\n2026-10-23,300 kcal\n";
  const nimbus::orch::CsvUpsert u2 =
      nimbus::orch::planCsvUpsert(ex2, "day,kcal\n2026-10-23,300 kcal\n", "day", "kcal", "2026-10-23");
  TEST_ASSERT_TRUE_MESSAGE(u2.ok && u2.hasTotal, u2.err.c_str());
  TEST_ASSERT_EQUAL_INT(1, u2.replaced);
  TEST_ASSERT_FALSE(u2.changed);   // the same row again: nothing to write
  TEST_ASSERT_TRUE(u2.todayTotal > 299.9 && u2.todayTotal < 300.1);
}


// v4.6.3 (the 30 Oct investigation): creating a keyed-eligible CSV without a
// header row is refused with the fix named - a headerless CSV can never be
// updated by key, so every later "correction" would append a duplicate (the
// exact loop the investigation documented). A header row (no digits) passes;
// an existing file never hits the bootstrap; a non-CSV never does either.
static void test_csv_creation_needs_its_header_row_first() {
  // A data row first: refused, and the reason names the header and the way out.
  nimbus::orch::AppendPlan p =
      nimbus::orch::planAppend("2026-10-30,breakfast,Yogurt,175,30\n", 0, '\0', true);
  TEST_ASSERT_FALSE(p.ok);
  TEST_ASSERT_TRUE(p.err.find("header row first") != std::string::npos);
  TEST_ASSERT_TRUE(p.err.find("cannot be updated by key") != std::string::npos);
  // The header row first (column names, no digits): the append plans normally.
  p = nimbus::orch::planAppend("date,meal,items,kcal,protein_g\n", 0, '\0', true);
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_EQUAL(1, p.rows);
  // Header AND rows in one call: fine (the header is the first row).
  p = nimbus::orch::planAppend(
      "date,meal,items,kcal,protein_g\n2026-10-30,breakfast,Yogurt,175,30\n", 0, '\0', true);
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_EQUAL(2, p.rows);
  // An existing file never hits the bootstrap: csvCreate is the CALLER's
  // "this append creates the file" (the device passes it only when the store
  // holds no such file); for a stored file the caller passes false and the
  // append keeps its old semantics.
  p = nimbus::orch::planAppend("2026-10-30,lunch,Soup,260,14\n", 41, '\n', false);
  TEST_ASSERT_TRUE(p.ok);
  // A non-CSV create is none of this rule's business.
  p = nimbus::orch::planAppend("2026-10-30 something happened today\n", 0, '\0', false);
  TEST_ASSERT_TRUE(p.ok);
}

// The merge-back obligation: a migrated ledger holds items with a comma inside
// parentheses and no quotes (ledger_migrate.h, on devices since v4.6.3). Every reader
// of a stored row must keep such an item in one field, or the row drops out of every
// total and can never be corrected by key. An unbalanced '(' shields nothing.
static void test_a_comma_inside_parentheses_stays_in_its_field() {
  const std::string h = "date,meal,items,kcal,protein_g\n";
  const std::string row = "2026-10-29,evening,Boba tea (no sugar, honey tapioca),300,1\n";
  double total = 0;
  TEST_ASSERT_TRUE(nimbus::orch::csvDayTotal(h + row, "kcal", "2026-10-29", total));
  TEST_ASSERT_TRUE(total > 299.9 && total < 300.1);
  // The tally counts it and reports nothing unread.
  nimbus::orch::CsvDayTally tally("kcal", "2026-10-29");
  tally.feed("date,meal,items,kcal,protein_g", true);
  tally.feed("2026-10-29,evening,Boba tea (no sugar, honey tapioca),300,1", true);
  tally.feed("2026-10-29,lunch,Soup (a, b (c, d)),100,2", true);
  TEST_ASSERT_EQUAL(2, tally.total().rows);
  TEST_ASSERT_EQUAL(0, tally.total().unread);
  // A keyed correction replaces it in place.
  const nimbus::orch::CsvUpsert fix = nimbus::orch::planCsvUpsert(
      h + row, h + "2026-10-29,evening,Boba tea (no sugar),250,1\n", "meal", "kcal", "2026-10-29");
  TEST_ASSERT_TRUE_MESSAGE(fix.ok, fix.err.c_str());
  TEST_ASSERT_EQUAL(1, fix.replaced);
  TEST_ASSERT_EQUAL(0, fix.appended);
  TEST_ASSERT_TRUE(fix.todayTotal > 249.9 && fix.todayTotal < 250.1);
  // The model may write one too: it is one field, not a refused row.
  const nimbus::orch::CsvUpsert fresh = nimbus::orch::planCsvUpsert(
      h, h + "2026-10-29,lunch,Rice (white, cooked),200,4\n", "meal", "kcal", "2026-10-29");
  TEST_ASSERT_TRUE_MESSAGE(fresh.ok, fresh.err.c_str());
  TEST_ASSERT_EQUAL(1, fresh.appended);
  // An unbalanced '(' keeps today's reading: the comma splits, so the row is too wide
  // for the header and is refused, never silently swallowed into one field.
  const nimbus::orch::CsvUpsert open = nimbus::orch::planCsvUpsert(
      h, h + "2026-10-29,lunch,Rice (white, cooked,200,4\n", "meal", "kcal", "2026-10-29");
  TEST_ASSERT_FALSE(open.ok);
  TEST_ASSERT_TRUE(open.err.find("double quotes") != std::string::npos);
  // Thousands of open parentheses cost linear time and read as plain commas.
  std::string opens = "2026-10-29,lunch,";
  for (int i = 0; i < 4000; i++) opens += "(,";
  const nimbus::orch::CsvUpsert wide = nimbus::orch::planCsvUpsert(h, h + opens + "\n", "meal", "kcal", "2026-10-29");
  TEST_ASSERT_FALSE(wide.ok);
  // One rule for a CSV name across the device, nimbusd and the lab.
  TEST_ASSERT_TRUE(nimbus::orch::isCsvName("log.csv"));
  TEST_ASSERT_TRUE(nimbus::orch::isCsvName("Log.CSV"));
  TEST_ASSERT_FALSE(nimbus::orch::isCsvName(".csv"));
  TEST_ASSERT_FALSE(nimbus::orch::isCsvName("log.txt"));
  // A power cut inside the parentheses still reads as a cut row, not a whole one.
  TEST_ASSERT_TRUE(nimbus::orch::csvLastRowCut("date,meal,items,kcal,protein_g", "2026-10-29,lunch,Soup,100,2",
                                               "2026-10-29,dinner,Rice (white, coo"));
}

// A keyed write onto a file created without a header row names the way out: an
// append can never fix it (v4.6.3). A stored header that holds the key column keeps
// the plain "send the stored header" refusal.
static void test_a_headerless_stored_file_refusal_names_the_rewrite_tools() {
  const std::string text = "date,meal,kcal\n2026-10-30,lunch,200\n";
  const nimbus::orch::CsvUpsert headerless =
      nimbus::orch::planCsvUpsert("2026-10-30 08:00,Yogurt,175\n", text, "meal", "kcal", "2026-10-30");
  TEST_ASSERT_FALSE(headerless.ok);
  TEST_ASSERT_TRUE(headerless.err.find("files.edit") != std::string::npos);
  TEST_ASSERT_TRUE(headerless.err.find("artifact.save") != std::string::npos);
  const nimbus::orch::CsvUpsert reordered =
      nimbus::orch::planCsvUpsert("meal,date,kcal\nlunch,2026-10-30,100\n", text, "meal", "kcal", "2026-10-30");
  TEST_ASSERT_FALSE(reordered.ok);
  TEST_ASSERT_TRUE(reordered.err.find("files.edit") == std::string::npos);
}

static void test_g1_upsert_refuses_a_header_mismatch() {
  const std::string ex = "date,kcal\n2026-10-23,450\n";
  // The text's header has no 'meal' column.
  nimbus::orch::CsvUpsert up =
      nimbus::orch::planCsvUpsert(ex, "date,meal,kcal\nx\n", "meal", "", "2026-10-23");
  TEST_ASSERT_FALSE(up.ok);
  TEST_ASSERT_FALSE(up.err.empty());
  // The stored file has no 'meal' column: refused, never merged blind.
  up = nimbus::orch::planCsvUpsert(ex, "date,meal,kcal\n2026-10-23,lunch,1\n", "meal", "",
                                  "2026-10-23");
  TEST_ASSERT_FALSE(up.ok);
  // A missing key argument is an error.
  up = nimbus::orch::planCsvUpsert(ex, "date,meal\nx\n", "", "", "2026-10-23");
  TEST_ASSERT_FALSE(up.ok);
}

static void test_g1_day_total_matches_only_today() {
  const std::string csv =
      "date,kcal\n2026-10-23 08:00,400\n2026-10-23 13:00,500\n2026-10-22,999\nno-date,1\n";
  double total = 0;
  TEST_ASSERT_TRUE(nimbus::orch::csvDayTotal(csv, "kcal", "2026-10-23", total));
  TEST_ASSERT_TRUE(total > 899.9 && total < 900.1);
  // No date column, no total.
  TEST_ASSERT_FALSE(nimbus::orch::csvDayTotal("meal,kcal\nlunch,5\n", "kcal", "2026-10-23", total));
  // No such sum column.
  TEST_ASSERT_FALSE(nimbus::orch::csvDayTotal(csv, "protein", "2026-10-23", total));
}

// ---- QW3: the food log can never lose rows, mix days or double-count -----------
// The class rules of the keyed upsert (file_append.h), over the whole-text form
// the device's stream shares:
//   - a row is its date plus its key: an update never touches another day's row;
//   - no update drops a row: every stored row is written back, as it was or
//     replaced in place, and the day's other rows survive;
//   - a correction replaces its row, so a day's total never counts it twice;
//   - anything ambiguous or malformed is refused and nothing is written.

namespace {
const char kHead[] = "date,meal,items,kcal,protein_g";

int rowCount(const std::string& file) { return (int)std::count(file.begin(), file.end(), '\n'); }

// Data rows only: history rows (#replaced:, #cut:, #superseded:) are comments,
// never matched or counted.
int dataRowCount(const std::string& file) {
  int n = 0;
  size_t p = 0;
  while (p < file.size()) {
    const size_t nl = file.find('\n', p);
    const std::string row = file.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
    if (!row.empty() && row[0] != '#') n++;
    if (nl == std::string::npos) break;
    p = nl + 1;
  }
  return n;
}

size_t occurrences(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) n++;
  return n;
}

// The file after a keyed write (the stored file when nothing changed).
std::string applied(const std::string& file, const CsvUpsert& up) { return up.changed ? up.out : file; }

double dayTotal(const std::string& file, const std::string& day) {
  double t = -1;
  TEST_ASSERT_TRUE(csvDayTotal(file, "kcal", day, t));
  return t;
}
}  // namespace

// The review's blocker, both halves: key=meal must not overwrite another day's
// row, and key=date must not collapse a day's meals into one.
static void test_qw3_keyed_update_never_touches_another_day() {
  const std::string ex = std::string(kHead) +
                         "\n2026-10-23,dinner,pasta + mozzarella,2422,142\n"
                         "2026-10-24,breakfast,sandwich + omelette,505,28\n";
  // A new day's dinner is ADDED; Friday's dinner stays exactly as it was.
  CsvUpsert up = planCsvUpsert(ex, std::string(kHead) + "\n2026-10-24,dinner,beef + skyr,990,87\n",
                               "meal", "kcal", "2026-10-24");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(0, up.replaced);
  TEST_ASSERT_EQUAL_INT(1, up.appended);
  TEST_ASSERT_TRUE(up.out.find("2026-10-23,dinner,pasta + mozzarella,2422,142\n") != std::string::npos);
  TEST_ASSERT_EQUAL_INT(4, rowCount(up.out));
  TEST_ASSERT_TRUE(up.hasTotal);
  TEST_ASSERT_TRUE(up.todayTotal > 1494.9 && up.todayTotal < 1495.1);   // 505 + 990, not + 2422
  // A correction to yesterday's dinner replaces THAT day's dinner only.
  const std::string after = up.out;
  up = planCsvUpsert(after, std::string(kHead) + "\n2026-10-23,dinner,pasta + mozzarella,2300,140\n",
                     "meal", "kcal", "2026-10-24");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  TEST_ASSERT_TRUE(up.out.find("2026-10-23,dinner,pasta + mozzarella,2300,140\n") != std::string::npos);
  TEST_ASSERT_TRUE(up.out.find("2026-10-24,dinner,beef + skyr,990,87\n") != std::string::npos);
  // The replaced row is kept as a #replaced: history row (5 lines: header, the
  // history row, and the three data rows).
  TEST_ASSERT_TRUE(up.out.find("#replaced: 2026-10-23,dinner,pasta + mozzarella,2422,142\n") != std::string::npos);
  TEST_ASSERT_EQUAL_INT(5, rowCount(up.out));
  TEST_ASSERT_EQUAL_STRING("2026-10-23", up.changes[0].date.c_str());
  TEST_ASSERT_TRUE(up.changes[0].was.find("2422") != std::string::npos);   // the reply can say what it was
  // The reply totals today AND the corrected day.
  TEST_ASSERT_EQUAL_INT(2, (int)up.days.size());
  TEST_ASSERT_EQUAL_STRING("2026-10-24", up.days[0].date.c_str());
  TEST_ASSERT_EQUAL_STRING("2026-10-23", up.days[1].date.c_str());
  TEST_ASSERT_TRUE(up.days[1].total > 2299.9 && up.days[1].total < 2300.1);
  // key=date on a day with several rows is ambiguous: refused, nothing written.
  up = planCsvUpsert(after, std::string(kHead) + "\n2026-10-24,lunch,salmon,807,66\n", "date", "kcal",
                     "2026-10-24");
  TEST_ASSERT_FALSE(up.ok);
  TEST_ASSERT_TRUE(up.err.find("cannot tell them apart") != std::string::npos);
  TEST_ASSERT_TRUE(up.out.empty());
  // ...while key=date on a one-row-per-day ledger replaces that day's row (the
  // replaced row is kept as a #replaced: history row).
  const std::string ledger = "date,kcal\n2026-10-23,2422\n2026-10-24,1495\n";
  up = planCsvUpsert(ledger, "date,kcal\n2026-10-24,1600\n", "date", "kcal", "2026-10-24");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("date,kcal\n2026-10-23,2422\n#replaced: 2026-10-24,1495\n2026-10-24,1600\n",
                           up.out.c_str());
}

// Property: for a seeded mix of days, meals and keyed writes, no update ever
// drops or moves a row - every stored row of every OTHER date is byte-identical
// afterwards, the file grows by exactly the appended rows, and each date+meal
// appears once.
static void test_qw3_no_update_drops_or_moves_a_row() {
  const char* meals[] = {"breakfast", "lunch", "dinner", "snack-1126", "snack-1530"};
  const char* days[] = {"2026-10-22", "2026-10-23", "2026-10-24", "2026-10-25"};
  std::string file = std::string(kHead) + "\n";
  unsigned seed = 7;
  for (int step = 0; step < 400; step++) {
    seed = seed * 1103515245u + 12345u;
    const std::string day = days[(seed >> 8) % 4];
    const std::string meal = meals[(seed >> 12) % 5];
    const std::string kcal = std::to_string(100 + (seed >> 16) % 900);
    const std::string text = std::string(kHead) + "\n" + day + "," + meal + ",x," + kcal + ",10\n";
    const CsvUpsert up = planCsvUpsert(file, text, "meal", "kcal", "2026-10-25");
    TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
    const std::string next = applied(file, up);
    TEST_ASSERT_EQUAL_INT(dataRowCount(file) + up.appended, dataRowCount(next));
    // Every row of another date, or another meal, is still there byte for byte
    // AND in the same order: the write touched only its own date and meal.
    // History rows (#...) are compared separately, below.
    const auto others = [&](const std::string& text) {
      std::string kept;
      size_t p = 0;
      while (p < text.size()) {
        const size_t nl = text.find('\n', p);
        const std::string row = text.substr(p, nl - p);
        p = nl + 1;
        if (row.empty() || row[0] == '#') continue;
        if (row.rfind(day + "," + meal + ",", 0) != 0) kept += row + "\n";
      }
      return kept;
    };
    TEST_ASSERT_EQUAL_STRING(others(file).c_str(), others(next).c_str());
    // Every history row the write left names a row that was in the file before,
    // as a data row or already as a history row (comments are kept as written).
    for (size_t hp = next.find("\n#replaced: "); hp != std::string::npos;
         hp = next.find("\n#replaced: ", hp + 1)) {
      const size_t he = next.find('\n', hp + 1);
      const std::string hrow = next.substr(hp + 12, he - hp - 12);
      const bool inFile = file.find("\n" + hrow + "\n") != std::string::npos ||
                          file.find("\n#replaced: " + hrow + "\n") != std::string::npos ||
                          file.find("\n#cut: " + hrow + "\n") != std::string::npos ||
                          file.find("\n#superseded: " + hrow + "\n") != std::string::npos ||
                          file.rfind(hrow + "\n", 0) == 0;
      TEST_ASSERT_TRUE_MESSAGE(inFile, hrow.c_str());
    }
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)occurrences(next, "\n" + day + "," + meal + ","));
    file = next;
  }
}

// A correction never double-counts: whatever sequence of logs and corrections,
// the day total equals the sum of the LATEST figure per meal.
static void test_qw3_correction_never_double_counts() {
  std::string file;
  const char* meals[] = {"breakfast", "lunch", "dinner"};
  int latest[3] = {0, 0, 0};
  unsigned seed = 3;
  for (int step = 0; step < 120; step++) {
    seed = seed * 1103515245u + 12345u;
    const int m = (int)((seed >> 10) % 3);
    latest[m] = 50 + (int)((seed >> 14) % 1000);
    const std::string text =
        std::string(kHead) + "\n2026-10-25," + meals[m] + ",x," + std::to_string(latest[m]) + ",1\n";
    const CsvUpsert up = planCsvUpsert(file, text, "meal", "kcal", "2026-10-25");
    TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
    file = applied(file, up);
    const int want = latest[0] + latest[1] + latest[2];
    TEST_ASSERT_TRUE(up.hasTotal);
    TEST_ASSERT_EQUAL_INT(want, (int)up.todayTotal);
    TEST_ASSERT_EQUAL_INT(want, (int)dayTotal(file, "2026-10-25"));
  }
  // Counter-test: the old playbook rule (a correction appends its own row) stores both
  // rows. The total no longer double-counts them (this read 970 until the 31 Oct receipt
  // fix): one meal logged twice counts once, with the last figures, and the reply says so.
  std::string appendOnly = std::string(kHead) + "\n";
  appendTo(appendOnly, "2026-10-25,lunch,x,450,1");
  appendTo(appendOnly, "2026-10-25,lunch,x,520,1");
  TEST_ASSERT_EQUAL_INT(520, (int)dayTotal(appendOnly, "2026-10-25"));
  // ...and the keyed path refuses to correct such a file blind: two stored rows
  // share the identity, so the key cannot tell them apart.
  const CsvUpsert up =
      planCsvUpsert(appendOnly, std::string(kHead) + "\n2026-10-25,lunch,x,520,1\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_FALSE(up.ok);
  TEST_ASSERT_TRUE(up.err.find("2 or more stored rows") != std::string::npos);
}

// The header check runs on the STORED header: a reordered or wider header is
// refused with both headers named, never mis-indexed.
static void test_qw3_header_mismatch_is_refused_not_misindexed() {
  const std::string ex = "date,meal,kcal\n2026-10-25,lunch,450\n";
  CsvUpsert up = planCsvUpsert(ex, "meal,date,kcal\nlunch,2026-10-25,520\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_FALSE(up.ok);
  TEST_ASSERT_TRUE(up.err.find("(meal,date,kcal)") != std::string::npos);   // the model's own header
  // The stored header is named when it is plain labels, so the retry is one call.
  TEST_ASSERT_TRUE_MESSAGE(up.err.find("the stored header (date,meal,kcal)") != std::string::npos, up.err.c_str());
  // A stored header that is not plain labels is only counted (stored free text is never quoted).
  up = planCsvUpsert("date,meal,kcal \"x\"\n2026-10-25,lunch,450\n", "date,meal,kcal\n2026-10-25,lunch,520\n", "meal",
                     "kcal", "2026-10-25");
  TEST_ASSERT_FALSE(up.ok);
  TEST_ASSERT_TRUE_MESSAGE(up.err.find("(3 columns; read it with files.read") != std::string::npos, up.err.c_str());
  up = planCsvUpsert(ex, "date,meal,kcal,protein_g\n2026-10-25,lunch,520,30\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_FALSE(up.ok);
  // Case and spaces around names do not matter; the order and the count do.
  up = planCsvUpsert(ex, "Date, Meal ,KCAL\n2026-10-25,lunch,520\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  // A stored file with a byte order mark (a hand edit) still matches.
  up = planCsvUpsert("\xEF\xBB\xBF" + ex, "date,meal,kcal\n2026-10-25,lunch,520\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
}

// Quoted fields: a comma inside quotes is part of the field, in the new rows and
// in the stored rows the total reads; an unquoted comma is refused (new rows) or
// reported as an unreadable row (stored rows), never read as a shifted number.
static void test_qw3_quoted_fields_and_honest_totals() {
  const std::string head = "date,meal,items,kcal\n";
  CsvUpsert up = planCsvUpsert(head, head + "2026-10-26,lunch,\"chicken, rice\",450\n", "meal", "kcal", "2026-10-26");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(450, (int)up.todayTotal);
  TEST_ASSERT_EQUAL_INT(1, up.days[0].rows);
  // "" inside quotes is a quote; the field still counts.
  up = planCsvUpsert(applied(head, up), head + "2026-10-26,dinner,\"the \"\"big\"\" one, again\",600\n",
                     "meal", "kcal", "2026-10-26");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(1050, (int)up.todayTotal);
  // An unquoted comma in a new row: refused with the reason.
  up = planCsvUpsert(head, head + "2026-10-26,lunch,chicken, rice,450\n", "meal", "kcal", "2026-10-26");
  TEST_ASSERT_FALSE(up.ok);
  TEST_ASSERT_TRUE(up.err.find("row 1 has 5 fields but the header has 4") == 0);
  TEST_ASSERT_TRUE(up.err.find("double quotes") != std::string::npos);
  // An unclosed quote: refused.
  up = planCsvUpsert(head, head + "2026-10-26,lunch,\"chicken,450\n", "meal", "kcal", "2026-10-26");
  TEST_ASSERT_FALSE(up.ok);
  TEST_ASSERT_TRUE(up.err.find("broken quote") != std::string::npos);
  up = planCsvUpsert(head, head + "2026-10-26,lunch,\"chicken\" rice,450\n", "meal", "kcal", "2026-10-26");
  TEST_ASSERT_FALSE(up.ok);   // text after a closing quote
  TEST_ASSERT_TRUE(up.err.find("broken quote") != std::string::npos);
  // A stored row with an unquoted comma (an old plain append) is read: its items hold the
  // extra field, its figure lands in the kcal column. (Until the 31 Oct receipt defect it
  // was left out and reported, and a day of such rows summed to a wrong figure.)
  const std::string legacy = head + "2026-10-26,lunch,chicken, rice,450\n2026-10-26,tea,x,52\n";
  DayTotal t;
  {
    CsvDayTally tally("kcal", "2026-10-26");
    RowSplitter rows([&](const std::string& r, bool c) { tally.feed(r, c); return true; });
    rows.push(legacy.data(), legacy.size());
    rows.end();
    TEST_ASSERT_TRUE(tally.ok());
    t = tally.total();
  }
  TEST_ASSERT_EQUAL_INT(502, (int)t.total);
  TEST_ASSERT_EQUAL_INT(2, t.rows);
  TEST_ASSERT_EQUAL_INT(0, t.unread);
  // So a keyed correction replaces it: nothing counts twice.
  up = planCsvUpsert(legacy, head + "2026-10-26,lunch,\"chicken, rice\",450\n", "meal", "kcal", "2026-10-26");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  TEST_ASSERT_EQUAL_INT(502, (int)up.todayTotal);
  // A row it cannot read stays out, and the receipt gives no figure as the day's.
  const std::string line = renderDayTotal(
      []() { DayTotal d; d.date = "2026-10-26"; d.total = 52; d.rows = 1; d.unread = 1; return d; }(), "kcal", true, "GMT");
  TEST_ASSERT_EQUAL_STRING(
      "today's kcal (2026-10-26, GMT): not known - 52 counted from 1 row, but 1 more row dated 2026-10-26 has no "
      "readable 'kcal' (not a number, or its columns are shifted); fix it with files.edit before stating a total",
      line.c_str());
}

// ---- the day-sum receipt on real logs (the 31 Oct 256) ---------------------------
// 31 Oct 20:54: the log reader's sum said 256 and the rows summed to 2,369. Exactly the
// two rows without a comma in their items were counted (190 + 66): every row with a bare
// comma in its items was left out. The shape below is that day's file, the food words
// changed. Arithmetic is code: the receipt must hold on what a real log holds.
static DayTotal tallyOf(const std::string& file, const std::string& col, const std::string& day) {
  CsvDayTally tally(col, day);
  RowSplitter rows([&](const std::string& r, bool c) { tally.feed(r, c); return true; });
  rows.push(file.data(), file.size());
  rows.end();
  return tally.total();
}

static const char k31Oct[] =
    "date,meal,items,kcal,protein_g\n"
    "2026-10-31,breakfast,100 g berries, 150 g yogurt,147,17.2\n"
    "2026-10-31,snack,1 protein bar,190,21\n"
    "2026-10-31,lunch,3 eggs, 1 bun, half avocado,471,24.4\n"
    "2026-10-31,drink,cappuccino (100 ml full milk),66,3.3\n"
    "2026-10-31,dinner,200 g steak, 120 g chicken, 200 g ravioli, salad,1038,100.2\n"
    "2026-10-31,snack2,bagel 85 g, fig jam 45 g, cream 18 g,457,9.1\n";

static void test_receipt_the_31_oct_file_sums_to_its_rows() {
  const DayTotal k = tallyOf(k31Oct, "kcal", "2026-10-31");
  TEST_ASSERT_EQUAL_INT(2369, (int)k.total);
  TEST_ASSERT_EQUAL_INT(6, k.rows);
  TEST_ASSERT_EQUAL_INT(0, k.unread);
  const DayTotal p = tallyOf(k31Oct, "protein_g", "2026-10-31");
  TEST_ASSERT_FLOAT_WITHIN(0.01, 175.2, (float)p.total);
}

// The class: every comma count in the free text, with the free text in any column.
static void test_receipt_a_bare_comma_never_changes_the_sum_property() {
  const std::vector<std::string> headers = {"date,meal,items,kcal,protein_g", "date,meal,kcal,protein_g,items",
                                            "items,date,meal,kcal,protein_g", "key,date,items,grams,kcal,protein_g,source"};
  for (const std::string& h : headers) {
    for (int commas = 0; commas <= 6; commas++) {
      std::string items = "toast";
      for (int i = 0; i < commas; i++) items += ", " + std::to_string(i + 2) + " g item" + std::to_string(i);
      std::string row;
      if (h == headers[0]) row = "2026-10-31,lunch," + items + ",450,31";
      if (h == headers[1]) row = "2026-10-31,lunch,450,31," + items;
      if (h == headers[2]) row = items + ",2026-10-31,lunch,450,31";
      if (h == headers[3]) row = "k1,2026-10-31," + items + ",200,450,31,photo";
      const std::string file = h + "\n" + row + "\n";
      const DayTotal t = tallyOf(file, "kcal", "2026-10-31");
      TEST_ASSERT_EQUAL_INT_MESSAGE(450, (int)t.total, row.c_str());
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.unread, row.c_str());
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01, 31, (float)tallyOf(file, "protein_g", "2026-10-31").total, row.c_str());
    }
  }
}

// A row the device cannot read for certain is never a wrong number: the receipt says so.
static void test_receipt_a_shifted_row_is_no_figure_never_a_wrong_one() {
  const std::string head = "date,meal,items,kcal,protein_g\n";
  const std::vector<std::string> shifted = {
      "2026-10-30 08:30,Yogurt,150 g,342,13.5,0.3,4.1,0.12",   // another schema, wider
      "2026-10-30 08:30,Raspberries,75,1.5",                   // another schema, narrower
      "2026-10-30,dinner,steak,1,038,100.2",                   // a bare comma in the figure
      "2026-10-30,lunch,450,675,90.0",                         // a number where the items go
      "2026-10-30,dinner,steak,about 900,40",                  // words where the figure goes
  };
  for (const std::string& row : shifted) {
    const DayTotal t = tallyOf(head + row + "\n2026-10-30,tea,black,5,0\n", "kcal", "2026-10-30");
    TEST_ASSERT_EQUAL_INT_MESSAGE(5, (int)t.total, row.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.unread, row.c_str());
    const std::string line = renderDayTotal(t, "kcal", true, "GMT");
    TEST_ASSERT_TRUE_MESSAGE(line.find("not known") != std::string::npos, line.c_str());
  }
}

static void test_receipt_quoted_commas_and_thousands() {
  const std::string file =
      "date,meal,items,kcal,protein_g\n"
      "2026-10-28,lunch,\"chicken, rice, \"\"hot\"\" sauce\",\"1,045\",60\n"
      "2026-10-28,tea,\"black, no milk\",5,0\n";
  const DayTotal t = tallyOf(file, "kcal", "2026-10-28");
  TEST_ASSERT_EQUAL_INT(1050, (int)t.total);
  TEST_ASSERT_EQUAL_INT(2, t.rows);
  TEST_ASSERT_EQUAL_INT(0, t.unread);
}

// A header pasted mid-file, a model's own total row and a meal logged twice are not meals.
static void test_receipt_counts_meal_rows_and_nothing_else() {
  const std::string head = "date,meal,items,kcal,protein_g\n";
  // the header again, mid-file: not a row, not "undated"
  CsvDayTally tally("kcal", "2026-10-31");
  const std::string withHeader = head + "2026-10-31,lunch,x,100,1\n" + head + "2026-10-31,dinner,y,200,2\n";
  RowSplitter rows([&](const std::string& r, bool c) { tally.feed(r, c); return true; });
  rows.push(withHeader.data(), withHeader.size());
  rows.end();
  TEST_ASSERT_EQUAL_INT(300, (int)tally.total().total);
  TEST_ASSERT_EQUAL_INT(0, tally.undated());
  // the 31 Oct "bogus 874 total row": stated, never summed
  for (const char* word : {"total", "Total", "daily total", "day total", "subtotal", "TOTALS"}) {
    const DayTotal t = tallyOf(head + "2026-10-31,lunch,x,100,1\n2026-10-31," + word + ",,874,70\n", "kcal", "2026-10-31");
    TEST_ASSERT_EQUAL_INT_MESSAGE(100, (int)t.total, word);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.totalRows, word);
    TEST_ASSERT_TRUE(renderDayTotal(t, "kcal", true, "").find("total row") != std::string::npos);
  }
  // A food that merely holds the word stays a food.
  TEST_ASSERT_EQUAL_INT(150, (int)tallyOf(head + "2026-10-31,lunch,Total Greek yogurt,150,15\n", "kcal", "2026-10-31").total);
  // The same row twice (a dead reply, the owner re-sent it): once. The 29 Oct boba, three
  // generations of one drink: the last one counts.
  DayTotal t = tallyOf(head + "2026-10-29,drink,boba tea,225,1\n2026-10-29,drink,boba tea,225,1\n", "kcal", "2026-10-29");
  TEST_ASSERT_EQUAL_INT(225, (int)t.total);
  TEST_ASSERT_EQUAL_INT(1, t.repeats);
  TEST_ASSERT_TRUE(renderDayTotal(t, "kcal", true, "").find("once") != std::string::npos);
  t = tallyOf(head + "2026-10-29,drink,boba tea,350,0\n2026-10-29,lunch,x,100,1\n2026-10-29,drink,boba tea,225,1\n", "kcal", "2026-10-29");
  TEST_ASSERT_EQUAL_INT(325, (int)t.total);
  // Separate items of one meal are separate rows.
  t = tallyOf(head + "2026-10-30,lunch,sandwich,675,27\n2026-10-30,lunch,berries,57,1\n", "kcal", "2026-10-30");
  TEST_ASSERT_EQUAL_INT(732, (int)t.total);
  TEST_ASSERT_EQUAL_INT(0, t.repeats);
  // A log that is not food keeps every row: two coffees bought are two coffees.
  const std::string money = "date,what,cost\n2026-10-30,coffee,3.5\n2026-10-30,coffee,3.5\n";
  TEST_ASSERT_EQUAL_INT(7, (int)tallyOf(money, "cost", "2026-10-30").total);
}

// Days across midnight: the row's own date decides, in every format a row carries it.
static void test_receipt_days_across_midnight() {
  const std::string file =
      "timestamp,meal,items,kcal\n"
      "2026-10-31 23:58,snack,toast, jam,120\n"
      "2026-10-31T23:59:30,drink,tea,5\n"
      "2026-11-01 00:01,snack2,apple,52\n"
      "2026-11-01T00:30:00,drink2,tea,5\n";
  TEST_ASSERT_EQUAL_INT(125, (int)tallyOf(file, "kcal", "2026-10-31").total);
  TEST_ASSERT_EQUAL_INT(57, (int)tallyOf(file, "kcal", "2026-11-01").total);
  TEST_ASSERT_EQUAL_INT(0, (int)tallyOf(file, "kcal", "2026-11-02").total);
}

// The keyed write reads a stored wide row the way the total does: a correction replaces
// it (it used to be added beside it), and a total row is refused as a new row.
static void test_receipt_a_correction_replaces_a_wide_row() {
  const std::string head = "date,meal,items,kcal,protein_g\n";
  const std::string file = head + "2026-10-31,lunch,3 eggs, 1 bun,471,24\n2026-10-31,tea,x,5,0\n";
  CsvUpsert up = planCsvUpsert(file, head + "2026-10-31,lunch,\"3 eggs, 1 bun\",500,24\n", "meal", "kcal", "2026-10-31");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  TEST_ASSERT_EQUAL_INT(505, (int)up.todayTotal);
  up = planCsvUpsert(file, head + "2026-10-31,total,all,476,24\n", "meal", "kcal", "2026-10-31");
  TEST_ASSERT_FALSE(up.ok);
  TEST_ASSERT_TRUE_MESSAGE(up.err.find("adds up") != std::string::npos, up.err.c_str());
}

// The same food across days (CUM-864): the reply names the earlier figure; dates and figures only.
static void test_same_food_earlier_day_at_a_far_figure() {
  const std::string head = "date,meal,items,kcal,protein_g\n";
  const std::string file = head + "2026-10-29,drink,Boba tea (no sugar; honey tapioca),225,1\n2026-10-30,lunch,chicken salad,500,40\n";
  CsvUpsert up = planCsvUpsert(file, head + "2026-11-01,drink2,boba black tea no sugar + tapioca pearls,90,0.5\n", "meal", "kcal", "2026-11-01");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  std::string reply = renderUpsertReply(up, "food/food-log.csv", "kcal", "GMT");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("a row dated 2026-10-29 holds the same food at 225 kcal; this row says 90") != std::string::npos, reply.c_str());
  TEST_ASSERT_TRUE(reply.find("honey") == std::string::npos);   // a stored row's words never ride the reply
  // Within a quarter, or another food, or the same day: nothing.
  up = planCsvUpsert(file, head + "2026-11-01,drink2,boba tea no sugar + tapioca,240,1\n", "meal", "kcal", "2026-11-01");
  TEST_ASSERT_TRUE(renderUpsertReply(up, "f", "kcal", "").find("same food") == std::string::npos);
  up = planCsvUpsert(file, head + "2026-11-01,drink2,green tea,2,0\n", "meal", "kcal", "2026-11-01");
  TEST_ASSERT_TRUE(renderUpsertReply(up, "f", "kcal", "").find("same food") == std::string::npos);
  up = planCsvUpsert(file, head + "2026-10-30,dinner,chicken salad,900,40\n", "meal", "kcal", "2026-10-30");
  TEST_ASSERT_TRUE(renderUpsertReply(up, "f", "kcal", "").find("same food") == std::string::npos);
  // The newest earlier row decides: a corrected figure is the one to match.
  const std::string two = head + "2026-10-20,drink,cappuccino,120,6\n2026-10-30,drink,cappuccino,66,3\n";
  up = planCsvUpsert(two, head + "2026-11-01,drink,cappucino,70,3\n", "meal", "kcal", "2026-11-01");
  TEST_ASSERT_TRUE(renderUpsertReply(up, "f", "kcal", "").find("same food") == std::string::npos);
  up = planCsvUpsert(two, head + "2026-11-01,drink,cappucino,110,3\n", "meal", "kcal", "2026-11-01");
  TEST_ASSERT_TRUE(renderUpsertReply(up, "f", "kcal", "").find("2026-10-30 holds the same food at 66") != std::string::npos);
  // The reply gives today's protein too, known and unknown apart: the head adds up nothing.
  up = planCsvUpsert(head + "2026-11-01,lunch,soup,300,10\n2026-11-01,tea,x,5,\n", head + "2026-11-01,dinner,steak,600,50\n", "meal", "kcal", "2026-11-01");
  reply = renderUpsertReply(up, "f", "kcal", "");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("today's protein_g: 60 from 2 rows, and 1 row has no figure") != std::string::npos, reply.c_str());
  // A log that is not food says nothing.
  const std::string money = "date,what,items,cost\n2026-10-20,x,coffee,3\n";
  up = planCsvUpsert(money, "date,what,items,cost\n2026-11-01,x,coffee,9\n", "what", "cost", "2026-11-01");
  TEST_ASSERT_TRUE(renderUpsertReply(up, "f", "cost", "").find("same food") == std::string::npos);
}

static void test_food_words_the_class() {
  const auto w = [](const char* s) { return foodWordsOf(s); };
  TEST_ASSERT_TRUE(sameFoodWords(w("Boba tea (no sugar, honey tapioca)"), w("boba black tea, no sugar, tapioca pearls ~70 g")));
  TEST_ASSERT_TRUE(sameFoodWords(w("1 cappucino"), w("cappuccino (100 ml full milk)")));
  TEST_ASSERT_TRUE(sameFoodWords(w("2 eggs + toast"), w("eggs + toast")));
  TEST_ASSERT_FALSE(sameFoodWords(w("chicken salad"), w("chicken sandwich")));
  TEST_ASSERT_FALSE(sameFoodWords(w("eggs"), w("3 eggs + bun + avocado + bacon")));
  TEST_ASSERT_FALSE(sameFoodWords(w(""), w("eggs")));
  TEST_ASSERT_FALSE(sameFoodWords(w("500 g"), w("500 g")));
}

// Every new row is checked before anything is written: a date that is not
// YYYY-MM-DD, a date after today, an empty key, a sum that is not a plain
// number, two rows naming the same date and meal, a header-only text.
static void test_qw3_new_rows_are_checked_before_any_write() {
  const std::string ex = std::string(kHead) + "\n2026-10-25,lunch,x,450,30\n";
  struct Case {
    const char* row;
    const char* why;
  };
  const Case cases[] = {
      {"26/10/2026,dinner,x,600,30", "the date column 'date' must hold YYYY-MM-DD dates (row 1: '26/10/2026')"},
      {"2026-13-01,dinner,x,600,30", "must hold YYYY-MM-DD dates"},
      {"2026-10-00,dinner,x,600,30", "must hold YYYY-MM-DD dates"},
      {"2026-10-26,dinner,x,600,30\n26/10/2026,tea,x,52,1", "the date column 'date' must hold YYYY-MM-DD dates (row 2: '26/10/2026')"},
      {"#2026-10-26,dinner,x,600,30", "starts with # (a comment row)"},
      {"2026-10-26,  ,x,600,30", "has no value in 'meal'"},
      {"2026-10-26,dinner,x,1234567890123,30", "is not a plain number"},
      {"2026-10-27,dinner,x,600,30", "after today (2026-10-26)"},
      {"2026-10-26,,x,600,30", "has no value in 'meal'"},
      {"2026-10-26,dinner,x,~600,30", "is not a plain number"},
      {"2026-10-26,dinner,x,600-700,30", "is not a plain number"},
      {"2026-10-26,dinner,x,inf,30", "is not a plain number"},
      {"2026-10-26,dinner,x,1e5,30", "is not a plain number"},
      {"2026-10-26,dinner,x,0x1A,30", "is not a plain number"},
      {"2026-10-26,dinner,x,,30", "is not a plain number"},
      {"2026-10-26,dinner,x,600,30\n2026-10-26,Dinner,y,100,1", "both name 2026-10-26 'Dinner'"},
  };
  for (const Case& c : cases) {
    const CsvUpsert up = planCsvUpsert(ex, std::string(kHead) + "\n" + c.row + "\n", "meal", "kcal", "2026-10-26");
    TEST_ASSERT_FALSE_MESSAGE(up.ok, c.row);
    TEST_ASSERT_TRUE_MESSAGE(up.err.find(c.why) != std::string::npos, up.err.c_str());
    TEST_ASSERT_TRUE(up.out.empty());
  }
  // A header with no data row, or no text at all, is an error - never a quiet
  // "updated 0 rows" that could back an "I logged it" claim.
  for (const std::string& t : {std::string(kHead) + "\n", std::string("\n\n"), std::string()}) {
    const CsvUpsert up = planCsvUpsert(ex, t, "meal", "kcal", "2026-10-26");
    TEST_ASSERT_FALSE(up.ok);
    TEST_ASSERT_TRUE(up.err.find("need 'text'") == 0);
  }
  // Too much text for one call.
  const CsvUpsert big = planCsvUpsert(ex, std::string(kHead) + "\n" + std::string(kAppendMaxBytes, 'x'),
                                      "meal", "kcal", "2026-10-26");
  TEST_ASSERT_FALSE(big.ok);
  // With the clock unset there is no "after today" and no total, and the reply says why.
  const CsvUpsert noClock = planCsvUpsert(ex, std::string(kHead) + "\n2030-01-01,dinner,x,600,30\n", "meal", "kcal", "");
  TEST_ASSERT_TRUE_MESSAGE(noClock.ok, noClock.err.c_str());
  TEST_ASSERT_FALSE(noClock.hasTotal);
  TEST_ASSERT_EQUAL_STRING("the device clock is not set", noClock.noTotal.c_str());
  // Every successful call writes at least one row (replaced or added).
  TEST_ASSERT_TRUE(noClock.replaced + noClock.appended >= 1);
}

// A power cut mid-row: the stored last row has no line end AND is provably
// incomplete (here: fewer fields than the header). A new row with its identity
// replaces it, otherwise it is kept as a #cut: row that never counts or matches
// again; the reply says which.
static void test_qw3_provably_cut_last_row_is_set_aside() {
  const std::string cut = "date,meal,kcal\n2026-10-26,lunch,450\n2026-10-26,dinner";   // cut before ",600"
  // Another meal: the cut row is kept but marked, and NOT counted.
  CsvUpsert up = planCsvUpsert(cut, "date,meal,kcal\n2026-10-26,snack-1530,95\n", "meal", "kcal", "2026-10-26");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n2026-10-26,lunch,450\n#cut: 2026-10-26,dinner\n2026-10-26,snack-1530,95\n",
                           up.out.c_str());
  TEST_ASSERT_EQUAL_INT(545, (int)up.todayTotal);
  TEST_ASSERT_TRUE(up.cut);
  TEST_ASSERT_EQUAL_STRING("2026-10-26", up.cutDate.c_str());
  TEST_ASSERT_FALSE(up.cutReplaced);
  // ...and it stays out: the next write neither counts nor matches it.
  const CsvUpsert again = planCsvUpsert(up.out, "date,meal,kcal\n2026-10-26,dinner,600\n", "meal", "kcal", "2026-10-26");
  TEST_ASSERT_TRUE_MESSAGE(again.ok, again.err.c_str());
  TEST_ASSERT_EQUAL_INT(0, again.replaced);
  TEST_ASSERT_EQUAL_INT(1145, (int)again.todayTotal);   // 450 + 95 + 600
  TEST_ASSERT_TRUE(again.out.find("#cut: 2026-10-26,dinner\n") != std::string::npos);   // kept, never lost
  // The same meal logged again in full: it replaces the cut row, and the cut
  // row is kept as a #replaced: history row (never matched or counted).
  up = planCsvUpsert(cut, "date,meal,kcal\n2026-10-26,dinner,600\n", "meal", "kcal", "2026-10-26");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_TRUE(up.cutReplaced);
  TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n2026-10-26,lunch,450\n#replaced: 2026-10-26,dinner\n2026-10-26,dinner,600\n",
                           up.out.c_str());
  TEST_ASSERT_EQUAL_INT(1050, (int)up.todayTotal);
  // The tail view's total never counts a cut row either.
  TEST_ASSERT_EQUAL_INT(450, (int)dayTotal(cut, "2026-10-26"));
  // Every proof of a cut: an open quote, fewer fields, no number where the row
  // before has one, an empty number column.
  for (const char* row : {"2026-10-26,\"din", "2026-10-26,dinner", "2026-10-26,dinner,6x0", "2026-10-26,dinner,"})
    TEST_ASSERT_TRUE_MESSAGE(csvLastRowCut("date,meal,kcal", "2026-10-26,lunch,450", row), row);
  // The reply names the cut row by its date only.
  const std::string reply = renderUpsertReply(planCsvUpsert(cut, "date,meal,kcal\n2026-10-26,tea,52\n", "meal",
                                                            "kcal", "2026-10-26"),
                                              "owner/food-log.csv", "kcal", "GMT");
  TEST_ASSERT_TRUE(reply.find("- the stored last row (dated 2026-10-26) was cut mid-write; it is kept as a #cut: "
                               "row and not counted - log it again in full") != std::string::npos);
  TEST_ASSERT_TRUE(reply.find("dinner") == std::string::npos);   // the cut row's text is never quoted
}

// MUST (merge review): a WELL-FORMED last row without a line end (artifact.save, a
// spreadsheet upload, a hand edit) is complete - counted, matched, and given its
// line end - never demoted to #cut:.
static void test_qw3_well_formed_last_row_without_line_end_is_complete() {
  const std::string saved = "date,meal,items,kcal\n2026-10-25,lunch,salad,450\n2026-10-25,dinner,pasta,800";
  // Another meal: the dinner stays, gains its line end, and counts.
  CsvUpsert up = planCsvUpsert(saved, "date,meal,items,kcal\n2026-10-25,tea,cake,52\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING(
      "date,meal,items,kcal\n2026-10-25,lunch,salad,450\n2026-10-25,dinner,pasta,800\n2026-10-25,tea,cake,52\n",
      up.out.c_str());
  TEST_ASSERT_EQUAL_INT(1302, (int)up.todayTotal);
  TEST_ASSERT_FALSE(up.cut);
  // A correction of that dinner replaces it (it is matched like any row).
  up = planCsvUpsert(saved, "date,meal,items,kcal\n2026-10-25,dinner,pasta,760\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  TEST_ASSERT_EQUAL_INT(1210, (int)up.todayTotal);
  // The same row re-sent: only its line end is added.
  up = planCsvUpsert(saved, "date,meal,items,kcal\n2026-10-25,dinner,pasta,800\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE(up.ok && up.changed);
  TEST_ASSERT_EQUAL_STRING((saved + "\n").c_str(), up.out.c_str());
  // The tail view counts it, and does not call it incomplete.
  TEST_ASSERT_EQUAL_INT(1250, (int)dayTotal(saved, "2026-10-25"));
  const TailView v = tailRows(saved, 0, 5);
  TEST_ASSERT_TRUE(v.lastCut);   // the bytes do end without a line end...
  TEST_ASSERT_TRUE(renderTail(v, "f.csv", saved.size(), "date,meal,items,kcal").find("incomplete") ==
                   std::string::npos);   // ...but the row is whole
  const std::string torn = saved.substr(0, saved.size() - 4);   // "...,dinner,pasta": a field short
  TEST_ASSERT_TRUE(renderTail(tailRows(torn, 0, 5), "f.csv", torn.size(), "date,meal,items,kcal")
                       .find("the last row is incomplete") != std::string::npos);
  TEST_ASSERT_FALSE(csvLastRowCut("date,meal,items,kcal", "2026-10-25,lunch,salad,450", "2026-10-25,dinner,pasta,800"));
  // The first data row with no line end and nothing before it: whole when wide enough.
  TEST_ASSERT_FALSE(csvLastRowCut("date,kcal", "", "2026-10-25,800"));
}

// Comment rows (#...) and blank rows are kept as written and never counted.
static void test_qw3_comment_rows_are_kept_and_never_counted() {
  const std::string ex = "date,meal,kcal\n# moved from 24 Oct\n\n2026-10-25,lunch,450\n";
  const CsvUpsert up = planCsvUpsert(ex, "date,meal,kcal\n2026-10-25,dinner,600\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n# moved from 24 Oct\n\n2026-10-25,lunch,450\n2026-10-25,dinner,600\n",
                           up.out.c_str());
  TEST_ASSERT_EQUAL_INT(1050, (int)up.todayTotal);
}

// The streamed form (the device) equals the whole-text form for every chunk size:
// RowSplitter cuts rows the same wherever the chunk boundaries fall, CRLF too.
static void test_qw3_stream_equals_whole_for_any_chunking() {
  const std::string stored = "date,meal,kcal\r\n2026-10-24,dinner,990\r\n2026-10-25,breakfast,945\n2026-10-25,lunch,7";
  const std::string text = "date,meal,kcal\n2026-10-24,dinner,1010\n2026-10-25,coffee,66\n";
  const CsvUpsert whole = planCsvUpsert(stored, text, "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(whole.ok, whole.err.c_str());
  for (size_t chunk = 1; chunk <= stored.size() + 1; chunk++) {
    CsvUpsertStream m("meal", "kcal", "2026-10-25");
    TEST_ASSERT_TRUE(m.begin(text));
    std::string out, piece;
    RowSplitter rows([&](const std::string& r, bool c) {
      if (!m.feed(r, c, piece)) return false;
      out += piece;
      return true;
    });
    for (size_t p = 0; p < stored.size(); p += chunk)
      TEST_ASSERT_TRUE(rows.push(stored.data() + p, std::min(chunk, stored.size() - p)));
    TEST_ASSERT_TRUE(rows.end());
    TEST_ASSERT_TRUE(m.finish(piece));
    out += piece;
    TEST_ASSERT_EQUAL_STRING(whole.out.c_str(), out.c_str());
    TEST_ASSERT_EQUAL_INT((int)whole.todayTotal, (int)m.result().todayTotal);
  }
  // A row longer than the cap stops the stream (not a log).
  RowSplitter rows([](const std::string&, bool) { return true; });
  const std::string huge(RowSplitter::kRowMaxBytes + 1, 'x');
  TEST_ASSERT_FALSE(rows.push(huge.data(), huge.size()));
  TEST_ASSERT_TRUE(rows.tooLong());
}

// The reply: what each row did (with what a replaced row was), then the totals,
// today first with its zone, each stating its row count.
static void test_qw3_reply_states_rows_days_and_zone() {
  const std::string ex = "date,meal,kcal\n2026-10-24,dinner,990\n2026-10-25,breakfast,945\n";
  const CsvUpsert up = planCsvUpsert(ex, "date,meal,kcal\n2026-10-24,dinner,1010\n2026-10-25,coffee,66\n", "meal",
                                     "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING(
      "replaced 1 row, added 1 row in owner/food-log.csv\n"
      "- replaced 2026-10-24 'dinner' (was 990 kcal)\n"
      "- added 2026-10-25 'coffee'\n"
      "today's kcal (2026-10-25, GMT): 1011 from 2 rows\n"
      "kcal on 2026-10-24: 1010 from 1 row\n",
      renderUpsertReply(up, "owner/food-log.csv", "kcal", "GMT").c_str());
  // No timezone set: the zone says UTC, so the owner can see the day boundary.
  const CsvUpsert first = planCsvUpsert("", "date,meal,kcal\n2026-10-25,lunch,450\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE(renderUpsertReply(first, "o/f.csv", "kcal", "UTC, no time zone is set")
                       .find("today's kcal (2026-10-25, UTC, no time zone is set): 450 from 1 row") !=
                   std::string::npos);
  // A day with no rows says so instead of a bare 0.
  DayTotal none;
  none.date = "2026-10-26";
  TEST_ASSERT_EQUAL_STRING("today's kcal (2026-10-26, GMT): 0 - no rows are dated 2026-10-26",
                           renderDayTotal(none, "kcal", true, "GMT").c_str());
  // No date column: a keyed write is refused (it would replace other days' rows).
  const CsvUpsert nodate = planCsvUpsert("", "meal,kcal\nlunch,450\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_FALSE(nodate.ok);
  TEST_ASSERT_TRUE(nodate.err.find("no date column") != std::string::npos);
}

// The device's read and write seams (mergeKeyed): a card read that fails or ends
// short is NEVER taken for a shorter log - the call is refused and the caller
// discards the output, so the stored log stays whole; a short write refuses too.
static void test_qw3_read_failure_or_short_write_never_rewrites_the_log() {
  std::string stored = std::string(kHead) + "\n";
  for (int i = 0; i < 40; i++)
    stored += "2026-10-" + std::to_string(10 + i % 15) + ",meal-" + std::to_string(i) + ",x,100,5\n";
  const std::string text = std::string(kHead) + "\n2026-10-25,dinner,x,600,30\n";
  const CsvUpsert whole = planCsvUpsert(stored, text, "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE(whole.ok);
  // A healthy read: the streamed output is the whole-text output.
  TEST_ASSERT_TRUE(stored.size() > 1024);   // three 512-byte reads
  for (int failAt : {-1, 0, 1, 2}) {   // the read call that fails (-1 = none)
    CsvUpsertStream m("meal", "kcal", "2026-10-25");
    TEST_ASSERT_TRUE(m.begin(text));
    size_t pos = 0;
    int calls = 0;
    std::string out, err;
    const bool ok = mergeKeyed(
        m, true, stored.size(),
        [&](char* d, size_t n) {
          if (calls++ == failAt) return -1;
          const size_t k = std::min(n, stored.size() - pos);
          std::memcpy(d, stored.data() + pos, k);
          pos += k;
          return (int)k;
        },
        [&](const std::string& b) {
          out += b;
          return true;
        },
        err);
    if (failAt < 0) {
      TEST_ASSERT_TRUE_MESSAGE(ok, err.c_str());
      TEST_ASSERT_EQUAL_STRING(whole.out.c_str(), out.c_str());
    } else {
      TEST_ASSERT_FALSE(ok);
      TEST_ASSERT_TRUE_MESSAGE(err.find("the card read failed - the log was not changed") == 0, err.c_str());
    }
  }
  // The card reports a size but returns fewer bytes and then nothing (a cut read).
  {
    CsvUpsertStream m("meal", "kcal", "2026-10-25");
    TEST_ASSERT_TRUE(m.begin(text));
    std::string err;
    bool once = false;
    const bool ok = mergeKeyed(
        m, true, stored.size(),
        [&](char* d, size_t n) {
          if (once) return 0;
          once = true;
          std::memcpy(d, stored.data(), std::min(n, (size_t)50));
          return (int)std::min(n, (size_t)50);
        },
        [](const std::string&) { return true; }, err);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(err.find("the card read failed") == 0);
  }
  // A short write: refused with its own reason.
  for (int failAt : {0, 2, 41}) {
    CsvUpsertStream m("meal", "kcal", "2026-10-25");
    TEST_ASSERT_TRUE(m.begin(text));
    size_t pos = 0;
    int writes = 0;
    std::string err;
    const bool ok = mergeKeyed(
        m, true, stored.size(),
        [&](char* d, size_t n) {
          const size_t k = std::min(n, stored.size() - pos);
          std::memcpy(d, stored.data() + pos, k);
          pos += k;
          return (int)k;
        },
        [&](const std::string&) { return writes++ != failAt; }, err);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE_MESSAGE(err.find("SD short write") == 0, err.c_str());
  }
  // A refused row stops the merge with the row's reason (nothing to keep).
  {
    CsvUpsertStream m("meal", "kcal", "2026-10-25");
    TEST_ASSERT_TRUE(m.begin("meal,date,items,kcal,protein_g\ndinner,2026-10-25,x,600,30\n"));
    std::string err;
    size_t pos = 0;
    TEST_ASSERT_FALSE(mergeKeyed(
        m, true, stored.size(),
        [&](char* d, size_t n) {
          const size_t k = std::min(n, stored.size() - pos);
          std::memcpy(d, stored.data() + pos, k);
          pos += k;
          return (int)k;
        },
        [](const std::string&) { return true; }, err));
    TEST_ASSERT_TRUE(err.find("differs from the stored header") != std::string::npos);
  }
}

// The edges the first review found: blank lines above the header, a cut header,
// an empty stored file, a CRLF log with a real change, a quoted key with spaces,
// key=date with a cut duplicate, rows with no date, an over-long stored row.
static void test_qw3_review_edges() {
  // Blank lines above the header are kept; the header is still found. The
  // correction keeps the replaced row as a #replaced: history row.
  CsvUpsert up = planCsvUpsert("\n\ndate,meal,kcal\n2026-10-25,lunch,450\n", "date,meal,kcal\n2026-10-25,lunch,520\n",
                               "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("\n\ndate,meal,kcal\n#replaced: 2026-10-25,lunch,450\n2026-10-25,lunch,520\n",
                           up.out.c_str());
  double t = 0;
  TEST_ASSERT_TRUE(csvDayTotal("\ndate,kcal\n2026-10-25,450\n", "kcal", "2026-10-25", t));
  TEST_ASSERT_EQUAL_INT(450, (int)t);
  // A header cut before its line end that still matches: the line end is added.
  up = planCsvUpsert("date,meal,kcal", "date,meal,kcal\n2026-10-25,lunch,450\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n2026-10-25,lunch,450\n", up.out.c_str());
  // A 0-byte stored file through the device's seam: it adopts the header.
  {
    CsvUpsertStream m("meal", "kcal", "2026-10-25");
    TEST_ASSERT_TRUE(m.begin("date,meal,kcal\n2026-10-25,lunch,450\n"));
    std::string out, err;
    TEST_ASSERT_TRUE(mergeKeyed(m, true, 0, [](char*, size_t) { return 0; },
                                [&](const std::string& b) { out += b; return true; }, err));
    TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n2026-10-25,lunch,450\n", out.c_str());
  }
  // A CRLF log with a real change: rewritten with LF line ends, nothing lost,
  // the replaced row kept as history.
  up = planCsvUpsert("date,meal,kcal\r\n2026-10-24,dinner,990\r\n2026-10-25,lunch,450\r\n",
                     "date,meal,kcal\n2026-10-25,lunch,520\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n2026-10-24,dinner,990\n#replaced: 2026-10-25,lunch,450\n2026-10-25,lunch,520\n",
                           up.out.c_str());
  // A quoted key with spaces is the same key.
  up = planCsvUpsert("date,meal,kcal\n2026-10-25,\" Lunch \",450\n", "date,meal,kcal\n2026-10-25,lunch,520\n", "meal",
                     "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  TEST_ASSERT_EQUAL_INT(520, (int)up.todayTotal);
  // key=date: one complete row of the day plus a provably cut one (its number
  // column empty) - the cut one is set aside (#cut:), never a second match.
  up = planCsvUpsert("date,kcal\n2026-10-25,450\n2026-10-25,", "date,kcal\n2026-10-25,500\n", "date", "kcal",
                     "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("date,kcal\n#replaced: 2026-10-25,450\n2026-10-25,500\n#cut: 2026-10-25,\n",
                           up.out.c_str());
  TEST_ASSERT_EQUAL_INT(500, (int)up.todayTotal);
  // Rows with no YYYY-MM-DD date count toward no day, and the reply says so.
  up = planCsvUpsert("date,meal,kcal\n25/10/2026,lunch,450\nOct 25,tea,52\n", "date,meal,kcal\n2026-10-25,dinner,600\n",
                     "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(2, up.undated);
  TEST_ASSERT_EQUAL_INT(600, (int)up.todayTotal);
  TEST_ASSERT_TRUE(renderUpsertReply(up, "o/f.csv", "kcal", "GMT")
                       .find("[2 rows have no YYYY-MM-DD date and count toward no day's total]") != std::string::npos);
  {
    CsvDayTally tally("kcal", "2026-10-25");
    const std::string f = "date,kcal\n25/10/2026,450\n2026-10-25,52\n";
    RowSplitter rows([&](const std::string& r, bool c) { tally.feed(r, c); return true; });
    rows.push(f.data(), f.size());
    rows.end();
    TEST_ASSERT_EQUAL_INT(1, tally.undated());
    TEST_ASSERT_EQUAL_INT(52, (int)tally.total().total);
  }
  // A stored row over the row cap: refused through the device's seam, nothing kept.
  {
    const std::string big = "date,meal,kcal\n2026-10-25,lunch," + std::string(RowSplitter::kRowMaxBytes, '9') + "\n";
    CsvUpsertStream m("meal", "kcal", "2026-10-25");
    TEST_ASSERT_TRUE(m.begin("date,meal,kcal\n2026-10-25,tea,52\n"));
    size_t pos = 0;
    std::string err;
    TEST_ASSERT_FALSE(mergeKeyed(
        m, true, big.size(),
        [&](char* d, size_t n) {
          const size_t k = std::min(n, big.size() - pos);
          std::memcpy(d, big.data() + pos, k);
          pos += k;
          return (int)k;
        },
        [](const std::string&) { return true; }, err));
    TEST_ASSERT_TRUE_MESSAGE(err.find("longer than 16 KB") != std::string::npos, err.c_str());
  }
  // A stored row of thousands of commas costs a header's worth of fields, is never
  // matched, and is reported instead of counted; a header over 64 columns is refused.
  {
    const std::string commas = "date,meal,kcal\n2026-10-25," + std::string(10000, ',') + "\n";
    up = planCsvUpsert(commas, "date,meal,kcal\n2026-10-25,lunch,450\n", "meal", "kcal", "2026-10-25");
    TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
    TEST_ASSERT_EQUAL_INT(450, (int)up.todayTotal);
    TEST_ASSERT_EQUAL_INT(1, up.days[0].unread);
    std::string wide = "c0";
    for (int i = 1; i <= 64; i++) wide += ",c" + std::to_string(i);
    up = planCsvUpsert("", wide + "\n" + wide + "\n", "c0", "", "2026-10-25");
    TEST_ASSERT_FALSE(up.ok);
    TEST_ASSERT_TRUE(up.err.find("more than 64 columns") != std::string::npos);
  }
  // The duplicate refusal names the way out instead of a third row.
  up = planCsvUpsert("date,meal,kcal\n2026-10-25,snack,95\n2026-10-25,snack,150\n", "date,meal,kcal\n2026-10-25,snack,120\n",
                     "meal", "kcal", "2026-10-25");
  TEST_ASSERT_FALSE(up.ok);
  TEST_ASSERT_TRUE(up.err.find("nothing was written") != std::string::npos);
  TEST_ASSERT_TRUE(up.err.find("replace: \"all\"") != std::string::npos);
  TEST_ASSERT_TRUE(up.err.find("95") == std::string::npos && up.err.find("150") == std::string::npos);
}

// SHOULD (merge review): a way out of the duplicate refusal the model can take on
// a log of any size - the same call with replace: "all". The new row replaces the
// first duplicate, the others stay as #superseded: rows, never counted; other
// days and other meals are untouched.
static void test_qw3_replace_all_resolves_old_duplicates_in_place() {
  const std::string old = "date,meal,kcal\n2026-10-24,dinner,1200\n2026-10-24,lunch,500\n2026-10-24,dinner,1100\n"
                          "2026-10-23,dinner,700\n";
  CsvUpsert up = planCsvUpsert(old, "date,meal,kcal\n2026-10-24,dinner,990\n", "meal", "kcal", "2026-10-25", true);
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n#replaced: 2026-10-24,dinner,1200\n2026-10-24,dinner,990\n2026-10-24,lunch,500\n"
                           "#superseded: 2026-10-24,dinner,1100\n2026-10-23,dinner,700\n",
                           up.out.c_str());
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  TEST_ASSERT_EQUAL_INT(1, up.superseded);
  TEST_ASSERT_EQUAL_INT(1490, (int)dayTotal(up.out, "2026-10-24"));   // 990 + 500, counted once
  TEST_ASSERT_EQUAL_INT(700, (int)dayTotal(up.out, "2026-10-23"));
  const std::string reply = renderUpsertReply(up, "o/f.csv", "kcal", "GMT");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("- replaced 2026-10-24 'dinner' (was 1200 kcal)\n- set aside 1 more row of 2026-10-24 "
                                      "'dinner' as #superseded: (not counted)") != std::string::npos,
                           reply.c_str());
  // Without replace: "all" the same call is refused and nothing is written.
  TEST_ASSERT_FALSE(planCsvUpsert(old, "date,meal,kcal\n2026-10-24,dinner,990\n", "meal", "kcal", "2026-10-25").ok);
  // A daily ledger of running totals keyed by date resolves the same way.
  up = planCsvUpsert("date,kcal\n2026-10-24,800\n2026-10-24,1500\n", "date,kcal\n2026-10-24,2354\n", "date", "kcal",
                     "2026-10-25", true);
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(2354, (int)dayTotal(up.out, "2026-10-24"));
}

// MUST (merge review): whatever the date column is called - or when only its
// content says it is the date - a keyed write never touches a row of another
// date; with no date column at all a keyed write is refused, never keyed by the
// meal alone.
static void test_qw3_any_header_naming_keeps_days_apart() {
  const char* names[] = {"date", "Day", "timestamp", "datetime", "logged_at", "when", "ts", "d", "x1", "Eaten On"};
  const char* days[] = {"2026-10-23", "2026-10-24", "2026-10-25"};
  const char* meals[] = {"breakfast", "lunch", "dinner"};
  for (const char* dn : names) {
    for (int dateFirst = 0; dateFirst < 2; dateFirst++) {
      const std::string head = dateFirst ? std::string(dn) + ",meal,kcal" : std::string("meal,") + dn + ",kcal";
      const auto row = [&](const std::string& day, const std::string& meal, int kcal) {
        const std::string stamp = day + (std::string(dn) == "timestamp" ? "T08:30" : "");
        return (dateFirst ? stamp + "," + meal : meal + "," + stamp) + "," + std::to_string(kcal);
      };
      std::string file = head + "\n";
      unsigned seed = 11;
      for (int step = 0; step < 60; step++) {
        seed = seed * 1103515245u + 12345u;
        const std::string day = days[(seed >> 8) % 3], meal = meals[(seed >> 12) % 3];
        const CsvUpsert up = planCsvUpsert(file, head + "\n" + row(day, meal, 100 + step) + "\n", "meal", "kcal",
                                           "2026-10-25");
        TEST_ASSERT_TRUE_MESSAGE(up.ok, (std::string(dn) + ": " + up.err).c_str());
        const std::string next = applied(file, up);
        // Every row of ANOTHER date is byte-identical and in order.
        const auto others = [&](const std::string& text) {
          std::string kept;
          size_t p = 0;
          while (p < text.size()) {
            const size_t nl = text.find('\n', p);
            const std::string r = text.substr(p, nl - p);
            p = nl + 1;
            if (r.find(day) == std::string::npos) kept += r + "\n";
          }
          return kept;
        };
        TEST_ASSERT_EQUAL_STRING_MESSAGE(others(file).c_str(), others(next).c_str(), dn);
        file = next;
      }
    }
  }
  // No date column at all: refused with the way out, nothing written.
  CsvUpsert up = planCsvUpsert("meal,kcal\nbreakfast,557\n", "meal,kcal\nbreakfast,600\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_FALSE(up.ok);
  TEST_ASSERT_TRUE(up.err.find("no date column") != std::string::npos);
  TEST_ASSERT_TRUE(up.out.empty());
  // A column named like a date that holds no dates: refused, naming it.
  up = planCsvUpsert("", "time,meal,kcal\n08:30,breakfast,557\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_FALSE(up.ok);
  TEST_ASSERT_TRUE_MESSAGE(up.err.find("the date column 'time' must hold YYYY-MM-DD dates (row 1: '08:30')") !=
                               std::string::npos,
                           up.err.c_str());
  // ...unless another column holds the dates: then that one is the date column.
  up = planCsvUpsert("", "time,meal,eaten,kcal\n08:30,breakfast,2026-10-25,557\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(557, (int)up.todayTotal);
  // The tail view's total finds a date column by its content too.
  double t = 0;
  TEST_ASSERT_TRUE(csvDayTotal("meal,eaten,kcal\nlunch,2026-10-25,450\ntea,2026-10-24,52\n", "kcal", "2026-10-25", t));
  TEST_ASSERT_EQUAL_INT(450, (int)t);
}

// ---- lane UPSERT: a reused key never silently replaces a different item ---------
// 4 Nov 20:04: the owner logged a second dinner under the key "dinner"; the keyed
// update replaced the brisket row and the day total dropped by 457 kcal. The class
// rule: a keyed write whose key names a reused label replaces the stored row ONLY
// when it describes the same item (decided in code, by the item's words); a different
// item appends under a unique key and the stored row is kept. Every replace keeps the
// replaced row as a #replaced: history row and the receipt names what it held.
static void test_upsert_reused_key_with_a_different_item_appends_under_a_new_key() {
  const std::string head = "date,meal,items,kcal,protein_g\n";
  const std::string file = head +
                           "2026-11-04,breakfast,skyr + granola,533,32.5\n"
                           "2026-11-04,dinner,smoked brisket + seeded bread,457,40.2\n";
  // The 20:04 write: same date and key "dinner", a different item.
  CsvUpsert up = planCsvUpsert(file, head + "2026-11-04,dinner,Frive Smoky Baharat Mango Chicken,445,26.0\n",
                               "meal", "kcal", "2026-11-04");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(0, up.replaced);   // nothing was replaced
  TEST_ASSERT_EQUAL_INT(1, up.appended);   // the new meal is its own row
  TEST_ASSERT_TRUE_MESSAGE(up.out.find("2026-11-04,dinner,smoked brisket + seeded bread,457,40.2\n") !=
                               std::string::npos,
                           "the stored dinner row was kept byte for byte");
  TEST_ASSERT_TRUE_MESSAGE(up.out.find("2026-11-04,dinner-2,Frive Smoky Baharat Mango Chicken,445,26.0\n") !=
                               std::string::npos,
                           up.out.c_str());
  TEST_ASSERT_TRUE_MESSAGE(up.todayTotal > 1434.9 && up.todayTotal < 1435.1,
                           "both dinners count: 533 + 457 + 445 = 1435");
  const std::string reply = renderUpsertReply(up, "food/food-log.csv", "kcal", "GMT");
  TEST_ASSERT_TRUE_MESSAGE(reply.find("different item") != std::string::npos, reply.c_str());
  // The unique key avoids keys the day already holds.
  up = planCsvUpsert(up.out, head + "2026-11-04,dinner,waffles with compote,277,5.7\n", "meal", "kcal", "2026-11-04");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_TRUE_MESSAGE(up.out.find("2026-11-04,dinner-3,waffles with compote,277,5.7\n") != std::string::npos,
                           up.out.c_str());
  // A correction of the SAME item still replaces, keeps the old row as history, and
  // the receipt names what it replaced.
  up = planCsvUpsert(up.out, head + "2026-11-04,dinner,smoked brisket + seeded bread,760,61\n", "meal", "kcal",
                     "2026-11-04");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  TEST_ASSERT_TRUE_MESSAGE(up.out.find("#replaced: 2026-11-04,dinner,smoked brisket + seeded bread,457,40.2\n") !=
                               std::string::npos,
                           up.out.c_str());
  const std::string reply2 = renderUpsertReply(up, "food/food-log.csv", "kcal", "GMT");
  TEST_ASSERT_TRUE_MESSAGE(reply2.find("replaced 2026-11-04 'dinner' (was 457 kcal; the replaced row's items: smoked "
                                       "brisket seeded bread)") != std::string::npos,
                           reply2.c_str());
  TEST_ASSERT_TRUE_MESSAGE(up.todayTotal > 2014.9 && up.todayTotal < 2015.1,
                           "533 + 760 + 445 + 277 = 2015, the corrected figure counted once");
}

// Exactly one side with no item words is a different item (a bare "x" names nothing).
static void test_upsert_one_side_without_item_words_is_a_different_item() {
  const std::string head = "date,meal,items,kcal,protein_g\n";
  CsvUpsert up = planCsvUpsert(head + "2026-11-04,dinner,x,457,40\n",
                               head + "2026-11-04,dinner,brisket + bread,760,61\n", "meal", "kcal", "2026-11-04");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(0, up.replaced);
  TEST_ASSERT_EQUAL_INT(1, up.appended);
  TEST_ASSERT_TRUE(up.out.find("2026-11-04,dinner,x,457,40\n") != std::string::npos);
  TEST_ASSERT_TRUE(up.out.find("2026-11-04,dinner-2,brisket + bread,760,61\n") != std::string::npos);
  // ...and the other way round.
  up = planCsvUpsert(head + "2026-11-04,dinner,brisket + bread,457,40\n",
                     head + "2026-11-04,dinner,x,760,61\n", "meal", "kcal", "2026-11-04");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(0, up.replaced);
  TEST_ASSERT_TRUE(up.out.find("2026-11-04,dinner-2,x,760,61\n") != std::string::npos);
}

// key=date is the one-row-per-day ledger: the key IS the date, so there is no reused
// label to protect and the row replaces as before.
static void test_upsert_key_date_still_replaces_one_row_per_day() {
  const std::string file = "date,note,kcal\n2026-11-04,morning,100\n";
  CsvUpsert up = planCsvUpsert(file, "date,note,kcal\n2026-11-04,evening,200\n", "date", "kcal", "2026-11-04");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  TEST_ASSERT_EQUAL_INT(0, up.appended);
  TEST_ASSERT_EQUAL_INT(200, (int)up.todayTotal);
}

// The class property over random writes: no stored row ever vanishes without a
// #replaced: trace of it - every row of the file before is either still there byte for
// byte or kept as a history row.
static void test_upsert_no_stored_row_vanishes_without_a_history_trace() {
  const char* items[] = {"brisket + bread", "mango chicken", "waffles", "skyr + granola", "x", "salad"};
  const char* meals[] = {"breakfast", "lunch", "dinner", "snack-1600"};
  std::string file = std::string(kHead) + "\n";
  unsigned seed = 42;
  for (int step = 0; step < 300; step++) {
    seed = seed * 1103515245u + 12345u;
    const std::string day = (seed & 8) ? "2026-11-04" : "2026-11-03";
    const std::string meal = meals[(seed >> 4) % 4];
    const std::string it = items[(seed >> 8) % 6];
    const int kcal = 50 + (int)((seed >> 12) % 900);
    const CsvUpsert up = planCsvUpsert(file, std::string(kHead) + "\n" + day + "," + meal + "," + it + "," +
                                                     std::to_string(kcal) + ",10\n",
                                       "meal", "kcal", "2026-11-04");
    TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
    const std::string next = applied(file, up);
    size_t p = 0;
    while (p < file.size()) {
      const size_t nl = file.find('\n', p);
      const std::string row = file.substr(p, nl - p);
      p = nl + 1;
      if (row.empty() || row[0] == '#' || row == kHead) continue;
      TEST_ASSERT_TRUE_MESSAGE(next.find(row + "\n") != std::string::npos ||
                                   next.find("#replaced: " + row + "\n") != std::string::npos,
                               row.c_str());
    }
    file = next;
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_append_never_rewrites_a_stored_byte);
  RUN_TEST(test_append_refuses_empty_and_oversize);
  RUN_TEST(test_cut_file_is_reported_not_joined);
  RUN_TEST(test_tail_returns_exactly_the_last_n_rows);
  RUN_TEST(test_tail_render_names_what_is_shown);
  RUN_TEST(test_g1_keyed_upsert_updates_by_key_and_appends_new);
  RUN_TEST(test_g1_upsert_reports_the_day_total);
  RUN_TEST(test_g1_upsert_refuses_a_header_mismatch);
  RUN_TEST(test_csv_creation_needs_its_header_row_first);
  RUN_TEST(test_a_comma_inside_parentheses_stays_in_its_field);
  RUN_TEST(test_a_headerless_stored_file_refusal_names_the_rewrite_tools);
  RUN_TEST(test_g1_day_total_matches_only_today);
  RUN_TEST(test_qw3_keyed_update_never_touches_another_day);
  RUN_TEST(test_qw3_no_update_drops_or_moves_a_row);
  RUN_TEST(test_qw3_correction_never_double_counts);
  RUN_TEST(test_qw3_header_mismatch_is_refused_not_misindexed);
  RUN_TEST(test_qw3_quoted_fields_and_honest_totals);
  RUN_TEST(test_same_food_earlier_day_at_a_far_figure);
  RUN_TEST(test_food_words_the_class);
  RUN_TEST(test_receipt_the_31_oct_file_sums_to_its_rows);
  RUN_TEST(test_receipt_a_bare_comma_never_changes_the_sum_property);
  RUN_TEST(test_receipt_a_shifted_row_is_no_figure_never_a_wrong_one);
  RUN_TEST(test_receipt_quoted_commas_and_thousands);
  RUN_TEST(test_receipt_counts_meal_rows_and_nothing_else);
  RUN_TEST(test_receipt_days_across_midnight);
  RUN_TEST(test_receipt_a_correction_replaces_a_wide_row);
  RUN_TEST(test_qw3_new_rows_are_checked_before_any_write);
  RUN_TEST(test_qw3_provably_cut_last_row_is_set_aside);
  RUN_TEST(test_qw3_well_formed_last_row_without_line_end_is_complete);
  RUN_TEST(test_qw3_comment_rows_are_kept_and_never_counted);
  RUN_TEST(test_qw3_stream_equals_whole_for_any_chunking);
  RUN_TEST(test_qw3_reply_states_rows_days_and_zone);
  RUN_TEST(test_qw3_read_failure_or_short_write_never_rewrites_the_log);
  RUN_TEST(test_qw3_review_edges);
  RUN_TEST(test_qw3_replace_all_resolves_old_duplicates_in_place);
  RUN_TEST(test_qw3_any_header_naming_keeps_days_apart);
  RUN_TEST(test_upsert_reused_key_with_a_different_item_appends_under_a_new_key);
  RUN_TEST(test_upsert_one_side_without_item_words_is_a_different_item);
  RUN_TEST(test_upsert_key_date_still_replaces_one_row_per_day);
  RUN_TEST(test_upsert_no_stored_row_vanishes_without_a_history_trace);
  return UNITY_END();
}
