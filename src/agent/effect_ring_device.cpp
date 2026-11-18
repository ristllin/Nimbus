#include "effect_ring_device.h"

#include <Arduino.h>   // xPortCanYield
#include <FS.h>
#include <cstring>
#include <ctime>
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
    if (!inside(path) || !memory::haveSd()) return false;
    memory::Lock g;
    File f = memory::dataFs().open(path.c_str(), FILE_READ);
    if (!f || f.isDirectory()) return false;
    out = (size_t)f.size();
    f.close();
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
    if (dir != fx::kFxRoot || !memory::haveSd()) return false;
    memory::Lock g;
    fs::FS& fs = memory::dataFs();
    if (!fs.exists(fx::kFxRoot)) return true;   // no folder yet: no files (a card is there)
    File d = fs.open(fx::kFxRoot);
    if (!d || !d.isDirectory()) return false;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      if (e.isDirectory()) continue;
      const char* slash = strrchr(e.path(), '/');   // the bare name, as files_subsystem reads it
      names.emplace_back(slash ? slash + 1 : e.name());
    }
    return true;
  }

 private:
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
};

CardFxFiles& cardFiles() {
  static CardFxFiles f;
  return f;
}

// The pending receipt slot. No lock: a tool's sink and the record() after the call both run
// on the turn task, one tool call at a time (no on-device concurrency, AGENTS.md section 4).
std::string g_pendingTool;
std::string g_pendingLine;

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
    g_pendingTool = tool;
    g_pendingLine = line;
  } catch (const std::bad_alloc&) {   // the tool's result stands in for it
    g_pendingTool.clear();
  }
}

void record(const std::string& chatId, const std::string& tool, const std::string& receipt) {
  const PsramFirstScope psram;
  try {
    std::string line;
    if (!g_pendingTool.empty() && g_pendingTool == tool) line.swap(g_pendingLine);
    g_pendingTool.clear();
    g_pendingLine.clear();
    ring().append(chatId, nowEpoch(), tool, line.empty() ? receipt : line);   // NoMemory is a state, not a throw
  } catch (const std::bad_alloc&) {   // the pending copy: the effect is not recorded, the turn goes on
  }
}

}  // namespace agent::fxdev
