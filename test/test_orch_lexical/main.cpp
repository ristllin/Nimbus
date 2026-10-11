// CUM-515 - the shared word-match rule (nimbus/orch/lexical.h) every text
// search on the device runs: memory.episodic + GET /api/mem/episodic, the
// memory.search / memory.archive keyword fallback, files.search, docs.search.
// Bench B, step 9: "Who is my PhD advisor again?" -> the model searched
// "advisor supervisor PhD", the old matcher required EVERY word, the owner had
// only said "my advisor is Dr. Leila Mansour", so the answer was "I don't have
// your advisor's name". These tests pin the rule itself and its memory bound.
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "nimbus/orch/episodic.h"
#include "nimbus/orch/lexical.h"

using namespace nimbus::orch;

// ---- heap allocation counter (the DoD memory bound is asserted, not claimed) --
static bool g_countAllocs = false;
static long g_allocs = 0;
void* operator new(std::size_t n) {
  if (g_countAllocs) g_allocs++;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

void setUp() {}
void tearDown() {}

static std::string term(const LexQuery& q, int i) { return std::string(q.term[i], q.len[i]); }
static bool hit(const char* hay, const char* needle) {
  const LexQuery q = lexParse(needle);
  return lexCoverage(hay, std::strlen(hay), q) > 0;
}

// ---- words: split on whitespace AND punctuation, lowercased ------------------
static void test_words_split_on_punctuation_and_fold_case() {
  const LexQuery q = lexParse("Who is my PhD-advisor, again?!");
  TEST_ASSERT_EQUAL_INT(2, q.count);           // who / is / my / again are common words
  TEST_ASSERT_EQUAL_STRING("phd", term(q, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("advisor", term(q, 1).c_str());
  // Curly apostrophe and quotes (U+2019, U+201C/D) separate like ASCII ones:
  // "advisor's" is "advisor" + a dropped one-letter "s".
  const LexQuery c = lexParse("\xE2\x80\x9Cmy advisor\xE2\x80\x99s name\xE2\x80\x9D");
  TEST_ASSERT_EQUAL_INT(2, c.count);
  TEST_ASSERT_EQUAL_STRING("advisor", term(c, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("name", term(c, 1).c_str());
  // The degree sign (U+00B0) and an em-dash-class symbol (U+2014) separate too.
  const LexQuery d = lexParse("39.5694\xC2\xB0N\xE2\x80\x94site");
  TEST_ASSERT_EQUAL_INT(3, d.count);           // "39", "5694", "site" ("n" is one letter)
  TEST_ASSERT_EQUAL_STRING("site", term(d, 2).c_str());
  // Case never matters, on either side.
  TEST_ASSERT_TRUE(hit("The BILGE Pump", "bilge PUMP"));
}

// ---- common words do not count -------------------------------------------------
static void test_common_words_do_not_count() {
  const LexQuery stop = lexParse("What is the... who are you?");
  TEST_ASSERT_TRUE(stop.filters());            // there WERE words
  TEST_ASSERT_TRUE(stop.onlyCommonWords());    // ...all of them common
  TEST_ASSERT_EQUAL_INT(0, stop.count);
  TEST_ASSERT_FALSE(hit("what is the answer, who are you", "what is the who are you"));
  // A common word next to a real one: only the real one counts.
  const LexQuery q = lexParse("the boat");
  TEST_ASSERT_EQUAL_INT(1, q.count);
  TEST_ASSERT_EQUAL_STRING("boat", term(q, 0).c_str());
  // Contraction stubs are common words too ("don't" -> "don" + "t").
  TEST_ASSERT_TRUE(lexParse("don't you've we'll").onlyCommonWords());
}

// Malformed UTF-8 (a punctuation prefix cut short, or followed by an ASCII
// byte) never eats the next word's first byte and never becomes a word.
static void test_malformed_utf8_never_eats_a_word() {
  const LexQuery a = lexParse(std::string("\xE2\x80" "boat"));
  TEST_ASSERT_EQUAL_INT(1, a.count);
  TEST_ASSERT_EQUAL_STRING("boat", term(a, 0).c_str());
  const LexQuery b = lexParse(std::string("boat \xE2\x80"));
  TEST_ASSERT_EQUAL_INT(1, b.count);
  TEST_ASSERT_EQUAL_STRING("boat", term(b, 0).c_str());
  const LexQuery c = lexParse(std::string("\xE3\x80" "a" "nchor"));
  TEST_ASSERT_EQUAL_STRING("anchor", term(c, 0).c_str());
}

static void test_no_words_filters_nothing() {
  for (const char* empty : {"", "   ", "\t\n", "?!...", "\xE2\x80\x94"}) {
    const LexQuery q = lexParse(empty);
    TEST_ASSERT_FALSE_MESSAGE(q.filters(), empty);
    TEST_ASSERT_FALSE(q.onlyCommonWords());
  }
}

// ---- stems reach the longer forms ------------------------------------------------
static void test_stems_reach_longer_forms() {
  TEST_ASSERT_TRUE(hit("our research boat is called the Nereid II", "boats"));
  TEST_ASSERT_TRUE(hit("my advisor is Dr. Leila Mansour", "advised"));
  TEST_ASSERT_TRUE(hit("the coordinate grid", "coordinates"));
  TEST_ASSERT_TRUE(hit("we are studying meadows", "studies"));
  TEST_ASSERT_TRUE(hit("the sailing log", "sailing"));
  // Not plurals: -s after s / u / i stays, so the word keeps its full form.
  TEST_ASSERT_EQUAL_STRING("class", term(lexParse("class"), 0).c_str());
  TEST_ASSERT_EQUAL_STRING("status", term(lexParse("status"), 0).c_str());
  TEST_ASSERT_EQUAL_STRING("analysis", term(lexParse("analysis"), 0).c_str());
  // Too short a stem keeps the word: "notes" -> "note", never "not".
  TEST_ASSERT_EQUAL_STRING("note", term(lexParse("notes"), 0).c_str());
  TEST_ASSERT_EQUAL_STRING("need", term(lexParse("need"), 0).c_str());
}

// ---- short words must start a word ----------------------------------------------
static void test_short_words_must_start_a_word() {
  TEST_ASSERT_TRUE(hit("finishing my PhD this year", "phd"));
  TEST_ASSERT_TRUE(hit("two PhDs in the lab", "phd"));
  TEST_ASSERT_FALSE(hit("xphd", "phd"));
  TEST_ASSERT_FALSE(hit("she said so", "ai"));          // the noise this rule removes
  TEST_ASSERT_TRUE(hit("new AI tools", "ai"));
  TEST_ASSERT_TRUE(hit("the Nereid II", "ii"));
  TEST_ASSERT_TRUE(hit("due 15 January 2027", "2027"));
  TEST_ASSERT_FALSE(hit("in 2015", "15"));               // short number: whole start only
  // 4+ bytes match anywhere (a stem must find its longer forms).
  TEST_ASSERT_TRUE(hit("the lifeboat drill", "boat"));
}

// ---- coverage = DIFFERENT words; duplicates collapse ------------------------------
static void test_coverage_counts_distinct_words() {
  const LexQuery q = lexParse("research boat boat vessel name");
  TEST_ASSERT_EQUAL_INT(4, q.count);           // the repeated "boat" counted once
  const char* row = "our research boat is called the Nereid II";
  TEST_ASSERT_EQUAL_INT(2, lexCoverage(row, std::strlen(row), q));
  const char* twice = "boat boat boat";
  TEST_ASSERT_EQUAL_INT(1, lexCoverage(twice, std::strlen(twice), q));
  // "boats" and "boat" stem to the same word.
  TEST_ASSERT_EQUAL_INT(1, lexParse("boat boats").count);
}

// ---- the parsed query is bounded ----------------------------------------------------
static void test_term_count_and_length_are_capped() {
  const LexQuery q = lexParse("alpha bravo charlie delta echo foxtrot golf hotel india juliet");
  TEST_ASSERT_EQUAL_INT(kLexMaxTerms, q.count);   // the first 8 counted words
  TEST_ASSERT_EQUAL_STRING("hotel", term(q, 7).c_str());
  // A word longer than the cap keeps its prefix, which still finds the word.
  const std::string longWord = "pneumonoultramicroscopicsilicovolcanoconiosis";
  const LexQuery l = lexParse(longWord);
  TEST_ASSERT_EQUAL_INT(kLexMaxTermLen, l.len[0]);
  TEST_ASSERT_TRUE(lexCoverage(longWord, l) > 0);
  // A cut never ends inside a UTF-8 code point: 23 ASCII bytes + a 2-byte
  // letter would split the letter at byte 24, so the prefix stops at 23.
  const std::string accented = std::string(23, 'a') + "\xC3\xA9tude";
  TEST_ASSERT_EQUAL_INT(23, lexParse(accented).len[0]);
}

// ---- non-ASCII words stay whole ------------------------------------------------------
static void test_non_ascii_words_match_whole() {
  // Arabic "carbon" (kurbun) inside an Arabic sentence.
  TEST_ASSERT_TRUE(hit("\xD8\xA7\xD9\x84\xD9\x83\xD8\xB1\xD8\xA8\xD9\x88\xD9\x86 \xD9\x81\xD9\x8A",
                       "\xD9\x83\xD8\xB1\xD8\xA8\xD9\x88\xD9\x86"));
  TEST_ASSERT_TRUE(hit("Caf\xC3\xA9 order: cortado", "caf\xC3\xA9"));
  TEST_ASSERT_FALSE(hit("Cafe order", "caf\xC3\xA9"));
}

// ---- rank and first hit -----------------------------------------------------------------
static void test_rank_and_first_hit() {
  const LexQuery q = lexParse("bilge pump");
  LexTally both, one;
  lexTally("bilge pump", 10, q, both);
  lexTally("pump pump pump pump", 19, q, one);
  TEST_ASSERT_EQUAL_INT(2002, lexRank(both));
  TEST_ASSERT_EQUAL_INT(1004, lexRank(one));
  TEST_ASSERT_EQUAL_INT(0, lexRank(LexTally()));
  // Two spans tally as one text (a docs section's title + body).
  LexTally spans;
  lexTally("Bilge", 5, q, spans);
  lexTally("the pump", 8, q, spans);
  TEST_ASSERT_EQUAL_INT(2, spans.coverage());
  const char* hay = "check the pump, then the bilge";
  TEST_ASSERT_EQUAL_INT(10, (int)lexFirstHit(hay, std::strlen(hay), q));
  TEST_ASSERT_TRUE(lexFirstHit("nothing here", 12, q) == std::string::npos);
}

// ---- memory bound: matching allocates NOTHING ------------------------------------------
// The old matcher lowercased a full copy of every row (and of the needle) and
// took a substring per word per row. The DoD bound is asserted here.
static void test_matching_allocates_nothing() {
  std::string text;
  for (int i = 0; i < 100; i++) text += "seagrass carbon meadow sampling notes, Posidonia oceanica. ";
  const std::string needle = "Who is my PhD advisor again? supervisor boats coordinates";
  g_allocs = 0;
  g_countAllocs = true;
  const LexQuery q = lexParse(needle);
  int cov = 0;
  for (int i = 0; i < 50; i++) cov += lexCoverage(text, q);
  LexTally t;
  lexTally(text.data(), text.size(), q, t);
  const size_t first = lexFirstHit(text.data(), text.size(), q);
  g_countAllocs = false;
  TEST_ASSERT_EQUAL_INT(0, (int)g_allocs);
  TEST_ASSERT_EQUAL_INT(0, cov);
  TEST_ASSERT_TRUE(first == std::string::npos);
}

// ...and a text search over a whole store allocates nothing PER ROW: rows that
// do not enter the page are never copied, and ids are compared in place.
static void test_store_scan_allocates_nothing_per_row() {
  InMemoryEpisodicStore st(2000);
  char id[16];
  for (int i = 0; i < 1000; i++) {
    EpisodicMessage m;
    snprintf(id, sizeof(id), "m%08x", i + 1);
    m.id = id; m.sessionId = "web"; m.tsHours = 100; m.role = "user";
    m.text = "a long enough chat row about seagrass meadows and sampling plans #" + std::to_string(i);
    st.addMessage(m);
  }
  for (const char* text : {"advisor supervisor PhD", "advisor", "what is the"}) {
    MsgQuery q;
    q.textContains = text;
    q.limit = 20;
    g_allocs = 0;
    g_countAllocs = true;
    const std::vector<EpisodicMessage> rows = st.query(q);
    g_countAllocs = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)rows.size(), text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)g_allocs, text);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_words_split_on_punctuation_and_fold_case);
  RUN_TEST(test_common_words_do_not_count);
  RUN_TEST(test_no_words_filters_nothing);
  RUN_TEST(test_malformed_utf8_never_eats_a_word);
  RUN_TEST(test_stems_reach_longer_forms);
  RUN_TEST(test_short_words_must_start_a_word);
  RUN_TEST(test_coverage_counts_distinct_words);
  RUN_TEST(test_term_count_and_length_are_capped);
  RUN_TEST(test_non_ascii_words_match_whole);
  RUN_TEST(test_rank_and_first_hit);
  RUN_TEST(test_matching_allocates_nothing);
  RUN_TEST(test_store_scan_allocates_nothing_per_row);
  return UNITY_END();
}
