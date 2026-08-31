// test_soak_state - CUM-257: the soak-observability fields that ride GET /api/state
// (uptimeMs, ringBackstopFires, touch health). Asserts every field appears with the
// right value, that uptime + the monotonic counters advance, and that a resistive
// board's touch block stays all-zero. This is the host seam for buildState()'s soak
// block: the device supplies the live values, this pins their shape and monotonicity.
#include <unity.h>

#include <ArduinoJson.h>

#include "nimbus/net/soak_state.h"

using nimbus::net::soakStateInto;
using solide::touch::Health;

void setUp() {}
void tearDown() {}

// Every documented field is present on the emitted object.
static void test_all_fields_present() {
  JsonDocument d;
  Health h;
  soakStateInto(d.to<JsonObject>(), 1234u, 0u, h);
  TEST_ASSERT_TRUE(d["uptimeMs"].is<uint32_t>());
  TEST_ASSERT_TRUE(d["ringBackstopFires"].is<uint32_t>());
  JsonObject t = d["touch"];
  TEST_ASSERT_FALSE(t.isNull());
  TEST_ASSERT_TRUE(t["failures"].is<uint32_t>());
  TEST_ASSERT_TRUE(t["recoveries"].is<uint32_t>());
  TEST_ASSERT_TRUE(t["busClears"].is<uint32_t>());
  TEST_ASSERT_TRUE(t["hardResets"].is<uint32_t>());
  TEST_ASSERT_TRUE(t["consecFailures"].is<uint32_t>());
  TEST_ASSERT_TRUE(t["lastRecoveryMs"].is<uint32_t>());
  TEST_ASSERT_TRUE(t["degraded"].is<bool>());
}

// Values are copied through faithfully.
static void test_values_passthrough() {
  JsonDocument d;
  Health h;
  h.failures = 7; h.recoveries = 3; h.busClears = 5; h.hardResets = 2;
  h.consecutiveFailures = 4; h.lastRecoveryMs = 99000; h.degraded = true;
  soakStateInto(d.to<JsonObject>(), 55u, 9u, h);
  TEST_ASSERT_EQUAL_UINT32(55u, d["uptimeMs"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(9u, d["ringBackstopFires"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(7u, d["touch"]["failures"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(3u, d["touch"]["recoveries"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(5u, d["touch"]["busClears"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(2u, d["touch"]["hardResets"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(4u, d["touch"]["consecFailures"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(99000u, d["touch"]["lastRecoveryMs"].as<uint32_t>());
  TEST_ASSERT_TRUE(d["touch"]["degraded"].as<bool>());
}

// Uptime and the monotonic counters only ever advance across two snapshots - the
// property the soak legs assert (a counter that went backwards is the bug).
static void test_monotonic_across_snapshots() {
  JsonDocument a, b;
  Health h0;
  soakStateInto(a.to<JsonObject>(), 1000u, 0u, h0);
  Health h1;
  h1.failures = 1; h1.recoveries = 1; h1.busClears = 1; h1.hardResets = 1;
  soakStateInto(b.to<JsonObject>(), 2000u, 1u, h1);

  TEST_ASSERT_TRUE(b["uptimeMs"].as<uint32_t>() > a["uptimeMs"].as<uint32_t>());
  TEST_ASSERT_TRUE(b["ringBackstopFires"].as<uint32_t>() >= a["ringBackstopFires"].as<uint32_t>());
  TEST_ASSERT_TRUE(b["touch"]["failures"].as<uint32_t>() >= a["touch"]["failures"].as<uint32_t>());
  TEST_ASSERT_TRUE(b["touch"]["recoveries"].as<uint32_t>() >= a["touch"]["recoveries"].as<uint32_t>());
  TEST_ASSERT_TRUE(b["touch"]["busClears"].as<uint32_t>() >= a["touch"]["busClears"].as<uint32_t>());
  TEST_ASSERT_TRUE(b["touch"]["hardResets"].as<uint32_t>() >= a["touch"]["hardResets"].as<uint32_t>());
}

// A resistive board never touches the FT6336U ladder, so every touch counter and
// the degraded flag stay zero/false - the honest "nothing to report" shape.
static void test_resistive_board_all_zero() {
  JsonDocument d;
  Health h;  // default-constructed = a healthy/resistive board
  soakStateInto(d.to<JsonObject>(), 42u, 0u, h);
  TEST_ASSERT_EQUAL_UINT32(0u, d["touch"]["failures"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(0u, d["touch"]["recoveries"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(0u, d["touch"]["busClears"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(0u, d["touch"]["hardResets"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(0u, d["touch"]["consecFailures"].as<uint32_t>());
  TEST_ASSERT_EQUAL_UINT32(0u, d["touch"]["lastRecoveryMs"].as<uint32_t>());
  TEST_ASSERT_FALSE(d["touch"]["degraded"].as<bool>());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_all_fields_present);
  RUN_TEST(test_values_passthrough);
  RUN_TEST(test_monotonic_across_snapshots);
  RUN_TEST(test_resistive_board_all_zero);
  return UNITY_END();
}
