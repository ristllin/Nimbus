#include <unity.h>

#include <string>
#include <vector>

#include "nimbus/text_page.h"

using nimbus::TextPager;
using nimbus::wrapText;

void setUp() {}
void tearDown() {}

static void expectLines(const std::vector<std::string>& got,
                        const std::vector<const char*>& want) {
  TEST_ASSERT_EQUAL_UINT32(uint32_t(want.size()), uint32_t(got.size()));
  for (size_t i = 0; i < want.size(); ++i) {
    TEST_ASSERT_EQUAL_STRING(want[i], got[i].c_str());
  }
}

// ---- wrapText ---------------------------------------------------------------

static void test_empty_string_yields_one_empty_line() {
  expectLines(wrapText("", 10), {""});
}

static void test_spaces_only_yield_one_empty_line() {
  expectLines(wrapText("   ", 5), {""});
}

static void test_simple_greedy_wrap() {
  expectLines(wrapText("the quick brown fox jumps", 10),
              {"the quick", "brown fox", "jumps"});
}

static void test_exact_fit_line_is_not_split() {
  // 10 chars in 10 cols: fills the line exactly, next word starts fresh.
  expectLines(wrapText("abcdefghij kl", 10), {"abcdefghij", "kl"});
}

static void test_single_long_word_hard_breaks() {
  expectLines(wrapText("abcdefghijk", 4), {"abcd", "efgh", "ijk"});
}

static void test_long_word_starts_on_fresh_line_then_breaks() {
  // "abcdef" doesn't fit after "hi": current line flushes first, then the
  // word is chopped into cols-sized chunks from column 0.
  expectLines(wrapText("hi abcdef", 5), {"hi", "abcde", "f"});
}

static void test_multiple_and_trailing_spaces_collapse() {
  expectLines(wrapText("a   b  c   ", 20), {"a b c"});
}

static void test_leading_spaces_are_dropped() {
  expectLines(wrapText("   x", 5), {"x"});
}

static void test_newline_forces_break() {
  expectLines(wrapText("ab\ncd ef", 20), {"ab", "cd ef"});
}

static void test_blank_and_trailing_newlines_keep_empty_lines() {
  expectLines(wrapText("ab\n\ncd\n", 10), {"ab", "", "cd", ""});
}

static void test_cols_zero_treated_as_one() {
  expectLines(wrapText("ab c", 0), {"a", "b", "c"});
}

// ---- TextPager --------------------------------------------------------------

static void test_pager_counts_pages_and_clamps_index() {
  TextPager p;
  // cols=4: "three" hard-breaks -> one two thre e four five = 6 lines.
  p.setText("one two three four five", 4, 2);
  TEST_ASSERT_EQUAL_UINT32(3, uint32_t(p.pageCount()));
  expectLines(p.page(0), {"one", "two"});
  expectLines(p.page(1), {"thre", "e"});
  expectLines(p.page(2), {"four", "five"});
  expectLines(p.page(7), {"four", "five"});  // out of range clamps to last
}

static void test_pager_pads_single_short_page() {
  TextPager p;
  p.setText("solo", 10, 3);
  TEST_ASSERT_EQUAL_UINT32(1, uint32_t(p.pageCount()));
  expectLines(p.page(0), {"solo", "", ""});  // exactly linesPerPage entries
}

static void test_pager_pads_partial_last_page() {
  TextPager p;
  p.setText("aa bb cc", 2, 2);  // 3 lines -> 2 pages
  TEST_ASSERT_EQUAL_UINT32(2, uint32_t(p.pageCount()));
  expectLines(p.page(1), {"cc", ""});
}

static void test_pager_empty_text_is_one_blank_page() {
  TextPager p;
  p.setText("", 10, 2);
  TEST_ASSERT_EQUAL_UINT32(1, uint32_t(p.pageCount()));
  expectLines(p.page(0), {"", ""});
  expectLines(p.page(5), {"", ""});  // clamp still works on the blank page
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_empty_string_yields_one_empty_line);
  RUN_TEST(test_spaces_only_yield_one_empty_line);
  RUN_TEST(test_simple_greedy_wrap);
  RUN_TEST(test_exact_fit_line_is_not_split);
  RUN_TEST(test_single_long_word_hard_breaks);
  RUN_TEST(test_long_word_starts_on_fresh_line_then_breaks);
  RUN_TEST(test_multiple_and_trailing_spaces_collapse);
  RUN_TEST(test_leading_spaces_are_dropped);
  RUN_TEST(test_newline_forces_break);
  RUN_TEST(test_blank_and_trailing_newlines_keep_empty_lines);
  RUN_TEST(test_cols_zero_treated_as_one);
  RUN_TEST(test_pager_counts_pages_and_clamps_index);
  RUN_TEST(test_pager_pads_single_short_page);
  RUN_TEST(test_pager_pads_partial_last_page);
  RUN_TEST(test_pager_empty_text_is_one_blank_page);
  return UNITY_END();
}
