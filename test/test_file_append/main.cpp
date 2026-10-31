#include <unity.h>

#include <algorithm>

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
  // A "day" column works too; numbers with units parse by their prefix.
  const std::string ex2 = "day,kcal\n2026-10-23,300 kcal\n";
  const nimbus::orch::CsvUpsert u2 =
      nimbus::orch::planCsvUpsert(ex2, "day,kcal\n", "day", "kcal", "2026-10-23");
  TEST_ASSERT_TRUE(u2.ok && u2.hasTotal);
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
  RUN_TEST(test_g1_day_total_matches_only_today);
  return UNITY_END();
}
