#include <unity.h>

#include <algorithm>
#include <string>
#include <vector>

#include "nimbus/orch/pending_events.h"

// test_orch_pending_events - device-timeline events captured before the clock synced
// survive any number of resets until a clock exists (crash forensics F3). The class:
// rows from boots that never synced all reach the timeline, in order, labelled with
// their boot, at the first sync on a later boot; the file stays bounded; a corrupt line
// is skipped, never fatal.

using namespace nimbus::orch;

void setUp() {}
void tearDown() {}

static PendingEvent ev(uint32_t boot, uint32_t up, const char* e, const std::string& text) {
  PendingEvent p;
  p.bootNo = boot;
  p.upMs = up;
  p.ev = e;
  p.text = text;
  return p;
}

static void test_codec_round_trip() {
  const PendingEvent in = ev(7, 1234, "boot", "Booted v4.5.14 reason=power-on mode=orchestrator");
  const std::string line = encodePendingEvent(in);
  TEST_ASSERT_EQUAL_STRING("7 1234 boot Booted v4.5.14 reason=power-on mode=orchestrator\n", line.c_str());
  PendingEvent out;
  TEST_ASSERT_TRUE(decodePendingEvent(line, out));
  TEST_ASSERT_EQUAL_UINT(7, out.bootNo);
  TEST_ASSERT_EQUAL_UINT(1234, out.upMs);
  TEST_ASSERT_EQUAL_STRING("boot", out.ev.c_str());
  TEST_ASSERT_EQUAL_STRING(in.text.c_str(), out.text.c_str());
}

// Text can never break the one-line-per-event format, and is capped.
static void test_encode_sanitizes_and_caps() {
  const std::string hostile = std::string("a\nb\rc") + std::string(2000, 'x');
  const std::string line = encodePendingEvent(ev(1, 2, "bad ev\n", hostile));
  TEST_ASSERT_EQUAL_INT(1, (int)std::count(line.begin(), line.end(), '\n'));
  TEST_ASSERT_TRUE(line.size() <= 40 + kPendingEvMaxText);
  PendingEvent out;
  TEST_ASSERT_TRUE(decodePendingEvent(line, out));
  TEST_ASSERT_EQUAL_STRING("bad_ev_", out.ev.c_str());
}

static void test_corrupt_lines_skipped() {
  const std::string content =
      "1 10 boot first\n"
      "garbage line\n"
      "x 10 boot bad boot number\n"
      "2 y boot bad uptime\n"
      "3 30 boot\n"                 // no text
      "4 40 ev!x bad class\n"
      "5 50 ota fifth\n"
      "6 60 boot no newline at end";
  const PendingFile f = parsePendingFile(content);
  TEST_ASSERT_EQUAL_UINT(3, (unsigned)f.rows.size());
  TEST_ASSERT_EQUAL_STRING("first", f.rows[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING("fifth", f.rows[1].text.c_str());
  TEST_ASSERT_EQUAL_STRING("no newline at end", f.rows[2].text.c_str());
}

// Bounded to 16 rows and 4 KB however many events are held; the oldest go first and
// the drop count is exact.
static void test_bounds_rows_and_bytes() {
  std::string file;
  for (uint32_t i = 0; i < 40; ++i) file = appendPendingEvent(file, ev(1, i, "boot", "row " + std::to_string(i)));
  PendingFile f = parsePendingFile(file);
  TEST_ASSERT_EQUAL_UINT(kPendingEvMaxRows, (unsigned)f.rows.size());
  TEST_ASSERT_EQUAL_UINT(40 - kPendingEvMaxRows, f.dropped);
  TEST_ASSERT_EQUAL_STRING("row 24", f.rows.front().text.c_str());
  TEST_ASSERT_EQUAL_STRING("row 39", f.rows.back().text.c_str());
  std::string big;
  for (uint32_t i = 0; i < 40; ++i) big = appendPendingEvent(big, ev(1, i, "boot", std::string(290, 'a' + (i % 26))));
  TEST_ASSERT_TRUE(big.size() <= kPendingEvMaxBytes);
  f = parsePendingFile(big);
  TEST_ASSERT_EQUAL_UINT(40, (unsigned)(f.rows.size() + f.dropped));
}

// The class: boot A and boot B never synced (each reset before a clock existed), boot C
// syncs. Every held row reaches the timeline in capture order, A's and B's rows say
// which boot they came from, C's own row keeps the pre-sync suffix, and the drop count
// leads when rows were lost.
static void test_rows_survive_resets_until_a_clock_exists() {
  std::string file;                                  // the file on the card
  file = appendPendingEvent(file, ev(11, 900, "boot", "Booted A reason=power-on"));
  file = appendPendingEvent(file, ev(11, 5000, "ota", "check failed offline"));
  // reset before sync
  file = appendPendingEvent(file, ev(12, 950, "boot", "Booted B reason=brownout"));
  // reset before sync
  file = appendPendingEvent(file, ev(13, 1000, "boot", "Booted C reason=power-on"));
  const std::vector<StampedRow> rows = planPendingFlush(parsePendingFile(file), 1);   // C held one row
  TEST_ASSERT_EQUAL_UINT(4, (unsigned)rows.size());
  TEST_ASSERT_EQUAL_STRING("Booted A reason=power-on [from boot n=11, clock never synced; stamped at first clock sync]",
                           rows[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING("ota", rows[1].ev.c_str());
  TEST_ASSERT_EQUAL_STRING("Booted B reason=brownout [from boot n=12, clock never synced; stamped at first clock sync]",
                           rows[2].text.c_str());
  TEST_ASSERT_EQUAL_STRING("Booted C reason=power-on [pre-sync, stamped at first clock sync]", rows[3].text.c_str());
}

// Dropped rows are never silent: the flush leads with one row giving the exact count.
static void test_flush_reports_dropped_count() {
  std::string file;
  for (uint32_t i = 0; i < 20; ++i) file = appendPendingEvent(file, ev(2, i, "boot", "b" + std::to_string(i)));
  const std::vector<StampedRow> rows = planPendingFlush(parsePendingFile(file), 0);   // all from boot 2
  TEST_ASSERT_EQUAL_UINT(1 + kPendingEvMaxRows, (unsigned)rows.size());
  TEST_ASSERT_EQUAL_STRING("4 earlier device event(s) were dropped while the clock was not set",
                           rows[0].text.c_str());
  TEST_ASSERT_TRUE(droppedPendingText(0).empty());
}

// A clock synced at boot (a panic reset keeps the RTC time): leftovers from an earlier
// boot flush at once. An unknown boot number never claims "from boot n=0".
static void test_synced_boot_and_unknown_boot_numbers() {
  const PendingEvent fromEarlier = ev(5, 10, "boot", "x");
  TEST_ASSERT_TRUE(stampedPendingText(fromEarlier, false).find("from boot n=5") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("x [from an earlier boot, clock never synced; stamped at first clock sync]",
                           stampedPendingText(ev(0, 10, "boot", "x"), false).c_str());
  TEST_ASSERT_TRUE(stampedPendingText(ev(5, 10, "boot", "x"), true).find("[pre-sync") != std::string::npos);
  TEST_ASSERT_TRUE(stampedPendingText(ev(0, 10, "boot", "x"), true).find("[pre-sync") != std::string::npos);
  TEST_ASSERT_TRUE(planPendingFlush(PendingFile(), 4).empty());   // nothing held: nothing written
}

// CUM-714 (bench, 22 Oct 2026): three offline boots 463, 464, 465 flushed as one "from
// boot n=463" row and TWO "[pre-sync]" rows: boot 464's row read as the syncing boot's
// own. Ownership is positional now, never inferred from the boot number. The class,
// over every split of a file between earlier boots and this boot, and over the boot
// numbers that fooled the old rule (0, and this boot's own number on an earlier row):
// exactly the last `ownTail` rows are this boot's, every other row names an earlier
// boot, and nothing is lost or written twice.
static void test_ownership_is_positional_for_every_split() {
  const uint32_t trickyNumbers[] = {0u, 465u, 463u, 7u};
  for (uint32_t tricky : trickyNumbers) {
    for (size_t earlier = 0; earlier <= 5; ++earlier) {
      for (size_t own = 0; own <= 4; ++own) {
        std::string file;
        for (size_t i = 0; i < earlier; ++i)
          file = appendPendingEvent(file, ev(i % 2 ? tricky : 460 + (uint32_t)i, (uint32_t)i, "boot", "e" + std::to_string(i)));
        for (size_t i = 0; i < own; ++i)
          file = appendPendingEvent(file, ev(i % 2 ? 0 : 465, 900 + (uint32_t)i, "boot", "o" + std::to_string(i)));
        const PendingFile f = parsePendingFile(file);
        const std::vector<StampedRow> rows = planPendingFlush(f, own);
        TEST_ASSERT_EQUAL_UINT((unsigned)(earlier + own), (unsigned)rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
          const bool presync = rows[i].text.find("[pre-sync") != std::string::npos;
          const bool from = rows[i].text.find("[from ") != std::string::npos;
          TEST_ASSERT_TRUE(presync != from);
          TEST_ASSERT_EQUAL(i >= earlier, presync);
          TEST_ASSERT_EQUAL_STRING((i < earlier ? "e" + std::to_string(i) : "o" + std::to_string(i - earlier)).c_str(),
                                   rows[i].text.substr(0, rows[i].text.find(' ')).c_str());
        }
      }
    }
  }
  // The 22 Oct shape: 464's row carries no number and the syncing boot is 465.
  std::string file;
  file = appendPendingEvent(file, ev(463, 2100, "boot", "Booted"));
  file = appendPendingEvent(file, ev(0, 2200, "boot", "Booted"));
  file = appendPendingEvent(file, ev(465, 2300, "boot", "Booted"));
  const std::vector<StampedRow> rows = planPendingFlush(parsePendingFile(file), 1);
  TEST_ASSERT_EQUAL_UINT(3, (unsigned)rows.size());
  TEST_ASSERT_TRUE(rows[0].text.find("[from boot n=463,") != std::string::npos);
  TEST_ASSERT_TRUE(rows[1].text.find("[from an earlier boot,") != std::string::npos);
  TEST_ASSERT_TRUE(rows[2].text.find("[pre-sync") != std::string::npos);
  // More own rows than the file kept (the bounds dropped some): all kept rows are own.
  const std::vector<StampedRow> all = planPendingFlush(parsePendingFile(file), 99);
  for (const auto& r : all) TEST_ASSERT_TRUE(r.text.find("[pre-sync") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_codec_round_trip);
  RUN_TEST(test_encode_sanitizes_and_caps);
  RUN_TEST(test_corrupt_lines_skipped);
  RUN_TEST(test_bounds_rows_and_bytes);
  RUN_TEST(test_rows_survive_resets_until_a_clock_exists);
  RUN_TEST(test_ownership_is_positional_for_every_split);
  RUN_TEST(test_flush_reports_dropped_count);
  RUN_TEST(test_synced_boot_and_unknown_boot_numbers);
  return UNITY_END();
}
