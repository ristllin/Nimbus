#include "nimbus/orch/lexical.h"

#include <cstring>

namespace nimbus {
namespace orch {

namespace {

// English function words and question filler. Content words are deliberately
// absent: "remember", "name" or "boat" are things a person really searches for.
// The contraction stubs ("don", "ll", "re", "ve") are what "don't", "we'll",
// "you're" and "I've" split into.
const char* const kStopWords[] = {
    "about", "above", "after", "again", "against", "all", "also", "am", "an", "and",
    "any", "are", "aren", "as", "at", "be", "because", "been", "before", "being",
    "below", "between", "both", "but", "by", "can", "could", "couldn", "did", "didn",
    "do", "does", "doesn", "doing", "don", "down", "during", "each", "either", "else",
    "ever", "every", "few", "for", "from", "further", "get", "got", "had", "hadn",
    "has", "hasn", "have", "haven", "having", "he", "her", "here", "hers", "herself",
    "him", "himself", "his", "how", "if", "in", "into", "is", "isn", "it",
    "its", "itself", "just", "let", "ll", "me", "might", "mine", "more", "most",
    "much", "must", "my", "myself", "no", "nor", "not", "now", "of", "off",
    "on", "once", "only", "or", "other", "our", "ours", "ourselves", "out", "over",
    "own", "please", "re", "same", "shall", "she", "should", "shouldn", "so", "some",
    "such", "than", "that", "the", "their", "theirs", "them", "themselves", "then", "there",
    "these", "they", "this", "those", "through", "to", "too", "under", "until", "up",
    "us", "ve", "very", "was", "wasn", "we", "were", "weren", "what", "when",
    "where", "which", "while", "who", "whom", "whose", "why", "will", "with", "won",
    "would", "wouldn", "yes", "yet", "you", "your", "yours", "yourself", "yourselves",
};

inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

inline bool asciiAlnum(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool utf8Cont(unsigned char c) { return (c & 0xC0) == 0x80; }

// Byte length of a punctuation code point starting at s[i], 0 when s[i] starts
// a word character. ASCII non-alphanumerics, U+00A0-U+00BF (no-break space,
// degree, guillemets ...), U+2000-U+203F (curly quotes, dashes, ellipsis ...)
// and U+3000-U+3003 (ideographic space and punctuation) separate words. A
// punctuation prefix cut short or followed by a non-continuation byte is
// dropped on its own (2 bytes), so it never swallows the next word's first
// byte nor becomes a word of its own.
size_t separatorLen(const unsigned char* s, size_t i, size_t n) {
  const unsigned char c = s[i];
  if (c < 0x80) return asciiAlnum(c) ? 0 : 1;
  if (c == 0xC2 && i + 1 < n && s[i + 1] >= 0xA0 && s[i + 1] <= 0xBF) return 2;
  if ((c != 0xE2 && c != 0xE3) || i + 1 >= n || s[i + 1] != 0x80) return 0;
  if (i + 2 >= n || !utf8Cont(s[i + 2])) return 2;   // malformed prefix
  if (c == 0xE2 || s[i + 2] <= 0x83) return 3;
  return 0;
}

bool endsWith(const char* w, size_t n, const char* suf, size_t sn) {
  return n >= sn && std::memcmp(w + n - sn, suf, sn) == 0;
}

// Length of `w` after trimming ONE plural / verb suffix, keeping at least
// minStem bytes. "-s" is not trimmed after s/u/i ("class", "status",
// "analysis" are not plurals).
size_t stemLen(const char* w, size_t n) {
  struct Rule { const char* suf; uint8_t sn; uint8_t minStem; };
  static const Rule kRules[] = {{"ies", 3, 4}, {"ing", 3, 4}, {"ed", 2, 4}, {"es", 2, 4}, {"s", 1, 3}};
  for (const Rule& r : kRules) {
    if (!endsWith(w, n, r.suf, r.sn) || n - r.sn < r.minStem) continue;
    if (r.sn == 1) {
      const char before = w[n - 2];
      if (before == 's' || before == 'u' || before == 'i') return n;
    }
    return n - r.sn;
  }
  return n;
}

// Bytes in the UTF-8 sequence a lead byte starts (1 for ASCII or a stray byte).
size_t utf8SeqLen(unsigned char lead) {
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;
}

// Drop a trailing UTF-8 sequence that the term cap cut short, so a kept
// prefix never ends mid code point.
size_t utf8TrimPartial(const char* w, size_t n) {
  size_t j = n;
  while (j > 0 && utf8Cont((unsigned char)w[j - 1])) j--;   // continuation bytes
  if (j == 0) return n;                                     // malformed: keep as-is
  const size_t lead = j - 1;
  return (n - lead < utf8SeqLen((unsigned char)w[lead])) ? lead : n;
}

bool isAsciiWord(const char* w, size_t n) {
  for (size_t i = 0; i < n; i++)
    if ((unsigned char)w[i] >= 0x80) return false;
  return true;
}

bool isCommonWord(const char* w, size_t n) {
  for (const char* sw : kStopWords)
    if (std::strlen(sw) == n && std::memcmp(sw, w, n) == 0) return true;
  return false;
}

// Add one raw word (already lowercased). `cut`: the word ran past the term cap
// and only its prefix was kept - a prefix has no real suffix to stem, and a
// common word is never that long, so both checks apply to whole words only.
void addWord(LexQuery& q, const char* w, size_t n, bool cut) {
  q.anyWord = true;
  if (q.count >= kLexMaxTerms) return;
  if (cut) n = utf8TrimPartial(w, n);
  if (n < 2 || isCommonWord(w, n)) return;
  if (!cut) n = stemLen(w, n);
  for (int i = 0; i < q.count; i++)
    if (q.len[i] == n && std::memcmp(q.term[i], w, n) == 0) return;   // duplicate
  std::memcpy(q.term[q.count], w, n);
  q.term[q.count][n] = '\0';
  q.len[q.count] = (uint8_t)n;
  q.wordStart[q.count] = n < 4 && isAsciiWord(w, n);
  q.count++;
}

// Next occurrence of counted word `i` of q in hay at or after `from`; npos if none.
size_t findTerm(const char* hay, size_t n, size_t from, const LexQuery& q, int i) {
  const char* t = q.term[i];
  const size_t tl = q.len[i];
  if (tl == 0 || n < tl) return std::string::npos;
  for (size_t at = from; at + tl <= n; at++) {
    if (lowerAscii(hay[at]) != t[0]) continue;
    if (q.wordStart[i] && at > 0 && asciiAlnum((unsigned char)hay[at - 1])) continue;
    size_t k = 1;
    while (k < tl && lowerAscii(hay[at + k]) == t[k]) k++;
    if (k == tl) return at;
  }
  return std::string::npos;
}

}  // namespace

LexQuery lexParse(const char* s, size_t n) {
  LexQuery q;
  const unsigned char* u = (const unsigned char*)s;
  char word[kLexMaxTermLen + 1];
  size_t wl = 0;
  bool cut = false;
  size_t i = 0;
  while (i < n) {
    const size_t sep = separatorLen(u, i, n);
    if (sep == 0) {                                   // a word byte
      if (wl < (size_t)kLexMaxTermLen) word[wl++] = lowerAscii(s[i]);
      else cut = true;
      i++;
      continue;
    }
    if (wl > 0) addWord(q, word, wl, cut);
    wl = 0;
    cut = false;
    i += sep;
  }
  if (wl > 0) addWord(q, word, wl, cut);
  return q;
}

LexShape lexShape(const std::string& s) {
  const LexQuery q = lexParse(s);
  if (!q.filters()) return LexShape::NoWords;
  if (q.onlyCommonWords()) return LexShape::CommonOnly;
  return q.ranks() ? LexShape::Ranked : LexShape::OneWord;
}

int LexTally::coverage() const {
  int c = 0;
  for (uint8_t m = mask; m; m &= (uint8_t)(m - 1)) c++;
  return c;
}

void lexTally(const char* hay, size_t n, const LexQuery& q, LexTally& t) {
  for (int i = 0; i < q.count; i++) {
    size_t at = findTerm(hay, n, 0, q, i);
    if (at == std::string::npos) continue;
    t.mask |= (uint8_t)(1u << i);
    for (; at != std::string::npos; at = findTerm(hay, n, at + q.len[i], q, i)) t.hits++;
  }
}

int lexCoverage(const char* hay, size_t n, const LexQuery& q) {
  int c = 0;
  for (int i = 0; i < q.count; i++)
    if (findTerm(hay, n, 0, q, i) != std::string::npos) c++;
  return c;
}

int lexRank(const LexTally& t) {
  const int c = t.coverage();
  if (c == 0) return 0;
  return c * 1000 + (int)(t.hits > 999 ? 999 : t.hits);
}

size_t lexFirstHit(const char* hay, size_t n, const LexQuery& q) {
  size_t best = std::string::npos;
  for (int i = 0; i < q.count; i++) {
    const size_t at = findTerm(hay, n, 0, q, i);
    if (at < best) best = at;
  }
  return best;
}

}  // namespace orch
}  // namespace nimbus
