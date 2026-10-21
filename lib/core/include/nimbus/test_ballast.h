#pragma once
// Multi-chunk memory ballast behind the test console's `FAULT sram_ballast <bytes>`.
//
// One allocation can never pin more than the heap's largest free block, and an
// ESP32 internal heap is fragmented: on a bench unit about 47 KB is free while the
// largest block is about 22 KB, so a single block never reaches the low-memory
// gates a test wants (the turn and drain admission gates sit near 12 KB free).
// This holds the target as up to kMaxChunks blocks, each as big as the largest
// free block allows, and never takes free memory below `keepFree` so the console
// itself can still answer.
//
// Portable (the heap is passed in as callbacks) so the host suite drives it on a
// fake fragmented heap. The device binds heap_caps internal allocations; only the
// test build uses it.
#include <cstddef>

namespace nimbus {

struct BallastHeap {
  void* (*alloc)(std::size_t n, void* ctx) = nullptr;
  void (*release)(void* p, void* ctx) = nullptr;
  std::size_t (*largest)(void* ctx) = nullptr;   // largest block alloc can return now
  std::size_t (*freeNow)(void* ctx) = nullptr;   // total free bytes now
  void* ctx = nullptr;
};

class Ballast {
 public:
  static constexpr int kMaxChunks = 16;
  static constexpr std::size_t kMinChunk = 64;   // tiny fragments are not worth chasing

  // Release what is held, then hold up to `target` bytes (0 = release only).
  // Returns the bytes held: less than `target` when the floor, the chunk cap or the
  // heap's fragments stop it.
  std::size_t fill(std::size_t target, std::size_t keepFree, const BallastHeap& h) {
    release(h);
    while (held_ < target && n_ < kMaxChunks) {
      const std::size_t take = nextChunk(target - held_, keepFree, h);
      if (take == 0) break;
      void* p = h.alloc(take, h.ctx);
      std::size_t got = take;
      if (!p && take / 2 >= kMinChunk) {   // the block moved under another task: try half
        got = take / 2;
        p = h.alloc(got, h.ctx);
      }
      if (!p) break;
      chunk_[n_++] = p;
      held_ += got;
    }
    return held_;
  }

  void release(const BallastHeap& h) {
    for (int i = 0; i < n_; i++) h.release(chunk_[i], h.ctx);
    n_ = 0;
    held_ = 0;
  }

  std::size_t held() const { return held_; }
  int chunks() const { return n_; }

 private:
  // The next block to take: the rest of the target, capped by the largest free
  // block and by the floor. 0 = stop.
  static std::size_t nextChunk(std::size_t want, std::size_t keepFree, const BallastHeap& h) {
    const std::size_t freeNow = h.freeNow(h.ctx);
    if (freeNow <= keepFree) return 0;
    std::size_t take = want;
    const std::size_t big = h.largest(h.ctx);
    if (take > big) take = big;
    if (take > freeNow - keepFree) take = freeNow - keepFree;
    // A small remainder of the target is taken exactly; a tiny block that is all
    // the heap offers is not.
    if (take == 0 || (take < kMinChunk && take < want)) return 0;
    return take;
  }

  void* chunk_[kMaxChunks] = {};
  int n_ = 0;
  std::size_t held_ = 0;
};

}  // namespace nimbus
