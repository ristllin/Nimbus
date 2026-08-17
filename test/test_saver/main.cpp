#include <unity.h>

#include "nimbus/saver.h"

// SaverTimer - the screensaver idle clock. Wrap-safe millis math, 0=disabled,
// activity resets. The interesting case is the millis() wrap at 49.7 days.

using nimbus::SaverTimer;

void setUp() {}
void tearDown() {}

static void test_disabled_never_due() {
  SaverTimer t;
  t.setThresholdMin(0);
  t.noteActivity(0);
  TEST_ASSERT_FALSE(t.due(0xFFFFFFFFu));
}

static void test_due_after_threshold() {
  SaverTimer t;
  t.setThresholdMin(60);
  t.noteActivity(1000);
  TEST_ASSERT_FALSE(t.due(1000));
  TEST_ASSERT_FALSE(t.due(1000 + 60u * 60000u - 1));
  TEST_ASSERT_TRUE(t.due(1000 + 60u * 60000u));
  TEST_ASSERT_TRUE(t.due(1000 + 90u * 60000u));
}

static void test_activity_resets() {
  SaverTimer t;
  t.setThresholdMin(1);
  t.noteActivity(0);
  TEST_ASSERT_TRUE(t.due(60001));
  t.noteActivity(60000);
  TEST_ASSERT_FALSE(t.due(60001));
  TEST_ASSERT_TRUE(t.due(120000));
}

static void test_wrap_safe() {
  SaverTimer t;
  t.setThresholdMin(60);
  const uint32_t nearWrap = 0xFFFFFFFFu - 30u * 60000u;  // 30 min before wrap
  t.noteActivity(nearWrap);
  TEST_ASSERT_FALSE(t.due(0xFFFFFFFFu));                 // 30 min later
  TEST_ASSERT_FALSE(t.due(uint32_t(nearWrap + 59u * 60000u)));  // wrapped, 59 min
  TEST_ASSERT_TRUE(t.due(uint32_t(nearWrap + 60u * 60000u)));   // wrapped, 60 min
}

static void test_default_is_one_hour() {
  SaverTimer t;
  TEST_ASSERT_EQUAL_UINT16(60, t.thresholdMin());
}

// R_saver_future_activity (field bug 2026-07-18, Board 1): noteActivity() runs
// mid-iteration with a fresher millis() than the caller's loop-top `now`, so
// lastMs can sit a few ms IN THE FUTURE. The bare uint32 subtraction
// underflowed (now=19937 last=19938 -> ~2^32 "idle") and fired the screensaver
// seconds after a knob interaction (menu close -> logo flash). Future lastMs
// must read as ZERO idle.
static void test_future_activity_is_zero_idle() {
  SaverTimer t;
  t.setThresholdMin(60);
  t.noteActivity(19938);
  TEST_ASSERT_FALSE(t.due(19937));                 // 1 ms "ahead" - the exact repro
  TEST_ASSERT_FALSE(t.due(19938));                 // same instant
  TEST_ASSERT_FALSE(t.due(19938 + 59u * 60000u));  // clock catches up: still short
  TEST_ASSERT_TRUE(t.due(19938 + 60u * 60000u));   // and dues normally after 60 min
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_disabled_never_due);
  RUN_TEST(test_due_after_threshold);
  RUN_TEST(test_activity_resets);
  RUN_TEST(test_wrap_safe);
  RUN_TEST(test_default_is_one_hour);
  RUN_TEST(test_future_activity_is_zero_idle);
  return UNITY_END();
}
