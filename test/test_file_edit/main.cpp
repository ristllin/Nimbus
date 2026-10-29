#include <unity.h>

#include <string>

#include "nimbus/orch/file_edit.h"

// files.edit's portable rules (file_edit.h): the decision (caps, policy,
// honest counts) and the streaming matcher (bounded carry, chunk-straddling
// matches, copy-through after the limit, a partial span at EOF is not a match).
// The device streams the file through the matcher in chunks; these tests feed
// the same content at every chunk size 1..8 so a boundary bug cannot hide
// behind one lucky split.

using namespace nimbus::orch;

namespace {

struct Run {
  std::string out;
  size_t found = 0, applied = 0;
};

Run streamAll(const std::string& src, const std::string& find, const std::string& replace,
              bool countOnly, size_t applyLimit, size_t chunk) {
  TextEditStream s(find, replace, countOnly);
  s.setApplyLimit(applyLimit);
  Run r;
  for (size_t i = 0; i < src.size(); i += chunk)
    s.feed(src.data() + i, std::min(chunk, src.size() - i), r.out);
  s.finish(r.out);
  r.found = s.found();
  r.applied = s.applied();
  return r;
}

// The out the matcher must produce, computed the plain way.
std::string expectOut(const std::string& src, const std::string& find,
                      const std::string& replace, size_t applyLimit) {
  std::string out;
  size_t from = 0, applied = 0;
  for (;;) {
    if (applied >= applyLimit) {
      out += src.substr(from);
      return out;
    }
    const size_t pos = src.find(find, from);
    if (pos == std::string::npos) return out + src.substr(from);
    out += src.substr(from, pos - from);
    out += replace;
    from = pos + find.size();
    applied++;
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---- the decision ------------------------------------------------------------

static void test_validate_the_caps_and_the_policy() {
  EditPolicy pol;
  TEST_ASSERT_EQUAL_STRING("", validateEdit(3, 0, pol).c_str());   // empty replace = delete
  TEST_ASSERT_TRUE(validateEdit(0, 1, pol).find("find text is empty") != std::string::npos);
  TEST_ASSERT_TRUE(validateEdit(kEditFindMaxBytes + 1, 1, pol).find("longer than") !=
                  std::string::npos);
  TEST_ASSERT_TRUE(validateEdit(1, kEditReplaceMaxBytes + 1, pol).find("replacement is longer") !=
                  std::string::npos);
  EditPolicy bad;
  bad.count = 0;
  TEST_ASSERT_TRUE(validateEdit(1, 1, bad).find("between 1 and") != std::string::npos);
  bad.count = kEditMaxMatches + 1;
  TEST_ASSERT_TRUE(validateEdit(1, 1, bad).find("between 1 and") != std::string::npos);
}

static void test_no_match_refuses_plainly() {
  EditDecision d = decideEdit(4, 2, EditPolicy{}, 0);
  TEST_ASSERT_FALSE(d.ok);
  TEST_ASSERT_EQUAL(0, d.apply);
  TEST_ASSERT_TRUE(d.err.find("no match") != std::string::npos);
  TEST_ASSERT_TRUE(d.err.find("nothing changed") != std::string::npos);
}

static void test_first_mode_replaces_the_first_count() {
  // The default: the first match only, however many there are.
  EditDecision d = decideEdit(3, 5, EditPolicy{}, 3);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL(1, d.apply);
  TEST_ASSERT_EQUAL(3, (int)d.matches);
  TEST_ASSERT_EQUAL(2, (int)d.sizeDelta);   // (5 - 3) * 1
  // count N: the first N matches, bounded by what is there.
  EditPolicy pol;
  pol.count = 2;
  d = decideEdit(3, 1, pol, 5);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL(2, d.apply);
  TEST_ASSERT_EQUAL(-4, (int)d.sizeDelta);  // (1 - 3) * 2, a shrink
  pol.count = 7;
  d = decideEdit(3, 1, pol, 3);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL(3, d.apply);            // fewer matches than asked: all of them
  // all: every match.
  EditPolicy all;
  all.all = true;
  d = decideEdit(3, 5, all, 3);
  TEST_ASSERT_TRUE(d.ok);
  TEST_ASSERT_EQUAL(3, d.apply);
  TEST_ASSERT_EQUAL(6, (int)d.sizeDelta);
  // all beyond the hard cap: refused WITH the count.
  d = decideEdit(1, 1, all, (size_t)kEditMaxMatches + 1);
  TEST_ASSERT_FALSE(d.ok);
  TEST_ASSERT_EQUAL(0, d.apply);
  TEST_ASSERT_TRUE(d.err.find("too many matches (201") != std::string::npos);
}

// ---- the matcher: counting ----------------------------------------------------

static void test_counting_is_non_overlapping_and_chunk_safe() {
  TEST_ASSERT_EQUAL(2, (int)streamAll("aaaa", "aa", "", true, 0, 4).found);   // not 3
  TEST_ASSERT_EQUAL(0, (int)streamAll("xxab", "abcd", "", true, 0, 4).found);
  TEST_ASSERT_EQUAL(1, (int)streamAll("xxabcdyy", "abcd", "", true, 0, 4).found);
  // The match straddles every chunk boundary at some size; all sizes agree.
  for (size_t chunk = 1; chunk <= 8; chunk++)
    TEST_ASSERT_EQUAL(1, (int)streamAll("xxabcdyy", "abcd", "", true, 0, chunk).found);
  TEST_ASSERT_EQUAL(3, (int)streamAll("a-b-a-b-a", "a", "", true, 0, 3).found);
  // Counting emits nothing.
  Run r = streamAll("one two one", "one", "", true, 0, 3);
  TEST_ASSERT_EQUAL(2, (int)r.found);
  TEST_ASSERT_EQUAL(0, (int)r.out.size());
}

// ---- the matcher: replacing ---------------------------------------------------

static void test_first_only_replaces_the_first_match() {
  Run r = streamAll("one two one two", "one", "1", false, 1, 4);
  TEST_ASSERT_EQUAL(1, (int)r.applied);
  // In replace mode the count stops at the limit (the scan pass is what
  // counts the whole file); the second match copies through untouched.
  TEST_ASSERT_EQUAL(1, (int)r.found);
  TEST_ASSERT_EQUAL_STRING("1 two one two", r.out.c_str());
}

static void test_all_replaces_every_match() {
  Run r = streamAll("one two one two", "one", "1", false, 2, 4);
  TEST_ASSERT_EQUAL(2, (int)r.applied);
  TEST_ASSERT_EQUAL_STRING("1 two 1 two", r.out.c_str());
}

static void test_copy_through_after_the_limit_keeps_later_matches() {
  // The limit is hit mid-file: everything left copies through unchanged,
  // including later matches.
  Run r = streamAll("aXaYa", "a", "R", false, 1, 5);
  TEST_ASSERT_EQUAL(1, (int)r.applied);
  TEST_ASSERT_EQUAL_STRING("RXaYa", r.out.c_str());
  // The size bookkeeping: (R - a) * 1 = no change here.
  TEST_ASSERT_EQUAL(5, (int)r.out.size());
}

static void test_a_replacement_is_never_rescanned() {
  // Replace "a" with "aa" on "aaa", limit 2: the first two matches become
  // "aa" each, the third byte copies through - 5 bytes, not a cascade.
  Run r = streamAll("aaa", "a", "aa", false, 2, 3);
  TEST_ASSERT_EQUAL(2, (int)r.applied);
  TEST_ASSERT_EQUAL_STRING("aaaaa", r.out.c_str());
}

static void test_a_partial_span_at_eof_is_not_a_match() {
  Run r = streamAll("xxab", "abcd", "zz", false, 2, 2);
  TEST_ASSERT_EQUAL(0, (int)r.applied);
  TEST_ASSERT_EQUAL_STRING("xxab", r.out.c_str());
  Run whole = streamAll("xxab", "abcd", "zz", false, 2, 8);
  TEST_ASSERT_EQUAL_STRING(r.out.c_str(), whole.out.c_str());
}

static void test_deletion_and_growth_sizes() {
  Run del = streamAll("keep DROPTHIS keep", "DROPTHIS", "", false, 1, 5);
  TEST_ASSERT_EQUAL_STRING("keep  keep", del.out.c_str());
  Run grow = streamAll("x axb x", "a", "a-b-c-d", false, 1, 3);
  TEST_ASSERT_EQUAL_STRING("x a-b-c-dxb x", grow.out.c_str());
  TEST_ASSERT_EQUAL(6, (int)(grow.out.size() - 7));   // (7 - 1) * 1 growth
}

// ---- the property: every chunk size agrees with the plain way ----------------

static void test_every_chunk_split_agrees_with_the_whole() {
  const char* srcs[] = {"the quick brown fox\njumps over\nthe quick brown dog\n",
                       "date,meal,kcal\n2026-10-29,breakfast,400\n2026-10-29,lunch,760\n",
                       "aaabaaabaaab", "no matches here", ""};
  const char* finds[] = {"quick brown", "2026-10-29,lunch,760", "aaab", "zz", "the"};
  const char* reps[] = {"slow red", "2026-10-29,lunch,720", "x", "yy", "THE"};
  for (size_t i = 0; i < 5; i++) {
    const size_t matches = streamAll(srcs[i], finds[i], "", true, 0, 8).found;
    for (size_t limit = 0; limit <= matches; limit++) {
      const std::string want = expectOut(srcs[i], finds[i], reps[i], limit);
      for (size_t chunk = 1; chunk <= 8; chunk++) {
        const Run r = streamAll(srcs[i], finds[i], reps[i], false, limit, chunk);
        char label[96];
        snprintf(label, sizeof label, "case %zu limit %zu chunk %zu", i, limit, chunk);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), r.out.c_str(), label);
        TEST_ASSERT_EQUAL_MESSAGE((int)std::min(limit, matches), (int)r.applied, label);
      }
    }
  }
}

// ---- the document-shaped cases ------------------------------------------------

static void test_editing_a_row_of_a_log_by_its_exact_span() {
  // The motivating shape: fix one bad row without touching the others.
  const std::string log = "date,meal,kcal\n2026-10-29,breakfast,400\n"
                          "2026-10-29,lunch,760\n2026-10-29,dinner,650\n";
  Run r = streamAll(log, "2026-10-29,lunch,760", "2026-10-29,lunch,720", false, 1, 7);
  TEST_ASSERT_EQUAL(1, (int)r.applied);
  TEST_ASSERT_EQUAL_STRING("date,meal,kcal\n2026-10-29,breakfast,400\n"
                           "2026-10-29,lunch,720\n2026-10-29,dinner,650\n",
                           r.out.c_str());
  // The other rows are byte-identical (a prefix and suffix check).
  const size_t rowAt = log.find("2026-10-29,lunch");
  TEST_ASSERT_TRUE(r.out.substr(0, rowAt) == log.substr(0, rowAt));
  TEST_ASSERT_TRUE(r.out.substr(r.out.find("2026-10-29,dinner")) ==
                  log.substr(log.find("2026-10-29,dinner")));
}

static void test_a_whole_line_is_just_a_longer_span() {
  const std::string doc = "# Title\nold line\nnew tail\n";
  Run r = streamAll(doc, "old line\n", "replaced line\n", false, 1, 4);
  TEST_ASSERT_EQUAL_STRING("# Title\nreplaced line\nnew tail\n", r.out.c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_validate_the_caps_and_the_policy);
  RUN_TEST(test_no_match_refuses_plainly);
  RUN_TEST(test_first_mode_replaces_the_first_count);
  RUN_TEST(test_counting_is_non_overlapping_and_chunk_safe);
  RUN_TEST(test_first_only_replaces_the_first_match);
  RUN_TEST(test_all_replaces_every_match);
  RUN_TEST(test_copy_through_after_the_limit_keeps_later_matches);
  RUN_TEST(test_a_replacement_is_never_rescanned);
  RUN_TEST(test_a_partial_span_at_eof_is_not_a_match);
  RUN_TEST(test_deletion_and_growth_sizes);
  RUN_TEST(test_every_chunk_split_agrees_with_the_whole);
  RUN_TEST(test_editing_a_row_of_a_log_by_its_exact_span);
  RUN_TEST(test_a_whole_line_is_just_a_longer_span);
  return UNITY_END();
}
