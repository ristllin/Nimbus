#include "nimbus/orch/path_rules.h"

#include "nimbus/orch/blob_store.h"   // blobHash - the suffix of a shortened segment
#include "nimbus/utf8.h"              // utf8SeqLen - one character at a time

namespace nimbus::orch {

namespace {

// U+00C0..U+00FF folded to ASCII; "-" for the two signs in the range.
const char* const kLatin1[64] = {
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I",  "I", "I",  "I",
    "D", "N", "O", "O", "O", "O", "O",  "-", "O", "U", "U", "U", "U",  "Y", "TH", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i",  "i", "i",  "i",
    "d", "n", "o", "o", "o", "o", "o",  "-", "o", "u", "u", "u", "u",  "y", "th", "y",
};

// A printable ASCII byte the store and the card's FAT both keep in a name.
bool keepAscii(unsigned char c) {
  if (c < 0x21 || c > 0x7E) return false;
  switch (c) {
    case '/': case '\\': case ':': case '*': case '?': case '"': case '<': case '>': case '|':
      return false;
    default:
      return true;
  }
}

// The text for one character starting at s (len bytes of it).
void mapChar(const unsigned char* s, size_t len, std::string& out) {
  if (len == 1) {
    out += keepAscii(s[0]) ? char(s[0]) : '-';
    return;
  }
  if (len == 2 && (s[0] == 0xC3)) {   // U+00C0..U+00FF
    out += kLatin1[(s[1] & 0x3F)];
    return;
  }
  out += '-';
}

char foldAscii(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool sameFolded(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (foldAscii(a[i]) != foldAscii(b[i])) return false;
  return true;
}

bool endsFolded(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && sameFolded(s.substr(s.size() - suffix.size()), suffix);
}

// A segment over its limit: the extension stays, the stem is cut, and '-' plus 8 hex of
// the asked segment's hash ends the stem so two long names stay two names.
std::string shorten(const std::string& s, std::string_view asked, size_t maxLen) {
  const std::string tag = "-" + blobHash(std::string(asked)).substr(0, 8);
  std::string ext;
  const size_t dot = s.rfind('.');
  if (dot != std::string::npos && dot > 0 && s.size() - dot <= kStoreExtMax && s.size() - dot + tag.size() < maxLen)
    ext = s.substr(dot);
  const size_t keep = maxLen - ext.size() - tag.size();
  return s.substr(0, keep) + tag + ext;
}

bool dotSegment(std::string_view s) { return s == "." || s == ".."; }

}  // namespace

std::string validCharsFor(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  const auto* u = reinterpret_cast<const unsigned char*>(text.data());
  size_t i = 0;
  while (i < text.size()) {
    size_t len = utf8SeqLen(u + i, text.size() - i);
    if (len == 0) len = 1;   // a byte that starts no character is one character here
    mapChar(u + i, len, out);
    i += len;
  }
  return out;
}

std::string validSegmentFor(std::string_view segment, size_t maxLen) {
  std::string s = validCharsFor(segment);
  if (!s.empty() && s[0] == '.') s[0] = '_';               // no hidden files
  while (!s.empty() && s.back() == '.') s.pop_back();      // FAT drops trailing dots
  if (s.empty() || maxLen < 8) return std::string();
  if (s.size() > maxLen) s = shorten(s, segment, maxLen);
  return s;
}

StorePath storePathFor(std::string_view asked) {
  StorePath out;
  if (asked.empty()) {
    out.error = PathError::Empty;
    return out;
  }
  if (asked[0] == '/') {
    out.error = PathError::Absolute;
    return out;
  }
  const size_t slash = asked.find('/');
  const std::string_view first = asked.substr(0, slash);
  const std::string_view rest = slash == std::string_view::npos ? std::string_view() : asked.substr(slash + 1);
  const size_t second = rest.find('/');
  // Any '.' or '..' anywhere is named as such, before the count of parts.
  for (std::string_view r = asked; !r.empty();) {
    const size_t k = r.find('/');
    if (dotSegment(r.substr(0, k))) {
      out.error = PathError::DotSegment;
      return out;
    }
    r = k == std::string_view::npos ? std::string_view() : r.substr(k + 1);
  }
  if (slash == std::string_view::npos || second != std::string_view::npos || first.empty() || rest.empty()) {
    out.error = PathError::NotTwoParts;
    return out;
  }
  out.project = validSegmentFor(first, kStoreProjectMax);
  out.name = validSegmentFor(rest, kStoreNameMax);
  if (out.project.empty() || out.name.empty()) {
    out.error = PathError::NothingLeft;
    out.project.clear();
    out.name.clear();
    return out;
  }
  out.changed = out.project != first || out.name != rest;
  return out;
}

bool skillIdValid(std::string_view id) {
  if (id.empty() || id.size() > 23) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

PathRule pathRuleFor(std::string_view project, std::string_view name) {
  PathRule r;
  if (sameFolded(project, "skills")) {
    r.kind = Reserved::Skill;
    r.writable = false;
    r.owner = "skill_save";
    if (endsFolded(name, ".md") && skillIdValid(name.substr(0, name.size() - 3)))
      r.skillId = std::string(name.substr(0, name.size() - 3));
    return r;
  }
  if (sameFolded(project, "export-pre-v2")) {
    r.kind = Reserved::Archive;
    r.writable = false;
    return r;
  }
  if (sameFolded(project, "inbox")) {
    r.kind = Reserved::Inbox;
    r.writable = false;
    return r;
  }
  if (sameFolded(project, "agents")) {
    r.kind = Reserved::Agents;
    r.writable = false;
    r.owner = "agent_start";
  }
  return r;
}

}  // namespace nimbus::orch
