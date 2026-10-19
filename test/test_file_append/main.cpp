#include <unity.h>

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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_append_never_rewrites_a_stored_byte);
  RUN_TEST(test_append_refuses_empty_and_oversize);
  RUN_TEST(test_cut_file_is_reported_not_joined);
  RUN_TEST(test_tail_returns_exactly_the_last_n_rows);
  RUN_TEST(test_tail_render_names_what_is_shown);
  return UNITY_END();
}
