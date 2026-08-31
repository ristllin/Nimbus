#include <unity.h>

#include "nimbus/panel_heal.h"

// The panel-heal REARM GUARD (CUM-167/CUM-231 white-screen class). test_panel_health
// pins the healthy() MADCTL compare; this pins the policy that CONSUMES it: past
// the trust window the panel is re-armed UNCONDITIONALLY, and healthy() only
// decides whether to ALSO repaint. The regression that shipped a white screen was
// exactly a rearm/repaint that returned early when healthy() read true - so every
// assertion below that ties rearm to a config-OK reading is a witness against that
// specific bug coming back.

using nimbus::panel::HealAction;
using nimbus::panel::tickHealthAction;
using nimbus::panel::unchangedFrameAction;

// The device uses a 5 s window (kHealMs). The policy is window-agnostic; tests use
// a nominal window and probe both sides of it.
static constexpr uint32_t W = 5000;

void setUp() {}
void tearDown() {}

// --- unchanged-frame dirty-gate branch --------------------------------------

// Inside the window an unchanged frame is trusted: no rearm, no repaint,
// regardless of what a (never-taken) config read would say.
static void test_within_window_does_nothing() {
  HealAction a = unchangedFrameAction(0, W, /*configOk=*/true);
  TEST_ASSERT_FALSE(a.rearm);
  TEST_ASSERT_FALSE(a.repaint);
  HealAction b = unchangedFrameAction(W - 1, W, /*configOk=*/false);
  TEST_ASSERT_FALSE(b.rearm);
  TEST_ASSERT_FALSE(b.repaint);
}

// THE GUARD: past the window, a panel that reads HEALTHY is STILL re-armed. This
// is the beacon-slept-but-configured case - healthy() returns true while the glass
// is white, and only the unconditional rearm wakes it. A regression that skipped
// rearm here (the pre-CUM-231 early return) fails this assertion.
static void test_past_window_rearms_even_when_config_ok() {
  HealAction a = unchangedFrameAction(W, W, /*configOk=*/true);
  TEST_ASSERT_TRUE(a.rearm);       // re-arm is NOT gated on healthy()
  TEST_ASSERT_FALSE(a.repaint);    // config fine -> no extra blit needed
}

// Past the window with a confirmed config loss: re-arm AND repaint on top.
static void test_past_window_config_loss_rearms_and_repaints() {
  HealAction a = unchangedFrameAction(W + 1, W, /*configOk=*/false);
  TEST_ASSERT_TRUE(a.rearm);
  TEST_ASSERT_TRUE(a.repaint);
}

// The boundary is inclusive: at exactly the window the frame is no longer trusted.
static void test_window_boundary_is_inclusive() {
  HealAction at = unchangedFrameAction(W, W, /*configOk=*/true);
  TEST_ASSERT_TRUE(at.rearm);
  HealAction below = unchangedFrameAction(W - 1, W, /*configOk=*/true);
  TEST_ASSERT_FALSE(below.rearm);
}

// Whatever the config readback, past the window rearm is ALWAYS asserted. This is
// the invariant stated directly: no configOk value turns the re-arm off.
static void test_rearm_is_never_gated_on_config() {
  for (int ok = 0; ok <= 1; ++ok) {
    HealAction a = unchangedFrameAction(W + 100, W, ok != 0);
    TEST_ASSERT_TRUE_MESSAGE(a.rearm, "past window must always rearm");
  }
}

// --- periodic watchdog (tickHealth) -----------------------------------------

// Within the window tickHealth is a no-op.
static void test_tick_within_window_does_nothing() {
  HealAction a = tickHealthAction(0, W);
  TEST_ASSERT_FALSE(a.rearm);
  TEST_ASSERT_FALSE(a.repaint);
}

// Past the window the periodic watchdog ALWAYS re-arms and ALWAYS repaints - the
// probe result never gates it (it only feeds the counters on the device). This is
// the cover for the undetectable pixel-loss mode; gating it was the field bug.
static void test_tick_past_window_always_rearms_and_repaints() {
  HealAction a = tickHealthAction(W, W);
  TEST_ASSERT_TRUE(a.rearm);
  TEST_ASSERT_TRUE(a.repaint);
  HealAction b = tickHealthAction(W * 3, W);
  TEST_ASSERT_TRUE(b.rearm);
  TEST_ASSERT_TRUE(b.repaint);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_within_window_does_nothing);
  RUN_TEST(test_past_window_rearms_even_when_config_ok);
  RUN_TEST(test_past_window_config_loss_rearms_and_repaints);
  RUN_TEST(test_window_boundary_is_inclusive);
  RUN_TEST(test_rearm_is_never_gated_on_config);
  RUN_TEST(test_tick_within_window_does_nothing);
  RUN_TEST(test_tick_past_window_always_rearms_and_repaints);
  return UNITY_END();
}
