#pragma once
// verdict_memo - "write this cached verdict only when it changes", per boot.
//
// A successful turn refreshes the provider verify cache (vfy_<host> = verified). The
// old rule ("write when the stored verdict is not 1") rewrote the key at the end of
// EVERY turn once a full NVS had refused it, because the stored value never read back
// as 1: one failed write per turn, the class that filled the persist log on a desk
// device in October 2026. The memo remembers the verdict last ATTEMPTED per host for
// this boot, so a refused write is tried once, and again only when the verdict moves.
// Portable (no Arduino), host-tested in test/test_nvs_full.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nimbus {
namespace store {

class VerdictMemo {
 public:
  static constexpr size_t kHosts  = 8;
  static constexpr size_t kKeyMax = 16;

  // True when `host`'s verdict `v` has not been attempted this boot (the caller then
  // writes it). Records the attempt. More than kHosts distinct hosts: the oldest
  // entry is reused (a re-attempt, never a missed change).
  bool shouldWrite(const char* host, int8_t v) {
    if (!host) return false;
    for (Entry& e : e_) {
      if (!e.used || std::strncmp(e.host, host, kKeyMax) != 0) continue;
      if (e.verdict == v) return false;
      e.verdict = v;
      return true;
    }
    Entry& e = e_[next_++ % kHosts];
    std::strncpy(e.host, host, kKeyMax - 1);
    e.host[kKeyMax - 1] = 0;
    e.verdict = v;
    e.used = true;
    return true;
  }

 private:
  struct Entry {
    char   host[kKeyMax] = {};
    int8_t verdict = 0;
    bool   used = false;
  };
  Entry  e_[kHosts];
  size_t next_ = 0;
};

}  // namespace store
}  // namespace nimbus
