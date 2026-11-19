// append_file's facts (file_aids.h): the newline fence and the identical-line fact.
//
// Class rules proven here:
//   - the fence: a file that does not end in a line end gets "\n" before the text, so an
//     appended row can never join a torn last line; an empty file and a file ending in
//     "\n" get none;
//   - over generated tails (three equal domains, repeated rows, CRLF rows, a tail that
//     starts mid-line), the fact names exactly the nearest identical line among the last
//     64 lines and the lines appended before it, with the line number the row will have,
//     and nothing when there is none; the text itself is never changed or refused.
#include <unity.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../support/fake_aid_card.h"
#include "nimbus/orch/file_aids.h"

using namespace nimbus::orch::aids;
using aidfake::Rng;

void setUp() {}
void tearDown() {}

namespace {

std::vector<std::string> splitLines(const std::string& f) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a < f.size()) {
    size_t b = f.find('\n', a);
    if (b == std::string::npos) { out.push_back(f.substr(a)); break; }
    out.push_back(f.substr(a, b - a));
    a = b + 1;
  }
  return out;
}
std::string noCr(std::string s) {
  if (!s.empty() && s.back() == '\r') s.pop_back();
  return s;
}

// The nearest earlier line identical to window[me-1]: the appended ones, then the
// file's last 64 lines. 0 for none.
size_t refNearest(const std::vector<std::string>& window, size_t me, size_t fileLines) {
  const std::string& row = window[me - 1];
  for (size_t lineNo = me - 1; lineNo >= 1; --lineNo) {
    if (lineNo <= fileLines && lineNo + kAppendTailLines <= fileLines) break;
    if (window[lineNo - 1] == row) return lineNo;
  }
  return 0;
}

std::string refFact(bool known, bool single, size_t me, size_t hit, size_t k) {
  const std::string where = known ? "line " + std::to_string(hit) : "the line " + std::to_string(me - hit) + " above it";
  if (single) return "identical to " + where;
  return (known ? "line " + std::to_string(me) : "appended line " + std::to_string(k + 1)) + " is identical to " + where;
}

// The reference note for a whole file and a text.
std::string referenceNote(const std::string& file, const std::string& text, bool linesKnown) {
  std::string note = (!file.empty() && file.back() != '\n') ? "; added a newline before the text" : "";
  std::vector<std::string> window = splitLines(file);
  const size_t fileLines = window.size();
  for (auto& l : window) l = noCr(l);
  const auto add = splitLines(text);
  std::vector<std::string> facts;
  for (size_t k = 0; k < add.size(); ++k) {
    window.push_back(noCr(add[k]));
    const size_t me = fileLines + 1 + k;
    const size_t hit = window.back().empty() ? 0 : refNearest(window, me, fileLines);
    if (hit) facts.push_back(refFact(linesKnown, add.size() == 1, me, hit, k));
  }
  for (size_t i = 0; i < facts.size() && i < kAidListMax; ++i) note += (i ? ", " : "; ") + facts[i];
  if (facts.size() > kAidListMax) note += " +" + std::to_string(facts.size() - kAidListMax);
  return note;
}

std::string tailOf(const std::string& f, bool& whole) {
  whole = f.size() <= kAppendTailBytes;
  return whole ? f : f.substr(f.size() - kAppendTailBytes);
}

}  // namespace

static void test_the_fence_and_the_identical_fact_have_their_exact_forms() {
  const std::string f = "date,amount\n2026-11-19,5\n2026-11-19,6\n";
  AppendFacts a = appendFacts(f, true, f.size(), 3, "2026-11-19,5\n");
  TEST_ASSERT_FALSE(a.fence);
  TEST_ASSERT_EQUAL_STRING("; identical to line 2", a.note.c_str());
  const std::string torn = "date,amount\n2026-11-19,5\n2026-11-19,6";
  a = appendFacts(torn, true, torn.size(), 3, "2026-11-19,7\n");
  TEST_ASSERT_TRUE(a.fence);
  TEST_ASSERT_EQUAL_STRING("; added a newline before the text", a.note.c_str());
  a = appendFacts("", true, 0, 0, "x\n");
  TEST_ASSERT_FALSE(a.fence);
  TEST_ASSERT_EQUAL_STRING("", a.note.c_str());
  a = appendFacts(f, true, f.size(), 3, "2026-11-19,6\n2026-11-19,6\n");
  TEST_ASSERT_EQUAL_STRING("; line 4 is identical to line 3, line 5 is identical to line 4", a.note.c_str());
  // Line count unknown (a big file): the distance instead.
  a = appendFacts(f, false, 900000, 0, "2026-11-19,5\n");
  TEST_ASSERT_EQUAL_STRING("; identical to the line 2 above it", a.note.c_str());
}

static void test_the_fact_names_the_nearest_identical_line_over_generated_tails() {
  size_t facts = 0, none = 0;
  const char* rows[] = {"2026-11-19,08:00,oats,40,150", "2026-11-19,cafe,misc,3.50", "2026-11-19,squat,3,5,100",
                        "2026-11-19,cafe,misc,3.50\r", "\"a, b\",1"};
  for (uint64_t seed = 1; seed <= 400; ++seed) {
    Rng g(seed);
    std::string f = "h\n";
    const size_t n = g.below(300);
    for (size_t i = 0; i < n; ++i) f += std::string(rows[g.below(5)]) + (g.chance(50) ? std::to_string(g.below(4)) : "") + "\n";
    if (g.chance(15)) f += "torn";
    std::string text;
    const size_t k = 1 + g.below(3);
    for (size_t i = 0; i < k; ++i) text += std::string(rows[g.below(5)]) + (g.chance(70) ? std::to_string(g.below(400)) : "") + "\n";
    if (g.chance(10)) text.pop_back();
    bool whole = false;
    const std::string tail = tailOf(f, whole);
    const size_t lines = splitLines(f).size();
    const bool known = g.chance(70);
    const AppendFacts a = appendFacts(tail, whole, f.size(), known ? lines : 0, text);
    // Line numbers are only claimed where the tail covers them.
    std::string want = referenceNote(f, text, known);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), a.note.c_str(), std::to_string(seed).c_str());
    TEST_ASSERT_EQUAL(!f.empty() && f.back() != '\n', a.fence);
    (a.note.find("identical") != std::string::npos ? facts : none)++;
  }
  TEST_ASSERT_TRUE(facts > 50);
  TEST_ASSERT_TRUE(none > 20);
}

static void test_a_tail_that_starts_mid_line_never_compares_the_cut_line() {
  // The 8 KB tail starts exactly at "2026-11-19,cafe,misc,3.50", but the line there is
  // really "PREFIX2026-11-19,cafe,misc,3.50": the cut first line is never compared.
  const std::string row = "2026-11-19,cafe,misc,3.50";
  const size_t need = kAppendTailBytes - row.size() - 1;
  std::string z;
  const size_t k = need / 1001;   // a few long lines: the cut line is inside the 64-line window
  for (size_t i = 0; i < k; ++i) z += std::string(i == 0 ? 1000 + need % 1001 : 1000, 'y') + "\n";
  const std::string f = "h\nPREFIX" + row + "\n" + z;
  bool whole = false;
  const std::string tail = tailOf(f, whole);
  TEST_ASSERT_FALSE(whole);
  TEST_ASSERT_EQUAL(0, tail.compare(0, row.size(), row));
  const size_t lines = splitLines(f).size();
  TEST_ASSERT_EQUAL_STRING("", appendFacts(tail, whole, f.size(), lines, row + "\n").note.c_str());
  // Read whole, the line is what it is: not identical either.
  TEST_ASSERT_EQUAL_STRING("", appendFacts(f, true, f.size(), lines, row + "\n").note.c_str());
  // And a whole line in the tail is compared.
  TEST_ASSERT_EQUAL_STRING(("; identical to line " + std::to_string(lines) + "").c_str(),
                           appendFacts(tail, whole, f.size(), lines, std::string(1000, 'y') + "\n").note.c_str());
}

static void test_shared_list_and_hash_rules() {
  LineList l;
  TEST_ASSERT_EQUAL_STRING("", renderLines(l).c_str());
  l.count = 1;
  l.lines = {7};
  TEST_ASSERT_EQUAL_STRING("line 7", renderLines(l).c_str());
  l.count = 11;
  l.lines = {1, 2, 3, 4, 5, 6, 7, 8};
  TEST_ASSERT_EQUAL_STRING("lines 1, 2, 3, 4, 5, 6, 7, 8 +3", renderLines(l).c_str());
  TEST_ASSERT_TRUE(fnv64(kFnv64Basis, "") == kFnv64Basis);
  TEST_ASSERT_TRUE(fnv64(fnv64(kFnv64Basis, "ab"), "cd") == fnv64(kFnv64Basis, "abcd"));
  TEST_ASSERT_EQUAL_STRING("unknown", sha8Hex(0).c_str());
  TEST_ASSERT_EQUAL_STRING("0123abcd", sha8Hex(0x0123abcd00000000ULL).c_str());
  TEST_ASSERT_EQUAL_STRING("last line has no newline (a write may have been cut short)", kTornTail);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_fence_and_the_identical_fact_have_their_exact_forms);
  RUN_TEST(test_the_fact_names_the_nearest_identical_line_over_generated_tails);
  RUN_TEST(test_a_tail_that_starts_mid_line_never_compares_the_cut_line);
  RUN_TEST(test_shared_list_and_hash_rules);
  return UNITY_END();
}
