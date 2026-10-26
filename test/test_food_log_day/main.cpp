#include <unity.h>

#include <cstdlib>
#include <ctime>
#include <string>

#include "nimbus/orch/file_append.h"

// The food log's day (file_append.h logDayOf) and the 24-25 Oct 2026 replay.
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

const char kHead[] = "date,meal,items,kcal,protein_g";

// One keyed log write at the device's day for `now` (the playbook's call).
CsvUpsert logAt(std::string& file, time_t now, const std::string& rows) {
  const LogDay d = logDayOf(now, true);
  const CsvUpsert up = planCsvUpsert(file, std::string(kHead) + "\n" + rows, "meal", "kcal", d.date);
  if (up.ok && up.changed) file = up.out;
  return up;
}

double total(const std::string& file, const char* day) {
  double t = -1;
  TEST_ASSERT_TRUE(csvDayTotal(file, "kcal", day, t));
  return t;
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

// The 24-25 Oct 2026 scenario, replayed through the device's keyed write at the
// device's local day: meals across a local midnight, yesterday's dinner wrongly
// overwritten by this morning's breakfast and put right, a correction, then both
// days' totals. Totals exact, no row lost or moved.
static void test_replay_24_25_oct_across_a_london_midnight() {
  useTz(kLondon);
  std::string file;
  // Saturday 24 Oct (BST).
  TEST_ASSERT_TRUE(logAt(file, 1792848540, "2026-10-24,breakfast,sandwich + omelette + cappuccino,557,32\n").ok);
  TEST_ASSERT_TRUE(logAt(file, 1792848960, "2026-10-24,lunch,bread + salmon + beans,807,66\n").ok);
  CsvUpsert up = logAt(file, 1792861020, "2026-10-24,dinner,beef + skyr + chicken + eggplant,990,88\n");
  TEST_ASSERT_TRUE(up.ok);
  TEST_ASSERT_EQUAL_INT(2354, (int)up.todayTotal);   // no Friday pasta in Saturday's total
  // 00:30 BST on Sunday (23:30 UTC on Saturday): the local day is already the
  // 25th, so a snack eaten now is the 25th's, and "today" agrees.
  up = logAt(file, 1792884600, "2026-10-25,snack-0030,clementine,35,1\n");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_STRING("2026-10-25", up.days[0].date.c_str());
  // Sunday 08:19 GMT: the breakfast photo, first mistaken for last night's dinner.
  up = logAt(file, 1792916340, "2026-10-24,dinner,chicken + eggs + skyr + berries,945,107\n");
  TEST_ASSERT_TRUE(up.ok);
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  TEST_ASSERT_EQUAL_STRING("990", up.changes[0].was.c_str());   // the reply shows the figure that was there
  // The owner: "that is today's breakfast". One call puts both days right.
  up = logAt(file, 1792916340,
             "2026-10-24,dinner,beef + skyr + chicken + eggplant,990,88\n"
             "2026-10-25,breakfast,chicken + eggs + skyr + berries,945,107\n");
  TEST_ASSERT_TRUE_MESSAGE(up.ok, up.err.c_str());
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  TEST_ASSERT_EQUAL_INT(1, up.appended);
  // Coffee, then a correction to the breakfast (smaller chicken breasts).
  TEST_ASSERT_TRUE(logAt(file, 1792917040, "2026-10-25,coffee-0831,coffee + 100 ml whole milk,66,3\n").ok);
  up = logAt(file, 1792917107, "2026-10-25,breakfast,chicken 225 g + eggs + skyr + berries,945,107\n");
  TEST_ASSERT_TRUE(up.ok);
  up = logAt(file, 1792917200, "2026-10-25,breakfast,chicken 225 g + eggs + skyr + berries,910,103\n");
  TEST_ASSERT_TRUE(up.ok);
  TEST_ASSERT_EQUAL_INT(1, up.replaced);
  // Both days' totals, exact: the correction counted once, nothing moved.
  TEST_ASSERT_EQUAL_INT(557 + 807 + 990, (int)total(file, "2026-10-24"));
  TEST_ASSERT_EQUAL_INT(35 + 910 + 66, (int)total(file, "2026-10-25"));
  TEST_ASSERT_EQUAL_INT((int)total(file, "2026-10-25"), (int)up.todayTotal);
  TEST_ASSERT_EQUAL_STRING(
      "date,meal,items,kcal,protein_g\n"
      "2026-10-24,breakfast,sandwich + omelette + cappuccino,557,32\n"
      "2026-10-24,lunch,bread + salmon + beans,807,66\n"
      "2026-10-24,dinner,beef + skyr + chicken + eggplant,990,88\n"
      "2026-10-25,snack-0030,clementine,35,1\n"
      "2026-10-25,breakfast,chicken 225 g + eggs + skyr + berries,910,103\n"
      "2026-10-25,coffee-0831,coffee + 100 ml whole milk,66,3\n",
      file.c_str());
}

// The counter-case: the same 00:30 snack on a device with no time zone. Its
// "today" is still the 24th (UTC), so a row dated the owner's 25th is refused as
// after today, and the reply's zone says UTC - the mismatch is visible, never a
// silent wrong day.
static void test_utc_device_refuses_the_owners_tomorrow_visibly() {
  useTz("UTC0");
  std::string file = std::string(kHead) + "\n";
  const LogDay d = logDayOf(1792884600, false);
  CsvUpsertStream m("meal", "kcal", d.date);
  m.setZone(d.zone);   // the device names the zone (files_subsystem upsertRows)
  TEST_ASSERT_FALSE(m.begin(std::string(kHead) + "\n2026-10-25,snack-0030,clementine,35,1\n"));
  TEST_ASSERT_TRUE_MESSAGE(m.result().err.find("after today (2026-10-24, UTC, no time zone is set)") !=
                               std::string::npos,
                           m.result().err.c_str());
  (void)file;
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_london_midnight_across_bst_and_gmt);
  RUN_TEST(test_no_timezone_is_utc_and_says_so);
  RUN_TEST(test_replay_24_25_oct_across_a_london_midnight);
  RUN_TEST(test_utc_device_refuses_the_owners_tomorrow_visibly);
  return UNITY_END();
}
