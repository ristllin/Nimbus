#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <string>

#include "nimbus/orch/day_start.h"

// test_stamp_day - the device's day start and the message stamp built on it (RB-REC5,
// rulings 11 and 42). By table: every hour around midnight and around the day start, both
// DST changes, New Year's Eve and Day, a leap day, a 00:00 start and odd starts, no clock,
// zones east and west of UTC. By property: the stamp agrees with the C library's calendar
// (localtime_r under a POSIX TZ) every 23 minutes over three years in eight zones, for the
// default start and for every start of the day; the year and the weekday are always
// there; the running day never runs backwards with the default start. Ruling 30: the
// stamp never allocates, so a refusing allocator still gets the whole stamp, and a buffer
// too short for the clause gets the plain stamp with its year.
namespace day = nimbus::orch::day;

// Ruling 30: the test binary's operator new refuses once gAllow allocations were made
// (-1 = never refuses); gNews counts every allocation.
static long gAllow = -1;
static long gNews = 0;
void* operator new(std::size_t n) {
  ++gNews;
  if (gAllow == 0) throw std::bad_alloc();
  if (gAllow > 0) --gAllow;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

void setUp() {}
void tearDown() {}

namespace {

// 2026-11-19 00:30 UTC (a Thursday).
constexpr uint64_t kThu0030 = 1795048200ULL;

std::string stampOf(uint64_t epoch, int32_t off, uint16_t start, day::StampForm* form = nullptr) {
  char buf[day::kFullCap];
  const day::StampForm f = day::stamp(epoch, off, start, buf, sizeof buf);
  if (form) *form = f;
  return buf;
}

void useTz(const char* tz) {
  setenv("TZ", tz, 1);
  tzset();
}

// The offset the C library gives `epoch` under the current TZ, in minutes east.
int32_t libOffset(time_t t) {
  struct tm lt, gt;
  localtime_r(&t, &lt);
  gmtime_r(&t, &gt);
  lt.tm_isdst = 0;
  gt.tm_isdst = 0;
  const int64_t l = (int64_t)timegm(&lt), g = (int64_t)timegm(&gt);
  return (int32_t)((l - g) / 60);
}

// The expected stamp from the C library's calendar alone: strftime for the stamp, the
// day before by normalizing tm_mday - 1 through timegm.
std::string expected(time_t t, uint16_t start) {
  struct tm lt;
  localtime_r(&t, &lt);
  char buf[64];
  strftime(buf, sizeof buf, "%Y-%m-%d %a %H:%M", &lt);
  std::string s = buf;
  if (lt.tm_hour * 60 + lt.tm_min < start) {
    struct tm y = lt;
    y.tm_isdst = 0;
    y.tm_hour = 12;
    y.tm_mday -= 1;
    const time_t yt = timegm(&y);
    struct tm yy;
    gmtime_r(&yt, &yy);
    char d[16];
    strftime(d, sizeof d, "%Y-%m-%d", &yy);
    char clause[48];
    std::snprintf(clause, sizeof clause, " (day %s until %02u:%02u)", d, start / 60, start % 60);
    s += clause;
  }
  return s;
}

uint64_t utc(int y, int mo, int d, int h, int mi) {
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  return (uint64_t)timegm(&t);
}

const char* const kZones[] = {
    "GMT0BST,M3.5.0/1,M10.5.0/2",       // UK: both DST changes at night
    "EST5EDT,M3.2.0,M11.1.0",           // US Eastern, west of UTC
    "PST8PDT,M3.2.0,M11.1.0",           // US Pacific
    "AEST-10AEDT,M10.1.0,M4.1.0/3",     // Sydney: east, southern DST
    "IST-5:30",                         // India: a half hour, no DST
    "<+0545>-5:45",                     // Nepal: a quarter hour
    "EET-2EEST,M3.5.0/3,M10.5.0/4",     // Eastern Europe: the fall back lands at 04:00
    "<-03>3",                           // Brazil, west, no DST
    "<+14>-14",                         // Kiribati: the far east
    "<-12>12",                          // the far west
};

}  // namespace

// ---- the table -----------------------------------------------------------------------------

void test_stamp_at_00_30_names_the_running_day() {
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 00:30 (day 2026-11-18 until 04:00)",
                           stampOf(kThu0030, 0, day::kDefaultStartMin).c_str());
}

void test_every_hour_around_midnight_and_the_day_start() {
  // 2026-11-18 20:00 to 2026-11-19 08:00 UTC, every hour, offset 0, start 04:00.
  const char* want[] = {
      "2026-11-18 Wed 20:00", "2026-11-18 Wed 21:00", "2026-11-18 Wed 22:00", "2026-11-18 Wed 23:00",
      "2026-11-19 Thu 00:00 (day 2026-11-18 until 04:00)", "2026-11-19 Thu 01:00 (day 2026-11-18 until 04:00)",
      "2026-11-19 Thu 02:00 (day 2026-11-18 until 04:00)", "2026-11-19 Thu 03:00 (day 2026-11-18 until 04:00)",
      "2026-11-19 Thu 04:00", "2026-11-19 Thu 05:00", "2026-11-19 Thu 06:00", "2026-11-19 Thu 07:00",
      "2026-11-19 Thu 08:00",
  };
  const uint64_t base = utc(2026, 11, 18, 20, 0);
  for (int h = 0; h <= 12; ++h)
    TEST_ASSERT_EQUAL_STRING(want[h], stampOf(base + h * 3600ULL, 0, 240).c_str());
  // The minute either side of the boundaries.
  TEST_ASSERT_EQUAL_STRING("2026-11-18 Wed 23:59", stampOf(utc(2026, 11, 18, 23, 59), 0, 240).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 03:59 (day 2026-11-18 until 04:00)",
                           stampOf(utc(2026, 11, 19, 3, 59), 0, 240).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 04:00", stampOf(utc(2026, 11, 19, 4, 0), 0, 240).c_str());
  // The last second before 04:00 still has the clause; the first second of 04:00 does not.
  TEST_ASSERT_NOT_NULL(std::strstr(stampOf(utc(2026, 11, 19, 4, 0) - 1, 0, 240).c_str(), "(day 2026-11-18"));
}

void test_new_year_and_a_leap_day() {
  struct Row {
    uint64_t epoch;
    int32_t off;
    const char* want;
  } rows[] = {
      {utc(2026, 12, 31, 23, 30), 0, "2026-12-31 Thu 23:30"},
      {utc(2027, 1, 1, 0, 30), 0, "2027-01-01 Fri 00:30 (day 2026-12-31 until 04:00)"},
      {utc(2027, 1, 1, 4, 0), 0, "2027-01-01 Fri 04:00"},
      {utc(2026, 12, 31, 23, 30), 60, "2027-01-01 Fri 00:30 (day 2026-12-31 until 04:00)"},
      {utc(2027, 1, 1, 0, 30), -300, "2026-12-31 Thu 19:30"},
      {utc(2028, 2, 29, 0, 30), 0, "2028-02-29 Tue 00:30 (day 2028-02-28 until 04:00)"},
      {utc(2028, 3, 1, 0, 30), 0, "2028-03-01 Wed 00:30 (day 2028-02-29 until 04:00)"},
      {utc(2027, 3, 1, 0, 30), 0, "2027-03-01 Mon 00:30 (day 2027-02-28 until 04:00)"},
      {utc(2100, 3, 1, 0, 30), 0, "2100-03-01 Mon 00:30 (day 2100-02-28 until 04:00)"},
      {utc(2000, 3, 1, 0, 30) + 400ULL * 0, 0, "time unknown"},   // before the clock-valid line
  };
  for (const Row& r : rows) TEST_ASSERT_EQUAL_STRING(r.want, stampOf(r.epoch, r.off, 240).c_str());
}

void test_dst_change_days_in_both_directions() {
  useTz(kZones[0]);   // UK: 29 Mar 2026 01:00 UTC springs to BST, 25 Oct 2026 01:00 UTC falls back
  struct Row {
    uint64_t epoch;
    const char* want;
  } rows[] = {
      {utc(2026, 3, 28, 23, 30), "2026-03-28 Sat 23:30"},
      {utc(2026, 3, 29, 0, 30), "2026-03-29 Sun 00:30 (day 2026-03-28 until 04:00)"},
      {utc(2026, 3, 29, 0, 59), "2026-03-29 Sun 00:59 (day 2026-03-28 until 04:00)"},
      {utc(2026, 3, 29, 1, 0), "2026-03-29 Sun 02:00 (day 2026-03-28 until 04:00)"},
      {utc(2026, 3, 29, 2, 59), "2026-03-29 Sun 03:59 (day 2026-03-28 until 04:00)"},
      {utc(2026, 3, 29, 3, 0), "2026-03-29 Sun 04:00"},
      {utc(2026, 10, 24, 23, 30), "2026-10-25 Sun 00:30 (day 2026-10-24 until 04:00)"},
      {utc(2026, 10, 25, 0, 30), "2026-10-25 Sun 01:30 (day 2026-10-24 until 04:00)"},
      {utc(2026, 10, 25, 1, 30), "2026-10-25 Sun 01:30 (day 2026-10-24 until 04:00)"},
      {utc(2026, 10, 25, 3, 59), "2026-10-25 Sun 03:59 (day 2026-10-24 until 04:00)"},
      {utc(2026, 10, 25, 4, 0), "2026-10-25 Sun 04:00"},
  };
  for (const Row& r : rows)
    TEST_ASSERT_EQUAL_STRING(r.want, stampOf(r.epoch, libOffset((time_t)r.epoch), 240).c_str());
  // US Eastern: 8 Mar 2026 07:00 UTC springs, 1 Nov 2026 06:00 UTC falls back.
  useTz(kZones[1]);
  TEST_ASSERT_EQUAL_STRING("2026-03-08 Sun 03:30 (day 2026-03-07 until 04:00)",
                           stampOf(utc(2026, 3, 8, 7, 30), libOffset((time_t)utc(2026, 3, 8, 7, 30)), 240).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-01 Sun 01:30 (day 2026-10-31 until 04:00)",
                           stampOf(utc(2026, 11, 1, 6, 30), libOffset((time_t)utc(2026, 11, 1, 6, 30)), 240).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-01 Sun 04:00",
                           stampOf(utc(2026, 11, 1, 9, 0), libOffset((time_t)utc(2026, 11, 1, 9, 0)), 240).c_str());
}

void test_zones_east_and_west_of_utc() {
  // One instant, 2026-11-19 00:30 UTC, in zones either side.
  struct Row {
    int32_t off;
    const char* want;
  } rows[] = {
      {0, "2026-11-19 Thu 00:30 (day 2026-11-18 until 04:00)"},
      {330, "2026-11-19 Thu 06:00"},
      {345, "2026-11-19 Thu 06:15"},
      {840, "2026-11-19 Thu 14:30"},
      {-180, "2026-11-18 Wed 21:30"},
      {-300, "2026-11-18 Wed 19:30"},
      {-720, "2026-11-18 Wed 12:30"},
      {180, "2026-11-19 Thu 03:30 (day 2026-11-18 until 04:00)"},
      {210, "2026-11-19 Thu 04:00"},
  };
  for (const Row& r : rows) TEST_ASSERT_EQUAL_STRING(r.want, stampOf(kThu0030, r.off, 240).c_str());
}

void test_a_00_00_start_never_has_a_clause_and_odd_starts_apply_exactly() {
  for (uint64_t t = utc(2026, 11, 18, 0, 0); t < utc(2026, 11, 20, 0, 0); t += 60)
    TEST_ASSERT_NULL(std::strstr(stampOf(t, 0, 0).c_str(), "(day"));
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 00:00 (day 2026-11-18 until 00:01)",
                           stampOf(utc(2026, 11, 19, 0, 0), 0, 1).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 00:01", stampOf(utc(2026, 11, 19, 0, 1), 0, 1).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 23:58 (day 2026-11-18 until 23:59)",
                           stampOf(utc(2026, 11, 19, 23, 58), 0, 23 * 60 + 59).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 23:59", stampOf(utc(2026, 11, 19, 23, 59), 0, 23 * 60 + 59).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 04:29 (day 2026-11-18 until 04:30)",
                           stampOf(utc(2026, 11, 19, 4, 29), 0, 270).c_str());
  // A start past the end of a day wraps (1440 is 00:00); the stored setting never holds one.
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 00:30", stampOf(kThu0030, 0, 1440).c_str());
}

void test_no_clock_is_said_plainly_and_never_dated() {
  for (uint64_t e : {0ULL, 1ULL, 3600ULL, 86400ULL * 365, 999999999ULL, 253402300800ULL, ~0ULL}) {
    day::StampForm f;
    const std::string s = stampOf(e, 0, 240, &f);
    TEST_ASSERT_EQUAL_STRING("time unknown", s.c_str());
    TEST_ASSERT_TRUE(f == day::StampForm::NoClock);
    char d[16] = "x";
    TEST_ASSERT_FALSE(day::runningDate(e, 0, 240, d, sizeof d));
    TEST_ASSERT_EQUAL_STRING("", d);
  }
  // The first second the clock counts as set.
  TEST_ASSERT_EQUAL_STRING("2001-09-09 Sun 01:46 (day 2001-09-08 until 04:00)", stampOf(day::kClockValidEpoch, 0, 240).c_str());
  TEST_ASSERT_EQUAL_STRING("time unknown", stampOf(day::kClockValidEpoch - 1, 0, 240).c_str());
  TEST_ASSERT_EQUAL_STRING("9999-12-31 Fri 23:59", stampOf(day::kLastEpoch, 0, 240).c_str());
}

void test_an_offset_out_of_range_writes_nothing() {
  for (int32_t off : {day::kMaxOffsetMin + 1, -day::kMaxOffsetMin - 1, 100000, -100000}) {
    char buf[day::kFullCap] = "x";
    TEST_ASSERT_TRUE(day::stamp(kThu0030, off, 240, buf, sizeof buf) == day::StampForm::None);
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_FALSE(day::runningDate(kThu0030, off, 240, buf, sizeof buf));
  }
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 18:30", stampOf(kThu0030, day::kMaxOffsetMin, 240).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-18 Wed 06:30", stampOf(kThu0030, -day::kMaxOffsetMin, 240).c_str());
}

// ---- buffers and the refusing allocator (ruling 30) --------------------------------------------

void test_a_short_buffer_gets_the_plain_stamp_with_its_year() {
  for (size_t cap = 0; cap <= day::kFullCap + 2; ++cap) {
    char buf[day::kFullCap + 8];
    std::memset(buf, '#', sizeof buf);
    const day::StampForm f = day::stamp(kThu0030, 0, 240, buf, cap);
    for (size_t i = cap; i < sizeof buf; ++i) TEST_ASSERT_EQUAL_CHAR('#', buf[i]);   // nothing past cap
    if (cap < day::kPlainCap) {
      TEST_ASSERT_TRUE(f == day::StampForm::None);
      if (cap) TEST_ASSERT_EQUAL_STRING("", buf);
    } else if (cap < day::kFullCap) {
      TEST_ASSERT_TRUE(f == day::StampForm::Plain);
      TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 00:30", buf);
    } else {
      TEST_ASSERT_TRUE(f == day::StampForm::Full);
      TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 00:30 (day 2026-11-18 until 04:00)", buf);
    }
  }
  // After the day start a plain-sized buffer is the whole stamp.
  char p[day::kPlainCap];
  TEST_ASSERT_TRUE(day::stamp(utc(2026, 11, 19, 8, 10), 0, 240, p, sizeof p) == day::StampForm::Full);
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 08:10", p);
  TEST_ASSERT_TRUE(day::stamp(kThu0030, 0, 240, nullptr, 64) == day::StampForm::None);
  TEST_ASSERT_EQUAL_UINT32(day::kPlainBytes, std::strlen("2026-11-19 Thu 08:10"));
  TEST_ASSERT_EQUAL_UINT32(day::kClauseBytes, std::strlen(" (day 2026-11-18 until 04:00)"));
}

void test_a_refusing_allocator_still_gets_the_whole_stamp() {
  gNews = 0;
  gAllow = 0;   // every allocation refused
  char buf[day::kFullCap], d[16];
  uint16_t m = 0;
  const day::StampForm f = day::stamp(kThu0030, 0, 240, buf, sizeof buf);
  const bool dated = day::runningDate(kThu0030, 0, 240, d, sizeof d);
  const char* err = day::parseStart("04:00", m);
  const uint16_t start = day::startOrDefault("bad");
  char hh[6];
  day::formatStart(start, hh);
  gAllow = -1;
  TEST_ASSERT_EQUAL_INT(0, (int)gNews);
  TEST_ASSERT_TRUE(f == day::StampForm::Full);
  TEST_ASSERT_EQUAL_STRING("2026-11-19 Thu 00:30 (day 2026-11-18 until 04:00)", buf);
  TEST_ASSERT_TRUE(dated);
  TEST_ASSERT_EQUAL_STRING("2026-11-18", d);
  TEST_ASSERT_NULL(err);
  TEST_ASSERT_EQUAL_STRING("04:00", hh);
}

// ---- the setting ---------------------------------------------------------------------------

void test_the_setting_reads_hh_mm_and_falls_back_to_04_00() {
  struct Good {
    const char* text;
    uint16_t min;
  } good[] = {{"00:00", 0}, {"04:00", 240}, {"04:30", 270}, {"12:00", 720}, {"23:59", 1439}, {"09:05", 545}};
  for (const Good& g : good) {
    uint16_t m = 9999;
    TEST_ASSERT_NULL(day::parseStart(g.text, m));
    TEST_ASSERT_EQUAL_UINT16(g.min, m);
    TEST_ASSERT_EQUAL_UINT16(g.min, day::startOrDefault(g.text));
    char back[6];
    day::formatStart(m, back);
    TEST_ASSERT_EQUAL_STRING(g.text, back);
  }
  const char* bad[] = {nullptr, "", "4:00", "04:0", "24:00", "04:60", "0400", "04:00 ", " 04:00", "04-00",
                       "-1:00", "04:0a", "a4:00", "99:99", "04:00:00", "4h", "04\xef\xbc\x9a" "00"};
  for (const char* b : bad) {
    uint16_t m = 777;
    const char* err = day::parseStart(b, m);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_STRING(day::kStartError, err);
    TEST_ASSERT_EQUAL_UINT16(777, m);   // untouched
    TEST_ASSERT_EQUAL_UINT16(day::kDefaultStartMin, day::startOrDefault(b));
  }
  // Every minute of the day round-trips.
  for (uint16_t m = 0; m < 1440; ++m) {
    char t[6];
    day::formatStart(m, t);
    uint16_t back = 0;
    TEST_ASSERT_NULL(day::parseStart(t, back));
    TEST_ASSERT_EQUAL_UINT16(m, back);
  }
  TEST_ASSERT_EQUAL_STRING("day_starts", day::kSettingKey);
}

// ---- properties ------------------------------------------------------------------------------

// The stamp agrees with the C library's calendar every 23 minutes over 2026 to 2028 in every
// zone, with the default start; the year and weekday are always present.
void test_property_the_stamp_matches_the_c_library_in_every_zone() {
  static const char* wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  for (const char* tz : kZones) {
    useTz(tz);
    for (uint64_t t = utc(2026, 1, 1, 0, 3); t < utc(2029, 1, 1, 0, 0); t += 23 * 60) {
      const std::string got = stampOf(t, libOffset((time_t)t), 240);
      const std::string want = expected((time_t)t, 240);
      if (got != want) {
        char msg[200];
        std::snprintf(msg, sizeof msg, "%s at %llu: got %s want %s", tz, (unsigned long long)t, got.c_str(),
                      want.c_str());
        TEST_FAIL_MESSAGE(msg);
      }
      struct tm lt;
      const time_t tt = (time_t)t;
      localtime_r(&tt, &lt);
      char year[8];
      std::snprintf(year, sizeof year, "%04d-", lt.tm_year + 1900);
      TEST_ASSERT_EQUAL_INT(0, (int)got.find(year));
      TEST_ASSERT_EQUAL_INT(11, (int)got.find(wd[lt.tm_wday]));
    }
  }
}

// Every start of the day, sampled across both UK DST days and an ordinary day.
void test_property_every_start_of_the_day_matches_the_c_library() {
  useTz(kZones[0]);
  const uint64_t days[] = {utc(2026, 3, 28, 22, 0), utc(2026, 10, 24, 22, 0), utc(2026, 11, 18, 22, 0)};
  for (uint64_t from : days)
    for (uint16_t start = 0; start < 1440; start += 1)
      for (uint64_t t = from + (start % 13) * 60; t < from + 30 * 3600; t += 89 * 60) {
        const std::string got = stampOf(t, libOffset((time_t)t), start);
        TEST_ASSERT_EQUAL_STRING(expected((time_t)t, start).c_str(), got.c_str());
      }
}

// The running day never runs backwards and never skips a day, every 5 minutes (every DST
// change and day start falls on one), in every
// zone over both of 2026's DST changes, with the default start; the clause names it.
void test_property_the_running_day_is_monotone_with_the_default_start() {
  for (const char* tz : kZones) {
    useTz(tz);
    const uint64_t spans[][2] = {{utc(2026, 3, 1, 0, 0), utc(2026, 4, 10, 0, 0)},
                                 {utc(2026, 9, 25, 0, 0), utc(2026, 11, 10, 0, 0)}};
    for (const auto& span : spans) {
      int64_t prev = day::runningDay(span[0], libOffset((time_t)span[0]), 240);
      for (uint64_t t = span[0]; t < span[1]; t += 300) {
        const int32_t off = libOffset((time_t)t);
        const int64_t d = day::runningDay(t, off, 240);
        TEST_ASSERT_TRUE_MESSAGE(d == prev || d == prev + 1, tz);
        prev = d;
        char date[16];
        TEST_ASSERT_TRUE(day::runningDate(t, off, 240, date, sizeof date));
        const std::string s = stampOf(t, off, 240);
        const bool clause = s.size() > day::kPlainBytes;
        // With a clause the running day is the date the clause names; without, the stamp's own.
        TEST_ASSERT_EQUAL_STRING(date, clause ? s.substr(day::kPlainBytes + 6, 10).c_str() : s.substr(0, 10).c_str());
      }
    }
  }
}

// The written limit (PROGRESS DECISION): a start inside the repeated hour of a fall-back
// change reads that hour's second pass as the day before again. Pinned so a change to it is seen.
void test_a_start_inside_a_repeated_hour_is_the_written_limit() {
  useTz(kZones[0]);
  const uint64_t firstPass = utc(2026, 10, 25, 0, 45);    // 01:45 BST
  const uint64_t secondPass = utc(2026, 10, 25, 1, 15);   // 01:15 GMT
  TEST_ASSERT_EQUAL_STRING("2026-10-25 Sun 01:45", stampOf(firstPass, libOffset((time_t)firstPass), 90).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-10-25 Sun 01:15 (day 2026-10-24 until 01:30)",
                           stampOf(secondPass, libOffset((time_t)secondPass), 90).c_str());
}

void test_running_date_and_calendar_day_agree_with_the_stamp() {
  char d[16];
  TEST_ASSERT_TRUE(day::runningDate(kThu0030, 0, 240, d, sizeof d));
  TEST_ASSERT_EQUAL_STRING("2026-11-18", d);
  TEST_ASSERT_TRUE(day::runningDate(kThu0030, 0, 0, d, sizeof d));
  TEST_ASSERT_EQUAL_STRING("2026-11-19", d);
  TEST_ASSERT_FALSE(day::runningDate(kThu0030, 0, 240, d, 10));   // no room for the NUL
  TEST_ASSERT_EQUAL_INT64(day::calendarDay(kThu0030, 0), day::runningDay(kThu0030, 0, 0));
  TEST_ASSERT_EQUAL_INT64(day::calendarDay(kThu0030, 0) - 1, day::runningDay(kThu0030, 0, 240));
  TEST_ASSERT_TRUE(day::formatDate(0, d, sizeof d));
  TEST_ASSERT_EQUAL_STRING("1970-01-01", d);
  TEST_ASSERT_TRUE(day::formatDate(-1, d, sizeof d));
  TEST_ASSERT_EQUAL_STRING("1969-12-31", d);
  TEST_ASSERT_FALSE(day::formatDate(-719163, d, sizeof d));   // 0000-12-31
  TEST_ASSERT_EQUAL_STRING("", d);
  TEST_ASSERT_FALSE(day::formatDate(2932897, d, sizeof d));   // 10000-01-01
  // The calendar walk: every day of 2002 to 2100 at 00:30 against an independent count.
  static const char* days[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
  int64_t n = day::calendarDay(utc(2002, 1, 1, 12, 0), 0);
  for (int y = 2002; y <= 2100; ++y) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    const int lengths[] = {31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m <= 12; ++m)
      for (int dd = 1; dd <= lengths[m - 1]; ++dd, ++n) {
        char want[24];
        std::snprintf(want, sizeof want, "%04d-%02d-%02d %s 12:00", y, m, dd, days[((n % 7) + 7) % 7]);
        TEST_ASSERT_EQUAL_STRING(want, stampOf((uint64_t)n * 86400 + 43200, 0, 240).c_str());
      }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_stamp_at_00_30_names_the_running_day);
  RUN_TEST(test_every_hour_around_midnight_and_the_day_start);
  RUN_TEST(test_new_year_and_a_leap_day);
  RUN_TEST(test_dst_change_days_in_both_directions);
  RUN_TEST(test_zones_east_and_west_of_utc);
  RUN_TEST(test_a_00_00_start_never_has_a_clause_and_odd_starts_apply_exactly);
  RUN_TEST(test_no_clock_is_said_plainly_and_never_dated);
  RUN_TEST(test_an_offset_out_of_range_writes_nothing);
  RUN_TEST(test_a_short_buffer_gets_the_plain_stamp_with_its_year);
  RUN_TEST(test_a_refusing_allocator_still_gets_the_whole_stamp);
  RUN_TEST(test_the_setting_reads_hh_mm_and_falls_back_to_04_00);
  RUN_TEST(test_property_the_stamp_matches_the_c_library_in_every_zone);
  RUN_TEST(test_property_every_start_of_the_day_matches_the_c_library);
  RUN_TEST(test_property_the_running_day_is_monotone_with_the_default_start);
  RUN_TEST(test_a_start_inside_a_repeated_hour_is_the_written_limit);
  RUN_TEST(test_running_date_and_calendar_day_agree_with_the_stamp);
  return UNITY_END();
}
