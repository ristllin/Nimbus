#pragma once
#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "nimbus/orch/tx/log.h"

// tx_battery/mem_files.h - storage for the battery (lane RB-B0b): the TxFiles interface over
// a map, with the faults the crash and log properties inject. A power cut is a thrown
// Killed: nothing after the cut runs, and what was written before it stays, including
// half a line when the cut tears a write.
namespace txb {

namespace tx = nimbus::orch::tx;

struct Killed {
  std::string where;
};

class MemFiles : public tx::TxFiles {
 public:
  std::map<std::string, std::string> files;

  // Faults, counted over the whole life of the store (a reboot keeps the files and the
  // counters; the test resets the plan).
  long killAtAppend = -1;    // the append with this index (0-based) is cut
  bool tornKill = false;     // ... after writing half of its bytes
  long refuseAppend = -1;    // the append with this index answers false (no card, lock)
  bool refuseAll = false;    // every write answers false
  long killAtRename = -1;    // the rename with this index is cut before it happens
  bool refuseRename = false;
  bool refuseWholeWrites = false;   // every whole-file write answers false (head.json; RB-X1)
  // Content-aware faults: cut or refuse the first append whose bytes match.
  std::function<bool(const char* data, size_t len)> killWhen;
  std::function<bool(const char* data, size_t len)> refuseWhen;

  size_t appends = 0;
  size_t renames = 0;
  size_t wholeWrites = 0;

  bool append(const std::string& path, const char* data, size_t len) override {
    const long idx = (long)appends++;
    if (refuseAll || idx == refuseAppend) return false;
    if (refuseWhen && refuseWhen(data, len)) {
      refuseWhen = nullptr;
      return false;
    }
    if (idx == killAtAppend || (killWhen && killWhen(data, len))) {
      killWhen = nullptr;
      if (tornKill) files[path].append(data, len / 2);
      throw Killed{"append " + std::to_string(idx) + " to " + path};
    }
    files[path].append(data, len);
    return true;
  }
  bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) override {
    got = 0;
    auto it = files.find(path);
    if (it == files.end()) return false;
    if (offset >= it->second.size()) return true;
    got = std::min(want, it->second.size() - offset);
    it->second.copy(buf, got, offset);
    return true;
  }
  bool size(const std::string& path, size_t& out) override {
    auto it = files.find(path);
    if (it == files.end()) return false;
    out = it->second.size();
    return true;
  }
  bool writeWhole(const std::string& path, const std::string& data) override {
    ++wholeWrites;
    if (refuseAll || refuseWholeWrites) return false;
    files[path] = data;
    return true;
  }
  bool rename(const std::string& from, const std::string& to) override {
    const long idx = (long)renames++;
    if (refuseAll || refuseRename) return false;
    if (idx == killAtRename) throw Killed{"rename " + from};
    auto it = files.find(from);
    if (it == files.end()) return false;
    files[to] = it->second;
    files.erase(it);
    return true;
  }
  bool remove(const std::string& path) override {
    if (refuseAll) return false;
    return files.erase(path) > 0;
  }
  bool list(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    const std::string pre = dir.empty() || dir.back() == '/' ? dir : dir + "/";
    for (const auto& kv : files)
      if (kv.first.rfind(pre, 0) == 0 && kv.first.find('/', pre.size()) == std::string::npos)
        names.push_back(kv.first.substr(pre.size()));
    return true;
  }
  void clearFaults() {
    killAtAppend = refuseAppend = killAtRename = -1;
    tornKill = refuseAll = refuseRename = refuseWholeWrites = false;
    killWhen = refuseWhen = nullptr;
  }
  // The segment files of one chat, in name order (seg-000001.jsonl, ...).
  std::vector<std::string> segments(const std::string& dir) {
    std::vector<std::string> names, out;
    list(dir, names);
    for (const std::string& n : names)
      if (n.rfind("seg-", 0) == 0) out.push_back(dir + "/" + n);
    return out;
  }
};

}  // namespace txb
