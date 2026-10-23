#include "nimbus/orch/no_dash.h"

#include "nimbus/orch/owner_prefs.h"   // kPrefFilterNoDash

namespace nimbus {
namespace orch {

namespace {

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// Does `form` (lowercase ASCII) start at s[i], ignoring ASCII case?
bool matchesAt(const std::string& s, size_t i, const char* form) {
  for (size_t k = 0; form[k]; k++)
    if (i + k >= s.size() || lowerAscii(s[i + k]) != form[k]) return false;
  return true;
}

// Byte length of an em dash form starting at s[i], or 0.
size_t dashLenAt(const std::string& s, size_t i) {
  const unsigned char c = (unsigned char)s[i];
  if (c == 0xE2 && i + 2 < s.size()) {
    const unsigned char b1 = (unsigned char)s[i + 1], b2 = (unsigned char)s[i + 2];
    if (b1 == 0x80 && b2 == 0x94) return 3;                      // U+2014
    if (b1 == 0xB8 && (b2 == 0xBA || b2 == 0xBB)) return 3;      // U+2E3A, U+2E3B
  }
  if (c == '&') {
    static const char* const kEntities[] = {"&mdash;", "&#8212;", "&#x2014;"};   // em-dash-ok: the parser
    for (const char* e : kEntities)
      if (matchesAt(s, i, e)) return std::char_traits<char>::length(e);
  }
  if (c == '\\' && matchesAt(s, i, "\\u2014")) return 6;   // em-dash-ok: the parser
  return 0;
}

// A ``` or ~~~ fence line starts at s[i] (i is a line start; up to 3 spaces of indent).
bool fenceAt(const std::string& s, size_t i) {
  size_t k = i;
  while (k < s.size() && k - i < 3 && s[k] == ' ') k++;
  return matchesAt(s, k, "```") || matchesAt(s, k, "~~~");
}

bool atLineStart(const std::string& out) {
  for (size_t k = out.size(); k > 0; k--) {
    if (out[k - 1] == '\n') return true;
    if (out[k - 1] != ' ' && out[k - 1] != '\t') return false;
  }
  return true;
}

void trimTrailingSpaces(std::string& out) {
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
}

// Emit the replacement for a run of dashes that ended at s[next].
void emitReplacement(const std::string& s, size_t next, std::string& out, size_t& skipTo) {
  const bool lead = atLineStart(out);
  const bool spaceBefore = !out.empty() && (out.back() == ' ' || out.back() == '\t');
  const bool atEnd = next >= s.size() || s[next] == '\n';
  const bool spaceAfter = next < s.size() && (s[next] == ' ' || s[next] == '\t');
  skipTo = next;
  if (lead) {
    out += spaceAfter ? "-" : "- ";
    return;
  }
  trimTrailingSpaces(out);
  if (atEnd) return;                        // a trailing dash just goes
  if (spaceBefore && spaceAfter) {
    out += ',';                             // "a - b" read as a pause: "a, b"
    return;
  }
  out += " - ";                             // joined or one-sided: "a - b"
  while (skipTo < s.size() && (s[skipTo] == ' ' || s[skipTo] == '\t')) skipTo++;
}

}  // namespace

std::string removeEmDashes(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool inFence = false;
  size_t i = 0;
  while (i < s.size()) {
    const bool lineStart = i == 0 || s[i - 1] == '\n';
    if (lineStart && fenceAt(s, i)) inFence = !inFence;
    size_t len = inFence ? 0 : dashLenAt(s, i);
    if (len == 0) {
      out += s[i++];
      continue;
    }
    size_t next = i + len;
    while (next < s.size() && (len = dashLenAt(s, next)) > 0) next += len;   // a run is one dash
    size_t skipTo = next;
    emitReplacement(s, next, out, skipTo);
    i = skipTo;
  }
  return out;
}

bool hasEmDash(const std::string& s) {
  bool inFence = false;
  for (size_t i = 0; i < s.size(); i++) {
    if ((i == 0 || s[i - 1] == '\n') && fenceAt(s, i)) inFence = !inFence;
    if (!inFence && dashLenAt(s, i)) return true;
  }
  return false;
}

std::string applyOutputFilters(const std::string& text, uint32_t mask) {
  if (!(mask & kPrefFilterNoDash)) return text;
  return removeEmDashes(text);
}

}  // namespace orch
}  // namespace nimbus
