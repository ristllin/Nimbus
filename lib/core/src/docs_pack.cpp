#include "nimbus/docs_pack.h"

#include <cstring>

#include "nimbus/docs_pack_data.h"   // GENERATED table (tools/gen_docs_pack.py)
#include "nimbus/orch/episodic.h"    // epiTextMatch - host-tested AND matcher

namespace nimbus {
namespace docs {

namespace {

std::string lowered(const char* s) {
  std::string o(s);
  for (char& c : o)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return o;
}

// First whitespace-separated token of the query, lowercased.
std::string firstToken(const std::string& q) {
  size_t a = 0;
  while (a < q.size() && (q[a] == ' ' || q[a] == '\t' || q[a] == '\n')) a++;
  size_t b = a;
  while (b < q.size() && q[b] != ' ' && q[b] != '\t' && q[b] != '\n') b++;
  std::string t = q.substr(a, b - a);
  for (char& c : t)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return t;
}

// Back a byte offset up to the start of a UTF-8 code point.
size_t utf8Floor(const char* s, size_t pos) {
  while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80) pos--;
  return pos;
}

}  // namespace

size_t sectionCount() { return kDocsSectionCount; }
const DocSection& section(size_t i) { return kDocsSections[i]; }
size_t fileCount() { return kDocsFileCount; }
const DocFile& file(size_t i) { return kDocsFiles[i]; }

const DocSection* find(const std::string& id) {
  for (size_t i = 0; i < kDocsSectionCount; i++)
    if (id == kDocsSections[i].id) return &kDocsSections[i];
  return nullptr;
}

const DocFile* findFile(const std::string& slug) {
  for (size_t i = 0; i < kDocsFileCount; i++)
    if (slug == kDocsFiles[i].slug) return &kDocsFiles[i];
  return nullptr;
}

size_t search(const std::string& query, const DocSection** out, size_t max) {
  size_t n = 0;
  if (max == 0) return 0;
  for (size_t i = 0; i < kDocsSectionCount && n < max; i++) {
    const DocSection& s = kDocsSections[i];
    // One transient hay per section (title + body), freed before the next -
    // epiTextMatch takes std::string; bounded (~3.5 KB) and never retained.
    std::string hay(s.title);
    hay += '\n';
    hay += s.body;
    if (orch::epiTextMatch(hay, query)) out[n++] = &s;
  }
  return n;
}

std::string snippet(const DocSection& s, const std::string& query) {
  const std::string body = lowered(s.body);
  const std::string tok = firstToken(query);
  size_t pos = tok.empty() ? std::string::npos : body.find(tok);
  if (pos == std::string::npos) pos = 0;   // term only in the title -> first line
  // The line containing the hit.
  size_t a = body.rfind('\n', pos);
  a = (a == std::string::npos) ? 0 : a + 1;
  size_t b = body.find('\n', pos);
  if (b == std::string::npos) b = body.size();
  // Bound to ~160 bytes around the hit, cut at UTF-8 boundaries.
  const size_t kMax = 160;
  if (b - a > kMax) {
    size_t start = (pos > a + 40) ? utf8Floor(s.body, pos - 40) : a;
    size_t end = utf8Floor(s.body, start + kMax);
    if (end > b) end = b;
    a = start;
    b = end;
  }
  std::string out(s.body + a, b - a);
  // Trim edge whitespace.
  while (!out.empty() && (out.front() == ' ' || out.front() == '\t')) out.erase(0, 1);
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
  return out;
}

std::string nearMisses(const std::string& id, size_t max) {
  const size_t hash = id.find('#');
  const std::string fileSlug = (hash == std::string::npos) ? id : id.substr(0, hash);
  const std::string head = (hash == std::string::npos) ? id : id.substr(hash + 1);
  std::string out;
  size_t n = 0;
  auto push = [&](const char* sid) {
    if (n >= max) return;
    if (!out.empty()) out += ", ";
    out += sid;
    n++;
  };
  // Same file first (the most likely mistake is a wrong heading slug)...
  for (size_t i = 0; i < kDocsSectionCount && n < max; i++) {
    const char* sid = kDocsSections[i].id;
    if (std::strncmp(sid, fileSlug.c_str(), fileSlug.size()) == 0 &&
        sid[fileSlug.size()] == '#')
      push(sid);
  }
  // ...then any id containing the heading part.
  if (!head.empty()) {
    for (size_t i = 0; i < kDocsSectionCount && n < max; i++) {
      const char* sid = kDocsSections[i].id;
      if (std::strncmp(sid, fileSlug.c_str(), fileSlug.size()) == 0 &&
          sid[fileSlug.size()] == '#')
        continue;   // already offered above
      if (std::strstr(sid, head.c_str())) push(sid);
    }
  }
  return out;
}

}  // namespace docs
}  // namespace nimbus
