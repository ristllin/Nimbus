#include <unity.h>

#include "nimbus/cloud/relay_timing.h"

using nimbus::cloud::kRelaySlotHoldFloorMs;
using nimbus::cloud::kRelaySlotHoldMarginMs;
using nimbus::cloud::kTaskWdtTimeoutMs;
using nimbus::cloud::relaySlotHoldBudgetMs;

void setUp() {}
void tearDown() {}

// The whole point of the budget: at the real watchdog it leaves the configured
// margin, so a fed waiter can take the freed slot and reset the dog before 8 s.
static void test_budget_at_the_real_watchdog() {
  TEST_ASSERT_EQUAL_UINT32(kTaskWdtTimeoutMs - kRelaySlotHoldMarginMs,
                           relaySlotHoldBudgetMs(kTaskWdtTimeoutMs));
  // and that is comfortably under the watchdog (the reset-avoidance invariant).
  TEST_ASSERT_TRUE(relaySlotHoldBudgetMs(kTaskWdtTimeoutMs) < kTaskWdtTimeoutMs);
}

// The core safety property across a wide range: the hold budget is ALWAYS strictly
// less than the watchdog whenever the watchdog has room for a margin.
static void test_budget_always_under_watchdog() {
  for (uint32_t wdt = kRelaySlotHoldMarginMs + 1; wdt <= 30000; wdt += 250)
    TEST_ASSERT_TRUE(relaySlotHoldBudgetMs(wdt) < wdt);
}

// Never so small that a TLS handshake cannot fit (a half-budget that always fails
// to pair is its own bug).
static void test_budget_never_below_floor() {
  for (uint32_t wdt = 1; wdt <= 30000; wdt += 137)
    TEST_ASSERT_TRUE(relaySlotHoldBudgetMs(wdt) >= (wdt < kRelaySlotHoldFloorMs ? wdt : kRelaySlotHoldFloorMs));
}

// Monotonic: a longer watchdog never yields a shorter budget.
static void test_budget_monotonic() {
  uint32_t prev = 0;
  for (uint32_t wdt = 0; wdt <= 30000; wdt += 500) {
    uint32_t b = relaySlotHoldBudgetMs(wdt);
    TEST_ASSERT_TRUE(b >= prev);
    prev = b;
  }
}

// A degenerate tiny watchdog still bounds the hold to at most the watchdog itself.
static void test_tiny_watchdog_is_bounded() {
  TEST_ASSERT_TRUE(relaySlotHoldBudgetMs(1000) <= 1000);
  TEST_ASSERT_TRUE(relaySlotHoldBudgetMs(0) <= 0 + kRelaySlotHoldFloorMs);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_budget_at_the_real_watchdog);
  RUN_TEST(test_budget_always_under_watchdog);
  RUN_TEST(test_budget_never_below_floor);
  RUN_TEST(test_budget_monotonic);
  RUN_TEST(test_tiny_watchdog_is_bounded);
  return UNITY_END();
}
