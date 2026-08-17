// Host tests for nimbus::fault - the runtime capability-fault registry that the
// resilience suite uses to drive degraded paths on demand (see lib/core/src/fault.cpp).
// The DEGRADED LOGIC each fault gates (VDB cap/eviction, corrupt-blob tolerance) is
// covered in test_orch_vector; here we pin the registry mechanics themselves.
#include <unity.h>

#include "nimbus/fault.h"

using namespace nimbus::fault;

void setUp() { clearAll(); }
void tearDown() { clearAll(); }

static void test_default_all_clear() {
  TEST_ASSERT_EQUAL_UINT16(0, mask());
  for (uint8_t i = 0; i < COUNT; i++) TEST_ASSERT_FALSE(active(Cap(i)));
}

static void test_set_and_clear_single() {
  set(MIC, true);
  TEST_ASSERT_TRUE(active(MIC));
  TEST_ASSERT_FALSE(active(SPEAKER));                 // isolation: only MIC set
  TEST_ASSERT_EQUAL_UINT16(uint16_t(1) << MIC, mask());
  set(MIC, false);
  TEST_ASSERT_FALSE(active(MIC));
  TEST_ASSERT_EQUAL_UINT16(0, mask());
}

static void test_independent_bits() {
  set(SD, true);
  set(SCREEN, true);
  TEST_ASSERT_TRUE(active(SD));
  TEST_ASSERT_TRUE(active(SCREEN));
  TEST_ASSERT_FALSE(active(LED));
  TEST_ASSERT_EQUAL_UINT16((uint16_t(1) << SD) | (uint16_t(1) << SCREEN), mask());
  set(SD, false);
  TEST_ASSERT_FALSE(active(SD));
  TEST_ASSERT_TRUE(active(SCREEN));                   // clearing one leaves the other
}

static void test_clear_all() {
  set(SD, true);
  set(MEMORY, true);
  set(LED, true);
  clearAll();
  TEST_ASSERT_EQUAL_UINT16(0, mask());
  for (uint8_t i = 0; i < COUNT; i++) TEST_ASSERT_FALSE(active(Cap(i)));
}

static void test_idempotent_set() {
  set(MEMORY, true);
  set(MEMORY, true);                                  // double-set is idempotent
  TEST_ASSERT_EQUAL_UINT16(uint16_t(1) << MEMORY, mask());
  set(MEMORY, false);
  set(MEMORY, false);                                 // double-clear is safe
  TEST_ASSERT_EQUAL_UINT16(0, mask());
}

static void test_parse_roundtrip() {
  Cap c;
  for (uint8_t i = 0; i < COUNT; i++) {
    TEST_ASSERT_TRUE(parse(name(Cap(i)), c));
    TEST_ASSERT_EQUAL_UINT8(i, (uint8_t)c);
  }
}

static void test_parse_case_insensitive() {
  Cap c;
  TEST_ASSERT_TRUE(parse("SD", c));       TEST_ASSERT_EQUAL(SD, c);
  TEST_ASSERT_TRUE(parse("Memory", c));   TEST_ASSERT_EQUAL(MEMORY, c);
  TEST_ASSERT_TRUE(parse("SPEAKER", c));  TEST_ASSERT_EQUAL(SPEAKER, c);
}

// sd_io is a DISTINCT capability from sd: injecting one must not set the other
// (sd = absent -> tier caps; sd_io = present-but-writes-fail -> demote path).
static void test_sd_io_distinct_from_sd() {
  clearAll();
  Cap c;
  TEST_ASSERT_TRUE(parse("sd_io", c));   TEST_ASSERT_EQUAL(SD_IO, c);
  set(SD_IO, true);
  TEST_ASSERT_TRUE(active(SD_IO));
  TEST_ASSERT_FALSE(active(SD));         // isolated: sd stays clear
  set(SD, true);
  TEST_ASSERT_TRUE(active(SD) && active(SD_IO));
  set(SD_IO, false);
  TEST_ASSERT_TRUE(active(SD) && !active(SD_IO));
  clearAll();
}

static void test_parse_rejects_unknown() {
  Cap c = SD;
  TEST_ASSERT_FALSE(parse("wifi", c));
  TEST_ASSERT_FALSE(parse("", c));
  TEST_ASSERT_FALSE(parse(nullptr, c));
  TEST_ASSERT_FALSE(parse("sdd", c));     // a valid name plus extra chars must not match
  TEST_ASSERT_FALSE(parse("s", c));       // a strict prefix must not match
}

static void test_out_of_range_cap_is_safe() {
  set(COUNT, true);                        // out of range -> no-op, no OOB write
  TEST_ASSERT_EQUAL_UINT16(0, mask());
  TEST_ASSERT_FALSE(active(COUNT));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_default_all_clear);
  RUN_TEST(test_set_and_clear_single);
  RUN_TEST(test_independent_bits);
  RUN_TEST(test_clear_all);
  RUN_TEST(test_idempotent_set);
  RUN_TEST(test_parse_roundtrip);
  RUN_TEST(test_parse_case_insensitive);
  RUN_TEST(test_sd_io_distinct_from_sd);
  RUN_TEST(test_parse_rejects_unknown);
  RUN_TEST(test_out_of_range_cap_is_safe);
  return UNITY_END();
}
