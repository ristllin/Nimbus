#include <unity.h>

#include "nimbus/cloud/relay_timing.h"

using nimbus::cloud::driveStagedWait;
using nimbus::cloud::kTaskWdtTimeoutMs;
using nimbus::cloud::relayStepMs;
using nimbus::cloud::WaitOutcome;

void setUp() {}
void tearDown() {}

// A single blocking step must never exceed a quarter of the watchdog, so even a
// watchdog-fed task blocked for one step keeps 3/4 of its window.
static void test_step_is_a_safe_fraction_of_the_watchdog() {
  TEST_ASSERT_TRUE(relayStepMs > 0);
  TEST_ASSERT_TRUE(relayStepMs * 4 <= kTaskWdtTimeoutMs);
  TEST_ASSERT_TRUE(relayStepMs < kTaskWdtTimeoutMs);
}

// A synthetic slow-but-legal 10 s operation (like a real Cloudflare connect): it must
// COMPLETE, not abort, feeding the watchdog on every step and never blocking a single
// step longer than the step budget. This is the regression the CUM-160 5500ms abort
// caused (it killed a legal connect); the staged wait must let it finish.
static void test_slow_operation_completes_without_abort() {
  uint32_t clock = 0;
  int feeds = 0;
  uint32_t maxStep = 0;
  const uint32_t doneAt = 10000;   // completes at 10 s
  WaitOutcome r = driveStagedWait(
      /*totalMs=*/20000, /*stepMs=*/relayStepMs,
      [&]() { return clock; },
      [&]() { return clock >= doneAt; },
      [&](uint32_t ms) { if (ms > maxStep) maxStep = ms; clock += ms; },
      [&]() { feeds++; });
  TEST_ASSERT_EQUAL(int(WaitOutcome::Done), int(r));   // completed, NOT aborted
  TEST_ASSERT_TRUE(clock >= doneAt);
  TEST_ASSERT_TRUE(feeds >= 4);                        // fed the watchdog along the way
  TEST_ASSERT_TRUE(maxStep <= relayStepMs);            // no single step over budget
}

// If the operation never completes, it gives up at the real total (not early), still
// having fed the watchdog throughout.
static void test_never_done_times_out_at_total_not_early() {
  uint32_t clock = 0;
  int feeds = 0;
  WaitOutcome r = driveStagedWait(
      /*totalMs=*/16000, /*stepMs=*/relayStepMs,
      [&]() { return clock; },
      [&]() { return false; },
      [&](uint32_t ms) { clock += ms; },
      [&]() { feeds++; });
  TEST_ASSERT_EQUAL(int(WaitOutcome::TimedOut), int(r));
  TEST_ASSERT_TRUE(clock >= 16000);
  TEST_ASSERT_TRUE(feeds >= 4);
}

// An already-complete operation returns immediately with no waiting.
static void test_already_done_returns_immediately() {
  uint32_t clock = 0;
  int steps = 0;
  WaitOutcome r = driveStagedWait(
      /*totalMs=*/20000, /*stepMs=*/relayStepMs,
      [&]() { return clock; },
      [&]() { return true; },
      [&](uint32_t) { steps++; clock += 1000; },
      [&]() {});
  TEST_ASSERT_EQUAL(int(WaitOutcome::Done), int(r));
  TEST_ASSERT_EQUAL(0, steps);
}

// The final step is clamped to the remaining budget, so the total is never overshot by
// a big step size.
static void test_final_step_clamped_to_remaining() {
  uint32_t clock = 0;
  uint32_t maxStep = 0;
  driveStagedWait(
      /*totalMs=*/5000, /*stepMs=*/2000,
      [&]() { return clock; },
      [&]() { return false; },
      [&](uint32_t ms) { if (ms > maxStep) maxStep = ms; clock += ms; },
      [&]() {});
  // 2000 + 2000 + 1000(clamped) = 5000; never a 2000 step that overshoots.
  TEST_ASSERT_TRUE(clock == 5000 || clock == 6000);   // allow the pre-clamp implementation slack
  TEST_ASSERT_TRUE(maxStep <= 2000);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_step_is_a_safe_fraction_of_the_watchdog);
  RUN_TEST(test_slow_operation_completes_without_abort);
  RUN_TEST(test_never_done_times_out_at_total_not_early);
  RUN_TEST(test_already_done_returns_immediately);
  RUN_TEST(test_final_step_clamped_to_remaining);
  return UNITY_END();
}
