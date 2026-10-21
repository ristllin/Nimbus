// The test console's multi-block memory ballast (FAULT sram_ballast), driven on a
// fake fragmented heap. Unit A showed the single-block hook could not reach the
// 12 KB admission gates: about 47 KB free, largest block about 22 KB, so any one
// allocation big enough to matter failed and held nothing.
#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "nimbus/test_ballast.h"

using nimbus::Ballast;
using nimbus::BallastHeap;

void setUp() {}
void tearDown() {}

// Free blocks of fixed sizes; an allocation carves from the first block that fits
// and a release gives the bytes back to that block. `failExactLargest` makes an
// allocation of exactly the largest block fail once (another task took it first).
struct FakeHeap {
  std::vector<size_t> blocks;
  std::map<uintptr_t, std::pair<size_t, size_t>> live;   // token -> (block, bytes)
  uintptr_t next = 1;
  bool failExactLargest = false;

  size_t largest() const {
    size_t m = 0;
    for (size_t b : blocks) m = b > m ? b : m;
    return m;
  }
  size_t freeBytes() const {
    size_t s = 0;
    for (size_t b : blocks) s += b;
    return s;
  }
  void* alloc(size_t n) {
    if (failExactLargest && n == largest()) {
      failExactLargest = false;
      return nullptr;
    }
    for (size_t i = 0; i < blocks.size(); i++) {
      if (blocks[i] < n) continue;
      blocks[i] -= n;
      live[next] = {i, n};
      return reinterpret_cast<void*>(next++);
    }
    return nullptr;
  }
  void release(void* p) {
    auto it = live.find(reinterpret_cast<uintptr_t>(p));
    TEST_ASSERT_TRUE_MESSAGE(it != live.end(), "released a block the ballast never held");
    blocks[it->second.first] += it->second.second;
    live.erase(it);
  }
  BallastHeap ops() {
    BallastHeap h;
    h.ctx = this;
    h.alloc = [](size_t n, void* c) { return static_cast<FakeHeap*>(c)->alloc(n); };
    h.release = [](void* p, void* c) { static_cast<FakeHeap*>(c)->release(p); };
    h.largest = [](void* c) { return static_cast<FakeHeap*>(c)->largest(); };
    h.freeNow = [](void* c) { return static_cast<FakeHeap*>(c)->freeBytes(); };
    return h;
  }
};

// Unit A's picture: about 47 KB free, the largest block 22516 B.
static FakeHeap unitA() { return FakeHeap{{22516, 15000, 7000, 2500}}; }

// The bench failure, fixed: a target past the largest block is held in several
// blocks, and the heap lands under the 12 KB gate.
static void test_target_past_the_largest_block_reaches_the_gate() {
  FakeHeap heap = unitA();
  const size_t start = heap.freeBytes();
  const size_t target = start - 11000;
  TEST_ASSERT_TRUE(target > heap.largest());
  TEST_ASSERT_NULL(heap.alloc(target));   // the old single-block hook held nothing here
  Ballast b;
  TEST_ASSERT_EQUAL_UINT32(target, b.fill(target, 4096, heap.ops()));
  TEST_ASSERT_TRUE(b.chunks() >= 2);
  TEST_ASSERT_EQUAL_UINT32(11000, heap.freeBytes());
  TEST_ASSERT_TRUE(heap.freeBytes() < 12000);
  b.release(heap.ops());
}

// A new size replaces the old ballast (it is released first), and 0 releases all.
static void test_refill_replaces_and_zero_releases() {
  FakeHeap heap = unitA();
  const size_t start = heap.freeBytes();
  Ballast b;
  b.fill(30000, 4096, heap.ops());
  TEST_ASSERT_EQUAL_UINT32(5000, b.fill(5000, 4096, heap.ops()));
  TEST_ASSERT_EQUAL_UINT32(start - 5000, heap.freeBytes());
  TEST_ASSERT_EQUAL_UINT32(0, b.fill(0, 4096, heap.ops()));
  TEST_ASSERT_EQUAL_INT(0, b.chunks());
  TEST_ASSERT_EQUAL_UINT32(start, heap.freeBytes());
  TEST_ASSERT_TRUE(heap.live.empty());
}

// Asking for more than there is stops at the floor: the console must still answer.
static void test_floor_is_kept_and_the_fill_ends() {
  FakeHeap heap = unitA();
  Ballast b;
  const size_t held = b.fill(1000000, 4096, heap.ops());
  TEST_ASSERT_TRUE(heap.freeBytes() >= 4096);
  TEST_ASSERT_TRUE(held > 0);
  TEST_ASSERT_TRUE(b.chunks() <= Ballast::kMaxChunks);
  b.release(heap.ops());
}

// A heap of many small blocks: the chunk cap bounds the work and the bookkeeping.
static void test_chunk_cap_bounds_the_fill() {
  FakeHeap heap{std::vector<size_t>(100, 500)};
  Ballast b;
  TEST_ASSERT_EQUAL_UINT32(Ballast::kMaxChunks * 500, b.fill(40000, 0, heap.ops()));
  TEST_ASSERT_EQUAL_INT(Ballast::kMaxChunks, b.chunks());
  b.release(heap.ops());
  TEST_ASSERT_EQUAL_UINT32(100 * 500, heap.freeBytes());
}

// The largest block went to another task between the size check and the
// allocation: half of it is taken instead of giving up.
static void test_a_moved_block_falls_back_to_half() {
  FakeHeap heap{{20000}};
  heap.failExactLargest = true;
  Ballast b;
  // The 20000 B block fails once: 10000 is taken, then the other 10000.
  TEST_ASSERT_EQUAL_UINT32(20000, b.fill(25000, 0, heap.ops()));
  TEST_ASSERT_EQUAL_INT(2, b.chunks());
  TEST_ASSERT_EQUAL_UINT32(0, heap.freeBytes());
  b.release(heap.ops());
}

// Tiny fragments are not chased; a small remainder of the target is taken exactly.
static void test_tiny_fragments_and_exact_remainders() {
  FakeHeap crumbs{{30, 30, 30}};
  Ballast b;
  TEST_ASSERT_EQUAL_UINT32(0, b.fill(80, 0, crumbs.ops()));
  FakeHeap one{{1000}};
  TEST_ASSERT_EQUAL_UINT32(10, b.fill(10, 0, one.ops()));
  b.release(one.ops());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_target_past_the_largest_block_reaches_the_gate);
  RUN_TEST(test_refill_replaces_and_zero_releases);
  RUN_TEST(test_floor_is_kept_and_the_fill_ends);
  RUN_TEST(test_chunk_cap_bounds_the_fill);
  RUN_TEST(test_a_moved_block_falls_back_to_half);
  RUN_TEST(test_tiny_fragments_and_exact_remainders);
  return UNITY_END();
}
