#include <unity.h>

#include "nimbus/harness/head_arc.h"

using nimbus::harness::HeadArcTracker;
using Action = nimbus::harness::HeadArcTracker::Action;

void setUp() {}
void tearDown() {}

// A childless turn: the tracker never lights; after the turn it owes ONE Clear
// (harmless duplicate of the TurnGuard's own Offline), then goes quiet.
static void test_childless_turn_clears_once_then_quiet() {
  HeadArcTracker t;
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(true, 0, 1000));    // in-turn
  TEST_ASSERT_EQUAL(Action::Clear, t.reconcile(false, 0, 2000));  // clear owed
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 0, 3000));   // quiet
  TEST_ASSERT_FALSE(t.lit());
}

// The core case: a fan-out turn ends with children still active -> the tracker
// lights the arc, holds it (no repeat before the refresh window), and clears it
// exactly once when the last child finishes.
static void test_fanout_lights_holds_then_clears() {
  HeadArcTracker t;
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(true, 3, 5000));    // turn spawns
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 3, 10000));
  TEST_ASSERT_TRUE(t.lit());
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 3, 12000));
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 2, 40000));
  TEST_ASSERT_EQUAL(Action::Clear, t.reconcile(false, 0, 45000));
  TEST_ASSERT_FALSE(t.lit());
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 0, 46000));
}

// While children run for longer than the refresh window, the arc re-lights so
// the router's ambient hold can't age it out mid-run (count keeps moving here,
// so the frozen backstop never trips).
static void test_refresh_relights_past_window() {
  HeadArcTracker t(60000, 1800000);
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 2, 0));
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 2, 59000));
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 2, 60000));
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 1, 90000));   // count moved
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 1, 121000));
}

// A new turn starting while children run stands the tracker down (the TurnGuard
// owns the arc during a turn); when that turn ends with children still present
// the tracker re-lights (a harmless refresh - the guard left it lit).
static void test_turn_stands_tracker_down_then_relights() {
  HeadArcTracker t;
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 2, 1000));
  TEST_ASSERT_TRUE(t.lit());
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(true, 2, 2000));
  TEST_ASSERT_FALSE(t.lit());
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 2, 3000));
}

// Prism #2: the guard leaves the arc lit (children>0 at turn end) but the last
// child fast-fails BEFORE the tracker's first post-turn tick. The tracker never
// lit, yet it must still clear the GUARD-lit arc - "clear owed after a turn".
static void test_guard_lit_arc_cleared_after_fast_fail() {
  HeadArcTracker t;
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(true, 1, 1000));    // turn, 1 pending
  // dispatch fast-failed on tg_poll before our first tick: children now 0
  TEST_ASSERT_EQUAL(Action::Clear, t.reconcile(false, 0, 6000));  // clears anyway
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 0, 11000));
}

// Prism #0: tg_poll dies with children stuck in the journal - the count freezes.
// After frozenMs with no count change the tracker clears the arc and LATCHES
// (no more re-lights), so a dead system can't pulse "working" forever.
static void test_frozen_children_backstop_latches() {
  HeadArcTracker t(60000, 1800000);
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 2, 0));
  // count frozen at 2; refresh keeps re-lighting until the freeze window passes
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 2, 60000));
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 2, 120000));
  // 30 min with no count change -> Clear + latch
  TEST_ASSERT_EQUAL(Action::Clear, t.reconcile(false, 2, 1800000));
  TEST_ASSERT_TRUE(t.backstopped());
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 2, 1860000));  // no re-light
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 2, 1920000));
  // a count CHANGE proves the system is alive again -> backstop releases
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 1, 1980000));
}

// A turn also releases the backstop (a turn proves the system is alive).
static void test_turn_releases_backstop() {
  HeadArcTracker t(60000, 1800000);
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 2, 0));
  TEST_ASSERT_EQUAL(Action::Clear, t.reconcile(false, 2, 1800000));  // backstop
  TEST_ASSERT_TRUE(t.backstopped());
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(true, 2, 1810000));    // a turn runs
  TEST_ASSERT_FALSE(t.backstopped());
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 2, 1815000));  // arc resumes
}

// CUM-11 root-cause repro + fix (the tracer case). A short wake-up FAN-OUT turn
// fires on tg_poll and finishes BETWEEN two 5 s watchdog ticks, so the tracker
// never samples turnInFlight=true. The TurnGuard left the arc lit (children>0 at
// turn end), then the children drained to 0 before the first post-turn tick.
// WITHOUT the completion edge the tracker owed no Clear (sawTurn_ never set) and the
// guard-lit arc stranded until a backstop. WITH the edge, the PRIMARY path clears it
// and no backstop fires.
static void test_short_wakeup_fanout_cleared_by_primary_path() {
  HeadArcTracker t;
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 0, 0));           // idle
  // turnCount moved (a wake-up turn completed) AND children already 0: the edge is
  // the only signal the tracker gets, and it must own the Clear of the guard-lit arc.
  TEST_ASSERT_EQUAL(Action::Clear, t.reconcile(false, 0, 5000, /*turnEnded=*/true));
  TEST_ASSERT_EQUAL(Action::None,  t.reconcile(false, 0, 10000));      // quiet
  TEST_ASSERT_FALSE(t.lit());
  TEST_ASSERT_EQUAL(0u, t.backstopFires());   // primary path did it - no backstop
}

// Regression guard for the OLD stranded behavior: without the edge, a between-ticks
// childless turn leaves nothing owed (this is the buggy path the edge repairs; kept
// so a refactor that drops the edge is caught here as a stuck arc, not silently).
static void test_without_edge_between_tick_turn_owes_no_clear() {
  HeadArcTracker t;
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 0, 0));
  TEST_ASSERT_EQUAL(Action::None, t.reconcile(false, 0, 5000));   // no edge -> nothing owed
}

// CUM-11 metric: the belt-and-braces backstop counter counts a genuine wedge.
static void test_backstop_counter_counts_a_real_wedge() {
  HeadArcTracker t(60000, 1800000);
  TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 2, 0));
  TEST_ASSERT_EQUAL(0u, t.backstopFires());
  TEST_ASSERT_EQUAL(Action::Clear, t.reconcile(false, 2, 1800000));   // frozen 30 min
  TEST_ASSERT_TRUE(t.backstopped());
  TEST_ASSERT_EQUAL(1u, t.backstopFires());
}

// CUM-11 acceptance shape (host analogue of the 24 h soak): 24 hourly wake-up
// cycles, each fan-out cleared by the primary path -> backstop counter stays 0.
static void test_hourly_wakeup_soak_never_backstops() {
  HeadArcTracker t;
  uint32_t now = 0;
  for (int hour = 0; hour < 24; ++hour) {
    TEST_ASSERT_EQUAL(Action::None,  t.reconcile(true,  0, now));            now += 5000;   // in-turn
    TEST_ASSERT_EQUAL(Action::Light, t.reconcile(false, 1, now, true));     now += 5000;   // ended, 1 child
    TEST_ASSERT_EQUAL(Action::Clear, t.reconcile(false, 0, now));           now += 5000;   // child done
    TEST_ASSERT_EQUAL(Action::None,  t.reconcile(false, 0, now));           now += 3600000 - 15000;
  }
  TEST_ASSERT_EQUAL(0u, t.backstopFires());   // 24 h, zero backstop fires
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_childless_turn_clears_once_then_quiet);
  RUN_TEST(test_fanout_lights_holds_then_clears);
  RUN_TEST(test_refresh_relights_past_window);
  RUN_TEST(test_turn_stands_tracker_down_then_relights);
  RUN_TEST(test_guard_lit_arc_cleared_after_fast_fail);
  RUN_TEST(test_frozen_children_backstop_latches);
  RUN_TEST(test_turn_releases_backstop);
  RUN_TEST(test_short_wakeup_fanout_cleared_by_primary_path);
  RUN_TEST(test_without_edge_between_tick_turn_owes_no_clear);
  RUN_TEST(test_backstop_counter_counts_a_real_wedge);
  RUN_TEST(test_hourly_wakeup_soak_never_backstops);
  return UNITY_END();
}
