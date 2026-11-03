#pragma once
#include <cstddef>
#include <string>
#include <vector>

// word_match - whole-word phrase tests over lowercase text, shared by the lexical
// classifiers: the standing rules' rule and dated-fact tests (owner_prefs.h) and the
// food ledger's food and intent tests (food_ledger.h). Portable, header-only.
namespace nimbus::orch::words {

inline std::string lowerAscii(const std::string& s) {
  std::string out = s;
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return out;
}

// A letter, a digit or any byte of a UTF-8 sequence, in lowercase text.
inline bool isWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (unsigned char)c >= 0x80;
}

// `phrase` occurs in the lowercase text `s` as whole words: no letter or digit
// touches it. A phrase that ends in ':' ("breakfast:") needs no boundary after it.
inline bool hasPhrase(const std::string& s, const std::string& phrase) {
  if (phrase.empty()) return false;
  const bool open = phrase.back() == ':';
  for (size_t at = s.find(phrase); at != std::string::npos; at = s.find(phrase, at + 1)) {
    const size_t end = at + phrase.size();
    if ((at == 0 || !isWordChar(s[at - 1])) && (open || end >= s.size() || !isWordChar(s[end]))) return true;
  }
  return false;
}

template <size_t N>
bool anyPhrase(const std::string& s, const char* const (&list)[N]) {
  for (const char* p : list)
    if (hasPhrase(s, p)) return true;
  return false;
}

// The subject words of a text: lowercase ASCII tokens of 3+ bytes with the
// question furniture (about, next, need, ask, ...) left out. Shared by the open
// list's raise-once matching (open_questions.h) and the standing gate's topic
// test (owner_prefs.h): "never ask about duolingo" has the one subject word
// "duolingo", so a routine prompt that mentions it is the same topic.
inline std::vector<std::string> subjectWords(const std::string& text) {
  static const char* const kStop[] = {
      "about", "this", "that", "with", "from", "when", "then", "them", "they", "will", "would",
      "should", "could", "have", "has", "had", "been", "need", "needs", "needed", "next", "time",
      "times", "please", "thanks", "thank", "just", "like", "want", "wants", "ask", "asked", "asks",
      "item", "open", "question", "follow", "check", "again", "later", "every", "each", "some",
      "more", "most", "over", "into", "your", "yours", "mine", "after", "before", "while", "there",
  };
  const std::string s = lowerAscii(text);
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && !isWordChar(s[i])) i++;
    size_t b = i;
    while (i < s.size() && isWordChar(s[i])) i++;
    if (i - b < 3) continue;
    std::string w = s.substr(b, i - b);
    bool stop = false;
    for (const char* st : kStop)
      if (w == st) stop = true;
    if (!stop) out.push_back(w);
  }
  return out;
}

}  // namespace nimbus::orch::words
