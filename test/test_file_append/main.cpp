#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include <string>
#include <vector>

#include "nimbus/orch/file_append.h"

// files.append and the files.read tail view (file_append.h): the class rules.
//   - an append never rewrites a stored byte: the file before is always a byte
//     prefix of the file after, whatever the text (CR/LF mixes, no final line
//     end, a cut last row) and whatever the file holds (owner ruling 41: a food
//     log, an expenses log and a training log, with identical expectations);
//   - a refused allocation (ruling 30) plans nothing: the file keeps every byte;
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
      // RB-TR1: the byte the first shown row starts at, so the reply can name the rest.
      size_t firstAt = 0;
      for (size_t k = 0; k < rows.size() - want; k++) firstAt += rows[k].size() + 1;
      TEST_ASSERT_EQUAL_UINT32((uint32_t)firstAt, (uint32_t)v.firstAt);
      if (v.earlier) {
        const std::string shown = renderTail(v, "log.csv", file.size(), "");
        const std::string says = "earlier rows not shown: bytes 0-" + std::to_string(firstAt) + ", read them with \"offset\":0";
        TEST_ASSERT_TRUE_MESSAGE(shown.find(says) != std::string::npos, shown.substr(0, 160).c_str());
      }
    }
  }
}

static void test_tail_render_names_what_is_shown() {
  const std::string file = "date,meal,kcal\na,b,1\nc,d,2\ne,f,3\n";
  const TailView all = tailRows(file, 0, 10);
  const std::string whole = renderTail(all, "log.csv", file.size(), "date,meal,kcal");
  TEST_ASSERT_TRUE(whole.find("last 4 rows of log.csv (") == 0);
  TEST_ASSERT_TRUE(whole.find("the whole file") != std::string::npos);
  TEST_ASSERT_TRUE(whole.find("header:") == std::string::npos);   // already in the rows
  const TailView two = tailRows(file, 0, 2);
  const std::string part = renderTail(two, "log.csv", file.size(), "date,meal,kcal");
  TEST_ASSERT_TRUE(part.find("earlier rows not shown") != std::string::npos);
  TEST_ASSERT_TRUE(part.find("header: date,meal,kcal\nc,d,2\ne,f,3\n") != std::string::npos);
}

// ---- the refusing allocator (ruling 30) --------------------------------------------
// This suite's own operator new: armed, it refuses the Nth allocation from arming on (and
// with gFailAt == 0, every one). Off, it is malloc.
static bool gArmed = false, gHit = false;
static long gAllocs = 0, gFailAt = -1;
void* operator new(std::size_t n) {
  if (gArmed && gAllocs++ >= gFailAt) {
    gHit = true;
    throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// The file's content hash as read_file shows it (8 hex of FNV-1a 64), for the "sha8 unchanged" check.
static std::string sha8(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
  return std::string(buf, 8);
}

// ---- three tables, one rule (owner ruling 41, ruling 42 item 7) ---------------------
struct Domain {
  const char* name;
  const char* header;
  const char* row;      // a row already stored
  const char* fix;      // the same date and item, another figure (what a keyed write used to merge)
  const char* col;      // the figure column
};
static const Domain kDomains[] = {
    {"food", "date,time,item,qty,kcal", "2026-11-19,12:40,soup,1,310", "2026-11-19,12:40,soup,1,280", "kcal"},
    {"expenses", "date,time,item,qty,amount", "2026-11-19,12:40,train ticket,1,24.30", "2026-11-19,12:40,train ticket,1,21.10", "amount"},
    {"training", "date,time,item,qty,kg", "2026-11-19,12:40,squat,5,80", "2026-11-19,12:40,squat,5,85", "kg"},
};

// Every path of a pure append on each table: a new row, the same row again, a "correction"
// of a stored row, the header again, a data row first in a new file, a file without its last
// line end. Each lands as sent after every stored byte; nothing is merged, refused or rewritten.
static void test_three_tables_append_alike_on_every_path() {
  for (const Domain& d : kDomains) {
    const std::string h = d.header, r = d.row, fix = d.fix;
    std::string file = h + "\n" + r + "\n";
    std::vector<std::string> sent = {r, fix, h, r + "\n" + fix};
    for (const std::string& t : sent) {
      const std::string before = file;
      AppendPlan p;
      TEST_ASSERT_TRUE_MESSAGE(appendTo(file, t, &p), d.name);
      TEST_ASSERT_EQUAL_STRING_MESSAGE((before + t + "\n").c_str(), file.c_str(), d.name);
      TEST_ASSERT_FALSE(p.repairedCut);
    }
    // A new file whose first row is data, not a header: created as sent (no refusal).
    std::string fresh;
    TEST_ASSERT_TRUE_MESSAGE(appendTo(fresh, r), d.name);
    TEST_ASSERT_EQUAL_STRING((r + "\n").c_str(), fresh.c_str());
    // A file whose last row has no line end: the rows start on a fresh line, the stored row
    // is left exactly as it was (never set aside or rewritten).
    std::string cut = h + "\n" + r;
    AppendPlan p;
    TEST_ASSERT_TRUE(appendTo(cut, fix, &p));
    TEST_ASSERT_TRUE(p.repairedCut);
    TEST_ASSERT_EQUAL_STRING_MESSAGE((h + "\n" + r + "\n" + fix + "\n").c_str(), cut.c_str(), d.name);
  }
}

// The errors are plain and the same for every table: no text, too much text. Nothing else
// is refused, and a refused call writes nothing.
static void test_three_tables_get_the_same_plain_errors() {
  std::string want[2];
  for (const Domain& d : kDomains) {
    std::string file = std::string(d.header) + "\n";
    const std::string before = file;
    AppendPlan p;
    TEST_ASSERT_FALSE(appendTo(file, " \n", &p));
    if (want[0].empty()) want[0] = p.err;
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want[0].c_str(), p.err.c_str(), d.name);
    TEST_ASSERT_FALSE(appendTo(file, std::string(kAppendMaxBytes + 1, 'x'), &p));
    if (want[1].empty()) want[1] = p.err;
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want[1].c_str(), p.err.c_str(), d.name);
    TEST_ASSERT_EQUAL_STRING(before.c_str(), file.c_str());
  }
  TEST_ASSERT_TRUE(want[0].find("need 'text'") == 0);
  TEST_ASSERT_TRUE(want[1].find("too large") != std::string::npos);
}

// Ruling 30: under an allocator that refuses every request, and then at every single
// allocation of the call, the append answers a clean error or the right plan, never an
// exception; on an error the file's bytes and sha8 are unchanged (nothing to write).
static void test_a_refused_allocation_plans_nothing_and_keeps_every_byte() {
  for (const Domain& d : kDomains) {
    const std::string stored = std::string(d.header) + "\n" + d.row;   // no final line end
    const std::string text = std::string(d.fix) + "\r\n" + d.row;
    const std::string want = stored + "\n" + d.fix + "\n" + d.row + "\n";
    int refusals = 0;
    for (long k = 0;; ++k) {
      std::string file = stored;
      const std::string hashBefore = sha8(file);
      AppendPlan p;
      bool threw = false;
      gAllocs = 0;
      gFailAt = k;
      gHit = false;
      gArmed = true;
      try {
        p = planAppend(text, file.size(), file.back());
      } catch (...) {
        threw = true;
      }
      gArmed = false;
      TEST_ASSERT_FALSE_MESSAGE(threw, d.name);
      if (p.ok) file += p.bytes;
      if (!gHit) {
        TEST_ASSERT_TRUE(p.ok);
        TEST_ASSERT_EQUAL_STRING(want.c_str(), file.c_str());
        break;
      }
      ++refusals;
      TEST_ASSERT_FALSE_MESSAGE(p.ok, "a refused allocation still planned bytes");
      TEST_ASSERT_EQUAL_STRING(kAppendNoMemory, p.err.c_str());
      TEST_ASSERT_EQUAL_STRING(stored.c_str(), file.c_str());
      TEST_ASSERT_EQUAL_STRING(hashBefore.c_str(), sha8(file).c_str());
    }
    TEST_ASSERT_TRUE_MESSAGE(refusals >= 1, "the sweep never reached an allocation");
  }
}

// ---- the day total (files.read {tail, sum}) ------------------------------------------
static DayTotal tallyOf(const std::string& file, const std::string& col, const std::string& day) {
  CsvDayTally tally(col, day);
  RowSplitter rows([&](const std::string& r, bool c) { tally.feed(r, c); return true; });
  rows.push(file.data(), file.size());
  rows.end();
  return tally.total();
}

// The same rows give the same total on every table: identical rows count twice (two coffees
// bought, two meals, two sets), a row of another day never counts, a row that states a total
// never counts, a row whose field count is not the header's is unread and the total says
// "not known", and a pasted header is no row. No item word or column name changes a figure.
static void test_three_tables_total_alike() {
  for (const Domain& d : kDomains) {
    const std::string h = std::string(d.header) + "\n";
    const std::string file = h + d.row + "\n" + d.row + "\n" + "2026-11-18,23:50,x,1,100\n" + h +
                             "2026-11-19,23:59,total,,999\n";
    const DayTotal t = tallyOf(file, d.col, "2026-11-19");
    const double one = tallyOf(h + d.row + "\n", d.col, "2026-11-19").total;
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, t.rows, d.name);
    TEST_ASSERT_TRUE_MESSAGE(t.total > 2 * one - 0.001 && t.total < 2 * one + 0.001, d.name);
    TEST_ASSERT_EQUAL_INT(1, t.totalRows);
    TEST_ASSERT_EQUAL_INT(0, t.unread);
    // a bare comma in a stored row: never guessed back into a field
    const DayTotal w = tallyOf(h + "2026-11-19,12:40,a, b,1,50\n", d.col, "2026-11-19");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, w.unread, d.name);
    TEST_ASSERT_TRUE(renderDayTotal(w, d.col, true, "GMT").find("not known") != std::string::npos);
    // a narrow row: unread, never a figure from the wrong column
    TEST_ASSERT_EQUAL_INT(1, tallyOf(h + "2026-11-19,12:40,50\n", d.col, "2026-11-19").unread);
  }
}

// A comma inside a balanced pair of parentheses stays in its field (rows written before
// v5.2.0 hold them); an unbalanced one splits, and the row is unread, not swallowed.
static void test_a_comma_inside_parentheses_stays_in_its_field() {
  const std::string h = "date,item,amount\n";
  DayTotal t = tallyOf(h + "2026-10-29,tea (no sugar, oat milk),3\n2026-10-29,soup (a, b (c, d)),4\n", "amount",
                       "2026-10-29");
  TEST_ASSERT_EQUAL(2, t.rows);
  TEST_ASSERT_EQUAL(0, t.unread);
  TEST_ASSERT_EQUAL_INT(7, (int)t.total);
  t = tallyOf(h + "2026-10-29,rice (white, cooked,4\n", "amount", "2026-10-29");
  TEST_ASSERT_EQUAL(1, t.unread);
  std::string opens = "2026-10-29,";
  for (int i = 0; i < 4000; i++) opens += "(,";
  TEST_ASSERT_EQUAL(1, tallyOf(h + opens + "\n", "amount", "2026-10-29").unread);
  // One rule for a CSV name across the device, nimbusd and the lab.
  TEST_ASSERT_TRUE(isCsvName("log.csv"));
  TEST_ASSERT_TRUE(isCsvName("Log.CSV"));
  TEST_ASSERT_FALSE(isCsvName(".csv"));
  TEST_ASSERT_FALSE(isCsvName("log.txt"));
  // A power cut inside the parentheses still reads as a cut row, not a whole one.
  TEST_ASSERT_TRUE(csvLastRowCut("date,item,amount", "2026-10-29,soup,100", "2026-10-29,rice (white, coo"));
}

static void test_quoted_commas_and_thousands() {
  const std::string file =
      "date,item,amount\n"
      "2026-10-28,\"rent, utilities, \"\"misc\"\"\",\"1,045\"\n"
      "2026-10-28,\"tea, black\",5\n";
  const DayTotal t = tallyOf(file, "amount", "2026-10-28");
  TEST_ASSERT_EQUAL_INT(1050, (int)t.total);
  TEST_ASSERT_EQUAL_INT(2, t.rows);
  TEST_ASSERT_EQUAL_INT(0, t.unread);
}

// Days across midnight: the row's own date decides, in every format a row carries it.
static void test_days_across_midnight() {
  const std::string file =
      "timestamp,item,amount\n"
      "2026-10-31 23:58,toast,120\n"
      "2026-10-31T23:59:30,tea,5\n"
      "2026-11-01 00:01,apple,52\n"
      "2026-11-01T00:30:00,tea,5\n";
  TEST_ASSERT_EQUAL_INT(125, (int)tallyOf(file, "amount", "2026-10-31").total);
  TEST_ASSERT_EQUAL_INT(57, (int)tallyOf(file, "amount", "2026-11-01").total);
  TEST_ASSERT_EQUAL_INT(0, (int)tallyOf(file, "amount", "2026-11-02").total);
}

// The whole-text line (nimbusd, the lab) is the device's stream, word for word.
static void test_the_whole_text_line_names_the_day_and_zone() {
  const std::string file = "date,item,kg\n2026-10-25,squat,80\n2026-10-25,row,60\n2026-10-24,curl,20\n";
  const std::string line = csvDayTotalLine(file, "kg", "2026-10-25", "GMT");
  TEST_ASSERT_EQUAL_STRING("today's kg (2026-10-25, GMT): 140 from 2 rows\n", line.c_str());
  TEST_ASSERT_TRUE(csvDayTotalLine(file, "amount", "2026-10-25", "GMT").find("[no day total: the file has no column 'amount'") == 0);
  TEST_ASSERT_TRUE(csvDayTotalLine("item,kg\nsquat,80\n", "kg", "2026-10-25", "GMT").find("no date column") != std::string::npos);
  TEST_ASSERT_TRUE(csvDayTotalLine(file, "kg", "", "GMT").find("clock is not set") != std::string::npos);
}

// An older keyed call names what the append does not take, the same words for every table,
// and is written as nothing (the binding returns it before planning).
static void test_old_keyed_arguments_are_named_not_merged() {
  TEST_ASSERT_EQUAL_STRING("", appendArgsError({}).c_str());
  TEST_ASSERT_EQUAL_STRING("files.append takes project, name and text; key is not an argument of it, and nothing was added",
                           appendArgsError({"key"}).c_str());
  TEST_ASSERT_TRUE(appendArgsError({"key", "sum", "replace"}).find("key, sum, replace are not an argument") !=
                   std::string::npos);
}

// A row of another width than the header lands as sent and the reply says so, the same on
// every table; a new file's own first line is its header.
static void test_a_row_of_another_width_lands_and_is_said() {
  for (const Domain& d : kDomains) {
    const std::string h = d.header;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", csvWidthNote(h, std::string(d.row) + "\n").c_str(), d.name);
    const std::string note = csvWidthNote(h, std::string(d.row) + "\n2026-11-19,x\n2026-11-19,12:40,a, b,1,2\n");
    TEST_ASSERT_TRUE_MESSAGE(note.find("2 rows have a field count other than the header's 5") != std::string::npos,
                             note.c_str());
    TEST_ASSERT_EQUAL_STRING("", csvWidthNote("", h + "\n" + d.row + "\n").c_str());
    TEST_ASSERT_TRUE(csvWidthNote("", h + "\n2026-11-19,x\n").find("1 row has") != std::string::npos);
    TEST_ASSERT_EQUAL_STRING("", csvWidthNote(h, "\n" + std::string(d.row) + "\n#note\n").c_str());
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_append_never_rewrites_a_stored_byte);
  RUN_TEST(test_append_refuses_empty_and_oversize);
  RUN_TEST(test_cut_file_is_reported_not_joined);
  RUN_TEST(test_tail_returns_exactly_the_last_n_rows);
  RUN_TEST(test_tail_render_names_what_is_shown);
  RUN_TEST(test_three_tables_append_alike_on_every_path);
  RUN_TEST(test_three_tables_get_the_same_plain_errors);
  RUN_TEST(test_a_refused_allocation_plans_nothing_and_keeps_every_byte);
  RUN_TEST(test_three_tables_total_alike);
  RUN_TEST(test_a_comma_inside_parentheses_stays_in_its_field);
  RUN_TEST(test_quoted_commas_and_thousands);
  RUN_TEST(test_days_across_midnight);
  RUN_TEST(test_the_whole_text_line_names_the_day_and_zone);
  RUN_TEST(test_old_keyed_arguments_are_named_not_merged);
  RUN_TEST(test_a_row_of_another_width_lands_and_is_said);
  return UNITY_END();
}
