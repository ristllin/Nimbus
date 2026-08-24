#include <unity.h>

#include "nimbus/cloud/loopback_target.h"

using nimbus::cloud::loopbackFallbackUsable;

void setUp() {}
void tearDown() {}

static uint32_t ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | uint32_t(d);
}

// The bug this closes: WiFi.localIP() can be 0.0.0.0 right after a (re)join, and the
// old fallback dialed it -> guaranteed 502.
static void test_zero_ip_is_never_a_fallback() {
  TEST_ASSERT_FALSE(loopbackFallbackUsable(ip(0, 0, 0, 0)));
}

// 127/8 is already the primary target, so it is not a distinct fallback.
static void test_loopback_range_is_not_a_fallback() {
  TEST_ASSERT_FALSE(loopbackFallbackUsable(ip(127, 0, 0, 1)));
  TEST_ASSERT_FALSE(loopbackFallbackUsable(ip(127, 255, 255, 255)));
}

// A real STA lease is a usable fallback.
static void test_real_sta_ip_is_usable() {
  TEST_ASSERT_TRUE(loopbackFallbackUsable(ip(192, 168, 50, 61)));
  TEST_ASSERT_TRUE(loopbackFallbackUsable(ip(10, 0, 0, 2)));
  TEST_ASSERT_TRUE(loopbackFallbackUsable(ip(172, 16, 3, 9)));
  TEST_ASSERT_TRUE(loopbackFallbackUsable(ip(126, 0, 0, 1)));   // just below the 127/8 block
  TEST_ASSERT_TRUE(loopbackFallbackUsable(ip(128, 0, 0, 1)));   // just above it
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_zero_ip_is_never_a_fallback);
  RUN_TEST(test_loopback_range_is_not_a_fallback);
  RUN_TEST(test_real_sta_ip_is_usable);
  return UNITY_END();
}
