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

FileKind fileKindForName(std::string_view name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot + 1 >= name.size()) return FileKind::Data;
  std::string ext(name.substr(dot + 1));
  for (auto& c : ext) c = char(::tolower((unsigned char)c));
  static const char* kDoc[]   = {"md", "txt", "pdf", "html", "csv", "json", "log"};
  static const char* kImage[] = {"png", "jpg", "jpeg", "gif", "webp", "bmp"};
  static const char* kAudio[] = {"ogg", "mp3", "wav", "m4a", "opus", "flac"};
  for (auto* e : kDoc)   if (ext == e) return FileKind::Doc;
  for (auto* e : kImage) if (ext == e) return FileKind::Image;
  for (auto* e : kAudio) if (ext == e) return FileKind::Audio;
  return FileKind::Data;
}

bool fileReadableAsText(std::string_view name, const std::string& bytes) {
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

bool FileStore::validSegment(std::string_view s, size_t maxLen) {
  if (s.empty() || s.size() > maxLen) return false;
  if (s == "." || s == "..") return false;
  if (s[0] == '.') return false;                       // no hidden files
  bool nonSpace = false;
  for (char ch : s) {
    const unsigned char c = (unsigned char)ch;
    if (c < 0x21 || c > 0x7E) return false;            // printable ASCII, no spaces/ctrl
    if (c == '/' || c == '\\' || c == ':') return false;  // separators / FAT quirks
    nonSpace = true;
  }
  return nonSpace;
}

std::string FileStore::relPath(std::string_view project, std::string_view name) const {
  if (!validSegment(project, lim_.maxProjectLen) || !validSegment(name, lim_.maxNameLen))
    return std::string();
  std::string out;
  out.reserve(project.size() + 1 + name.size());
  out.append(project.data(), project.size());
  out += '/';
  out.append(name.data(), name.size());
  return out;
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
// Allocation-free on purpose: indexOf runs inside add()/wouldExceed, and with no
// entry cap a per-comparison std::string would put transient churn on the
// internal heap at every save (lane FSCAP review fix).
static char foldAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}
static bool fatSameSegment(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (foldAscii(a[i]) != foldAscii(b[i])) return false;
  return true;
}

// A row the device owner controls: the owner field is empty (the v4.1 data
// boundary) or explicitly the owner namespace. load()'s case-variant dedupe
// uses this to pick which row of a pre-fix duplicate pair survives.
static bool ownerControlled(const FileEntry& e) {
  return e.owner.empty() || e.owner == kOwnerNs;
}

int FileStore::indexOf(std::string_view project, std::string_view name) const {
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

bool FileStore::wouldExceed(std::string_view project, std::string_view name,
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

bool FileStore::remove(std::string_view project, std::string_view name) {
  const int i = indexOf(project, name);
  if (i < 0) return false;
  entries_.erase(entries_.begin() + i);
  return true;
}

bool FileStore::setProviderId(std::string_view project, std::string_view name,
                              const std::string& tag, const std::string& id) {
  const int i = indexOf(project, name);
  if (i < 0) return false;
  entries_[size_t(i)].providerTag.assign(tag.data(), tag.size());
  entries_[size_t(i)].providerFileId.assign(id.data(), id.size());
  return true;
}

const FileEntry* FileStore::find(std::string_view project, std::string_view name) const {
  const int i = indexOf(project, name);
  return i < 0 ? nullptr : &entries_[size_t(i)];
}

std::vector<const FileEntry*> FileStore::list(std::string_view project) const {
  std::vector<const FileEntry*> out;
  for (const auto& e : entries_)
    if (project.empty() || std::string_view(e.project) == project) out.push_back(&e);
  return out;
}

std::vector<const FileEntry*> FileStore::listPage(std::string_view project, size_t offset,
                                                  size_t limit) const {
  std::vector<const FileEntry*> out;
  size_t seen = 0;
  for (const auto& e : entries_) {
    if (!project.empty() && std::string_view(e.project) != project) continue;
    if (seen++ < offset) continue;
    if (limit && out.size() >= limit) break;
    out.push_back(&e);
  }
  return out;
}

// ---- regenerable copies (lane FSCAP eviction) --------------------------------

bool FileStore::isRegenerableCopy(const FileEntry& e) const {
  // Only a chat-attachment copy is regenerable: the Telegram archive keeps the
  // original. Anything outside those folders is someone's own file and is never
  // a candidate, even when a newer entry holds the same content (review fix).
  return isAttachmentProject(e.project);
}

std::vector<const FileEntry*> FileStore::planEviction(uint64_t needBytes) const {
  std::vector<const FileEntry*> cands;
  cands.reserve(entries_.size() / 4 + 8);
  for (const auto& e : entries_)
    if (isAttachmentProject(e.project)) cands.push_back(&e);
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
    if (std::find_if(out.begin(), out.end(),
                     [&](const std::string& p) { return std::string_view(e.project) == p; }) == out.end())
      out.push_back(plainCopy(e.project));
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
  // call that must stay bounded as the index scales with the card. The map's keys
  // AND nodes ride the working-set allocator too (review fix: sub-128 B map nodes
  // would otherwise land on the internal heap, one per row, at boot).
  // Lane LINUXFIX: WorkingStringMap names an explicit content hasher - libstdc++
  // (g++ 12, the nimbusd image) has no std::hash for a custom-allocator string.
  using FoldMap = WorkingStringMap<size_t>;
  FoldMap byFolded;
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
    e.project.assign(f[0].data(), f[0].size());
    e.name.assign(f[1].data(), f[1].size());
    e.bytes = uint32_t(strtoul(f[2].c_str(), nullptr, 10));
    e.createdAt = uint32_t(strtoul(f[3].c_str(), nullptr, 10));
    unsigned k = unsigned(strtoul(f[4].c_str(), nullptr, 10));
    e.kind = k <= 3 ? FileKind(k) : FileKind::Data;
    e.hash = strtoull(f[5].c_str(), nullptr, 16);
    e.providerTag.assign(f[6].data(), f[6].size());
    e.providerFileId.assign(f[7].data(), f[7].size());
    if (n == 9) {
      e.owner.assign(f[8].data(), f[8].size());
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
    // The folded identity key, built straight into a working-set string: no
    // per-row std::string temporaries on the internal heap at boot.
    WorkingString folded;
    folded.reserve(e.project.size() + 1 + e.name.size());
    for (char c : e.project) folded += foldAscii(c);
    folded += '\t';
    for (char c : e.name) folded += foldAscii(c);
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
