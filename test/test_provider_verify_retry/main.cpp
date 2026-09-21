#include <unity.h>

#include <cstdint>

// The pure policy behind CUM-447's low-memory verify deferral: the reason token +
// number decision, and the bounded self-retry backoff. Host-testable because the
// header carries no Arduino/hardware dependency (the device seam in
// provider_verify.cpp owns the per-provider clocks and the hardware calls).
#include "../../src/agent/provider_verify_retry.h"

using agent::provider_verify::DeferralRetry;
using agent::provider_verify::deferReason;

void setUp() {}
void tearDown() {}

static const uint32_t kFloor = 8000;   // mirrors VERIFY_MIN_MAX8

// ---- the status token: below the gate -> "low-memory", the number rides along ----
// The class, not one instance: EVERY block strictly under the floor defers with the
// low-memory token; everything at/above it does not. A new gate value with no matching
// guard would make this fail.
static void test_defer_reason_is_the_class_not_a_point() {
  for (uint32_t max8 = 0; max8 < kFloor; max8 += 500) {
    TEST_ASSERT_EQUAL_STRING("low-memory", deferReason(max8, kFloor));
  }
  // The exact boundary and above are sufficient: no deferral, empty token.
  TEST_ASSERT_EQUAL_STRING("", deferReason(kFloor, kFloor));
  for (uint32_t max8 = kFloor; max8 < kFloor + 40000; max8 += 1000) {
    TEST_ASSERT_EQUAL_STRING("", deferReason(max8, kFloor));
  }
}

// ---- the backoff grows 2, 4, 8, 16 min then holds at the 16 min cap --------------
static void test_backoff_sequence_is_bounded_and_capped() {
  DeferralRetry r;
  const uint32_t kFirst = DeferralRetry::kFirstDelayMs;   // 2 min
  const uint32_t kMax   = DeferralRetry::kMaxDelayMs;     // 16 min
  TEST_ASSERT_EQUAL_UINT32(120000u, kFirst);
  TEST_ASSERT_EQUAL_UINT32(960000u, kMax);

  uint32_t now = 1000;
  r.onDeferred(now);
  TEST_ASSERT_EQUAL_UINT32(kFirst, r.delayMs);
  TEST_ASSERT_EQUAL_UINT32(now + kFirst, r.nextAtMs);

  uint32_t prev = r.delayMs;
  // Ten more consecutive deferrals: each step at most doubles, never shrinks, never
  // passes the cap, and once at the cap it stays there.
  for (int i = 0; i < 10; ++i) {
    now += r.delayMs;
    r.onDeferred(now);
    TEST_ASSERT_TRUE_MESSAGE(r.delayMs >= prev, "backoff must never shrink");
    TEST_ASSERT_TRUE_MESSAGE(r.delayMs <= 2u * prev, "backoff at most doubles");
    TEST_ASSERT_TRUE_MESSAGE(r.delayMs <= kMax, "backoff never exceeds the cap");
    prev = r.delayMs;
  }
  TEST_ASSERT_EQUAL_UINT32(kMax, r.delayMs);   // saturated at the cap
}

// A fresh arm (after a clear) restarts at the first delay, not wherever it saturated.
static void test_clear_resets_the_streak() {
  DeferralRetry r;
  uint32_t now = 5000;
  for (int i = 0; i < 6; ++i) { r.onDeferred(now); now += r.delayMs; }
  TEST_ASSERT_EQUAL_UINT32(DeferralRetry::kMaxDelayMs, r.delayMs);
  r.clear();
  TEST_ASSERT_FALSE(r.armed);
  TEST_ASSERT_EQUAL_UINT32(0u, r.delayMs);
  r.onDeferred(now);
  TEST_ASSERT_EQUAL_UINT32(DeferralRetry::kFirstDelayMs, r.delayMs);   // back to 2 min
}

// ---- due(): fires only when armed, past the backoff, and the device is free ------
static void test_due_respects_backoff_and_gates() {
  DeferralRetry r;
  const uint32_t t0 = 100000;
  // Not armed -> never due.
  TEST_ASSERT_FALSE(r.due(t0, /*inTurn=*/false, /*online=*/true, /*busy=*/false));

  r.onDeferred(t0);
  const uint32_t fire = r.nextAtMs;
  // Before the backoff elapses: not due even when the device is idle and online.
  TEST_ASSERT_FALSE(r.due(fire - 1, false, true, false));
  // At/after the backoff, device free: due.
  TEST_ASSERT_TRUE(r.due(fire, false, true, false));
  TEST_ASSERT_TRUE(r.due(fire + 60000, false, true, false));
}

// The three gates that must hold the retry back even when the backoff is due.
static void test_due_is_skipped_during_a_turn_offline_or_busy() {
  DeferralRetry r;
  const uint32_t t0 = 200000;
  r.onDeferred(t0);
  const uint32_t fire = r.nextAtMs;
  TEST_ASSERT_TRUE(r.due(fire, false, true, false));         // baseline: would fire
  TEST_ASSERT_FALSE(r.due(fire, /*inTurn=*/true,  true,  false));  // a turn holds the TLS slot
  TEST_ASSERT_FALSE(r.due(fire, false, /*online=*/false, false));  // no network
  TEST_ASSERT_FALSE(r.due(fire, false, true, /*busy=*/true));      // a verify already queued
}

// ---- stuckFor(): the Memory health line only after the window ---------------------
static void test_stuck_window_tracks_the_first_defer_not_the_last() {
  DeferralRetry r;
  const uint32_t t0 = 300000;
  const uint32_t win = 1200000;   // 20 min, mirrors kStuckWindowMs
  r.onDeferred(t0);
  TEST_ASSERT_FALSE(r.stuckFor(t0, win));
  TEST_ASSERT_FALSE(r.stuckFor(t0 + win - 1, win));
  // Later deferrals in the same streak must NOT reset the "since first" clock.
  r.onDeferred(t0 + 130000);
  r.onDeferred(t0 + 400000);
  TEST_ASSERT_TRUE(r.stuckFor(t0 + win, win));
  // Once cleared, it is no longer stuck.
  r.clear();
  TEST_ASSERT_FALSE(r.stuckFor(t0 + win + 1000000, win));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_defer_reason_is_the_class_not_a_point);
  RUN_TEST(test_backoff_sequence_is_bounded_and_capped);
  RUN_TEST(test_clear_resets_the_streak);
  RUN_TEST(test_due_respects_backoff_and_gates);
  RUN_TEST(test_due_is_skipped_during_a_turn_offline_or_busy);
  RUN_TEST(test_stuck_window_tracks_the_first_defer_not_the_last);
  return UNITY_END();
}
