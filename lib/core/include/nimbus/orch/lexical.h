#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// lexical - the ONE word-match rule behind every text search on the device:
// episodic history (memory.episodic, GET /api/mem/episodic), the keyword
// fallback of memory.search / memory.archive, files.search and docs.search.
//
// CUM-515: the old rule required EVERY query word to appear in a row, so a
// natural question failed the moment the model added a synonym - "advisor
// supervisor PhD" found nothing because the owner had only ever said "my
// advisor is Dr. Leila Mansour". The rule now:
//
//   * Words: the query is split on whitespace and punctuation (ASCII
//     punctuation, plus the Latin-1 and General Punctuation symbols such as
//     curly quotes, dashes and the degree sign); ASCII letters are lowercased.
//     Other non-ASCII bytes are word characters, so accented, Arabic or CJK
//     words stay whole.
//   * Common words do not count: a fixed English stop list ("the", "is", "my",
//     "what", "who", "again", ...) and every one-character word are dropped.
//   * Stems: one plural / verb suffix is trimmed when enough of the word is
//     left ("boats" -> "boat", "advised" -> "advis", "coordinates" ->
//     "coordinat"), and because matching is by substring a stem also finds
//     every longer form ("advis" matches "advisor").
//   * Where a word may match: a word of 4+ bytes matches anywhere, case-
//     insensitively; a shorter ASCII word must start a word ("phd" matches
//     "PhD" and "PhDs" but "ai" does not match "said").
//   * A text matches when ANY counted word matches. Its coverage is the number
//     of DIFFERENT counted words it contains; searches rank by coverage, so a
//     row holding all the words comes first, and one holding a single word is
//     still found.
//   * No words at all (empty, whitespace, punctuation): no text filter.
//     Words, but every one a common word: nothing matches (callers say why).
//
// Memory: allocation-free. A parsed query is a fixed-size value
// (kLexMaxTerms x kLexMaxTermLen, ~220 bytes, lives on the caller's stack) and
// matching scans the text in place - nothing is lowercased into a copy.
// Arduino-free -> host-tested (test/test_orch_lexical).
namespace nimbus {
namespace orch {

inline constexpr int kLexMaxTerms   = 8;    // counted words kept per query (the first 8)
inline constexpr int kLexMaxTermLen = 24;   // bytes kept per word (a longer word keeps its prefix)

struct LexQuery {
  char    term[kLexMaxTerms][kLexMaxTermLen + 1] = {};
  uint8_t len[kLexMaxTerms] = {};
  bool    wordStart[kLexMaxTerms] = {};   // short ASCII word: must start a word in the text
  uint8_t count = 0;                      // counted words (after stop words, stems, dedupe)
  bool    anyWord = false;                // the query held at least one word, counted or not

  // False for an empty / whitespace / punctuation-only query: it filters nothing.
  bool filters() const { return anyWord; }
  // Every word was a common word: no text can match.
  bool onlyCommonWords() const { return anyWord && count == 0; }
  // Two or more counted words: matches can differ in coverage, so results rank.
  // With one word every match scores the same and newest-first is best-first.
  bool ranks() const { return count >= 2; }
};

LexQuery lexParse(const char* s, size_t n);
inline LexQuery lexParse(const std::string& s) { return lexParse(s.data(), s.size()); }

// The shape of a query's words, for callers that only branch on it: the parsed
// query lives and dies inside lexShape, so the caller's stack never holds it
// alongside the store's own copy.
enum class LexShape : uint8_t { NoWords, CommonOnly, OneWord, Ranked };
LexShape lexShape(const std::string& s);

// Running result of matching one query against one or more spans of text (a
// docs section is title + body, scanned in place without joining them).
struct LexTally {
  uint8_t  mask = 0;   // bit i set: counted word i occurs
  uint32_t hits = 0;   // total occurrences
  int coverage() const;
};
static_assert(kLexMaxTerms <= 8, "LexTally::mask holds one bit per counted word");
void lexTally(const char* hay, size_t n, const LexQuery& q, LexTally& t);

// Distinct counted words found in the text (0 = no match). Early-exits per word.
int lexCoverage(const char* hay, size_t n, const LexQuery& q);
inline int lexCoverage(const std::string& hay, const LexQuery& q) {
  return lexCoverage(hay.data(), hay.size(), q);
}

// Document rank: coverage first, total occurrences second
// (coverage * 1000 + min(hits, 999)). 0 = no match.
int lexRank(const LexTally& t);

// Byte offset of the earliest occurrence of any counted word; npos when none.
// Anchors snippets on a word that actually matched.
size_t lexFirstHit(const char* hay, size_t n, const LexQuery& q);

}  // namespace orch
}  // namespace nimbus
