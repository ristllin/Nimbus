#pragma once
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/log.h"
#include "posix_fs.h"

// hosted_tx.h - engine 2's transcript store on a Virtual Nimbus (lane E2-HOST): the TxFiles
// seam of nimbus/orch/tx/log.h over the instance volume, <data>/mem/tx/<chatKey>/ (segments,
// head.json, digests). The same shape as PosixFxFiles (hosted_effects.h), confined to /mem/tx/:
// store paths are absolute ("/mem/tx/owner/seg-000001.jsonl") and land under the data
// directory; any other path, or one holding "/..", is refused, so a log bug cannot write
// elsewhere on the volume. A write returns only with its bytes on the disk (written, synced,
// closed); a folder is made on the first write; a missing folder lists as empty.
namespace nimbusd {

class PosixTxFiles : public nimbus::orch::tx::TxFiles {
 public:
  explicit PosixTxFiles(std::string dataDir) : root_(std::move(dataDir)) {}

  bool append(const std::string& path, const char* data, size_t len) override {
    std::string p;
    if (!mapForWrite(path, p)) return false;
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return false;
    const bool ok = fsutil::writeAll(fd, std::string(data, len)) && ::fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
  }
  bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) override {
    got = 0;
    std::string p;
    if (!map(path, p)) return false;
    const int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool ok = true;
    while (got < want) {
      const ssize_t n = ::pread(fd, buf + got, want - got, (off_t)(offset + got));
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) ok = false;   // a read fault is said, never taken for the end of the file
      if (n <= 0) break;
      got += (size_t)n;
    }
    ::close(fd);
    return ok;
  }
  bool size(const std::string& path, size_t& out) override {
    std::string p;
    struct stat st;
    if (!map(path, p) || ::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    out = (size_t)st.st_size;
    return true;
  }
  // Whole files go through the atomic writer (tmp, fsync, rename): a cut leaves the old bytes.
  bool writeWhole(const std::string& path, const std::string& data) override {
    std::string p;
    return mapForWrite(path, p) && fsutil::writeFileAtomic(p, data);
  }
  bool rename(const std::string& from, const std::string& to) override {
    std::string a, b;
    if (!map(from, a) || !mapForWrite(to, b) || ::rename(a.c_str(), b.c_str()) != 0) return false;
    fsutil::syncDir(fsutil::dirOf(b));
    return true;
  }
  bool remove(const std::string& path) override {
    std::string p;
    return map(path, p) && ::unlink(p.c_str()) == 0;
  }
  bool list(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    std::string p;
    if (!map(dir + "/", p)) return false;
    DIR* d = ::opendir(p.c_str());
    if (!d) return errno == ENOENT;   // no folder yet: no files
    for (struct dirent* e = ::readdir(d); e; e = ::readdir(d)) {
      const std::string n = e->d_name;
      struct stat st;
      if (n == "." || n == ".." || ::stat((p + n).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      names.push_back(n);
    }
    ::closedir(d);
    return true;
  }

  // Where a store path lands on the volume ("" when it is refused): tests and the boot log.
  std::string diskPath(const std::string& path) const {
    std::string p;
    return map(path, p) ? p : std::string();
  }

 private:
  bool map(const std::string& path, std::string& out) const {
    static const std::string kPrefix = std::string(nimbus::orch::tx::kTxRoot) + "/";
    if (path.rfind(kPrefix, 0) != 0 || path.find("/..") != std::string::npos) return false;
    out = root_ + path;
    return true;
  }
  bool mapForWrite(const std::string& path, std::string& out) const {
    return map(path, out) && fsutil::mkdirs(fsutil::dirOf(out));
  }
  std::string root_;
};

}  // namespace nimbusd
