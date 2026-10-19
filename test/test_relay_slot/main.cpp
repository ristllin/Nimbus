#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "nimbus/cloud/relay_slot.h"

using namespace nimbus::cloud;

void setUp() {}
void tearDown() {}

// ---- a simulated device: one work slot, a turn task, the relay ----------------------
//
// Single-threaded and deterministic on a fake clock. The turn task wants the slot at
// scheduled times and holds it for an exchange; it outranks the relay (tg_poll runs at
// a higher priority on the same core), so whenever the slot is released while a turn
// waits, the turn gets it. The relay drives the real policy templates against it.
struct Sim {
  struct TurnReq { uint32_t wantAt, holdMs; uint32_t gotAt = UINT32_MAX; };
  struct Span { uint32_t a, b; };
  enum Holder { None, Relay, Turn };

  uint32_t now = 0;
  Holder holder = None;
  uint32_t turnEnd = 0;
  std::vector<TurnReq> turns;   // sorted by wantAt
  size_t next = 0;
  std::vector<Span> turnHolds, bulkTx;
  uint32_t bytesPerSec = 200000;   // the relay's link speed
  uint32_t maxRelayHold = 0, holdStart = 0;
  std::vector<uint32_t> pumps;
  int writesWithoutSlot = 0;
  uint32_t lastTx = 0;   // start time of the most recent write (any frame)

  bool turnWaiting() const { return next < turns.size() && turns[next].wantAt <= now; }
  // At `now`: end a finished turn hold, then hand a free slot to a waiting turn.
  void settle() {
    for (;;) {
      if (holder == Turn && turnEnd <= now) holder = None;
      if (holder != None || !turnWaiting()) return;
      TurnReq& t = turns[next++];
      t.gotAt = now;
      holder = Turn;
      turnEnd = now + t.holdMs;
      turnHolds.push_back({now, turnEnd});
    }
  }
  // The next moment the slot can change hands (UINT32_MAX: none pending).
  uint32_t nextEvent() const {
    if (holder == Turn) return turnEnd;
    if (holder == None && next < turns.size() && turns[next].wantAt > now) return turns[next].wantAt;
    return UINT32_MAX;
  }
  void advanceTo(uint32_t t) {
    settle();
    for (uint32_t e = nextEvent(); e != UINT32_MAX && e <= t; e = nextEvent()) {
      now = e;
      settle();
    }
    if (t > now) now = t;
    settle();
  }
  bool grab() {
    settle();
    if (holder != None || turnWaiting()) return false;
    holder = Relay;
    holdStart = now;
    return true;
  }
  // The relay's acquireWork(ms).
  bool take(uint32_t ms) {
    const uint32_t until = now + ms;
    for (;;) {
      if (grab()) return true;
      const uint32_t e = nextEvent();
      if (e == UINT32_MAX || e > until) {
        advanceTo(until);
        return grab();
      }
      advanceTo(e);
    }
  }
  void release() {
    if (holder != Relay) return;
    maxRelayHold = std::max(maxRelayHold, now - holdStart);
    holder = None;
    settle();
  }
  // Time passes while the relay writes; a turn wanting the slot meanwhile waits.
  bool write(size_t n, bool bulk) {
    if (bulk && holder != Relay) ++writesWithoutSlot;
    const uint32_t dur = (uint32_t)((uint64_t)n * 1000 / bytesPerSec) + 1;
    lastTx = now;
    if (bulk) bulkTx.push_back({now, now + dur});
    advanceTo(now + dur);
    return true;
  }
};

struct Outcome {
  bool small = false;
  SlotWait wait = SlotWait::Busy;
  BulkResult bulk = BulkResult::Failed;
  uint32_t answeredAt = 0;   // when the res frame (or the 503) finished
};

// One tunneled request through the real templates: loopback (no slot), then the
// frame; small frames go straight out, big ones wait (pumping) and write per chunk.
static Outcome serve(Sim& s, uint32_t reqAt, uint32_t loopbackMs, size_t frameBytes) {
  Outcome o;
  s.advanceTo(reqAt + loopbackMs);   // the slot is never held across the loopback
  if (!frameNeedsSlot(frameBytes)) {
    o.small = true;
    s.write(frameBytes, false);
    o.answeredAt = s.now;
    return o;
  }
  s.pumps.clear();
  s.pumps.push_back(s.now);
  o.wait = waitForSlot(
      reqAt + kRelayBusyAfterMs, [&] { return s.now; }, [&](uint32_t ms) { return s.take(ms); },
      [&] {
        s.pumps.push_back(s.now);
        return true;
      });
  if (o.wait == SlotWait::Busy) {
    s.write(64, false);   // the small 503 answer, no slot
    o.answeredAt = s.now;
    return o;
  }
  o.bulk = writeBulk(
      frameBytes, reqAt + kRelayResBudgetMs,
      [&](uint32_t deadline) {
        while ((int32_t)(deadline - s.now) > 0)
          if (s.take(std::min<uint32_t>(kRelaySlotTakeMs, deadline - s.now))) return true;
        return false;
      },
      [&] { s.release(); }, [&](size_t, size_t n) { return s.write(n, true); }, [&] {});
  o.answeredAt = s.now;
  return o;
}

static bool overlaps(const Sim::Span& x, const Sim::Span& y) { return x.a < y.b && y.a < x.b; }

// ---- policy ---------------------------------------------------------------------

static void test_policy_which_frames_take_the_slot() {
  // Heartbeat, hello, pong, acks and the dashboard's polls (20 B to 6,384 B) never do.
  for (size_t n : {(size_t)2, (size_t)20, (size_t)180, (size_t)6384, kRelayBulkBytes})
    TEST_ASSERT_FALSE(frameNeedsSlot(n));
  for (size_t n : {kRelayBulkBytes + 1, (size_t)65536, (size_t)523055})
    TEST_ASSERT_TRUE(frameNeedsSlot(n));
  TEST_ASSERT_TRUE(kRelayBusyAfterMs < kRelayResBudgetMs);
  TEST_ASSERT_TRUE(kRelayResBudgetMs < kRelayCloudTimeoutMs);
  TEST_ASSERT_FALSE(firstDialReady(false, 0));
  TEST_ASSERT_FALSE(firstDialReady(false, kRelayFirstDialWaitMs - 1));
  TEST_ASSERT_TRUE(firstDialReady(false, kRelayFirstDialWaitMs));
  TEST_ASSERT_TRUE(firstDialReady(true, 0));
}

// The pre-frame wait pumps at least every 200 ms and answers busy before 30 s when a
// turn holds the slot for the whole window (a 120 s exchange).
static void test_wait_pumps_every_200ms_and_answers_busy_before_30s() {
  Sim s;
  s.turns.push_back({0, 120000});
  s.advanceTo(1);
  const Outcome o = serve(s, 1000, 300, 375598 * 4 / 3 + 200);
  TEST_ASSERT_EQUAL((int)SlotWait::Busy, (int)o.wait);
  TEST_ASSERT_TRUE(o.answeredAt < 1000 + kRelayCloudTimeoutMs);
  TEST_ASSERT_TRUE(o.answeredAt >= 1000 + kRelayBusyAfterMs);
  for (size_t i = 1; i < s.pumps.size(); ++i)
    TEST_ASSERT_TRUE_MESSAGE(s.pumps[i] - s.pumps[i - 1] <= kRelaySlotTakeMs, "a pump gap over 200 ms");
  TEST_ASSERT_TRUE(s.pumps.size() >= kRelayBusyAfterMs / kRelaySlotTakeMs - 3);
  TEST_ASSERT_TRUE(s.bulkTx.empty());   // only the small 503 went out
}

// Counter-test: a small poll is never delayed, even while a turn holds the slot.
static void test_small_polls_never_wait_for_the_slot() {
  for (size_t n : {(size_t)20, (size_t)6384, kRelayBulkBytes}) {
    Sim s;
    s.turns.push_back({0, 60000});
    s.advanceTo(10);
    const Outcome o = serve(s, 5000, 5, n);
    TEST_ASSERT_TRUE(o.small);
    TEST_ASSERT_EQUAL_UINT32(5005, s.lastTx);   // straight out after the loopback
  }
}

// Counter-test: the longest single hold of a maximal frame (523,055 B, the 383 KiB cap)
// stays under the turn's 10 s acquire, even on a link far slower than the field's
// (the 375 KB page took 2.7 s uncontended): one chunk at 1 KB/s is 8.2 s.
static void test_maximal_frame_hold_stays_under_the_turn_acquire() {
  for (uint32_t bps : {1000u, 40000u, 200000u}) {
    Sim s;
    s.bytesPerSec = bps;
    const Outcome o = serve(s, 0, 100, 523055);
    TEST_ASSERT_EQUAL((int)SlotWait::Acquired, (int)o.wait);
    TEST_ASSERT_TRUE(s.maxRelayHold < kTurnSlotAcquireMs);
  }
  TEST_ASSERT_TRUE((uint64_t)kRelayBulkBytes * 1000 / 1000 < kTurnSlotAcquireMs);
}

// Property over random interleavings (fake clock): turns wanting the slot at random
// times for holds of 0-120 s, relay requests of random sizes. The relay never writes a
// big frame without the slot, its bulk writes never overlap a turn's hold, no turn
// ever waits as long as its 10 s acquire, every busy answer lands before 30 s, and a
// completed page lands inside the 28 s budget (a stall past it is reported, never
// silently over time).
static void test_property_relay_bulk_never_overlaps_a_turn() {
  std::mt19937 rng(20261018);
  int busy = 0, sent = 0, stalled = 0;
  for (int run = 0; run < 300; ++run) {
    Sim s;
    s.bytesPerSec = 20000 + rng() % 400000;
    // Turn exchanges come from one task (tg_poll), so they never overlap each other:
    // each wants the slot some time after the previous one ended.
    uint32_t t = 0;
    const int nTurns = 1 + (int)(rng() % 6);
    for (int i = 0; i < nTurns; ++i) {
      t += rng() % 40000;
      const uint32_t hold = (uint32_t)(rng() % 120001);
      s.turns.push_back({t, hold});
      t += hold + 11000;   // after the previous exchange plus its worst slot wait
    }
    std::vector<Outcome> outs;
    uint32_t reqAt = rng() % 30000;
    for (int i = 0; i < 4; ++i) {
      const size_t frame = (rng() % 3 == 0) ? (size_t)(rng() % 8000) : (size_t)(9000 + rng() % 514000);
      const uint32_t at = std::max(reqAt, s.now);
      const Outcome o = serve(s, at, rng() % 3000, frame);
      if (!o.small) {
        if (o.wait == SlotWait::Busy) {
          ++busy;
          TEST_ASSERT_TRUE(o.answeredAt < at + kRelayCloudTimeoutMs);
        } else if (o.bulk == BulkResult::Sent) {
          ++sent;
          TEST_ASSERT_TRUE(o.answeredAt <= at + kRelayResBudgetMs + 600);   // + one chunk past the check
        } else {
          ++stalled;
        }
      }
      reqAt = s.now + rng() % 20000;
    }
    s.advanceTo(std::max(s.now, s.turns.back().wantAt + s.turns.back().holdMs) + 15000);   // all turns done
    TEST_ASSERT_EQUAL_MESSAGE(0, s.writesWithoutSlot, "a big frame went out without the slot");
    for (const Sim::Span& tx : s.bulkTx)
      for (const Sim::Span& h : s.turnHolds)
        TEST_ASSERT_FALSE_MESSAGE(overlaps(tx, h), "relay bulk TX overlapped a turn exchange");
    for (const Sim::TurnReq& tr : s.turns) {
      TEST_ASSERT_TRUE(tr.gotAt != UINT32_MAX);
      TEST_ASSERT_TRUE_MESSAGE(tr.gotAt - tr.wantAt < kTurnSlotAcquireMs, "a turn waited 10 s for the slot");
    }
  }
  TEST_ASSERT_TRUE(busy > 0);    // the property exercised every branch
  TEST_ASSERT_TRUE(sent > 0);
  (void)stalled;
}

// Overlap check proper: the turn holds and the relay's big-frame writes, as spans.
static void test_property_spans_are_disjoint() {
  std::mt19937 rng(7);
  for (int run = 0; run < 200; ++run) {
    Sim s;
    s.bytesPerSec = 30000 + rng() % 300000;
    s.turns.push_back({rng() % 5000, rng() % 30000});
    s.turns.push_back({10000 + rng() % 20000, rng() % 60000});
    serve(s, rng() % 8000, 50, 200000 + rng() % 300000);
    s.advanceTo(s.now + 100000);
    for (const Sim::Span& tx : s.bulkTx)
      for (const Sim::Span& h : s.turnHolds)
        TEST_ASSERT_FALSE_MESSAGE(overlaps(tx, h), "relay bulk TX overlapped a turn exchange");
  }
}

// writeBulk returns the slot on every path: a write failure, a stall, an empty frame.
static void test_bulk_always_returns_the_slot() {
  int held = 1, releases = 0, chunks = 0;
  auto release = [&] { --held; ++releases; };
  auto reacquire = [&](uint32_t) { ++held; return true; };
  BulkResult r = writeBulk(
      3 * kRelayBulkBytes + 5, 0, reacquire, release,
      [&](size_t, size_t) { return ++chunks < 2; }, [] {});
  TEST_ASSERT_EQUAL((int)BulkResult::Failed, (int)r);
  TEST_ASSERT_EQUAL(0, held);
  held = 1;
  r = writeBulk(3 * kRelayBulkBytes, 0, [&](uint32_t) { return false; }, release,
                [&](size_t, size_t) { return true; }, [] {});
  TEST_ASSERT_EQUAL((int)BulkResult::Stalled, (int)r);
  TEST_ASSERT_EQUAL(0, held);
  held = 1;
  r = writeBulk(0, 0, reacquire, release, [&](size_t, size_t) { return true; }, [] {});
  TEST_ASSERT_EQUAL((int)BulkResult::Sent, (int)r);
  TEST_ASSERT_EQUAL(0, held);
  // A whole frame: one release per chunk, never a write past the length.
  held = 1;
  size_t last = 0, total = 0;
  r = writeBulk(5 * kRelayBulkBytes + 1, 0, reacquire, release,
                [&](size_t off, size_t n) { TEST_ASSERT_EQUAL(total, off); last = n; total += n; return true; },
                [] {});
  TEST_ASSERT_EQUAL((int)BulkResult::Sent, (int)r);
  TEST_ASSERT_EQUAL((size_t)(5 * kRelayBulkBytes + 1), total);
  TEST_ASSERT_EQUAL(1, (int)last);
  TEST_ASSERT_EQUAL(0, held);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_policy_which_frames_take_the_slot);
  RUN_TEST(test_wait_pumps_every_200ms_and_answers_busy_before_30s);
  RUN_TEST(test_small_polls_never_wait_for_the_slot);
  RUN_TEST(test_maximal_frame_hold_stays_under_the_turn_acquire);
  RUN_TEST(test_property_relay_bulk_never_overlaps_a_turn);
  RUN_TEST(test_property_spans_are_disjoint);
  RUN_TEST(test_bulk_always_returns_the_slot);
  return UNITY_END();
}
