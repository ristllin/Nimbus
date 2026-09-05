#include <unity.h>

#include "nimbus/saver.h"

// SaverTimer - the screensaver idle clock. Wrap-safe millis math, 0=disabled,
// activity resets. The interesting case is the millis() wrap at 49.7 days.

using nimbus::SaverTimer;
using nimbus::SaverStage;

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

// ============================================================================
// Deep-dim stage (CUM-292): the backlight-fully-off screensaver stage that sits
// AFTER Rest. deepDimDue()/stage() extend the same idle clock; the class rule is
// "the stage only advances Active -> Rest -> DeepDim on idle and only resets to
// Active on activity - it never thrashes, and a deepExtraMs of 0 (external power)
// caps the panel at Rest." These test that class, not one timeout instance.
// ============================================================================

static constexpr uint32_t kMin = 60000u;

// deepExtraMs == 0 means deep-dim disabled - the external-power rule. The panel
// still rests, but never goes fully dark no matter how long it idles.
static void test_deepdim_disabled_when_extra_zero() {
  SaverTimer t;
  t.setThresholdMin(1);
  t.noteActivity(0);
  TEST_ASSERT_FALSE(t.deepDimDue(kMin, 0));
  TEST_ASSERT_FALSE(t.deepDimDue(100u * kMin, 0));          // hours later: still no
  TEST_ASSERT_EQUAL(int(SaverStage::Rest), int(t.stage(100u * kMin, 0)));
}

// Deep-dim is due only after the rest threshold PLUS the extra idle.
static void test_deepdim_due_after_rest_plus_extra() {
  SaverTimer t;
  t.setThresholdMin(1);           // rest at 60 s
  t.noteActivity(0);
  const uint32_t extra = 30000u;  // + 30 s => deep-dim at 90 s
  TEST_ASSERT_FALSE(t.deepDimDue(kMin, extra));            // at rest, not yet deep
  TEST_ASSERT_FALSE(t.deepDimDue(kMin + extra - 1, extra));
  TEST_ASSERT_TRUE(t.deepDimDue(kMin + extra, extra));
  TEST_ASSERT_TRUE(t.deepDimDue(kMin + extra + 5u * kMin, extra));
}

// Deep-dim requires the screensaver itself to be enabled (threshold 0 = off).
static void test_deepdim_requires_threshold() {
  SaverTimer t;
  t.setThresholdMin(0);
  t.noteActivity(0);
  TEST_ASSERT_FALSE(t.deepDimDue(0xFFFFFFFFu, 30000u));
  TEST_ASSERT_EQUAL(int(SaverStage::Active), int(t.stage(0xFFFFFFFFu, 30000u)));
}

// Same wrap-safe millis math as due(): a deep-dim window that straddles the
// 49.7-day rollover still fires at the right idle, never early or stuck.
static void test_deepdim_wrap_safe() {
  SaverTimer t;
  t.setThresholdMin(1);                       // rest at 60 s
  const uint32_t extra = 60000u;              // deep-dim at 120 s idle
  const uint32_t nearWrap = 0xFFFFFFFFu - 30000u;   // 30 s before wrap
  t.noteActivity(nearWrap);
  TEST_ASSERT_FALSE(t.deepDimDue(uint32_t(nearWrap + 119000u), extra));  // 119 s
  TEST_ASSERT_TRUE(t.deepDimDue(uint32_t(nearWrap + 120000u), extra));   // 120 s
}

// Future lastMs (activity a few ms "ahead" of the loop's now) reads as ZERO
// idle for deep-dim too - the 2026-07-18 underflow class, extended.
static void test_deepdim_future_activity_is_zero_idle() {
  SaverTimer t;
  t.setThresholdMin(1);
  t.noteActivity(19938);
  TEST_ASSERT_FALSE(t.deepDimDue(19937, 30000u));   // 1 ms "ahead"
  TEST_ASSERT_FALSE(t.deepDimDue(19938, 30000u));
}

// The composite stage walks Active -> Rest -> DeepDim as idle grows, and snaps
// back to Active on activity. This is the state machine the device drives.
static void test_stage_transitions() {
  SaverTimer t;
  t.setThresholdMin(1);            // rest at 60 s
  t.noteActivity(0);
  const uint32_t extra = 30000u;   // deep-dim at 90 s
  TEST_ASSERT_EQUAL(int(SaverStage::Active),  int(t.stage(0, extra)));
  TEST_ASSERT_EQUAL(int(SaverStage::Active),  int(t.stage(kMin - 1, extra)));
  TEST_ASSERT_EQUAL(int(SaverStage::Rest),    int(t.stage(kMin, extra)));
  TEST_ASSERT_EQUAL(int(SaverStage::Rest),    int(t.stage(kMin + extra - 1, extra)));
  TEST_ASSERT_EQUAL(int(SaverStage::DeepDim), int(t.stage(kMin + extra, extra)));
  // Activity at any stage resets straight to Active.
  t.noteActivity(kMin + extra);
  TEST_ASSERT_EQUAL(int(SaverStage::Active),  int(t.stage(kMin + extra, extra)));
}

// The class rule the recurring stuck-ring/thrash bugs paid for: while idle, the
// stage is MONOTONIC (never bounces back a level without activity) and crosses
// exactly two boundaries (Active->Rest, Rest->DeepDim) - no oscillation. Then a
// single activity returns it to Active exactly once.
static void test_stage_is_monotonic_no_thrash() {
  SaverTimer t;
  t.setThresholdMin(1);            // rest at 60 s
  t.noteActivity(0);
  const uint32_t extra = 30000u;   // deep-dim at 90 s
  int prev = int(SaverStage::Active);
  int transitions = 0;
  for (uint32_t now = 0; now <= 5u * kMin; now += 250u) {   // 5 min, 250 ms steps
    const int s = int(t.stage(now, extra));
    TEST_ASSERT_TRUE(s >= prev);                 // never regresses while idle
    if (s != prev) transitions++;
    prev = s;
  }
  TEST_ASSERT_EQUAL(2, transitions);             // Active->Rest->DeepDim, once each
  TEST_ASSERT_EQUAL(int(SaverStage::DeepDim), prev);
  t.noteActivity(5u * kMin);
  TEST_ASSERT_EQUAL(int(SaverStage::Active), int(t.stage(5u * kMin, extra)));
}

// The wake-tap-swallow invariant: a touch while deep-dimmed (backlight off) is a
// wake ONLY; a touch while lit (Rest glow or Active) actuates normally.
static void test_wake_tap_swallow_invariant() {
  TEST_ASSERT_TRUE(nimbus::wakeTapIsSwallowed(true));    // backlight was off -> swallow
  TEST_ASSERT_FALSE(nimbus::wakeTapIsSwallowed(false));  // screen was lit -> actuate
}

// FIX 1: the remote screen-rest setter (POST /api/config saverMin) parses a signed
// value, so the clamp must fold both ends of the range. The critical case is a
// NEGATIVE input: it must land at 0 (screen always on), NOT wrap through the
// uint16 store to the 1440 ceiling (the owner asking for "always on" and getting
// "rest at a full day" is the honesty bug this closes).
static void test_clamp_saver_minutes() {
  using nimbus::clampSaverMinutes;
  TEST_ASSERT_EQUAL_INT(0, clampSaverMinutes(0));         // 0 = always on, kept
  TEST_ASSERT_EQUAL_INT(5, clampSaverMinutes(5));         // in-range passthrough
  TEST_ASSERT_EQUAL_INT(1440, clampSaverMinutes(1440));   // ceiling kept exactly
  TEST_ASSERT_EQUAL_INT(1440, clampSaverMinutes(1441));   // above ceiling -> clamp
  TEST_ASSERT_EQUAL_INT(1440, clampSaverMinutes(100000)); // far above -> clamp
  TEST_ASSERT_EQUAL_INT(0, clampSaverMinutes(-1));        // negative -> 0, NOT 1440
  TEST_ASSERT_EQUAL_INT(0, clampSaverMinutes(-9999));     // deep negative -> 0
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_disabled_never_due);
  RUN_TEST(test_due_after_threshold);
  RUN_TEST(test_activity_resets);
  RUN_TEST(test_wrap_safe);
  RUN_TEST(test_default_is_one_hour);
  RUN_TEST(test_future_activity_is_zero_idle);
  RUN_TEST(test_deepdim_disabled_when_extra_zero);
  RUN_TEST(test_deepdim_due_after_rest_plus_extra);
  RUN_TEST(test_deepdim_requires_threshold);
  RUN_TEST(test_deepdim_wrap_safe);
  RUN_TEST(test_deepdim_future_activity_is_zero_idle);
  RUN_TEST(test_stage_transitions);
  RUN_TEST(test_stage_is_monotonic_no_thrash);
  RUN_TEST(test_wake_tap_swallow_invariant);
  RUN_TEST(test_clamp_saver_minutes);
  return UNITY_END();
}
