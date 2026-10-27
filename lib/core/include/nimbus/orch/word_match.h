#pragma once
#include <cstddef>
#include <string>

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

}  // namespace nimbus::orch::words
