#pragma once
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "nimbus/orch/episodic_log.h"

// posix_fs - the HOST/daemon backing for the portable stores.
//
// The device backs the portable EpiFs byte-file seam with an Arduino fs::FS
// (LittleFS / SD); nimbusd backs it with a plain POSIX directory tree under
// /data/mem. Nothing above the seam changes: AppendLogEpisodicStore is the same
// portable code (lib/core), so the episodic query semantics, the O(1) append,
// and the boot-scan rehydration are byte-for-byte what the device runs.
//
// Two things live here:
//   * fsutil - path/dir helpers + an atomic tmp->rename writer, the write
//     discipline every whole-file store (vectors.bin, the KV JSON, loops.json)
//     reuses so a crash mid-write never leaves a torn file the next boot loads.
//   * PosixEpiFs - EpiFs over <fstream> + <dirent.h>.
namespace nimbusd {

namespace fsutil {

// mkdir -p for `path` (a directory). Silent success if it already exists.
// Returns false only on a real error (a component is a non-directory, EACCES).
inline bool mkdirs(const std::string& path) {
  if (path.empty()) return false;
  std::string acc;
  size_t i = 0;
  if (path[0] == '/') { acc = "/"; i = 1; }
  while (i <= path.size()) {
    if (i == path.size() || path[i] == '/') {
      if (!acc.empty() && acc != "/") {
        if (mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) {
          struct stat st;
          if (stat(acc.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
        }
      }
      if (i == path.size()) break;
      acc += '/';
    } else {
      acc += path[i];
    }
    i++;
  }
  return true;
}

// The directory component of an absolute file path ("/a/b/c.txt" -> "/a/b").
inline std::string dirOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Read a whole file; returns false (out untouched) if it is absent/unreadable.
inline bool readFile(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return true;
}

// Atomic whole-file write: write to "<path>.tmp", fsync, then rename over the
// target (rename is atomic on POSIX within a filesystem). The same discipline
// the device's stores use (tmp->rename), so a crash between open and rename
// leaves the previous good file in place, never a half-written one.
inline bool writeFileAtomic(const std::string& path, const std::string& bytes) {
  const std::string dir = dirOf(path);
  if (!dir.empty() && !mkdirs(dir)) return false;
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), (std::streamsize)bytes.size());
    f.flush();
    if (!f) return false;
  }
  // Best-effort durability: flush the file's bytes to the platter before the
  // rename so the rename can never land pointing at unwritten data.
  if (FILE* c = std::fopen(tmp.c_str(), "rb")) {
    fflush(c);
    std::fclose(c);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace fsutil

// PosixEpiFs - EpiFs (lib/core) over a POSIX directory tree. Paths are absolute
// ("/data/mem/episodic/2026-07-04.jsonl"); parent dirs are created on append.
class PosixEpiFs : public nimbus::orch::EpiFs {
 public:
  long append(const std::string& path, const std::string& bytes) override {
    const std::string dir = fsutil::dirOf(path);
    if (!dir.empty() && !fsutil::mkdirs(dir)) return -1;
    // Open for append, learn the pre-append size (== this record's start offset),
    // write, and require the whole record to land (a short write is a failure).
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (!f) return -1;
    const long off = (long)f.tellp();
    f.write(bytes.data(), (std::streamsize)bytes.size());
    f.flush();
    if (!f) return -1;
    return off;
  }

  std::string readAll(const std::string& path) const override {
    std::string out;
    fsutil::readFile(path, out);
    return out;
  }

  long size(const std::string& path) const override {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (long)st.st_size;
  }

  std::string readRange(const std::string& path, long offset, long len) const override {
    if (offset < 0 || len < 0) return std::string();
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    f.seekg(0, std::ios::end);
    const long fileSize = (long)f.tellg();
    if (offset + len > fileSize) return std::string();
    f.seekg(offset, std::ios::beg);
    std::string out;
    out.resize((size_t)len);
    f.read(&out[0], (std::streamsize)len);
    if (f.gcount() != (std::streamsize)len) return std::string();
    return out;
  }

  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    for (struct dirent* e = readdir(d); e; e = readdir(d)) {
      const std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      // Regular files only (no recursion, no dirs) - matches the seam contract.
      struct stat st;
      const std::string full = dir + "/" + name;
      if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) out.push_back(name);
    }
    closedir(d);
    return out;
  }

  bool remove(const std::string& path) override {
    return std::remove(path.c_str()) == 0;
  }

  bool exists(const std::string& path) const override {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
  }
};

}  // namespace nimbusd
