#include "nimbus/orch/file_store.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

int FileStore::indexOf(const std::string& project, const std::string& name) const {
  for (size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i].project == project && entries_[i].name == name) return int(i);
  return -1;
}

uint64_t FileStore::totalBytes() const {
  uint64_t t = 0;
  for (const auto& e : entries_) t += e.bytes;
  return t;
}

bool FileStore::wouldExceed(const std::string& project, const std::string& name,
                            uint64_t addBytes, std::string& err) const {
  if (!validSegment(project, lim_.maxProjectLen)) { err = "bad project name"; return true; }
  if (!validSegment(name, lim_.maxNameLen))       { err = "bad file name"; return true; }
  if (addBytes > lim_.maxFileBytes) { err = "file too large"; return true; }
  const int existing = indexOf(project, name);
  const uint64_t replaced = existing >= 0 ? entries_[size_t(existing)].bytes : 0;
  if (totalBytes() - replaced + addBytes > lim_.maxTotalBytes) {
    err = "store full (artifacts are never auto-deleted - remove some first)";
    return true;
  }
  if (existing < 0 && entries_.size() >= lim_.maxEntries) {
    err = "too many files (remove some first)";
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
    std::string err;
    if (validSegment(e.project, lim_.maxProjectLen) &&
        validSegment(e.name, lim_.maxNameLen) &&
        entries_.size() < lim_.maxEntries)
      entries_.push_back(e);
  }
  return true;
}

}  // namespace nimbus::orch
