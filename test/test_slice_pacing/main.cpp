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

using agent::kPaceDeadlineShare;
using agent::kRequestPaceMaxMs;
using agent::kSliceHeadroom;
using agent::kSliceStallMs;
using agent::kSliceWaitMaxMs;
using agent::kSliceWaitStepMs;
using agent::SliceWait;

void setUp() {}
void tearDown() {}

using Heap = std::function<uint32_t(uint32_t slice, uint32_t waitedMs)>;
// FREE memory over the same fake clock; the default heap keeps it constant (the
// old suites' shape: only the largest block tells the story).
using FreeHeap = std::function<uint32_t(uint32_t slice, uint32_t waitedMs)>;

// One slice's wait through the helper the device runs (agent::paceSlice), on a fake
// clock that only the helper's sleep advances; returns the ms it waited and checks the
// helper slept exactly that long and added it to the exchange's total.
static uint32_t paceOne(const Heap& heap, uint32_t slice, uint32_t requestPacedMs,
                        const FreeHeap& free8 = [](uint32_t, uint32_t) { return 40000u; },
                        uint32_t capMs = kRequestPaceMaxMs, uint32_t floorMs = agent::kSliceHeadroom) {
  uint32_t clock = 0, steps = 0, paced = requestPacedMs;
  auto largest = [&] { return heap(slice, clock); };
  auto freeNow = [&] { return free8(slice, clock); };
  auto sleep = [&](uint32_t ms) {
    TEST_ASSERT_EQUAL_UINT32(kSliceWaitStepMs, ms);
    clock += ms;
    TEST_ASSERT_TRUE_MESSAGE(++steps <= kSliceWaitMaxMs / kSliceWaitStepMs, "a slice waited past its bound");
  };
  const uint32_t w = agent::paceSlice(paced, capMs, floorMs,
                                     agent::SliceIo{largest, freeNow, sleep, [] { return false; }});
  TEST_ASSERT_EQUAL_UINT32(clock, w);
  TEST_ASSERT_EQUAL_UINT32(requestPacedMs + w, paced);
  return w;
}

// A whole request body of `bytes`: the waits of every slice after the first.
static std::vector<uint32_t> paceRequest(const Heap& heap, uint32_t bytes) {
  std::vector<uint32_t> waits;
  uint32_t paced = 0;
  for (uint32_t off = 4096, slice = 1; off < bytes; off += 4096, ++slice) {
    const uint32_t w = paceOne(heap, slice, paced);
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
    TEST_ASSERT_EQUAL_UINT32(kSliceStallMs, paceOne(heap, 1, 0));
  }
}

static void test_stable_fragmented_chat_request_stays_under_the_cap() {
  const Heap lumi = [](uint32_t, uint32_t) { return 7156u; };
  // Lumi's chat requests on v4.6.0 (70,198 B paced 25,500 ms; 84,439 B paced 30,000 ms),
  // the largest it sent (185,517 B, paced 67,500 ms) and a 600 KB body.
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
  TEST_ASSERT_EQUAL_UINT32(kSliceStallMs, paceOne(jitter, 1, 0));
}

static void test_a_draining_free_keeps_the_wait_alive() {
  // The photo-DMA regression class (bench unit L, the 21.5 KB leg): a fragmented
  // heap whose LARGEST block never moves at all (7,156 B all through the send)
  // while the acknowledged segments return their pbufs and FREE climbs - the
  // drain is real and the wait must follow it, or the next slice's AES chunks
  // fail; the largest-only stall rule read exactly this heap as "waiting helps
  // nothing" and streamed the rest of the body into the failures.
  const Heap still = [](uint32_t, uint32_t) { return 7156u; };
  const FreeHeap drain = [](uint32_t, uint32_t t) { return 12000u + t / kSliceWaitStepMs * 512u; };
  // Grows one 512 B step per step forever: the slice waits to its own bound.
  TEST_ASSERT_EQUAL_UINT32(kSliceWaitMaxMs, paceOne(still, 1, 0, drain));
  // A drain that stops (free holds): one stall window after the last growth.
  const FreeHeap stops = [](uint32_t, uint32_t t) { return t < 300 ? 12000u + t / kSliceWaitStepMs * 512u : 13560u; };
  TEST_ASSERT_EQUAL_UINT32(290 + kSliceStallMs, paceOne(still, 1, 0, stops));   // the last growth step is t=290
  // A drain that frees the LARGEST block into room ends the wait exactly there
  // (the same rule as a largest-only drain).
  const Heap recovers = [](uint32_t, uint32_t t) { return t < 80 ? 7156u : 14324u; };
  TEST_ASSERT_EQUAL_UINT32(80, paceOne(recovers, 1, 0, drain));
}

// ---- draining: wait while it grows, until room or a bound -----------------------------

static void test_draining_heap_waits_until_it_has_room() {
  // Acknowledged segments free a buffer every 20 ms: 7 KB grows 1,460 B a step pair and
  // has room again after 80 ms. The wait ends exactly there, not at a bound.
  const Heap drain = [](uint32_t, uint32_t t) { return 7168u + (t / 20) * 1460u; };
  const uint32_t w = paceOne(drain, 1, 0);
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
    TEST_ASSERT_EQUAL_UINT32(at, paceOne(ack, 1, 0));
  }
  // A step later than the window is not waited for: the slice goes out at the window.
  const Heap late = [](uint32_t, uint32_t t) { return t < kSliceStallMs + 10 ? 9204u : 14324u; };
  TEST_ASSERT_EQUAL_UINT32(kSliceStallMs, paceOne(late, 1, 0));
}

static void test_growth_within_the_stall_window_keeps_waiting() {
  // A slow drain whose steps come just inside the stall window still reaches room.
  const uint32_t gap = kSliceStallMs - kSliceWaitStepMs;
  const Heap slow = [gap](uint32_t, uint32_t t) { return 7168u + (t / gap) * 1024u; };
  const uint32_t w = paceOne(slow, 1, 0);
  TEST_ASSERT_TRUE(slow(1, w) >= kSliceHeadroom);
  TEST_ASSERT_TRUE(w > kSliceStallMs);
}

static void test_growth_at_the_window_edge_counts() {
  // A new high that lands exactly one window after the baseline, still short of room,
  // restarts the window: the slice waits one more window after it.
  const Heap edge = [](uint32_t, uint32_t t) { return t < kSliceStallMs ? 7168u : 8192u; };
  TEST_ASSERT_EQUAL_UINT32(2 * kSliceStallMs, paceOne(edge, 1, 0));
}

static void test_a_drain_that_stops_short_stops_the_wait() {
  // Grows for 100 ms, then rests below the headroom: the wait ends one stall window
  // after the last growth.
  const Heap stalls = [](uint32_t, uint32_t t) { return 7168u + (t < 100 ? t : 100u) * 20u; };
  TEST_ASSERT_EQUAL_UINT32(100 + kSliceStallMs, paceOne(stalls, 1, 0));
}

static void test_a_slice_never_waits_past_its_bound() {
  // Grows by one byte every step and never reaches the headroom: the slice bound ends it.
  const Heap creep = [](uint32_t, uint32_t t) { return 7168u + t; };
  TEST_ASSERT_EQUAL_UINT32(kSliceWaitMaxMs, paceOne(creep, 1, 0));
}

static void test_the_request_cap_holds_against_a_creeping_heap() {
  // Every slice starts short and creeps upward the whole time it waits: without the cap
  // a 600 KB body would wait 1.5 s on each of 149 slices.
  const Heap creep = [](uint32_t, uint32_t t) { return 7168u + t; };
  const std::vector<uint32_t> waits = paceRequest(creep, 600 * 1024);
  TEST_ASSERT_EQUAL_UINT32(kRequestPaceMaxMs, total(waits));
  // Once the cap is spent every later slice goes out at once.
  TEST_ASSERT_EQUAL_UINT32(0, waits.back());
  // A request that already paid its cap waits nothing more, whatever the heap does.
  TEST_ASSERT_EQUAL_UINT32(0, paceOne(creep, 9, kRequestPaceMaxMs));
  TEST_ASSERT_EQUAL_UINT32(0, paceOne(creep, 9, kRequestPaceMaxMs + 500));
  // The last slice before the cap is clipped to what is left of it.
  TEST_ASSERT_EQUAL_UINT32(30, paceOne(creep, 9, kRequestPaceMaxMs - 30));
}

static void test_the_cap_leaves_a_measured_upload_its_drain() {
  // The counter-test for the cap: a 130 KB photo upload (31 slices after the first)
  // whose every slice drains one round trip later. Up to 60 ms a slice (the measured
  // median is 40 ms) every slice waits for its step under the cap; the measured uploads
  // paced 1,105 to 1,363 ms in all and none reached it.
  for (uint32_t rtt : {40u, 60u}) {
    const Heap upload = [rtt](uint32_t, uint32_t t) { return t < rtt ? 9204u : 14324u; };
    const std::vector<uint32_t> waits = paceRequest(upload, 130435);
    TEST_ASSERT_EQUAL_UINT32(31, waits.size());
    for (uint32_t w : waits) TEST_ASSERT_EQUAL_UINT32(rtt, w);
    TEST_ASSERT_TRUE(total(waits) < kRequestPaceMaxMs);
  }
  // The trade the cap makes: at 70 ms on every slice the first 28 slices get their step
  // and the tail goes out at once, as every slice did before v4.6.0.
  const Heap slow = [](uint32_t, uint32_t t) { return t < 70 ? 9204u : 14324u; };
  const std::vector<uint32_t> waits = paceRequest(slow, 130435);
  for (size_t i = 0; i < 28; ++i) TEST_ASSERT_EQUAL_UINT32(70, waits[i]);
  TEST_ASSERT_EQUAL_UINT32(kRequestPaceMaxMs, total(waits));
  TEST_ASSERT_EQUAL_UINT32(0, waits.back());
  // A 0.6 MB inline body draining at the median on every slice: the first 50 slices get
  // their step, the other 99 go out at once (the trade the http.h comment names).
  const Heap inlineBody = [](uint32_t, uint32_t t) { return t < 40 ? 9204u : 14324u; };
  const std::vector<uint32_t> big = paceRequest(inlineBody, 600 * 1024);
  TEST_ASSERT_EQUAL_UINT32(149, big.size());
  for (size_t i = 0; i < big.size(); ++i) TEST_ASSERT_EQUAL_UINT32(i < 50 ? 40 : 0, big[i]);
}

static void test_each_slice_is_judged_on_its_own() {
  // Stable for three slices, then draining: the first three pay the window, the rest
  // wait for their step; a stall in one slice never shortens the next.
  const Heap mixed = [](uint32_t slice, uint32_t t) {
    if (slice <= 3) return 7156u;
    return t < 40 ? 9204u : 14324u;
  };
  const std::vector<uint32_t> waits = paceRequest(mixed, 6 * 4096 + 1);
  const std::vector<uint32_t> want = {kSliceStallMs, kSliceStallMs, kSliceStallMs, 40, 40, 40};
  TEST_ASSERT_EQUAL_UINT32(want.size(), waits.size());
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_UINT32(want[i], waits[i]);
}

static void test_bounds_hold_on_any_heap() {
  // The class: pseudo-random heaps (rest, jumps, creeps, dips, room) over whole
  // requests. Every wait is whole steps, no slice passes its bound, no request passes
  // the cap, a slice that starts with room never waits, and a wait that ends without
  // room, the slice bound or the cap ends exactly one window after the last new high.
  uint32_t seed = 12345;
  auto rnd = [&seed]() {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) & 0xFFFFu;
  };
  for (int run = 0; run < 400; ++run) {
    const uint32_t base = 2000 + rnd() % 14000, jumpAt = rnd() % 400, jumpTo = rnd() % 16000;
    const uint32_t creep = rnd() % 4, dip = rnd() % 2;
    const Heap heap = [=](uint32_t slice, uint32_t t) {
      uint32_t l = (base + slice * 97) % 16000 + creep * t;
      if (t >= jumpAt) l = jumpTo > l ? jumpTo : l;
      if (dip && (t / kSliceWaitStepMs) % 3 == 1) l = l > 300 ? l - 300 : 0;
      return l;
    };
    uint32_t paced = 0;
    for (uint32_t slice = 1; slice < 40; ++slice) {
      SliceWait w;
      uint32_t best = 0, lastHigh = 0, last = 0;
      for (;;) {
        last = heap(slice, w.waitedMs);
        if (last > best) {
          best = last;
          lastHigh = w.waitedMs;
        }
        if (!w.next(last, 40000u, paced, kRequestPaceMaxMs)) break;
      }
      TEST_ASSERT_EQUAL_UINT32(0, w.waitedMs % kSliceWaitStepMs);
      TEST_ASSERT_TRUE(w.waitedMs <= kSliceWaitMaxMs);
      if (heap(slice, 0) >= kSliceHeadroom) TEST_ASSERT_EQUAL_UINT32(0, w.waitedMs);
      const bool room = last >= kSliceHeadroom, bound = w.waitedMs >= kSliceWaitMaxMs;
      const bool capped = paced + w.waitedMs >= kRequestPaceMaxMs;
      if (!room && !bound && !capped) TEST_ASSERT_EQUAL_UINT32(lastHigh + kSliceStallMs, w.waitedMs);
      paced += w.waitedMs;
      TEST_ASSERT_TRUE(paced <= kRequestPaceMaxMs);
    }
  }
}

static void test_the_deadline_ends_the_wait_and_counts_only_slept_steps() {
  // The exchange's deadline passes during a wait: the helper stops at once and the
  // total holds only the steps it slept (expired() is asked before next()).
  uint32_t paced = 300, clock = 0;
  auto largest = [] { return 7156u; };
  auto freeNow = [] { return 40000u; };
  auto sleep = [&](uint32_t ms) { clock += ms; };
  auto expired = [&] { return clock >= 30; };
  TEST_ASSERT_EQUAL_UINT32(30, agent::paceSlice(paced, kRequestPaceMaxMs, agent::kSliceHeadroom,
                                                 agent::SliceIo{largest, freeNow, sleep, expired}));
  TEST_ASSERT_EQUAL_UINT32(330, paced);
  // Already past the deadline: no wait, nothing added.
  TEST_ASSERT_EQUAL_UINT32(0, agent::paceSlice(paced, kRequestPaceMaxMs, agent::kSliceHeadroom,
                                               agent::SliceIo{largest, freeNow, sleep, [] { return true; }}));
  TEST_ASSERT_EQUAL_UINT32(330, paced);
}

static void test_pacing_never_eats_the_deadline() {
  // The cap is 2 s and never more than a tenth of the exchange's deadline: Lumi's v4.6.0
  // photo request paced 58,280 ms of its 60,000 and got no answer.
  TEST_ASSERT_EQUAL_UINT32(kRequestPaceMaxMs, agent::paceCapMs(120000));   // a head round
  TEST_ASSERT_EQUAL_UINT32(kRequestPaceMaxMs, agent::paceCapMs(60000));    // a vision caption
  TEST_ASSERT_EQUAL_UINT32(kRequestPaceMaxMs, agent::paceCapMs(30000));    // the default
  TEST_ASSERT_EQUAL_UINT32(1000, agent::paceCapMs(10000));
  TEST_ASSERT_EQUAL_UINT32(800, agent::paceCapMs(8090));   // whole steps, never 809
  TEST_ASSERT_EQUAL_UINT32(0, agent::paceCapMs(5));
  // The class: for every deadline (a prime step, so caps off the 10 ms grid are hit), a
  // whole request on a heap that never recovers (a 245 KB photo body, Lumi's largest)
  // paces at most a tenth of it, in whole steps.
  const Heap stuck = [](uint32_t, uint32_t) { return 7156u; };
  for (uint32_t timeout = 0; timeout <= 130000; timeout += 997) {
    const uint32_t cap = agent::paceCapMs(timeout);
    TEST_ASSERT_TRUE(cap * kPaceDeadlineShare <= timeout);
    TEST_ASSERT_EQUAL_UINT32(0, cap % kSliceWaitStepMs);
    uint32_t paced = 0, clock = 0;
    for (uint32_t off = 4096; off < 245 * 1024; off += 4096)
      agent::paceSlice(paced, cap, agent::kSliceHeadroom,
                       agent::SliceIo{[&] { return stuck(0, clock); }, [] { return 40000u; },
                                      [&](uint32_t ms) { clock += ms; }, [] { return false; }});
    TEST_ASSERT_EQUAL_UINT32(cap, paced);   // the stuck heap spends the whole cap, never more
    TEST_ASSERT_TRUE(paced * kPaceDeadlineShare <= timeout);
    TEST_ASSERT_EQUAL_UINT32(clock, paced);
  }
}

// ---- the DMA pool (the photo-DMA regression's floor) ----------------------------

static void test_the_dma_pool_floor_waits_for_the_slice_drain() {
  // The class on the pool the chunks actually allocate from (bench unit L, the
  // 21.5 KB leg): the DMA pool rests at 7,668 B largest (above the 6,144 B
  // floor, so a fresh slice never waits), dips while a slice's pbufs are still
  // queued, and comes back as they are acknowledged - the wait follows the
  // drain in the pool the send spends from. The old rule watched the INTERNAL
  // reading, carried by the ~7.6 KB of free internal memory DMA cannot reach,
  // and read "nothing is growing" while the DMA pool starved the AES chunks.
  const uint32_t floor = agent::kDmaChunkFloor;
  TEST_ASSERT_TRUE(floor < 7668u);   // the 21.5 KB leg's resting largest clears it
  TEST_ASSERT_TRUE(floor > 2 * 1600u + 1024u);   // two AES chunks and a pbuf margin
  const Heap rest = [](uint32_t, uint32_t) { return 7668u; };
  TEST_ASSERT_EQUAL_UINT32(0, paceOne(rest, 1, 0, [](uint32_t, uint32_t) { return 11408u; },
                                      kRequestPaceMaxMs, floor));
  const Heap dips = [](uint32_t, uint32_t t) { return t < 60 ? 3500u : 7668u; };
  TEST_ASSERT_EQUAL_UINT32(60, paceOne(dips, 1, 0, [](uint32_t, uint32_t t) { return 11000u + t; },
                                       kRequestPaceMaxMs, floor));
  // Paced requests still cap: a heap that never recovers spends the body-sized
  // cap and no more, then streams (typed by the trace).
  const Heap stuck = [](uint32_t, uint32_t) { return 3500u; };
  uint32_t paced = 0, clock = 0;
  const uint32_t cap = agent::paceCapForBody(120000, 280550);
  for (uint32_t off = 4096; off < 280550; off += 4096)
    agent::paceSlice(paced, cap, floor,
                     agent::SliceIo{[&] { return stuck(0, clock); }, [&] { return 11000u + clock; },
                                    [&](uint32_t ms) { clock += ms; }, [] { return false; }});
  TEST_ASSERT_EQUAL_UINT32(cap, paced);
}

static void test_a_body_sized_request_gets_a_body_sized_cap() {  // The photo-DMA regression class: a 280 KB photo body (69 slices after the first)
  // at a deadline that allows it (120 s) needs a drain window per slice, so the
  // flat 2 s cap grows with the body - but the deadline's tenth stays the ceiling,
  // and a small deadline keeps the original min(share, 2 s) cap.
  TEST_ASSERT_EQUAL_UINT32(2000 + 69 * 100, agent::paceCapForBody(120000, 280550));
  TEST_ASSERT_EQUAL_UINT32(6000, agent::paceCapForBody(60000, 280550));    // the share binds
  TEST_ASSERT_EQUAL_UINT32(1000, agent::paceCapForBody(10000, 280550));    // a small deadline: untouched
  TEST_ASSERT_EQUAL_UINT32(0, agent::paceCapForBody(5, 280550));
  TEST_ASSERT_EQUAL_UINT32(agent::paceCapMs(120000), agent::paceCapForBody(120000, 0));   // no body: the flat cap
  TEST_ASSERT_EQUAL_UINT32(2000 + 18 * 100, agent::paceCapForBody(120000, 70198));        // a 70 KB request (18 slices)
  for (uint32_t timeout = 0; timeout <= 130000; timeout += 997) {
    const uint32_t cap = agent::paceCapForBody(timeout, 600 * 1024);
    TEST_ASSERT_TRUE(cap * kPaceDeadlineShare <= timeout);   // never eats the deadline
    TEST_ASSERT_EQUAL_UINT32(0, cap % kSliceWaitStepMs);
    TEST_ASSERT_TRUE(cap >= agent::paceCapForBody(timeout, 0));
  }
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
  TEST_ASSERT_EQUAL_UINT32(0, kSliceStallMs % kSliceWaitStepMs);
  TEST_ASSERT_EQUAL_UINT32(0, kRequestPaceMaxMs % kSliceWaitStepMs);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_room_never_waits);
  RUN_TEST(test_stable_fragmented_heap_waits_only_the_stall_window);
  RUN_TEST(test_stable_fragmented_chat_request_stays_under_the_cap);
  RUN_TEST(test_a_jittering_heap_does_not_keep_the_wait_alive);
  RUN_TEST(test_a_draining_free_keeps_the_wait_alive);
  RUN_TEST(test_the_dma_pool_floor_waits_for_the_slice_drain);
  RUN_TEST(test_draining_heap_waits_until_it_has_room);
  RUN_TEST(test_one_step_recovery_within_the_window_is_waited_for);
  RUN_TEST(test_growth_within_the_stall_window_keeps_waiting);
  RUN_TEST(test_growth_at_the_window_edge_counts);
  RUN_TEST(test_a_drain_that_stops_short_stops_the_wait);
  RUN_TEST(test_a_slice_never_waits_past_its_bound);
  RUN_TEST(test_the_request_cap_holds_against_a_creeping_heap);
  RUN_TEST(test_the_cap_leaves_a_measured_upload_its_drain);
  RUN_TEST(test_each_slice_is_judged_on_its_own);
  RUN_TEST(test_bounds_hold_on_any_heap);
  RUN_TEST(test_the_deadline_ends_the_wait_and_counts_only_slept_steps);
  RUN_TEST(test_pacing_never_eats_the_deadline);
  RUN_TEST(test_a_body_sized_request_gets_a_body_sized_cap);
  RUN_TEST(test_the_bounds_are_ordered);
  return UNITY_END();
}
