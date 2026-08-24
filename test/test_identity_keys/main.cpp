#include <unity.h>

#include <cstring>

#include "nimbus/config/identity_keys.h"

using namespace nimbus::config;

void setUp() {}
void tearDown() {}

// Change-detector: the set of hardware-identity keys the factory reset must
// preserve is frozen. If a new identity key is added (e.g. a second panel-
// orientation setting), this test forces adding it here AND to the preserve loop
// in src/main.cpp - the whole point, so a TFT board never boots white again.
static void test_identity_key_set_is_frozen() {
  TEST_ASSERT_EQUAL_INT(3, kIdentityStrKeyCount);
  TEST_ASSERT_EQUAL_STRING("scrModel", kIdentityStrKeys[0]);
  TEST_ASSERT_EQUAL_STRING("tchCal", kIdentityStrKeys[1]);
  TEST_ASSERT_EQUAL_STRING("otaType", kIdentityStrKeys[2]);
  TEST_ASSERT_EQUAL_STRING("tftFlip", kIdentityIntKeyTftFlip);
}

// The identity keys must be distinct - a duplicate would mean the preserve loop
// silently skips a real key.
static void test_identity_keys_are_distinct() {
  for (int i = 0; i < kIdentityStrKeyCount; i++) {
    TEST_ASSERT_TRUE(strcmp(kIdentityStrKeys[i], kIdentityIntKeyTftFlip) != 0);
    for (int j = i + 1; j < kIdentityStrKeyCount; j++)
      TEST_ASSERT_TRUE(strcmp(kIdentityStrKeys[i], kIdentityStrKeys[j]) != 0);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_identity_key_set_is_frozen);
  RUN_TEST(test_identity_keys_are_distinct);
  return UNITY_END();
}
