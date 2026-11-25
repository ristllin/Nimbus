#include "tx_store_device.h"

#include <Arduino.h>   // xPortCanYield, millis
#include <FS.h>
#include <esp_heap_caps.h>

#include <dirent.h>
#include <errno.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "memory_subsystem.h"   // the card lock, dataFs, haveSd
#include "nimbus/fault.h"       // FAULT sd_io
#include "nimbus/orch/schedule_tool.h"   // kStateDir - the routine files (lane E2-SCHED)

namespace agent::txdev {

namespace {

// The card as tx::CardTxFiles needs it (the effect ring's CardFxFiles calls, one per method).
class SdCard : public nimbus::orch::tx::CardOps {
 public:
  bool present() override { return memory::haveSd(); }
  bool mayWrite() override { return xPortCanYield() && !nimbus::fault::active(nimbus::fault::SD_IO); }
  uint32_t largestInternal() override {
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  uint32_t nowMs() override { return (uint32_t)millis(); }
  void lock() override { memory::lock(); }
  void unlock() override { memory::unlock(); }
  bool exists(const std::string& path) override { return memory::dataFs().exists(path.c_str()); }
  bool isDir(const std::string& path) override {
    File f = memory::dataFs().open(path.c_str(), FILE_READ);
    const bool d = f && f.isDirectory();
    if (f) f.close();
    return d;
  }
  bool mkdir(const std::string& path) override { return memory::dataFs().mkdir(path.c_str()); }
  size_t append(const std::string& path, const char* data, size_t len) override {
    File f = memory::dataFs().open(path.c_str(), FILE_APPEND);
    if (!f) return 0;
    const size_t n = f.write(reinterpret_cast<const uint8_t*>(data), len);
    f.close();
    return n;
  }
  bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) override {
    got = 0;
    File f = memory::dataFs().open(path.c_str(), FILE_READ);
    if (!f) return false;
    if (f.isDirectory()) {
      f.close();
      return false;
    }
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
  bool fileSize(const std::string& path, size_t& out) override {
    File f = memory::dataFs().open(path.c_str(), FILE_READ);
    if (!f) return false;
    const bool dir = f.isDirectory();
    if (!dir) out = (size_t)f.size();
    f.close();
    return !dir;
  }
  size_t writeWhole(const std::string& path, const char* data, size_t len) override {
    File f = memory::dataFs().open(path.c_str(), FILE_WRITE);
    if (!f) return 0;
    const size_t w = f.write(reinterpret_cast<const uint8_t*>(data), len);
    f.close();
    return w;
  }
  bool rename(const std::string& from, const std::string& to) override {
    return memory::dataFs().rename(from.c_str(), to.c_str());
  }
  bool remove(const std::string& path) override { return memory::dataFs().remove(path.c_str()); }
  // Both listings walk the folder with POSIX readdir (lane E2-STORE): File::openNextFile ends
  // the walk on a refused allocation or a read error as if the folder ended, so a short
  // listing could not be told from a whole one. Here errno tells them apart.
  bool listFiles(const std::string& dir, std::vector<std::string>& names) override {
    return walk(dir, names, /*withFolders=*/false);
  }
  bool listNames(const std::string& dir, std::vector<std::string>& names) override {
    return walk(dir, names, /*withFolders=*/true);
  }

 private:
  static constexpr size_t kVfsPathMax = 160;
  // The bare names of `dir`'s entries; false on any failure (no partial answer).
  static bool walk(const std::string& dir, std::vector<std::string>& names, bool withFolders) {
    names.clear();
    const char* mp = memory::dataFs().mountpoint();
    char vp[kVfsPathMax];
    if (!mp) return false;
    const int n = snprintf(vp, sizeof(vp), "%s%s", mp, dir.c_str());
    if (n <= 0 || n >= (int)sizeof(vp)) return false;
    DIR* d = opendir(vp);
    if (!d) return false;
    bool ok = true;
    for (;;) {
      errno = 0;
      struct dirent* e = readdir(d);
      if (!e) {
        ok = errno == 0;   // the end of the folder, or a read error
        break;
      }
      if (e->d_name[0] == '.' && (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0'))) continue;
      // Every name counts as present for the absence proof; files are the regular entries.
      if (!withFolders && e->d_type != DT_REG) continue;
      try {
        names.emplace_back(e->d_name);
      } catch (const std::bad_alloc&) {
        ok = false;
        break;
      }
    }
    closedir(d);
    if (!ok) names.clear();
    return ok;
  }
};

SdCard& sdCard() {
  static SdCard card;
  return card;
}

}  // namespace

nimbus::orch::tx::CardTxFiles& cardTxFiles() {
  static nimbus::orch::tx::CardTxFiles files(sdCard());
  return files;
}

nimbus::orch::tx::CardTxFiles& cardRoutineFiles() {
  static nimbus::orch::tx::CardTxFiles files(sdCard(), nimbus::orch::tx::kCardWriteInternalFloor,
                                             nimbus::orch::schedule::kStateDir);
  return files;
}

}  // namespace agent::txdev
