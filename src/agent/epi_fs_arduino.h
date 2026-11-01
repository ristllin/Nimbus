#pragma once
#include <Arduino.h>   // xPortCanYield - the FILE-write context guard
#include <FS.h>
#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/orch/episodic_log.h"
#include "nimbus/fault.h"   // FAULT sd_io - simulate a mid-op SD write failure

// ArduinoEpiFs - device backing for the portable EpiFs byte-file seam
// (docs/orchestrator-storage.md §3), over an Arduino fs::FS (LittleFS or the SD
// card). Kept tiny and dependency-free so the portable AppendLogEpisodicStore
// stays host-testable while the device does the real SD read/write/list/delete.
//
// SD-gated: memory_subsystem constructs this only when a card is mounted.
namespace agent {
namespace memory {

class ArduinoEpiFs : public nimbus::orch::EpiFs {
 public:
  explicit ArduinoEpiFs(fs::FS& fs) : fs_(fs) {}

  long append(const std::string& path, const std::string& bytes) override {
    // FAULT sd_io: emulate a card that vanished mid-write (present at mount, now
    // failing) so the sudden-loss demote path is testable without a physical pull.
    if (nimbus::fault::active(nimbus::fault::SD_IO)) return -1;
    // The 2026-10-29 Lumi panic class (see errlog_fs.cpp's append guard): a FILE
    // touch at a raised CPU interrupt level aborts inside newlib's recursive FILE
    // lock. Refusing here rides the store's designed SD-loss path - the row is
    // held (RAM recent-window + the held queue) and the next append flushes the
    // held rows in arrival order; one refusal sample cannot demote the card (the
    // tracker needs a streak of 2).
    if (!xPortCanYield()) return -1;
    File f = fs_.open(path.c_str(), FILE_APPEND);
    if (!f) return -1;
    long off = (long)f.size();  // existing size == start offset of this append
    size_t n = f.write(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    f.close();
    return (n == bytes.size()) ? off : -1;
  }

  std::string readAll(const std::string& path) const override {
    File f = fs_.open(path.c_str(), FILE_READ);
    if (!f) return std::string();
    std::string out;
    out.reserve(f.size());
    uint8_t buf[256];
    int r;
    while ((r = f.read(buf, sizeof(buf))) > 0) out.append((const char*)buf, r);
    f.close();
    return out;
  }

  // size() and exists() stat the file instead of opening it: every open
  // allocates a stdio stream and its lock on the internal heap, and the boot
  // scan, the index backfill and retention ask these once per day-stream.
  long size(const std::string& path) const override {
    char vp[kVfsPathMax];
    struct stat st;
    if (!vfsPath(path, vp) || stat(vp, &st) != 0 || S_ISDIR(st.st_mode)) return 0;
    return (long)st.st_size;
  }

  std::string readRange(const std::string& path, long offset, long len) const override {
    if (offset < 0 || len < 0) return std::string();
    File f = fs_.open(path.c_str(), FILE_READ);
    if (!f) return std::string();
    if ((size_t)(offset + len) > f.size() || !f.seek((uint32_t)offset)) { f.close(); return std::string(); }
    std::string out;
    out.resize((size_t)len);
    int got = f.read(reinterpret_cast<uint8_t*>(&out[0]), (size_t)len);
    f.close();
    if (got != len) return std::string();
    return out;
  }

  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    forEachName(dir, [&](const char* name) { out.emplace_back(name); });
    return out;
  }

  // Streams the directory with readdir() on the mounted path: no File object,
  // no open per entry and no name kept, so walking a directory of hundreds of
  // blobs costs one directory handle instead of hundreds of small internal-heap
  // allocations. Entries whose type is unknown are passed on as files (the
  // callers parse the name, and a delete of a directory simply fails).
  void forEachName(const std::string& dir,
                   const std::function<void(const char* name)>& fn) const override {
    char vp[kVfsPathMax];
    if (!vfsPath(dir, vp)) return;
    DIR* d = opendir(vp);
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
      if (e->d_type == DT_DIR) continue;
      if (e->d_name[0] == '.' && (e->d_name[1] == '\0' ||
                                  (e->d_name[1] == '.' && e->d_name[2] == '\0')))
        continue;
      fn(e->d_name);
    }
    closedir(d);
  }

  bool remove(const std::string& path) override { return fs_.remove(path.c_str()); }

  bool exists(const std::string& path) const override {
    char vp[kVfsPathMax];
    struct stat st;
    return vfsPath(path, vp) && stat(vp, &st) == 0;
  }

 private:
  static constexpr size_t kVfsPathMax = 128;
  // The POSIX path of `path` on this file system ("/sd" + "/mem/blobs").
  bool vfsPath(const std::string& path, char (&out)[kVfsPathMax]) const {
    const char* mp = fs_.mountpoint();
    if (!mp) return false;
    const int n = snprintf(out, sizeof(out), "%s%s", mp, path.c_str());
    return n > 0 && n < (int)sizeof(out);
  }

  fs::FS& fs_;
};

}  // namespace memory
}  // namespace agent
