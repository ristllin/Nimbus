#include <unity.h>

#include "nimbus/sd_health.h"

using nimbus::SdHealthTracker;
using Event = nimbus::SdHealthTracker::Event;

void setUp() {}
void tearDown() {}

// Demote only after N consecutive failures; a success mid-streak resets it.
static void test_demote_after_streak() {
  SdHealthTracker t(3, 2);
  TEST_ASSERT_FALSE(t.lost());
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(false)));  // 1
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(false)));  // 2
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(true)));   // reset
  TEST_ASSERT_FALSE(t.lost());
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(false)));  // 1
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(false)));  // 2
  TEST_ASSERT_EQUAL(int(Event::Demote), int(t.note(false)));// 3 -> demote
  TEST_ASSERT_TRUE(t.lost());
}

// Once lost, promote only after M consecutive successes; a failure resets it.
static void test_promote_after_streak() {
  SdHealthTracker t(2, 3);
  t.note(false); t.note(false);                     // -> lost
  TEST_ASSERT_TRUE(t.lost());
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(true)));   // 1
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(true)));   // 2
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(false)));  // reset (flapping card)
  TEST_ASSERT_TRUE(t.lost());
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(true)));   // 1
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(true)));   // 2
  TEST_ASSERT_EQUAL(int(Event::Promote), int(t.note(true)));// 3 -> promote
  TEST_ASSERT_FALSE(t.lost());
}

// A single failure never demotes (debounce); a single success never promotes.
static void test_single_blip_debounced() {
  SdHealthTracker t(2, 2);
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(false)));
  TEST_ASSERT_FALSE(t.lost());
  t.note(false);  // now lost
  TEST_ASSERT_TRUE(t.lost());
  TEST_ASSERT_EQUAL(int(Event::None), int(t.note(true)));
  TEST_ASSERT_TRUE(t.lost());
}

// Explicit force transitions report the edge only on a real change.
static void test_force_transitions() {
  SdHealthTracker t(2, 2);
  TEST_ASSERT_EQUAL(int(Event::Demote), int(t.forceDemote()));
  TEST_ASSERT_EQUAL(int(Event::None), int(t.forceDemote()));   // already lost
  TEST_ASSERT_TRUE(t.lost());
  TEST_ASSERT_EQUAL(int(Event::Promote), int(t.forcePromote()));
  TEST_ASSERT_EQUAL(int(Event::None), int(t.forcePromote()));  // already healthy
  TEST_ASSERT_FALSE(t.lost());
}

// threshold of 1 demotes/promotes on the first result each way.
static void test_threshold_one() {
  SdHealthTracker t(1, 1);
  TEST_ASSERT_EQUAL(int(Event::Demote), int(t.note(false)));
  TEST_ASSERT_EQUAL(int(Event::Promote), int(t.note(true)));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_demote_after_streak);
  RUN_TEST(test_promote_after_streak);
  RUN_TEST(test_single_blip_debounced);
  RUN_TEST(test_force_transitions);
  RUN_TEST(test_threshold_one);
  return UNITY_END();
}
