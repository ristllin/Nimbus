#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

#include "nimbus/orch/file_store.h"
#include "nimbus/orch/file_tools.h"
#include "posix_fs.h"

// posix_file_tools - the plain file tools' card seam (file_tools.h FileToolsIo) over a POSIX
// tree, so nimbusd runs the same library as the device: bytes at <root>/<project>/<name>, the
// part file at <root>/<project>/.<name>.tools-part (outside the index), the index persisted
// with the atomic writer at <root>/.index. Every result text comes from the shared library
// (file_tools.h, file_aids.h), so on the same files the daemon and the device answer byte for
// byte the same (nimbusd/tests/test_hosted_file_aids.cpp against test/golden/file_aids_results.txt).
// The engine-2 registration on the daemon is RB-N2b's; this is the seam it binds.
namespace nimbusd {

class PosixFileToolsIo : public nimbus::orch::FileToolsIo {
 public:
  explicit PosixFileToolsIo(std::string root) : root_(std::move(root)) {
    std::string idx;
    if (fsutil::readFile(root_ + "/.index", idx)) store_.load(idx);
  }

  // One hold: the daemon's file tools run one at a time per store.
  bool locked(const std::function<void()>& fn) override {
    std::lock_guard<std::mutex> g(mu_);
    fn();
    return true;
  }
  nimbus::orch::FileStore& index() override { return store_; }

  bool readAt(const std::string& p, const std::string& n, uint64_t offset, size_t len, std::string& out) override {
    out.clear();
    const int fd = ::open(file(p, n).c_str(), O_RDONLY);
    if (fd < 0) return false;
    out.resize(len);
    size_t got = 0;
    while (got < len) {
      const ssize_t r = ::pread(fd, &out[got], len - got, off_t(offset + got));
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) break;
      got += size_t(r);
    }
    ::close(fd);
    out.resize(got);
    return true;
  }

  // All or nothing: a short write is cut back to the old size.
  bool append(const std::string& p, const std::string& n, std::string_view bytes) override {
    if (!fsutil::mkdirs(root_ + "/" + p)) return false;
    const std::string path = file(p, n);
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) return false;
    struct stat st;
    const off_t before = ::fstat(fd, &st) == 0 ? st.st_size : 0;
    const bool ok = fsutil::writeAll(fd, std::string(bytes)) && ::fsync(fd) == 0;
    if (!ok && ::ftruncate(fd, before) != 0) {
      // the cut back failed too: the index still holds the old size, and the next read stops there
    }
    ::close(fd);
    return ok;
  }

  bool partWrite(const std::string& p, const std::string& n, std::string_view bytes, bool first) override {
    if (!fsutil::mkdirs(root_ + "/" + p)) return false;
    const int fd = ::open(part(p, n).c_str(), O_CREAT | O_WRONLY | (first ? O_TRUNC : O_APPEND), 0644);
    if (fd < 0) return false;
    const bool ok = fsutil::writeAll(fd, std::string(bytes)) && ::fsync(fd) == 0;
    ::close(fd);
    return ok;
  }
  // rename() replaces the file in one step: no moment where it is gone.
  bool partSwap(const std::string& p, const std::string& n) override {
    if (std::rename(part(p, n).c_str(), file(p, n).c_str()) != 0) return false;
    fsutil::syncDir(root_ + "/" + p);
    return true;
  }
  void partDiscard(const std::string& p, const std::string& n) override { std::remove(part(p, n).c_str()); }
  bool persistIndex() override { return fsutil::writeFileAtomic(root_ + "/.index", store_.dump()); }
  uint32_t now() override { return uint32_t(std::time(nullptr)); }

  std::string file(const std::string& p, const std::string& n) const { return root_ + "/" + p + "/" + n; }

 private:
  std::string part(const std::string& p, const std::string& n) const { return root_ + "/" + p + "/." + n + ".tools-part"; }

  std::string root_;
  std::mutex mu_;
  nimbus::orch::FileStore store_;
};

}  // namespace nimbusd
