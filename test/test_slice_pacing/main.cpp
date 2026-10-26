#include <unity.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "nimbus/harness/http.h"

// The send side's slice pacing (http.h SliceWait), driven the way the device transport
// drives it (src/agent/transport_tls.cpp paceForHeadroom): before each body slice after
// the first, sample the largest free internal block, ask next(), sleep one step while it
// says so. A heap is a function of the time waited in this slice and of the slice number.
//
// The class under test: a wait is paid only while it helps. A fragmented-but-stable heap
// (Lumi on v4.6.0: 7,156 B all through its first hour) pays the stall window per slice
// and the request cap in all; a draining heap waits until it has room, stops growing,
// or a bound.

using agent::kRequestPaceMaxMs;
using agent::kSliceHeadroom;
using agent::kSliceStallMs;
using agent::kSliceWaitMaxMs;
using agent::kSliceWaitStepMs;
using agent::SliceWait;

void setUp() {}
void tearDown() {}

using Heap = std::function<uint32_t(uint32_t slice, uint32_t waitedMs)>;

// One slice's wait, as paceForHeadroom runs it; returns the ms it waited.
static uint32_t paceSlice(const Heap& heap, uint32_t slice, uint32_t requestPacedMs) {
  SliceWait w;
  uint32_t steps = 0;
  while (w.next(heap(slice, w.waitedMs), requestPacedMs)) {
    ++steps;
    TEST_ASSERT_TRUE_MESSAGE(steps <= kSliceWaitMaxMs / kSliceWaitStepMs, "a slice waited past its bound");
  }
  TEST_ASSERT_EQUAL_UINT32(steps * kSliceWaitStepMs, w.waitedMs);
  return w.waitedMs;
}

// A whole request body of `bytes`: the waits of every slice after the first.
static std::vector<uint32_t> paceRequest(const Heap& heap, uint32_t bytes) {
  std::vector<uint32_t> waits;
  uint32_t paced = 0;
  for (uint32_t off = 4096, slice = 1; off < bytes; off += 4096, ++slice) {
    const uint32_t w = paceSlice(heap, slice, paced);
    waits.push_back(w);
    paced += w;
  }
  return waits;
}

static uint32_t total(const std::vector<uint32_t>& waits) {
  uint32_t t = 0;
  for (uint32_t w : waits) t += w;
  return t;
}

// ---- room: no wait at all -------------------------------------------------------------

static void test_room_never_waits() {
  for (uint32_t largest : {kSliceHeadroom, kSliceHeadroom + 1, 40000u, 172020u}) {
    const Heap heap = [largest](uint32_t, uint32_t) { return largest; };
    TEST_ASSERT_EQUAL_UINT32(0, total(paceRequest(heap, 600 * 1024)));
  }
}

// ---- fragmented but stable: the stall window per slice, the cap per request ------------

static void test_stable_fragmented_heap_waits_only_the_stall_window() {
  // Lumi's resting heap on v4.6.0 and its neighbors, down to an almost empty heap.
  for (uint32_t largest : {7156u, kSliceHeadroom - 1, 4096u, 0u}) {
    const Heap heap = [largest](uint32_t, uint32_t) { return largest; };
    TEST_ASSERT_EQUAL_UINT32(kSliceStallMs, paceSlice(heap, 1, 0));
  }
}

static void test_stable_fragmented_chat_request_stays_under_the_cap() {
  const Heap lumi = [](uint32_t, uint32_t) { return 7156u; };
  // Lumi's model requests on v4.6.0 (70,198 to 84,439 B, paced 24,000 to 30,000 ms each)
  // and the largest it sent (185,517 B, paced 67,500 ms).
  for (uint32_t body : {70198u, 84439u, 185517u, 600u * 1024u}) {
    const std::vector<uint32_t> waits = paceRequest(lumi, body);
    const uint32_t paced = total(waits);
    TEST_ASSERT_TRUE_MESSAGE(paced <= kRequestPaceMaxMs, "a request paid more than its cap");
    for (uint32_t w : waits) TEST_ASSERT_TRUE(w <= kSliceStallMs);
  }
  // A 70 KB chat request: every slice pays only the stall window, well under 3 s.
  const uint32_t slices = (70198 + 4095) / 4096 - 1;
  TEST_ASSERT_EQUAL_UINT32(slices * kSliceStallMs, total(paceRequest(lumi, 70198)));
  TEST_ASSERT_TRUE(slices * kSliceStallMs < 3000);
}

static void test_a_jittering_heap_does_not_keep_the_wait_alive() {
  // Other tasks allocate and free while the transport waits: the block dips and comes
  // back to the same size. Only a new high counts as growth.
  const Heap jitter = [](uint32_t, uint32_t t) { return (t / kSliceWaitStepMs) % 2 ? 6900u : 7156u; };
  TEST_ASSERT_EQUAL_UINT32(kSliceStallMs, paceSlice(jitter, 1, 0));
}

// ---- draining: wait while it grows, until room or a bound -----------------------------

static void test_draining_heap_waits_until_it_has_room() {
  // Acknowledged segments free a buffer every 20 ms: 7 KB grows 1,460 B a step pair and
  // has room again after 80 ms. The wait ends exactly there, not at a bound.
  const Heap drain = [](uint32_t, uint32_t t) { return 7168u + (t / 20) * 1460u; };
  const uint32_t w = paceSlice(drain, 1, 0);
  TEST_ASSERT_EQUAL_UINT32(80, w);
  TEST_ASSERT_TRUE(drain(1, w) >= kSliceHeadroom);
  TEST_ASSERT_TRUE(drain(1, w - kSliceWaitStepMs) < kSliceHeadroom);
}

static void test_one_step_recovery_within_the_window_is_waited_for() {
  // The bench shape (unit L, 26 Oct): the block does not grow at all until the slice's
  // segments are acknowledged, then comes back in one step (9,204 B -> 14,324 B) one round
  // trip later (measured 10 to 110 ms, p99 90). Every step inside the window is waited for
  // and the slice goes out the moment it lands.
  for (uint32_t at = kSliceWaitStepMs; at <= kSliceStallMs; at += kSliceWaitStepMs) {
    const Heap ack = [at](uint32_t, uint32_t t) { return t < at ? 9204u : 14324u; };
    TEST_ASSERT_EQUAL_UINT32(at, paceSlice(ack, 1, 0));
  }
  // A step later than the window is not waited for: the slice goes out at the window.
  const Heap late = [](uint32_t, uint32_t t) { return t < kSliceStallMs + 10 ? 9204u : 14324u; };
  TEST_ASSERT_EQUAL_UINT32(kSliceStallMs, paceSlice(late, 1, 0));
}

static void test_growth_within_the_stall_window_keeps_waiting() {
  // A slow drain whose steps come just inside the stall window still reaches room.
  const uint32_t gap = kSliceStallMs - kSliceWaitStepMs;
  const Heap slow = [gap](uint32_t, uint32_t t) { return 7168u + (t / gap) * 1024u; };
  const uint32_t w = paceSlice(slow, 1, 0);
  TEST_ASSERT_TRUE(slow(1, w) >= kSliceHeadroom);
  TEST_ASSERT_TRUE(w > kSliceStallMs);
}

static void test_a_drain_that_stops_short_stops_the_wait() {
  // Grows for 100 ms, then rests below the headroom: the wait ends one stall window
  // after the last growth.
  const Heap stalls = [](uint32_t, uint32_t t) { return 7168u + (t < 100 ? t : 100u) * 20u; };
  TEST_ASSERT_EQUAL_UINT32(100 + kSliceStallMs, paceSlice(stalls, 1, 0));
}

static void test_a_slice_never_waits_past_its_bound() {
  // Grows by one byte every step and never reaches the headroom: the slice bound ends it.
  const Heap creep = [](uint32_t, uint32_t t) { return 7168u + t; };
  TEST_ASSERT_EQUAL_UINT32(kSliceWaitMaxMs, paceSlice(creep, 1, 0));
}

static void test_the_request_cap_holds_against_a_draining_heap() {
  // Every slice starts short and grows the whole time it waits: without the cap a
  // 600 KB body would wait 1.5 s on each of 149 slices.
  const Heap creep = [](uint32_t, uint32_t t) { return 7168u + t; };
  const std::vector<uint32_t> waits = paceRequest(creep, 600 * 1024);
  TEST_ASSERT_EQUAL_UINT32(kRequestPaceMaxMs, total(waits));
  // Once the cap is spent every later slice goes out at once.
  TEST_ASSERT_EQUAL_UINT32(0, waits.back());
  // A request that already paid its cap waits nothing more, whatever the heap does.
  TEST_ASSERT_EQUAL_UINT32(0, paceSlice(creep, 9, kRequestPaceMaxMs));
  TEST_ASSERT_EQUAL_UINT32(0, paceSlice(creep, 9, kRequestPaceMaxMs + 500));
  // The last slice before the cap is clipped to what is left of it.
  TEST_ASSERT_EQUAL_UINT32(30, paceSlice(creep, 9, kRequestPaceMaxMs - 30));
}

static void test_the_bounds_are_ordered() {
  // The headroom covers one slice plus its TLS record overhead, the stall window is
  // a few steps, and the cap stays a few seconds: the hotfix's promise.
  TEST_ASSERT_TRUE(kSliceHeadroom >= 4096 + 4096);
  TEST_ASSERT_TRUE(kSliceStallMs >= 2 * kSliceWaitStepMs);
  TEST_ASSERT_TRUE(kSliceStallMs <= 100);
  TEST_ASSERT_TRUE(kSliceStallMs < kSliceWaitMaxMs);
  TEST_ASSERT_TRUE(kSliceWaitMaxMs <= kRequestPaceMaxMs);
  TEST_ASSERT_TRUE(kRequestPaceMaxMs <= 3000);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_room_never_waits);
  RUN_TEST(test_stable_fragmented_heap_waits_only_the_stall_window);
  RUN_TEST(test_stable_fragmented_chat_request_stays_under_the_cap);
  RUN_TEST(test_a_jittering_heap_does_not_keep_the_wait_alive);
  RUN_TEST(test_draining_heap_waits_until_it_has_room);
  RUN_TEST(test_one_step_recovery_within_the_window_is_waited_for);
  RUN_TEST(test_growth_within_the_stall_window_keeps_waiting);
  RUN_TEST(test_a_drain_that_stops_short_stops_the_wait);
  RUN_TEST(test_a_slice_never_waits_past_its_bound);
  RUN_TEST(test_the_request_cap_holds_against_a_draining_heap);
  RUN_TEST(test_the_bounds_are_ordered);
  return UNITY_END();
}
