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
  TEST_ASSERT_TRUE(up.out.find("2026-10-23,lunch,450") == std::string::npos);
  TEST_ASSERT_TRUE(up.out.find("2026-10-22,dinner,900") != std::string::npos);
  // One row per key: the header plus three data rows (lunch, the 22nd's
  // dinner, the new snack), and "lunch" appears exactly once.
  TEST_ASSERT_EQUAL_INT(4, (int)std::count(up.out.begin(), up.out.end(), '\n'));
  size_t at = 0, lunches = 0;
  while ((at = up.out.find("lunch", at)) != std::string::npos) { lunches++; at++; }
  TEST_ASSERT_EQUAL_INT(1, (int)lunches);
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
  TEST_ASSERT_EQUAL_INT(4, rowCount(up.out));
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
  // ...while key=date on a one-row-per-day ledger replaces that day's row.
  const std::string ledger = "date,kcal\n2026-10-23,2422\n2026-10-24,1495\n";
  up = planCsvUpsert(ledger, "date,kcal\n2026-10-24,1600\n", "date", "kcal", "2026-10-24");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("date,kcal\n2026-10-23,2422\n2026-10-24,1600\n", up.out.c_str());
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
    TEST_ASSERT_EQUAL_INT(rowCount(file) + up.appended, rowCount(next));
    // Every row of another date, or another meal, is still there byte for byte
    // AND in the same order: the write touched only its own date and meal.
    const auto others = [&](const std::string& text) {
      std::string kept;
      size_t p = 0;
      while (p < text.size()) {
        const size_t nl = text.find('\n', p);
        const std::string row = text.substr(p, nl - p);
        p = nl + 1;
        if (row.rfind(day + "," + meal + ",", 0) != 0) kept += row + "\n";
      }
      return kept;
    };
    TEST_ASSERT_EQUAL_STRING(others(file).c_str(), others(next).c_str());
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
  // Counter-test: the old playbook rule (a correction appends its own row) is
  // exactly what double-counts, which is why the keyed replace is the rule.
  std::string appendOnly = std::string(kHead) + "\n";
  appendTo(appendOnly, "2026-10-25,lunch,x,450,1");
  appendTo(appendOnly, "2026-10-25,lunch,x,520,1");
  TEST_ASSERT_EQUAL_INT(970, (int)dayTotal(appendOnly, "2026-10-25"));
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
  // A stored row with an unquoted comma (an old plain append) is not counted as a
  // shifted number: the total says one row could not be read.
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
  TEST_ASSERT_EQUAL_INT(52, (int)t.total);
  TEST_ASSERT_EQUAL_INT(1, t.rows);
  TEST_ASSERT_EQUAL_INT(1, t.unread);
  // Its columns cannot be trusted, so a keyed write never matches it: the fixed
  // row is added and counted once; the wide row stays, reported, not counted.
  up = planCsvUpsert(legacy, head + "2026-10-26,lunch,\"chicken, rice\",450\n", "meal", "kcal", "2026-10-26");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(0, up.replaced);
  TEST_ASSERT_EQUAL_INT(502, (int)up.todayTotal);
  TEST_ASSERT_EQUAL_INT(1, up.days[0].unread);
  const std::string line = renderDayTotal(t, "kcal", true, "GMT");
  TEST_ASSERT_EQUAL_STRING(
      "today's kcal (2026-10-26, GMT): 52 from 1 row; 1 row dated 2026-10-26 is not counted: its "
      "'kcal' field could not be read (not a plain number, or an unquoted comma shifted it)",
      line.c_str());
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
  // The same meal logged again in full: it replaces the cut row.
  up = planCsvUpsert(cut, "date,meal,kcal\n2026-10-26,dinner,600\n", "meal", "kcal", "2026-10-26");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_TRUE(up.cutReplaced);
  TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n2026-10-26,lunch,450\n2026-10-26,dinner,600\n", up.out.c_str());
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
  // Blank lines above the header are kept; the header is still found.
  CsvUpsert up = planCsvUpsert("\n\ndate,meal,kcal\n2026-10-25,lunch,450\n", "date,meal,kcal\n2026-10-25,lunch,520\n",
                               "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("\n\ndate,meal,kcal\n2026-10-25,lunch,520\n", up.out.c_str());
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
  // A CRLF log with a real change: rewritten with LF line ends, nothing lost.
  up = planCsvUpsert("date,meal,kcal\r\n2026-10-24,dinner,990\r\n2026-10-25,lunch,450\r\n",
                     "date,meal,kcal\n2026-10-25,lunch,520\n", "meal", "kcal", "2026-10-25");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n2026-10-24,dinner,990\n2026-10-25,lunch,520\n", up.out.c_str());
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
  TEST_ASSERT_EQUAL_STRING("date,kcal\n2026-10-25,500\n#cut: 2026-10-25,\n", up.out.c_str());
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
  TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n2026-10-24,dinner,990\n2026-10-24,lunch,500\n#superseded: 2026-10-24,dinner,1100\n"
                           "2026-10-23,dinner,700\n",
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
  RUN_TEST(test_g1_day_total_matches_only_today);
  RUN_TEST(test_qw3_keyed_update_never_touches_another_day);
  RUN_TEST(test_qw3_no_update_drops_or_moves_a_row);
  RUN_TEST(test_qw3_correction_never_double_counts);
  RUN_TEST(test_qw3_header_mismatch_is_refused_not_misindexed);
  RUN_TEST(test_qw3_quoted_fields_and_honest_totals);
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
  return UNITY_END();
}
