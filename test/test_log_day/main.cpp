#include <unity.h>

#include <cstdlib>
#include <ctime>
#include <string>

#include "nimbus/orch/file_append.h"

// The log's day (file_append.h logDayOf) and the 24-25 Oct 2026 replay, on three tables.
//
// The day boundary is the device's LOCAL midnight in the owner's time zone (the
// devTz POSIX setting the device applies as TZ). Europe/London changes offset
// twice a year; on 25 Oct 2026 BST ended at 01:00 UTC, in the middle of the
// owner's two bad days. With no time zone set the device runs on UTC and every
// total says so.

using namespace nimbus::orch;

namespace {
const char kLondon[] = "GMT0BST,M3.5.0/1,M10.5.0";   // what the wizard sets for Europe/London

void useTz(const char* tz) {
  setenv("TZ", tz, 1);
  tzset();
}

// files.append at the device's day for `now`: a pure append (owner ruling 41).
void logAt(std::string& file, const std::string& rows) {
  const AppendPlan p = planAppend(rows, file.size(), file.empty() ? '\0' : file.back());
  TEST_ASSERT_TRUE_MESSAGE(p.ok, p.err.c_str());
  file += p.bytes;
}

// The day total line of files.read {tail, sum} at `now` (today), as the device words it.
std::string todayLine(const std::string& file, const char* col, time_t now, bool tzSet) {
  const LogDay d = logDayOf(now, tzSet);
  return csvDayTotalLine(file, col, d.date, d.zone);
}
}  // namespace

void setUp() {}
void tearDown() { useTz("UTC0"); }

// Midnight in Europe/London on both sides of each change (BST and GMT).
static void test_london_midnight_across_bst_and_gmt() {
  useTz(kLondon);
  struct Case {
    time_t utc;
    const char* day;
    const char* zone;
  };
  const Case cases[] = {
      {1792882799, "2026-10-24", "BST"},   // 22:59:59 UTC = 23:59:59 BST
      {1792882800, "2026-10-25", "BST"},   // 23:00:00 UTC = 00:00 BST: a new local day
      {1792889999, "2026-10-25", "BST"},   // 00:59:59 UTC = 01:59:59 BST
      {1792890000, "2026-10-25", "GMT"},   // 01:00:00 UTC: BST ends, 01:00 GMT
      {1792972799, "2026-10-25", "GMT"},   // 23:59:59 GMT
      {1792972800, "2026-10-26", "GMT"},   // 00:00 GMT
      {1774742399, "2026-03-28", "GMT"},   // 23:59:59 GMT, the night BST starts
      {1774745999, "2026-03-29", "GMT"},   // 00:59:59 GMT
      {1774746000, "2026-03-29", "BST"},   // 01:00 UTC = 02:00 BST
      {1774825199, "2026-03-29", "BST"},   // 22:59:59 UTC = 23:59:59 BST
      {1774825200, "2026-03-30", "BST"},   // 23:00 UTC = 00:00 BST
  };
  for (const Case& c : cases) {
    const LogDay d = logDayOf(c.utc, true);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.day, d.date.c_str(), std::to_string((long long)c.utc).c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.zone, d.zone.c_str(), std::to_string((long long)c.utc).c_str());
  }
  // Never synced (a 1970-ish clock): no day at all, never a wrong one.
  TEST_ASSERT_TRUE(logDayOf(1000, true).date.empty());
}

// No time zone set: the device runs on UTC (behaves as before) and the zone says
// so - the owner can see which boundary the total uses.
static void test_no_timezone_is_utc_and_says_so() {
  useTz("UTC0");
  const LogDay d = logDayOf(1792884600, false);   // 23:30 UTC on 24 Oct
  TEST_ASSERT_EQUAL_STRING("2026-10-24", d.date.c_str());
  TEST_ASSERT_EQUAL_STRING("UTC, no time zone is set", d.zone.c_str());
  // The same instant in London is already the 25th.
  useTz(kLondon);
  TEST_ASSERT_EQUAL_STRING("2026-10-25", logDayOf(1792884600, true).date.c_str());
}

// The 24-25 Oct 2026 shape, replayed through files.append at the device's local day on a food
// log, an expenses log and a training log with identical expectations: rows across a London
// midnight land on their local day, the 00:30 row is the 25th's, and each day's total counts its
// own rows only, whatever the table holds.
static void test_replay_24_25_oct_across_a_london_midnight() {
  useTz(kLondon);
  const char* tables[][2] = {{"date,time,item,qty,kcal", "kcal"},
                             {"date,time,item,qty,amount", "amount"},
                             {"date,time,item,qty,kg", "kg"}};
  for (const auto& t : tables) {
    std::string file = std::string(t[0]) + "\n";
    logAt(file, "2026-10-24,08:29,a,1,557\n2026-10-24,12:36,b,1,807\n");
    logAt(file, "2026-10-24,21:57,c,1,990");
    TEST_ASSERT_EQUAL_STRING(("today's " + std::string(t[1]) + " (2026-10-24, BST): 2354 from 3 rows\n").c_str(),
                             todayLine(file, t[1], 1792861020, true).c_str());
    // 00:30 BST on Sunday (23:30 UTC on Saturday): the local day is already the 25th.
    TEST_ASSERT_EQUAL_STRING("2026-10-25", logDayOf(1792884600, true).date.c_str());
    logAt(file, "2026-10-25,00:30,d,1,35\n");
    logAt(file, "2026-10-25,08:19,e,1,910\n");
    TEST_ASSERT_EQUAL_STRING(("today's " + std::string(t[1]) + " (2026-10-25, GMT): 945 from 2 rows\n").c_str(),
                             todayLine(file, t[1], 1792916340, true).c_str());
    // Yesterday is untouched by today's rows.
    TEST_ASSERT_EQUAL_STRING(("today's " + std::string(t[1]) + " (2026-10-24, GMT): 2354 from 3 rows\n").c_str(),
                             csvDayTotalLine(file, t[1], "2026-10-24", "GMT").c_str());
  }
}

// The counter-case: the same 00:30 row on a device with no time zone. Its "today" is still the
// 24th (UTC), and the total line names the zone, so the mismatch is visible, never a silent
// wrong day. The row itself is appended as sent (no refusal).
static void test_a_utc_device_names_its_day_visibly() {
  useTz("UTC0");
  std::string file = "date,time,item,qty,amount\n";
  logAt(file, "2026-10-25,00:30,d,1,35\n");
  TEST_ASSERT_EQUAL_STRING("date,time,item,qty,amount\n2026-10-25,00:30,d,1,35\n", file.c_str());
  const std::string line = todayLine(file, "amount", 1792884600, false);
  TEST_ASSERT_TRUE_MESSAGE(line.find("(2026-10-24, UTC, no time zone is set): 0") != std::string::npos, line.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_london_midnight_across_bst_and_gmt);
  RUN_TEST(test_no_timezone_is_utc_and_says_so);
  RUN_TEST(test_replay_24_25_oct_across_a_london_midnight);
  RUN_TEST(test_a_utc_device_names_its_day_visibly);
  return UNITY_END();
}
