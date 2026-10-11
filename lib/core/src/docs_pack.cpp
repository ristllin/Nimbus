#include "nimbus/docs_pack.h"

#include <cstring>

#include "nimbus/docs_pack_data.h"   // GENERATED table (tools/gen_docs_pack.py)
#include "nimbus/orch/lexical.h"     // the shared word-match rule (CUM-515)

namespace nimbus {
namespace docs {

namespace {

// Rank of one section for a parsed query: title + body scanned in place
// (flash rodata, never joined into a heap copy). 0 = no counted word occurs.
int sectionRank(const DocSection& s, const orch::LexQuery& q) {
  orch::LexTally t;
  orch::lexTally(s.title, std::strlen(s.title), q, t);
  orch::lexTally(s.body, std::strlen(s.body), q, t);
  return orch::lexRank(t);
}

// Insert section `s` with rank `r` into the best-first top list out[0..n)
// (capacity max). Ties keep table order: a later section never displaces an
// equal one.
void insertRanked(const DocSection** out, int* ranks, size_t& n, size_t max,
                  const DocSection* s, int r) {
  if (n == max && r <= ranks[n - 1]) return;
  if (n < max) n++;          // room to grow; else the worst entry is overwritten
  size_t at = n - 1;
  while (at > 0 && ranks[at - 1] < r) {
    out[at] = out[at - 1];
    ranks[at] = ranks[at - 1];
    at--;
  }
  out[at] = s;
  ranks[at] = r;
}

// Most hits one search ranks (the tools ask for 8). Bounds the rank scratch.
constexpr size_t kSearchMax = 16;

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
  if (max > kSearchMax) max = kSearchMax;
  if (max == 0) return 0;
  const orch::LexQuery q = orch::lexParse(query);
  int ranks[kSearchMax];
  size_t n = 0;
  for (size_t i = 0; i < kDocsSectionCount; i++) {
    const int r = sectionRank(kDocsSections[i], q);
    if (r > 0) insertRanked(out, ranks, n, max, &kDocsSections[i], r);
  }
  return n;
}

std::string snippet(const DocSection& s, const std::string& query) {
  const orch::LexQuery q = orch::lexParse(query);
  const size_t bodyLen = std::strlen(s.body);
  size_t pos = orch::lexFirstHit(s.body, bodyLen, q);
  if (pos == std::string::npos) pos = 0;   // words only in the title -> first line
  // The line containing the hit (scanned in the rodata body, no lowered copy).
  size_t a = pos;
  while (a > 0 && s.body[a - 1] != '\n') a--;
  size_t b = pos;
  while (b < bodyLen && s.body[b] != '\n') b++;
  // Bound to ~160 bytes around the hit, cut at UTF-8 boundaries. The end is
  // clamped to the line BEFORE it is floored, so the floor never reads past it.
  const size_t kMax = 160;
  if (b - a > kMax) {
    size_t start = (pos > a + 40) ? utf8Floor(s.body, pos - 40) : a;
    size_t end = start + kMax;
    end = (end >= b) ? b : utf8Floor(s.body, end);
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
