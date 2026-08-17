#include <unity.h>

#include "nimbus/touch_cal.h"

// A resistive panel's calibration is MEASURED per unit and then stored, so the
// parse sits between a human typing corner values and the driver that maps
// every tap. A silently-wrong parse here is indistinguishable from broken
// hardware - hence the emphasis on rejecting rather than coercing.

using nimbus::touch::Cal;
using nimbus::touch::formatCal;
using nimbus::touch::parseCal;

void setUp() {}
void tearDown() {}

static void test_parses_four_fields() {
  Cal c;
  TEST_ASSERT_TRUE(parseCal("200,3900,240,3850", c));
  TEST_ASSERT_EQUAL_UINT16(200, c.minX);
  TEST_ASSERT_EQUAL_UINT16(3900, c.maxX);
  TEST_ASSERT_EQUAL_UINT16(240, c.minY);
  TEST_ASSERT_EQUAL_UINT16(3850, c.maxY);
  TEST_ASSERT_FALSE(c.swapXY);
  TEST_ASSERT_FALSE(c.invertX);
  TEST_ASSERT_FALSE(c.invertY);
}

static void test_parses_flags() {
  Cal c;
  TEST_ASSERT_TRUE(parseCal("200,3900,240,3850,7", c));
  TEST_ASSERT_TRUE(c.swapXY);
  TEST_ASSERT_TRUE(c.invertX);
  TEST_ASSERT_TRUE(c.invertY);

  TEST_ASSERT_TRUE(parseCal("200,3900,240,3850,4", c));
  TEST_ASSERT_FALSE(c.swapXY);
  TEST_ASSERT_FALSE(c.invertX);
  TEST_ASSERT_TRUE(c.invertY);
}

static void test_round_trips() {
  Cal a;
  a.minX = 310; a.maxX = 3700; a.minY = 280; a.maxY = 3820;
  a.swapXY = true; a.invertY = true;
  Cal b;
  TEST_ASSERT_TRUE(parseCal(formatCal(a), b));
  TEST_ASSERT_TRUE(a == b);
}

// A partially-applied calibration is WORSE than the default, because it looks
// deliberate. Every malformed input must leave the caller's value untouched.
static void test_rejects_malformed_without_mutating() {
  const Cal original;                      // defaults
  const char* bad[] = {
      "",                       // empty
      "200,3900,240",           // too few
      "200,3900,240,3850,1,2",  // too many
      "200,,240,3850",          // empty field
      "200,3900,240,abc",       // non-numeric
      "200,3900,240,-50",       // negative
      "-1,3900,240,3850",       // negative
      "200,3900,240,9999",      // beyond the 12-bit ADC
      "3900,200,240,3850",      // min > max on X
      "200,3900,3850,240",      // min > max on Y
      "200,200,240,3850",       // zero span (would divide by zero)
      "200,3900,240,3850,8",    // flags out of range
  };
  for (const char* s : bad) {
    Cal c = original;
    TEST_ASSERT_FALSE_MESSAGE(parseCal(s, c), s);
    TEST_ASSERT_TRUE_MESSAGE(c == original, s);   // untouched
  }
}

static void test_accepts_boundaries() {
  Cal c;
  TEST_ASSERT_TRUE(parseCal("0,4095,0,4095,0", c));
  TEST_ASSERT_EQUAL_UINT16(0, c.minX);
  TEST_ASSERT_EQUAL_UINT16(4095, c.maxX);
}

static void test_tolerates_spaces() {
  Cal c;
  TEST_ASSERT_TRUE(parseCal(" 200 , 3900 , 240 , 3850 ", c));
  TEST_ASSERT_EQUAL_UINT16(3900, c.maxX);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parses_four_fields);
  RUN_TEST(test_parses_flags);
  RUN_TEST(test_round_trips);
  RUN_TEST(test_rejects_malformed_without_mutating);
  RUN_TEST(test_accepts_boundaries);
  RUN_TEST(test_tolerates_spaces);
  return UNITY_END();
}
