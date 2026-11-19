#pragma once
// The card the file-aid suites drive (file_aids.h's AidSource and AidSink), with faults:
// every read and write is one recorded call; failAt makes the Nth card call fail; a
// mutate hook changes the file between calls (a write from elsewhere). The refusing
// allocator refuses the Nth workingAlloc call, or every call past a byte budget, and
// counts live bytes and the peak.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/file_aids.h"
#include "nimbus/orch/psram_alloc.h"

namespace aidfake {

struct Card : nimbus::orch::aids::AidSource, nimbus::orch::aids::AidSink {
  std::string file, part;
  bool partOpen = false;
  long calls = 0, failAt = -1;  // the 1-based card call that fails (-1 none)
  size_t maxRead = 0, maxWrite = 0, reads = 0, writes = 0, swaps = 0, discards = 0;
  std::function<void(long)> beforeCall;  // runs before each card call (its 1-based number)

  bool step() {
    ++calls;
    if (beforeCall) beforeCall(calls);
    return calls != failAt;
  }
  bool readAt(uint64_t off, size_t len, char* out, size_t& got) override {
    got = 0;
    ++reads;
    if (len > maxRead) maxRead = len;
    if (!step()) return false;
    if (off > file.size()) return false;
    got = std::min(len, size_t(file.size() - off));
    std::memcpy(out, file.data() + off, got);
    return true;
  }
  bool partWrite(std::string_view b, bool first) override {
    ++writes;
    if (b.size() > maxWrite) maxWrite = b.size();
    if (!step()) return false;
    if (first) part.clear();
    partOpen = true;
    part.append(b.data(), b.size());
    return true;
  }
  bool partSwap() override {
    ++swaps;
    if (!step() || !partOpen) return false;
    file = part;
    part.clear();
    partOpen = false;
    return true;
  }
  void partDiscard() override {
    ++discards;
    part.clear();
    partOpen = false;
  }
};

// ---- the refusing allocator -------------------------------------------------------
inline long gAllocCalls = 0, gRefuseAt = -1;    // refuse the Nth call (1-based), -1 never
inline size_t gByteBudget = SIZE_MAX;           // refuse a call that would pass this many live bytes
inline size_t gLive = 0, gPeak = 0, gLiveBlocks = 0;
struct Hdr { size_t n; size_t pad; };

inline void* refusingAlloc(size_t n) {
  ++gAllocCalls;
  if (gAllocCalls == gRefuseAt || gLive + n > gByteBudget) return nullptr;
  Hdr* h = static_cast<Hdr*>(std::malloc(sizeof(Hdr) + n));
  if (!h) return nullptr;
  h->n = n;
  gLive += n;
  ++gLiveBlocks;
  if (gLive > gPeak) gPeak = gLive;
  return h + 1;
}
inline void refusingFree(void* p) {
  if (!p) return;
  Hdr* h = static_cast<Hdr*>(p) - 1;
  gLive -= h->n;
  --gLiveBlocks;
  std::free(h);
}
inline void arm(long refuseAt, size_t budget = SIZE_MAX) {
  gAllocCalls = 0;
  gRefuseAt = refuseAt;
  gByteBudget = budget;
  gPeak = gLive;
  nimbus::orch::setWorkingAllocators(refusingAlloc, refusingFree);
}
inline void disarm() {
  gRefuseAt = -1;
  gByteBudget = SIZE_MAX;
  nimbus::orch::setWorkingAllocators(nullptr, nullptr);
}

// A small deterministic generator (xorshift), so every property run is reproducible.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 88172645463325252ULL) {}
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  size_t below(size_t n) { return n ? size_t(next() % n) : 0; }
  bool chance(unsigned pct) { return below(100) < pct; }
};

}  // namespace aidfake
