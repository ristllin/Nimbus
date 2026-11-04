#include "nimbus/orch/file_store.h"
#include "nimbus/orch/word_match.h"   // words::lowerAscii: the one ASCII fold

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace nimbus::orch {

// ---- kind vocabulary --------------------------------------------------------

const char* fileKindName(FileKind k) {
  switch (k) {
    case FileKind::Doc:   return "doc";
    case FileKind::Image: return "image";
    case FileKind::Audio: return "audio";
    default:              return "data";
  }
}

FileKind fileKindForName(const std::string& name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return FileKind::Data;
  std::string ext = name.substr(dot + 1);
  for (auto& c : ext) c = char(::tolower((unsigned char)c));
  static const char* kDoc[]   = {"md", "txt", "pdf", "html", "csv", "json", "log"};
  static const char* kImage[] = {"png", "jpg", "jpeg", "gif", "webp", "bmp"};
  static const char* kAudio[] = {"ogg", "mp3", "wav", "m4a", "opus", "flac"};
  for (auto* e : kDoc)   if (ext == e) return FileKind::Doc;
  for (auto* e : kImage) if (ext == e) return FileKind::Image;
  for (auto* e : kAudio) if (ext == e) return FileKind::Audio;
  return FileKind::Data;
}

bool fileReadableAsText(const std::string& name, const std::string& bytes) {
  const FileKind kind = fileKindForName(name);
  if (kind == FileKind::Image || kind == FileKind::Audio) return false;
  const size_t n = bytes.size() < kTextSniffBytes ? bytes.size() : kTextSniffBytes;
  size_t controls = 0;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)bytes[i];
    if (c == 0) return false;
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') controls++;
  }
  return controls * 10 <= n;
}

// ---- path safety ------------------------------------------------------------

bool FileStore::validSegment(const std::string& s, size_t maxLen) {
  if (s.empty() || s.size() > maxLen) return false;
  if (s == "." || s == "..") return false;
  if (s[0] == '.') return false;                       // no hidden files
  bool nonSpace = false;
  for (unsigned char c : s) {
    if (c < 0x21 || c > 0x7E) return false;            // printable ASCII, no spaces/ctrl
    if (c == '/' || c == '\\' || c == ':') return false;  // separators / FAT quirks
    nonSpace = true;
  }
  return nonSpace;
}

std::string FileStore::relPath(const std::string& project, const std::string& name) const {
  if (!validSegment(project, lim_.maxProjectLen) || !validSegment(name, lim_.maxNameLen))
    return std::string();
  return project + "/" + name;
}

// ---- index ------------------------------------------------------------------

// FAT opens the card's file names without regard to case, so the index must
// resolve identity the same way or every ownership check can be bypassed with a
// letter-case variant (CUM-836: a member writing "MEALS.CSV" onto the owner's
// "meals.csv" was a NEW file to the index, and the card then wrote the owner's
// file). validSegment() admits printable ASCII only, so folding ASCII is exact.
// The SAME fold as the food-ledger gate (words::lowerAscii) is used here on
// purpose: the index-identity rule and the reserved-ledger-name rule are two
// gates over one case-blind FAT, and they must not drift (review finding 4).
static bool fatSameSegment(const std::string& a, const std::string& b) {
  return words::lowerAscii(a) == words::lowerAscii(b);
}

// A row the device owner controls: the owner field is empty (the v4.1 data
// boundary) or explicitly the owner namespace. load()'s case-variant dedupe
// uses this to pick which row of a pre-fix duplicate pair survives.
static bool ownerControlled(const FileEntry& e) {
  return e.owner.empty() || e.owner == kOwnerNs;
}

int FileStore::indexOf(const std::string& project, const std::string& name) const {
  for (size_t i = 0; i < entries_.size(); ++i)
    if (fatSameSegment(entries_[i].project, project) &&
        fatSameSegment(entries_[i].name, name)) return int(i);
  return -1;
}

uint64_t FileStore::totalBytes() const {
  uint64_t t = 0;
  for (const auto& e : entries_) t += e.bytes;
  return t;
}

const char FileStore::kErrTooManyFiles[] = "too many files (remove some first)";
const char FileStore::kErrStoreFull[] = "store full (artifacts are never auto-deleted - remove some first)";

bool FileStore::fullRefusal(const std::string& err) const {
  // Lane FSCAP: with no entry cap (the default) a count refusal cannot occur and
  // is not reported as fullness; a zero entry limit (a store that takes nothing)
  // is not fullness either.
  return (err == kErrTooManyFiles && lim_.maxEntries > 0 && lim_.maxEntries != kNoEntryCap) ||
         (err == kErrStoreFull && lim_.maxTotalBytes > 0);
}

bool FileStore::wouldExceed(const std::string& project, const std::string& name,
                            uint64_t addBytes, std::string& err) const {
  if (!validSegment(project, lim_.maxProjectLen)) { err = "bad project name"; return true; }
  if (!validSegment(name, lim_.maxNameLen))       { err = "bad file name"; return true; }
  if (addBytes > lim_.maxFileBytes) { err = "file too large"; return true; }
  const int existing = indexOf(project, name);
  const uint64_t replaced = existing >= 0 ? entries_[size_t(existing)].bytes : 0;
  if (totalBytes() - replaced + addBytes > lim_.maxTotalBytes) {
    err = kErrStoreFull;
    return true;
  }
  if (existing < 0 && entries_.size() >= lim_.maxEntries) {
    err = kErrTooManyFiles;
    return true;
  }
  err.clear();
  return false;
}

bool FileStore::add(const FileEntry& e, std::string& err) {
  if (wouldExceed(e.project, e.name, e.bytes, err)) return false;
  const int i = indexOf(e.project, e.name);
  if (i >= 0) entries_[size_t(i)] = e;   // replace (content changed -> caller passes
  else entries_.push_back(e);            // a fresh entry; provider cache resets with it)
  return true;
}

bool FileStore::remove(const std::string& project, const std::string& name) {
  const int i = indexOf(project, name);
  if (i < 0) return false;
  entries_.erase(entries_.begin() + i);
  return true;
}

bool FileStore::setProviderId(const std::string& project, const std::string& name,
                              const std::string& tag, const std::string& id) {
  const int i = indexOf(project, name);
  if (i < 0) return false;
  entries_[size_t(i)].providerTag = tag;
  entries_[size_t(i)].providerFileId = id;
  return true;
}

const FileEntry* FileStore::find(const std::string& project, const std::string& name) const {
  const int i = indexOf(project, name);
  return i < 0 ? nullptr : &entries_[size_t(i)];
}

std::vector<const FileEntry*> FileStore::list(const std::string& project) const {
  std::vector<const FileEntry*> out;
  for (const auto& e : entries_)
    if (project.empty() || e.project == project) out.push_back(&e);
  return out;
}

std::vector<const FileEntry*> FileStore::listPage(const std::string& project, size_t offset,
                                                  size_t limit) const {
  std::vector<const FileEntry*> out;
  size_t seen = 0;
  for (const auto& e : entries_) {
    if (!project.empty() && e.project != project) continue;
    if (seen++ < offset) continue;
    if (limit && out.size() >= limit) break;
    out.push_back(&e);
  }
  return out;
}

// ---- regenerable copies (lane FSCAP eviction) --------------------------------

bool FileStore::isRegenerableCopy(const FileEntry& e) const {
  if (isAttachmentProject(e.project)) return true;
  if (!e.hash) return false;
  // A newer entry holding the same content makes this one the older copy.
  for (size_t j = 0; j < entries_.size(); ++j) {
    const FileEntry& o = entries_[j];
    if (o.hash != e.hash || !o.hash) continue;
    if (o.createdAt > e.createdAt) return true;
    if (o.createdAt == e.createdAt && &o > &e) return true;
  }
  return false;
}

std::vector<const FileEntry*> FileStore::planEviction(uint64_t needBytes) const {
  // O(n log n), not O(n^2): isRegenerableCopy's same-hash test is a linear scan, so
  // calling it per entry would be quadratic at 10,000 files. One sort by
  // (hash, createdAt, index) marks the newest entry of each content group; every
  // other entry of a non-zero-hash group is an older copy.
  std::vector<const FileEntry*> byHash;
  byHash.reserve(entries_.size());
  for (const auto& e : entries_) byHash.push_back(&e);
  std::sort(byHash.begin(), byHash.end(),
            [](const FileEntry* a, const FileEntry* b) {
              if (a->hash != b->hash) return a->hash < b->hash;
              if (a->createdAt != b->createdAt) return a->createdAt < b->createdAt;
              return a < b;
            });
  std::vector<char> newestOfHash(entries_.size(), 0);
  for (size_t i = 0; i < byHash.size();) {
    size_t j = i;
    while (j < byHash.size() && byHash[j]->hash == byHash[i]->hash) ++j;
    if (byHash[i]->hash != 0 && j > i)
      newestOfHash[size_t(byHash[j - 1] - entries_.data())] = 1;   // the group's last = newest
    i = j;
  }
  std::vector<const FileEntry*> cands;
  cands.reserve(entries_.size() / 4 + 8);
  for (size_t i = 0; i < entries_.size(); ++i) {
    const FileEntry& e = entries_[i];
    if (isAttachmentProject(e.project)) cands.push_back(&e);
    else if (e.hash && !newestOfHash[i]) cands.push_back(&e);
  }
  // Oldest first: createdAt, then index order (stable).
  std::stable_sort(cands.begin(), cands.end(),
                   [](const FileEntry* a, const FileEntry* b) {
                     if (a->createdAt != b->createdAt) return a->createdAt < b->createdAt;
                     return a < b;
                   });
  std::vector<const FileEntry*> plan;
  uint64_t freed = 0;
  for (const FileEntry* e : cands) {
    if (freed >= needBytes) break;
    plan.push_back(e);
    freed += e->bytes;
  }
  return plan;
}

std::vector<std::string> FileStore::projects() const {
  std::vector<std::string> out;
  for (const auto& e : entries_)
    if (std::find(out.begin(), out.end(), e.project) == out.end()) out.push_back(e.project);
  std::sort(out.begin(), out.end());
  return out;
}

// ---- persistence ------------------------------------------------------------
// v1 line format (tab-separated; segments can't contain tabs/newlines):
//   FILESv1
//   <project>\t<name>\t<bytes>\t<createdAt>\t<kind>\t<hash-hex>\t<provTag>\t<provId>[\t<owner>]
// The 9th field (owner namespace, v3.7.0) is OPTIONAL on read: eight fields is
// a legacy line whose owner is the device owner. Appending rather than bumping
// the header matters - files_subsystem answers a failed load by rebuilding from
// a filesystem scan, which would erase createdAt, hashes and provider ids.

std::string FileStore::dump() const {
  std::string out = "FILESv1\n";
  char buf[64];
  for (const auto& e : entries_) {
    out += e.project; out += '\t';
    out += e.name;    out += '\t';
    std::snprintf(buf, sizeof buf, "%lu\t%lu\t%u\t%016llx\t",
                  (unsigned long)e.bytes, (unsigned long)e.createdAt,
                  unsigned(e.kind), (unsigned long long)e.hash);
    out += buf;
    out += e.providerTag; out += '\t';
    out += e.providerFileId;
    if (!e.owner.empty() || e.shared) {
      out += '\t'; out += e.owner;
      if (e.shared) out += "|shared";   // rides the owner field, no new column
    }
    out += '\n';
  }
  return out;
}

bool FileStore::load(const std::string& blob) {
  entries_.clear();
  size_t pos = 0;
  auto nextLine = [&](std::string& line) -> bool {
    if (pos >= blob.size()) return false;
    size_t nl = blob.find('\n', pos);
    if (nl == std::string::npos) nl = blob.size();
    line = blob.substr(pos, nl - pos);
    pos = nl + 1;
    return true;
  };
  std::string line;
  if (!nextLine(line) || line != "FILESv1") return false;
  // Lane FSCAP: the duplicate lookup is a map, not a per-row linear scan - at
  // 10,000 rows indexOf() per row is quadratic and the boot-time load is the one
  // call that must stay bounded as the index scales with the card.
  std::unordered_map<std::string, size_t> byFolded;
  byFolded.reserve(1024);
  while (nextLine(line)) {
    if (line.empty()) continue;
    // split on tabs into 8 fields, plus an OPTIONAL 9th (v3.7.0 owner);
    // skip malformed lines (tolerant load).
    std::string f[9];
    size_t start = 0; int n = 0;
    for (; n < 9; ++n) {
      size_t tab = line.find('\t', start);
      if (tab == std::string::npos) { f[n] = line.substr(start); ++n; break; }
      f[n] = line.substr(start, tab - start);
      start = tab + 1;
    }
    if (n != 8 && n != 9) continue;   // 9 = v3.7.0 owner field (optional)
    FileEntry e;
    e.project = f[0]; e.name = f[1];
    e.bytes = uint32_t(strtoul(f[2].c_str(), nullptr, 10));
    e.createdAt = uint32_t(strtoul(f[3].c_str(), nullptr, 10));
    unsigned k = unsigned(strtoul(f[4].c_str(), nullptr, 10));
    e.kind = k <= 3 ? FileKind(k) : FileKind::Data;
    e.hash = strtoull(f[5].c_str(), nullptr, 16);
    e.providerTag = f[6]; e.providerFileId = f[7];
    if (n == 9) {
      e.owner = f[8];
      const size_t bar = e.owner.find('|');
      if (bar != std::string::npos) {
        e.shared = (e.owner.compare(bar + 1, std::string::npos, "shared") == 0);
        e.owner.resize(bar);
      }
    }   // else legacy -> adopted as the owner's below
    // Re-validate on load: a corrupted/hand-edited index can't smuggle a bad path.
    if (!validSegment(e.project, lim_.maxProjectLen) ||
        !validSegment(e.name, lim_.maxNameLen))
      continue;
    // CUM-836 migration (review finding 2): a pre-fix, case-SENSITIVE index can
    // hold two rows whose (project, name) differ only by letter case - one FAT
    // file, two owners. Collapse to ONE row per folded identity. Which row
    // survives: the one that keeps the DEVICE OWNER in control. A surviving
    // member row would keep readableBy(member) over the physical file, and in
    // the poisoned order (the member's variant landed first, the owner's write
    // then landed IN that file) the file physically holds the OWNER's content -
    // a cross-tenant read. The owner's row grants a member nothing, and the
    // owner passes every ownership check on it. If NO row of the group is
    // owner-controlled (two members fought over one name pre-fix), the FIRST
    // row wins: pre-fix first-writer semantics, fail-closed for the later dup.
    const std::string folded = words::lowerAscii(e.project) + '\t' + words::lowerAscii(e.name);
    const auto dupIt = byFolded.find(folded);
    if (dupIt == byFolded.end()) {
      // Lane FSCAP: load() keeps every valid row whatever the entry limit says -
      // an upgrade (or a downgrade-then-upgrade) never loses a file. The limit
      // gates new adds in wouldExceed, not what persistence keeps.
      byFolded.emplace(folded, entries_.size());
      entries_.push_back(e);
    } else if (ownerControlled(e) && !ownerControlled(entries_[dupIt->second])) {
      entries_[dupIt->second] = e;   // the owner's row takes the identity over
    }                                // else: the earlier row already rules this file
  }
  return true;
}

}  // namespace nimbus::orch
