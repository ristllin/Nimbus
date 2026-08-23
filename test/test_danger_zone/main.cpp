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

// Identity preservation: exactly the device name is on the factory-reset keep list
// (a regression pin so nothing else silently survives a "factory" reset).
static void test_factory_keeps_only_identity() {
  TEST_ASSERT_EQUAL_INT(1, kFactoryKeepKeyCount);
  TEST_ASSERT_EQUAL_STRING("nimbus_name", kFactoryKeepKeys[0]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_confirm_phrases_are_exact_and_distinct);
  RUN_TEST(test_factory_keeps_only_identity);
  return UNITY_END();
}
