// test_touch_liveness - FIX 4: honest resistive (XPT2046) touch liveness.
//
// The health/status "touch: ok, 0 failures" was hardwired zero on resistive boards
// and LIED while the controller was dead (SPI MISO stuck high => every raw read
// pegged at 4095). These pin the nimbus-side detector that reads solide::touch::
// readRaw and trips ONLY on the persistent all-4095 signature - never on a real
// press (plausible x/y) and never on a normal idle (readRaw reports nothing).
#include <unity.h>

#include "nimbus/display/resistive_touch.h"

using nimbus::display::rawLooksDead;
using nimbus::display::ResistiveTouchLiveness;

void setUp() {}
void tearDown() {}

// The per-read signature: all three axes pegged at the all-ones sentinel is dead;
// anything with a plausible position axis is not.
static void test_raw_looks_dead_signature() {
  TEST_ASSERT_TRUE(rawLooksDead(4095, 4095, 4095));   // MISO stuck high: dead
  TEST_ASSERT_FALSE(rawLooksDead(0, 0, 0));           // all low: not the pattern
  TEST_ASSERT_FALSE(rawLooksDead(2000, 1800, 3000));  // a firm press: plausible x/y
  TEST_ASSERT_FALSE(rawLooksDead(4095, 4095, 10));    // pegged x/y but low z: not dead
  TEST_ASSERT_FALSE(rawLooksDead(100, 4095, 4095));   // one axis plausible: not dead
  TEST_ASSERT_FALSE(rawLooksDead(4095, 120, 4095));   // other axis plausible: not dead
}

// A normal idle: readRaw returns false (the live controller answers "no finger").
// That is a sign of life and must never trip, no matter how long it idles.
static void test_idle_never_trips() {
  ResistiveTouchLiveness live(4);
  for (int i = 0; i < 100; i++)
    TEST_ASSERT_FALSE(live.update(/*gotRaw=*/false, 0, 0, 0));
  TEST_ASSERT_FALSE(live.degraded());
}

// A genuine sustained press (plausible x/y, mid z) is alive across the whole hold,
// even a long one - the streak never advances because rawLooksDead stays false.
static void test_real_press_stays_alive() {
  ResistiveTouchLiveness live(4);
  for (int i = 0; i < 100; i++)
    TEST_ASSERT_FALSE(live.update(/*gotRaw=*/true, 2100, 1750, 900));
  TEST_ASSERT_FALSE(live.degraded());
  // A firm press near the edge (high on ONE axis) is still not the all-pegged
  // pattern, so it stays alive.
  TEST_ASSERT_FALSE(live.update(true, 4095, 1200, 4095));
  TEST_ASSERT_FALSE(live.degraded());
}

// The dead controller: persistent all-4095 reads trip only after the debounce
// window, then stay tripped.
static void test_stuck_high_trips_after_debounce() {
  ResistiveTouchLiveness live(4);
  TEST_ASSERT_FALSE(live.update(true, 4095, 4095, 4095));  // 1
  TEST_ASSERT_FALSE(live.update(true, 4095, 4095, 4095));  // 2
  TEST_ASSERT_FALSE(live.update(true, 4095, 4095, 4095));  // 3
  TEST_ASSERT_TRUE(live.update(true, 4095, 4095, 4095));   // 4 -> dead
  TEST_ASSERT_TRUE(live.update(true, 4095, 4095, 4095));   // stays dead
  TEST_ASSERT_TRUE(live.degraded());
}

// A lone glitched stuck read does not trip, and any sign of life resets the streak
// so the debounce must restart from scratch.
static void test_single_glitch_does_not_trip_and_life_resets() {
  ResistiveTouchLiveness live(4);
  live.update(true, 4095, 4095, 4095);   // one stuck read
  live.update(true, 4095, 4095, 4095);   // two
  TEST_ASSERT_FALSE(live.degraded());
  live.update(false, 0, 0, 0);           // idle: a sign of life resets the streak
  TEST_ASSERT_EQUAL_UINT16(0, live.deadStreak());
  live.update(true, 4095, 4095, 4095);   // must climb from scratch again
  live.update(true, 4095, 4095, 4095);
  live.update(true, 4095, 4095, 4095);
  TEST_ASSERT_FALSE(live.degraded());    // only three since reset
  TEST_ASSERT_TRUE(live.update(true, 4095, 4095, 4095));  // fourth trips
}

// After tripping, a real press (the panel came back, or a probe error cleared)
// clears the fault immediately - the report never stays stale.
static void test_recovery_clears() {
  ResistiveTouchLiveness live(4);
  for (int i = 0; i < 6; i++) live.update(true, 4095, 4095, 4095);
  TEST_ASSERT_TRUE(live.degraded());
  TEST_ASSERT_FALSE(live.update(true, 2000, 1500, 800));  // a real read clears it
  TEST_ASSERT_FALSE(live.degraded());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_raw_looks_dead_signature);
  RUN_TEST(test_idle_never_trips);
  RUN_TEST(test_real_press_stays_alive);
  RUN_TEST(test_stuck_high_trips_after_debounce);
  RUN_TEST(test_single_glitch_does_not_trip_and_life_resets);
  RUN_TEST(test_recovery_clears);
  return UNITY_END();
}
