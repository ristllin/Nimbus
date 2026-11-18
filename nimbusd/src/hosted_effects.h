#pragma once
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/effect_ring.h"
#include "posix_fs.h"

// hosted_effects.h - each chat's recent effects on a Virtual Nimbus (lane RB-FX, PRD section 8
// rail 8). The device's ring (nimbus/orch/effect_ring.h) over the instance volume:
// <data>/mem/fx/<key>.ring, beside the file store and outside its index, so no file tool can
// reach it. Phase 1 is the storage and the ring; the engine hooks and the ledger's receipt
// sink are wired in phase 2 (after RB-Q1b and RB-L1b), through HostedEffects::record.
namespace nimbusd {

// TxFiles over the instance disk, confined to /mem/fx/. Store paths are absolute
// ("/mem/fx/12345.ring") and land under the data directory; any other path, or one holding
// "/..", is refused, so a ring bug cannot write elsewhere on the volume. The folder is made on
// the first write; a missing folder lists as empty (the TxFiles contract).
class PosixFxFiles : public nimbus::orch::tx::TxFiles {
 public:
  explicit PosixFxFiles(std::string dataDir) : root_(std::move(dataDir)) {}

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
    const ssize_t n = ::pread(fd, buf, want, (off_t)offset);
    ::close(fd);
    if (n < 0) return false;
    got = (size_t)n;
    return true;
  }
  bool size(const std::string& path, size_t& out) override {
    std::string p;
    struct stat st;
    if (!map(path, p) || ::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    out = (size_t)st.st_size;
    return true;
  }
  // Returns only with the bytes on the disk (written, synced, closed).
  bool writeWhole(const std::string& path, const std::string& data) override {
    std::string p;
    if (!mapForWrite(path, p)) return false;
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const bool ok = fsutil::writeAll(fd, data) && ::fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
  }
  bool rename(const std::string& from, const std::string& to) override {
    std::string a, b;
    if (!map(from, a) || !map(to, b) || ::rename(a.c_str(), b.c_str()) != 0) return false;
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
    if (dir != nimbus::orch::fx::kFxRoot || !map(dir + "/", p)) return false;
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

 private:
  bool map(const std::string& path, std::string& out) const {
    static const std::string kPrefix = std::string(nimbus::orch::fx::kFxRoot) + "/";
    if (path.rfind(kPrefix, 0) != 0 || path.find("/..") != std::string::npos) return false;
    out = root_ + path;
    return true;
  }
  bool mapForWrite(const std::string& path, std::string& out) const {
    return map(path, out) && fsutil::mkdirs(fsutil::dirOf(out));
  }
  std::string root_;
};

// One instance's effect rings. The engine records each effect of a chat here (phase 2); the
// truth check and the old engine's recent-actions view read ring().
class HostedEffects {
 public:
  explicit HostedEffects(const std::string& dataDir) : files_(dataDir), ring_(&files_) {}

  nimbus::orch::fx::AppendResult record(const std::string& chatId, int64_t epoch, const std::string& tool,
                                        const std::string& receipt) {
    return ring_.append(chatId, epoch, tool, receipt);
  }
  nimbus::orch::fx::EffectRing& ring() { return ring_; }

 private:
  PosixFxFiles files_;
  nimbus::orch::fx::EffectRing ring_;
};

}  // namespace nimbusd
