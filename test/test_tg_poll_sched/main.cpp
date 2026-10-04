// Host tests for the tg_poll scheduling rule (lib/core nimbus/net/tg_poll_sched.h,
// CUM-462).
//
// The owner's report: release the mic, see "Transcribing" then "Thinking", and the
// turn may not start for up to ~30 s. The transcript sat in the inject queue while
// the tg_poll task blocked in the Telegram long-poll, which never looked at it.
// These pin the CLASS, not the instance (AGENTS.md section 3):
//   - the per-tick wait decision over EVERY input combination: a waiting local turn
//     cuts an idle long-poll short, and never a response in progress or a Check;
//   - every idle pause ends the moment a local turn is waiting, and otherwise
//     sleeps exactly its length in bounded slices;
//   - the cycle plan over EVERY local-waiting sequence: never two skips running,
//     and the cycle after a skip Checks Telegram;
//   - a millisecond model of the device loop (test/support/tg_poll_sim.h), driven by
//     the real rule: a local message injected at ANY point of the cycle waits on no
//     idle wait beyond one pause slice; a steady stream of local turns still serves
//     every Telegram message, even on a link slower than a second; an idle device
//     does no new TLS churn; a Telegram outage's backoff no longer holds a turn.

#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../support/tg_poll_sim.h"
#include "nimbus/net/tg_poll_sched.h"

using nimbus::net::kTgIdleSliceMs;
using nimbus::net::TgPoll;
using nimbus::net::TgPollScheduler;
using nimbus::net::TgWait;
using nimbus::net::tgIdleSleepMs;
using nimbus::net::tgPollTimeoutS;
using nimbus::net::tgWaitStep;

void setUp() {}
void tearDown() {}

// ---- the per-tick wait decision -------------------------------------------------

// Every combination of the wait's inputs, for a long-poll (18 s and 30 s) and a
// Check (timeout 0).
static void test_wait_step_over_every_input() {
  int yields = 0;
  for (int bits = 0; bits < 16; ++bits) {
    const bool bytes = bits & 1, connected = bits & 2, late = bits & 4, local = bits & 8;
    for (int timeoutS : {0, 18, 30}) {
      const TgWait w = tgWaitStep(bytes, connected, late, local, timeoutS);
      if (bytes) {
        // A response in progress is read whole: no waiting local turn, deadline or
        // socket state ever abandons it mid-response.
        TEST_ASSERT_EQUAL_MESSAGE(int(TgWait::Read), int(w), "abandoned a response in progress");
        continue;
      }
      if (!connected || late) {
        TEST_ASSERT_EQUAL_MESSAGE(int(TgWait::GiveUp), int(w), "lost the failure path");
        continue;
      }
      // Only a long-poll's idle wait yields, and only to a waiting local turn. A
      // Check answers within a round trip, so it is always read.
      const bool expectYield = local && timeoutS > 0;
      TEST_ASSERT_EQUAL_MESSAGE(int(expectYield ? TgWait::Yield : TgWait::Keep), int(w),
                                expectYield ? "an idle long-poll held a waiting local turn"
                                            : "yielded with nothing waiting, or yielded a Check");
      yields += (w == TgWait::Yield);
    }
  }
  TEST_ASSERT_EQUAL(2, yields);
}

// The timeout a plan asks for: a Check always asks Telegram to answer at once; a
// long-poll keeps the loop's own timeout (both the idle and the jobs-running one).
static void test_poll_timeout_per_plan() {
  for (int longPollS : {18, 30}) {
    TEST_ASSERT_EQUAL(0, tgPollTimeoutS(TgPoll::Check, longPollS));
    TEST_ASSERT_EQUAL(longPollS, tgPollTimeoutS(TgPoll::LongPoll, longPollS));
  }
}

// ---- idle pauses ----------------------------------------------------------------

// With nothing waiting a pause sleeps EXACTLY its length (the old cadence), in
// slices no longer than kTgIdleSliceMs, and never asks for a zero sleep early.
static void test_idle_pause_sleeps_its_length_in_slices() {
  for (uint32_t pause : {0u, 1u, 49u, 50u, 51u, 600u, 1000u, 1500u, 12000u}) {
    uint32_t elapsed = 0, slices = 0;
    for (;;) {
      const uint32_t ms = tgIdleSleepMs(false, elapsed, pause);
      if (ms == 0) break;
      TEST_ASSERT_TRUE_MESSAGE(ms <= kTgIdleSliceMs, "a pause slice is longer than the slice");
      elapsed += ms;
      ++slices;
      TEST_ASSERT_TRUE(slices <= pause);   // terminates
    }
    TEST_ASSERT_EQUAL_UINT32(pause, elapsed);
  }
}

// A waiting local turn ends a pause at once, wherever in it the loop looks.
static void test_idle_pause_ends_for_a_waiting_local_turn() {
  for (uint32_t pause : {600u, 1000u, 12000u})
    for (uint32_t elapsed = 0; elapsed <= pause; elapsed += 13)
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, tgIdleSleepMs(true, elapsed, pause),
                                       "a pause held a waiting local turn");
}

// ---- the cycle plan ---------------------------------------------------------------

// Every local-waiting sequence of 12 cycles: nothing waiting is always a LongPoll; a
// waiting local turn Skips unless the previous cycle skipped, in which case Telegram
// gets a Check. So never two skips running.
static void test_plan_over_every_sequence() {
  constexpr int kLen = 12;
  int skips = 0, checks = 0;
  for (uint32_t seq = 0; seq < (1u << kLen); ++seq) {
    TgPollScheduler s;
    bool skippedLast = false;
    for (int i = 0; i < kLen; ++i) {
      const bool local = (seq >> i) & 1u;
      const TgPoll p = s.plan(local);
      const TgPoll expect = !local ? TgPoll::LongPoll : (skippedLast ? TgPoll::Check : TgPoll::Skip);
      TEST_ASSERT_EQUAL_MESSAGE(int(expect), int(p), "plan rule broke");
      TEST_ASSERT_FALSE_MESSAGE(p == TgPoll::Skip && skippedLast, "skipped two cycles running");
      skippedLast = (p == TgPoll::Skip);
      skips += (p == TgPoll::Skip);
      checks += (p == TgPoll::Check);
    }
  }
  TEST_ASSERT_TRUE(skips > 0 && checks > 0);
}

// ---- the device loop, modeled ---------------------------------------------------

// One local message's wait: on no idle wait longer than one pause slice, and in all
// no longer than one poll reconnect plus a slice.
static void assertPrompt(const tgsim::Config& c, const tgsim::Dispatched& d) {
  TEST_ASSERT_TRUE_MESSAGE(d.idleMs <= kTgIdleSliceMs, "a local turn waited on an idle wait");
  TEST_ASSERT_TRUE_MESSAGE(d.latency() <= c.handshakeMs + kTgIdleSliceMs,
                           "a local turn waited longer than a reconnect plus a slice");
}

// The headline: a local message injected at EVERY point of the loop (a prime step so
// every phase is hit) - against up to a whole long-poll before CUM-462. Swept twice:
// across three cycles of an idle device (poll socket open), and across the cycle
// after a local turn (the socket was closed for it, so that cycle reconnects first).
static void test_local_turn_never_waits_on_an_idle_wait() {
  const tgsim::Config c;
  const uint32_t cycleMs = c.longPollS * 1000 + c.pauseMs + c.handshakeMs;
  for (uint32_t at = 5000; at < 5000 + 3 * cycleMs; at += 89) {
    tgsim::Sim sim(c);
    sim.injectLocal(at);
    sim.runUntil(at + 2 * c.longPollS * 1000);
    TEST_ASSERT_EQUAL(1, int(sim.local().size()));
    assertPrompt(c, sim.local()[0]);
  }
  const uint32_t first = 40000;   // mid-long-poll: cut short, its turn runs to 44000
  uint32_t worst = 0;
  for (uint32_t at = first + c.turnMs; at < first + c.turnMs + cycleMs; at += 89) {
    tgsim::Sim sim(c);
    sim.injectLocal(first);
    sim.injectLocal(at);
    sim.runUntil(at + 2 * c.longPollS * 1000);
    TEST_ASSERT_EQUAL(2, int(sim.local().size()));
    TEST_ASSERT_EQUAL_UINT32(first, sim.local()[0].startedAt);
    assertPrompt(c, sim.local()[1]);
    worst = std::max(worst, sim.local()[1].latency());
  }
  TEST_ASSERT_TRUE(worst > kTgIdleSliceMs);   // the sweep did land inside the reconnect
}

// The owner's case: the device sits idle in an established long-poll (socket open,
// no reconnect due) and a voice transcript arrives. It starts within a tick, not at
// the end of the long-poll - the issue's "within about 1 s" with room to spare.
static void test_transcript_in_an_established_long_poll_starts_at_once() {
  const tgsim::Config c;
  for (uint32_t into : {0u, 1u, 150u, 1500u, 15000u, 29000u}) {
    tgsim::Sim sim(c);
    sim.runUntil(100000);              // whole cycles: ends at a cycle boundary
    const uint32_t base = sim.now();   // the next poll goes out now (socket open)
    sim.injectLocal(base + into);
    sim.runUntil(base + into + c.longPollS * 1000);
    TEST_ASSERT_EQUAL(1, int(sim.local().size()));
    TEST_ASSERT_TRUE_MESSAGE(sim.local()[0].latency() <= 1, "an established long-poll held the transcript");
  }
}

// No new TLS churn: an idle device keeps its one poll socket (no skip, no yield, no
// check, one handshake in five minutes), and each local turn costs at most the single
// reconnect the old loop also paid (it closed the socket for the turn anyway).
static void test_no_new_tls_churn() {
  const tgsim::Config c;
  tgsim::Sim idle(c);
  idle.runUntil(300000);
  TEST_ASSERT_EQUAL_UINT32(0, idle.stats().skips);
  TEST_ASSERT_EQUAL_UINT32(0, idle.stats().yields);
  TEST_ASSERT_EQUAL_UINT32(0, idle.stats().checks);
  TEST_ASSERT_EQUAL_UINT32(1, idle.stats().handshakes);
  TEST_ASSERT_EQUAL_UINT32(idle.stats().cycles, idle.stats().longPolls);

  tgsim::Sim busy(c);
  uint32_t injects = 0;
  for (uint32_t at = 7000; at < 300000; at += 45000, ++injects) busy.injectLocal(at);
  busy.runUntil(330000);
  TEST_ASSERT_EQUAL_UINT32(injects, uint32_t(busy.local().size()));
  TEST_ASSERT_TRUE_MESSAGE(busy.stats().handshakes <= 1 + injects, "local turns added TLS churn");
}

// Fairness under pressure: a local message every 300 ms, forever (far more than any
// person or web page sends), on a normal link and on one slower than a second.
// Skips never run two cycles, Telegram gets a Check every other cycle, every Telegram
// message is served - none lost, none starved past a skip cycle and a check cycle -
// and local turns keep flowing.
static void test_steady_local_stream_never_starves_telegram() {
  for (uint32_t rtt : {150u, 1800u}) {
    tgsim::Config c;
    c.rttMs = rtt;
    tgsim::Sim sim(c);
    for (uint32_t at = 1000; at < 200000; at += 300) sim.injectLocal(at);
    for (uint32_t at : {10000u, 47000u, 90013u, 151000u}) sim.telegramArrives(at);
    sim.runUntil(210000);
    const tgsim::Stats& s = sim.stats();
    TEST_ASSERT_TRUE(s.skips > 0 && s.checks > 0);
    TEST_ASSERT_TRUE_MESSAGE(s.maxSkipRun <= 1, "skipped the Telegram poll two cycles running");
    TEST_ASSERT_EQUAL_MESSAGE(4, int(sim.telegram().size()), "a Telegram message was starved or lost");
    TEST_ASSERT_EQUAL(0, int(sim.telegramPending()));
    // Worst case: it lands just after a Check was sent, then waits out that cycle and
    // a skip cycle (each carrying a local turn) until the next Check answers.
    const uint32_t cycleMs = c.handshakeMs + c.rttMs + c.turnMs + c.workMs + kTgIdleSliceMs;
    for (const tgsim::Dispatched& d : sim.telegram())
      TEST_ASSERT_TRUE_MESSAGE(d.latency() <= 3 * cycleMs, "Telegram starved by local turns");
    TEST_ASSERT_TRUE(sim.local().size() > 20);
  }
}

// A Telegram message racing a local turn: it reaches the server just before, just
// after, and exactly when a transcript is injected. A yielded poll leaves the offset
// untouched, so it is always served exactly once.
static void test_telegram_racing_a_local_turn_is_served_once() {
  const tgsim::Config c;
  for (int32_t skew = -2000; skew <= 2000; skew += 37) {
    tgsim::Sim sim(c);
    const uint32_t at = 40000;
    sim.injectLocal(at);
    sim.telegramArrives(uint32_t(int32_t(at) + skew));
    sim.runUntil(at + 3 * c.longPollS * 1000);
    TEST_ASSERT_EQUAL_MESSAGE(1, int(sim.telegram().size()), "a Telegram message was lost or run twice");
    TEST_ASSERT_EQUAL(1, int(sim.local().size()));
  }
}

// Telegram unreachable: every poll burns a failed connect and backs off (up to 12 s).
// The backoff is an idle wait, so a local turn never sits in it; the only wait left is
// the failed connect already in progress.
static void test_telegram_outage_backoff_never_holds_a_local_turn() {
  tgsim::Config c;
  c.telegramDown = true;
  for (uint32_t at = 30000; at < 200000; at += 2311) {
    tgsim::Sim sim(c);
    sim.injectLocal(at);
    sim.runUntil(at + 2 * c.connectFailMs + 13000);
    TEST_ASSERT_EQUAL(1, int(sim.local().size()));
    const tgsim::Dispatched& d = sim.local()[0];
    TEST_ASSERT_TRUE_MESSAGE(d.idleMs <= kTgIdleSliceMs, "a poll-error backoff held a local turn");
    TEST_ASSERT_TRUE(d.latency() <= c.connectFailMs + kTgIdleSliceMs);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_wait_step_over_every_input);
  RUN_TEST(test_poll_timeout_per_plan);
  RUN_TEST(test_idle_pause_sleeps_its_length_in_slices);
  RUN_TEST(test_idle_pause_ends_for_a_waiting_local_turn);
  RUN_TEST(test_plan_over_every_sequence);
  RUN_TEST(test_local_turn_never_waits_on_an_idle_wait);
  RUN_TEST(test_transcript_in_an_established_long_poll_starts_at_once);
  RUN_TEST(test_no_new_tls_churn);
  RUN_TEST(test_steady_local_stream_never_starves_telegram);
  RUN_TEST(test_telegram_racing_a_local_turn_is_served_once);
  RUN_TEST(test_telegram_outage_backoff_never_holds_a_local_turn);
  return UNITY_END();
}
