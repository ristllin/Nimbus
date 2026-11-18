#include "effect_ring_device.h"

#include <Arduino.h>   // xPortCanYield
#include <FS.h>
#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <new>
#include <vector>

#include "memory_subsystem.h"                // the card lock, dataFs, haveSd
#include "nimbus/fault.h"                    // FAULT sd_io
#include "nimbus/harness/turn_memory.h"      // PsramFirstScope

namespace agent::fxdev {

namespace fx = nimbus::orch::fx;

namespace {

// TxFiles over the card, confined to /mem/fx/. Each call is one card step under the card lock
// (recursive, so a caller that holds it may record). A refusal answers false and never throws.
class CardFxFiles : public nimbus::orch::tx::TxFiles {
 public:
  bool append(const std::string& path, const char* data, size_t len) override {
    if (!writable(path)) return false;
    memory::Lock g;
    if (!makeRoot()) return false;
    File f = memory::dataFs().open(path.c_str(), FILE_APPEND);
    if (!f) return false;
    const size_t n = f.write(reinterpret_cast<const uint8_t*>(data), len);
    f.close();
    return n == len;
  }
  bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) override {
    got = 0;
    if (!inside(path) || !memory::haveSd()) return false;
    memory::Lock g;
    File f = memory::dataFs().open(path.c_str(), FILE_READ);
    if (!f) return false;
    const size_t sz = f.size();
    if (offset >= sz) {   // at or past the end: nothing to read (the TxFiles contract)
      f.close();
      return true;
    }
    const bool sought = f.seek((uint32_t)offset);
    const int r = sought ? f.read(reinterpret_cast<uint8_t*>(buf), want) : -1;
    f.close();
    if (r < 0) return false;
    got = (size_t)r;
    return true;
  }
  bool size(const std::string& path, size_t& out) override {
    char vp[kVfsPathMax];
    struct stat st;
    if (!inside(path) || !memory::haveSd() || !vfsPath(path, vp)) return false;
    memory::Lock g;
    if (::stat(vp, &st) != 0 || S_ISDIR(st.st_mode)) return false;
    out = (size_t)st.st_size;
    return true;
  }
  // At most kRingEntries * kLineMax (12,150 B): one open, write and close (RB-M2: a 16 KB
  // append holds the card lock about 28 ms).
  bool writeWhole(const std::string& path, const std::string& data) override {
    if (!writable(path)) return false;
    memory::Lock g;
    if (!makeRoot()) return false;
    File f = memory::dataFs().open(path.c_str(), FILE_WRITE);
    if (!f) return false;
    const size_t w = f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    f.close();
    return w == data.size();
  }
  // FAT cannot rename over a file: the target is removed first. The ring keeps the temp as
  // the only copy if the rename then fails, and settles it at the chat's next use.
  bool rename(const std::string& from, const std::string& to) override {
    if (!writable(from) || !inside(to)) return false;
    memory::Lock g;
    fs::FS& fs = memory::dataFs();
    if (fs.exists(to.c_str())) fs.remove(to.c_str());
    return fs.rename(from.c_str(), to.c_str());
  }
  bool remove(const std::string& path) override {
    if (!writable(path)) return false;
    memory::Lock g;
    return memory::dataFs().remove(path.c_str());
  }
  bool list(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    char vp[kVfsPathMax];
    if (dir != fx::kFxRoot || !memory::haveSd() || !vfsPath(dir, vp)) return false;
    memory::Lock g;
    DIR* d = ::opendir(vp);
    if (!d) return errno == ENOENT;   // no folder yet: no files (a card is there)
    while (struct dirent* e = ::readdir(d)) {
      if (e->d_type == DT_DIR || e->d_name[0] == '.') continue;
      names.emplace_back(e->d_name);
    }
    ::closedir(d);
    return true;
  }

 private:
  static constexpr size_t kVfsPathMax = 128;
  static bool inside(const std::string& path) {
    static const std::string kPrefix = std::string(fx::kFxRoot) + "/";
    return path.rfind(kPrefix, 0) == 0 && path.find("/..") == std::string::npos;
  }
  // A write may run: inside /mem/fx, a card, a task that may touch a file (the 2026-10-29
  // panic class: no FILE at a raised interrupt level), no injected card fault.
  static bool writable(const std::string& path) {
    return inside(path) && memory::haveSd() && xPortCanYield() && !nimbus::fault::active(nimbus::fault::SD_IO);
  }
  static bool makeRoot() {
    fs::FS& fs = memory::dataFs();
    if (fs.exists(fx::kFxRoot)) return true;
    fs.mkdir("/mem");
    return fs.mkdir(fx::kFxRoot) || fs.exists(fx::kFxRoot);
  }
  static bool vfsPath(const std::string& path, char (&out)[kVfsPathMax]) {
    const char* mp = memory::dataFs().mountpoint();
    if (!mp) return false;
    const int n = snprintf(out, sizeof(out), "%s%s", mp, path.c_str());
    return n > 0 && n < (int)sizeof(out);
  }
};

CardFxFiles& cardFiles() {
  static CardFxFiles f;
  return f;
}

// The pending receipt slot (one tool call at a time runs on the turn task).
std::mutex& pendingMu() {
  static std::mutex m;
  return m;
}
std::string& pendingTool() {
  static std::string s;
  return s;
}
std::string& pendingLine() {
  static std::string s;
  return s;
}

// The clock is set once it is past 2023 (the ledger's kMinClockEpoch rule); 0 otherwise.
int64_t nowEpoch() {
  const time_t t = time(nullptr);
  return t >= (time_t)1672531200 ? (int64_t)t : 0;
}

}  // namespace

fx::EffectRing& ring() {
  static fx::EffectRing r(&cardFiles());
  return r;
}

void notePendingReceipt(const std::string& tool, const std::string& line) {
  try {
    std::lock_guard<std::mutex> g(pendingMu());
    pendingTool() = tool;
    pendingLine() = line;
  } catch (const std::bad_alloc&) {   // the tool's result stands in for it
  }
}

void record(const std::string& chatId, const std::string& tool, const std::string& receipt) {
  const PsramFirstScope psram;
  try {
    std::string line;
    {
      std::lock_guard<std::mutex> g(pendingMu());
      if (!pendingTool().empty() && pendingTool() == tool) line.swap(pendingLine());
      pendingTool().clear();
      pendingLine().clear();
    }
    ring().append(chatId, nowEpoch(), tool, line.empty() ? receipt : line);   // NoMemory is a state, not a throw
  } catch (const std::bad_alloc&) {   // the pending copy: the effect is not recorded, the turn goes on
  }
}

}  // namespace agent::fxdev
