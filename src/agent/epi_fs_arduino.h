#pragma once
#include <FS.h>

#include <string>
#include <vector>

#include "nimbus/orch/episodic_log.h"
#include "nimbus/fault.h"   // FAULT sd_io - simulate a mid-op SD write failure

// ArduinoEpiFs - device backing for the portable EpiFs byte-file seam
// (docs/orchestrator-storage.md §3), over an Arduino fs::FS (LittleFS or the SD
// card). Kept tiny and dependency-free so the portable AppendLogEpisodicStore
// stays host-testable while the device does the real SD read/write/list/delete.
//
// ⚠ SD-gated: on a board with no detected card the whole append-log path is dead
// (memory_subsystem constructs this only when g_haveSd), so it is compile-verified
// but not yet exercised on hardware (bench board reads cardType=0).
namespace agent {
namespace memory {

class ArduinoEpiFs : public nimbus::orch::EpiFs {
 public:
  explicit ArduinoEpiFs(fs::FS& fs) : fs_(fs) {}

  long append(const std::string& path, const std::string& bytes) override {
    // FAULT sd_io: emulate a card that vanished mid-write (present at mount, now
    // failing) so the sudden-loss demote path is testable without a physical pull.
    if (nimbus::fault::active(nimbus::fault::SD_IO)) return -1;
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

  long size(const std::string& path) const override {
    File f = fs_.open(path.c_str(), FILE_READ);
    if (!f) return 0;
    const long n = (long)f.size();
    f.close();
    return n;
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
    File d = fs_.open(dir.c_str());
    if (!d || !d.isDirectory()) { if (d) d.close(); return out; }
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      if (!f.isDirectory()) {
        std::string name = f.name();
        size_t slash = name.find_last_of('/');  // some cores return a full path
        out.push_back(slash == std::string::npos ? name : name.substr(slash + 1));
      }
      f.close();
    }
    d.close();
    return out;
  }

  bool remove(const std::string& path) override { return fs_.remove(path.c_str()); }

  bool exists(const std::string& path) const override { return fs_.exists(path.c_str()); }

 private:
  fs::FS& fs_;
};

}  // namespace memory
}  // namespace agent
