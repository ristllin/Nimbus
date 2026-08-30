#include <unity.h>

#include "nimbus/orch/danger_zone.h"

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

// Each destructive action has its OWN exact phrase; a confirm for one never
// matches another, and a missing/empty/wrong phrase is rejected.
static void test_confirm_phrases_are_exact_and_distinct() {
  TEST_ASSERT_TRUE(confirmOk(kConfirmFactoryReset, "FACTORY RESET"));
  TEST_ASSERT_TRUE(confirmOk(kConfirmEraseStorage, "ERASE STORAGE"));
  TEST_ASSERT_TRUE(confirmOk(kConfirmFormatCard, "FORMAT CARD"));
  // wrong action's phrase never satisfies another action
  TEST_ASSERT_FALSE(confirmOk(kConfirmFactoryReset, "ERASE STORAGE"));
  TEST_ASSERT_FALSE(confirmOk(kConfirmFormatCard, "FACTORY RESET"));
  // case-sensitive, non-empty, no partials
  TEST_ASSERT_FALSE(confirmOk(kConfirmFactoryReset, "factory reset"));
  TEST_ASSERT_FALSE(confirmOk(kConfirmFactoryReset, "FACTORY RESET "));
  TEST_ASSERT_FALSE(confirmOk(kConfirmFactoryReset, "FACTORY"));
  TEST_ASSERT_FALSE(confirmOk(kConfirmFactoryReset, ""));
  TEST_ASSERT_FALSE(confirmOk(kConfirmFactoryReset, nullptr));
  // the three phrases are all distinct
  TEST_ASSERT_FALSE(confirmOk(kConfirmEraseStorage, kConfirmFormatCard));
}

// Nothing user-facing survives a factory reset (CUM-230 ruling): the keep-list beyond
// the board's physical identity is EMPTY - not even the device name. A regression pin
// so no secret or user setting is ever silently added to the keep-list, and so the
// device-name-wipe (fresh identity + mDNS on reset) can't quietly regress.
static void test_factory_keeps_nothing_user_facing() {
  TEST_ASSERT_EQUAL_INT(0, kFactoryKeepKeyCount);
}

// Factory reset seeds the operating mode to Orchestrator so a reset device boots into
// the Wi-Fi setup AP with the onboarding wizard reachable. Merely clearing the mode
// key defaults to Notifier (Wi-Fi off, no AP) - the CUM-230 field bug this pins shut.
static void test_factory_seeds_setup_mode() {
  TEST_ASSERT_EQUAL_INT(1, kFactoryResetSeedMode);  // == sys::Mode::Orchestrator
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_confirm_phrases_are_exact_and_distinct);
  RUN_TEST(test_factory_keeps_nothing_user_facing);
  RUN_TEST(test_factory_seeds_setup_mode);
  return UNITY_END();
}
