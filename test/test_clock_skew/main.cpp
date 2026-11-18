// The bench console's CLOCK verb arithmetic (nimbus/clock_skew.h): the parser refuses
// every malformed form, the held skew is capped like nimbusd's test clock, and a wall-time
// skew lands on the next HH:MM local time, across midnight and across a DST change.
#include <unity.h>

#include <cstdlib>
#include <ctime>

#include "nimbus/clock_skew.h"

using namespace nimbus::clockskew;

void setUp() {}
void tearDown() {}

static void london() {
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);   // the POSIX form the device applies
  tzset();
}

static time_t local(int y, int mo, int d, int h, int mi) {
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_isdst = -1;
  return mktime(&t);
}

static void test_parse_every_form() {
  TEST_ASSERT_TRUE(parse("?").op == Op::Read);
  TEST_ASSERT_TRUE(parse("off").op == Op::Off);
  Cmd f = parse("+3600");
  TEST_ASSERT_TRUE(f.op == Op::Forward);
  TEST_ASSERT_EQUAL_INT64(3600, f.value);
  Cmd a = parse("at 04:00");
  TEST_ASSERT_TRUE(a.op == Op::At);
  TEST_ASSERT_EQUAL_INT(4, a.hour);
  TEST_ASSERT_EQUAL_INT(0, a.minute);
  Cmd s = parse("1795000000");
  TEST_ASSERT_TRUE(s.op == Op::Set);
  TEST_ASSERT_EQUAL_INT64(1795000000LL, s.value);
  TEST_ASSERT_TRUE(parse("+31622400").op == Op::Forward);   // 366 days exactly
}

static void test_parse_refuses_the_rest() {
  const char* bad[] = {"", "+", "+0", "-60", "+31622401", "+1e3", "+ 60", "+60s", "at 4:00", "at 24:00",
                       "at 04:60", "at 04:00 ", "at", "OFF", "??", "12ab", "1234567890123456", " 60"};
  for (const char* b : bad) TEST_ASSERT_TRUE_MESSAGE(parse(b).op == Op::Bad, b);
}

static void test_the_held_skew_is_capped() {
  int64_t out = -1;
  TEST_ASSERT_TRUE(addSkew(0, 60, &out));
  TEST_ASSERT_EQUAL_INT64(60, out);
  TEST_ASSERT_TRUE(addSkew(kMaxSkewSec - kMaxStepSec, kMaxStepSec, &out));
  TEST_ASSERT_EQUAL_INT64(kMaxSkewSec, out);
  out = 7;
  TEST_ASSERT_FALSE(addSkew(kMaxSkewSec, 1, &out));
  TEST_ASSERT_FALSE(addSkew(0, 0, &out));
  TEST_ASSERT_FALSE(addSkew(0, kMaxStepSec + 1, &out));
  TEST_ASSERT_EQUAL_INT64(7, out);   // a refused step moves nothing
}

static void test_0400_rollover_lands_on_the_next_0400() {
  london();
  TEST_ASSERT_EQUAL_INT64(local(2026, 11, 18, 4, 0), nextLocal(local(2026, 11, 17, 23, 10), 4, 0));
  TEST_ASSERT_EQUAL_INT64(local(2026, 11, 18, 4, 0), nextLocal(local(2026, 11, 18, 3, 59), 4, 0));
  // Strictly ahead: at 04:00 it is the next day's.
  TEST_ASSERT_EQUAL_INT64(local(2026, 11, 19, 4, 0), nextLocal(local(2026, 11, 18, 4, 0), 4, 0));
  // Month and year ends roll the calendar.
  TEST_ASSERT_EQUAL_INT64(local(2027, 1, 1, 4, 0), nextLocal(local(2026, 12, 31, 22, 0), 4, 0));
}

static void test_the_night_summer_time_ends() {
  london();
  // 25 Oct 2026: BST ends at 02:00. 23:00 BST to 04:00 GMT is six real hours.
  const time_t evening = local(2026, 10, 24, 23, 0);
  TEST_ASSERT_EQUAL_INT64(6 * 3600, (int64_t)(nextLocal(evening, 4, 0) - evening));
  // 29 Mar 2026: BST starts at 01:00. 23:00 GMT to 04:00 BST is four real hours.
  const time_t spring = local(2026, 3, 28, 23, 0);
  TEST_ASSERT_EQUAL_INT64(4 * 3600, (int64_t)(nextLocal(spring, 4, 0) - spring));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_every_form);
  RUN_TEST(test_parse_refuses_the_rest);
  RUN_TEST(test_the_held_skew_is_capped);
  RUN_TEST(test_0400_rollover_lands_on_the_next_0400);
  RUN_TEST(test_the_night_summer_time_ends);
  return UNITY_END();
}
