#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include "nimbus/harness/platform.h"
#include "posix_fs.h"

// posix_platform - the daemon's agent::Platform.
//
// clock + delay are trivial. The load-bearing part is freeHeap: the engine's
// turn/recall/loop gates all read it and shed load when it drops below their
// floors. On the ESP32 that is a real ~46 KB internal heap; in a container the
// honest analogue is cgroup v2 memory accounting - `memory.current` against
// `memory.max` - so the engine degrades gracefully BEFORE the kernel OOM-kills
// the pod mid-turn (reporting a fake-large heap, the lab shortcut, is not
// production-safe: the first sign of trouble would be a SIGKILL).
//
// The mapping is deliberately simple and testable:
//   available = memory.max - memory.current            (bytes headroom)
//   reported  = clamp(available - reserve, 0, cap)
// The `reserve` is the graceful-degradation headroom: the engine "sees" memory
// running out `reserve` bytes before the cgroup actually does, so it sheds the
// tool loop / recall while there is still room to finish the turn and flush.
// Outside a cgroup (dev box, macOS, `memory.max == "max"`) there is no limit to
// gate against, so it reports the cap - gates never fire, exactly like the lab.
namespace nimbusd {

class CgroupMemory {
 public:
  // Defaults: cgroup v2 unified paths; a 32 MiB degradation reserve; a 64 MiB
  // reported cap (well above every engine floor, so an unconstrained process
  // never gates). Env overrides keep it operable without a rebuild.
  CgroupMemory() {
    curPath_ = envOr("NIMBUSD_CGROUP_CURRENT", "/sys/fs/cgroup/memory.current");
    maxPath_ = envOr("NIMBUSD_CGROUP_MAX", "/sys/fs/cgroup/memory.max");
    reserve_ = (uint64_t)envU("NIMBUSD_HEAP_RESERVE_BYTES", 32ull * 1024 * 1024);
    cap_ = (uint64_t)envU("NIMBUSD_HEAP_CAP_BYTES", 64ull * 1024 * 1024);
  }

  // Test seam: point at fixture files and set the knobs directly.
  void configure(std::string curPath, std::string maxPath, uint64_t reserve, uint64_t cap) {
    curPath_ = std::move(curPath);
    maxPath_ = std::move(maxPath);
    reserve_ = reserve;
    cap_ = cap;
  }

  // The reported memory cap in bytes (the "total" the web tile shows against the
  // free figure). Honest for a hosted instance: the container's memory ceiling.
  uint32_t capBytes() const { return (uint32_t)cap_; }

  // Reported "free heap" in bytes for the engine gates. Never throws; on any
  // read failure or an unlimited cgroup it returns the cap (no gating).
  uint32_t freeBytes() const {
    uint64_t limit = 0;
    if (!readLimit(limit)) return (uint32_t)cap_;   // no limit / not in a cgroup
    uint64_t cur = 0;
    if (!readU(curPath_, cur)) return (uint32_t)cap_;
    const uint64_t avail = cur >= limit ? 0 : (limit - cur);
    const uint64_t headroom = avail > reserve_ ? (avail - reserve_) : 0;
    return (uint32_t)std::min(headroom, cap_);
  }

 private:
  static std::string envOr(const char* k, const char* dflt) {
    const char* v = std::getenv(k);
    return v && *v ? std::string(v) : std::string(dflt);
  }
  static uint64_t envU(const char* k, uint64_t dflt) {
    const char* v = std::getenv(k);
    if (!v || !*v) return dflt;
    return std::strtoull(v, nullptr, 10);
  }
  static bool readU(const std::string& path, uint64_t& out) {
    std::string s;
    if (!fsutil::readFile(path, s)) return false;
    // Trim whitespace/newline.
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return false;
    out = std::strtoull(s.c_str() + b, nullptr, 10);
    return true;
  }
  // memory.max is either a byte count or the literal "max" (no limit).
  bool readLimit(uint64_t& out) const {
    std::string s;
    if (!fsutil::readFile(maxPath_, s)) return false;
    if (s.find("max") != std::string::npos) return false;   // unlimited
    return readU(maxPath_, out);
  }

  std::string curPath_, maxPath_;
  uint64_t reserve_ = 32ull * 1024 * 1024;
  uint64_t cap_ = 64ull * 1024 * 1024;
};

// Build the daemon Platform. `mem` must outlive the returned Platform (the
// freeHeap closure captures it by pointer).
inline agent::Platform makePosixPlatform(CgroupMemory* mem) {
  agent::Platform p;
  p.nowMs = [] {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  p.nowEpoch = [] {
    return (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
  };
  p.freeHeap = [mem] { return mem->freeBytes(); };
  p.delayMs = [](uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  };
  p.allocLarge = [](size_t n) { return std::malloc(n); };
  p.freeLarge = [](void* q) { std::free(q); };
  return p;
}

}  // namespace nimbusd
