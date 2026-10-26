#include <unity.h>

#include "nimbus/orch/onboard_gate.h"

// QW5: an abandoned setup wizard was marked finished on the next boot, because the
// boot migration counted saved Wi-Fi (which the wizard's own Wi-Fi step stores) as
// "set up before the wizard existed". The whole truth table: only saved Wi-Fi plus a
// verified provider, on a device not yet onboarded, migrates.

using nimbus::orch::migrateOnboarded;

void setUp() {}
void tearDown() {}

static void test_truth_table() {
  for (int m = 0; m < 8; ++m) {
    const bool onboarded = m & 1, wifi = m & 2, verified = m & 4;
    const bool want = !onboarded && wifi && verified;
    TEST_ASSERT_EQUAL_MESSAGE(want, migrateOnboarded(onboarded, wifi, verified), "row");
  }
}

// The finding's exact scenario: Wi-Fi saved, no verified provider yet, restart.
static void test_wizard_left_at_the_provider_step_is_not_finished() {
  TEST_ASSERT_FALSE(migrateOnboarded(false, true, false));
}

// A device upgraded from before the wizard (Wi-Fi + a working key) skips setup.
static void test_pre_wizard_device_still_migrates() {
  TEST_ASSERT_TRUE(migrateOnboarded(false, true, true));
}

// A factory-fresh device keeps the wizard.
static void test_fresh_device_keeps_the_wizard() {
  TEST_ASSERT_FALSE(migrateOnboarded(false, false, false));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_truth_table);
  RUN_TEST(test_wizard_left_at_the_provider_step_is_not_finished);
  RUN_TEST(test_pre_wizard_device_still_migrates);
  RUN_TEST(test_fresh_device_keeps_the_wizard);
  return UNITY_END();
}
