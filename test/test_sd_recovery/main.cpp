// What the device does around a memory-card outage (nimbus/sd_recovery.h): a remount only
// once memory is back and at most once a minute; the held writes flushed right after the
// recovery and then never more than once a minute (owed vectors once per recovery); and
// exactly one owner notice per outage that outlasts its grace, rate-limited across a card
// that keeps dropping.
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "nimbus/sd_recovery.h"

using nimbus::SdRecovery;

void setUp() {}
void tearDown() {}

static constexpr uint32_t kMin = 60u * 1000u;

// Never a remount while the card is healthy, or while memory is short, whatever the time.
static void test_no_remount_unless_lost_and_memory_ok() {
  SdRecovery r;
  for (uint32_t t = 0; t < 10 * kMin; t += 7000) {
    TEST_ASSERT_FALSE(r.remountDue(true, t));
    TEST_ASSERT_FALSE(r.remountDue(false, t));
  }
  r.onLost(1000);
  for (uint32_t t = 1000; t < 10 * kMin; t += 7000) TEST_ASSERT_FALSE(r.remountDue(false, t));
}

// The first remount waits a full interval after the loss (the probe promotes a card that
// answers again long before that); then at most one per interval.
static void test_remount_cadence() {
  SdRecovery r;
  r.onLost(5000);
  TEST_ASSERT_FALSE(r.remountDue(true, 5000));
  TEST_ASSERT_FALSE(r.remountDue(true, 5000 + kMin - 1));
  TEST_ASSERT_TRUE(r.remountDue(true, 5000 + kMin));
  int remounts = 0;
  for (uint32_t t = 5000 + kMin; t < 5000 + 11 * kMin; t += 1000) {
    if (r.remountDue(true, t)) {
      r.noteRemount(t);
      remounts++;
    }
  }
  TEST_ASSERT_EQUAL(10, remounts);   // one a minute over ten minutes: no thrash
  r.onRecovered();
  TEST_ASSERT_FALSE(r.remountDue(true, 5000 + 20 * kMin));
}

// millis() wraps after about 49 days: the elapsed-time rule still holds across it.
static void test_remount_across_millis_wrap() {
  SdRecovery r;
  const uint32_t lost = 0xFFFFFFFFu - 10000u;
  r.onLost(lost);
  TEST_ASSERT_FALSE(r.remountDue(true, lost + 30000u));   // wrapped, 30 s later
  TEST_ASSERT_TRUE(r.remountDue(true, lost + kMin));      // wrapped, a minute later
}

// No flush while lost or with nothing held; right after the recovery, step after step while
// each step makes progress; then a refusing card is retried at most once a minute.
static void test_flush_cadence() {
  SdRecovery r;
  TEST_ASSERT_FALSE(r.flushDue(false, false, 1000));
  r.onLost(1000);
  TEST_ASSERT_FALSE(r.flushDue(true, true, 1000 + 10 * kMin));   // lost: never
  r.onRecovered();
  const uint32_t t0 = 2000 + 10 * kMin;
  TEST_ASSERT_TRUE(r.flushDue(true, false, t0));                  // right away
  TEST_ASSERT_TRUE(r.flushAfterRecovery());
  r.noteFlush(t0, true);                                          // progress, more held
  TEST_ASSERT_TRUE(r.flushDue(true, false, t0 + 10));             // the next step follows at once
  r.noteFlush(t0 + 10, false);                                    // the card refused the rest
  TEST_ASSERT_FALSE(r.flushAfterRecovery());
  int steps = 0;
  for (uint32_t t = t0 + 10; t <= t0 + 10 + 10 * kMin; t += 500) {
    if (r.flushDue(true, true, t)) {
      r.noteFlush(t, false);
      steps++;
    }
  }
  TEST_ASSERT_EQUAL(10, steps);   // ten minutes of a refusing card: ten tries, no thrash
}

// Owed vectors alone make a flush due only right after a recovery, never on the timer: a
// card that refuses the blob costs one attempt per recovery.
static void test_owed_vectors_once_per_recovery() {
  SdRecovery r;
  for (uint32_t t = 0; t < 30 * kMin; t += 1000) TEST_ASSERT_FALSE(r.flushDue(false, true, t));
  r.onLost(30 * kMin);
  r.onRecovered();
  TEST_ASSERT_TRUE(r.flushDue(false, true, 31 * kMin));
  r.noteFlush(31 * kMin, false);
  for (uint32_t t = 31 * kMin; t < 60 * kMin; t += 1000) TEST_ASSERT_FALSE(r.flushDue(false, true, t));
}

// One notice per outage, only after the grace; a blip that heals in time says nothing.
static void test_one_notice_per_outage_after_the_grace() {
  SdRecovery r;
  TEST_ASSERT_FALSE(r.noticeDue(100000));   // healthy
  r.onLost(1000);
  TEST_ASSERT_FALSE(r.noticeDue(1000 + SdRecovery::kNoticeGraceMs - 1));
  TEST_ASSERT_TRUE(r.noticeDue(1000 + SdRecovery::kNoticeGraceMs));
  int told = 0;
  for (uint32_t t = 1000 + SdRecovery::kNoticeGraceMs; t < 1000 + 30 * kMin; t += 1000) {
    if (r.noticeDue(t)) {
      r.noteNoticeSent(t);
      told++;
    }
  }
  TEST_ASSERT_EQUAL(1, told);

  SdRecovery blip;
  blip.onLost(1000);
  blip.onRecovered();   // back within the grace
  for (uint32_t t = 1000; t < 10 * kMin; t += 1000) TEST_ASSERT_FALSE(blip.noticeDue(t));
}

// A card that keeps dropping is told about at most once per kNoticeEveryMs, and again
// after that for a fresh outage.
static void test_flapping_card_is_rate_limited() {
  SdRecovery r;
  uint32_t t = 1000;
  int told = 0;
  for (int outage = 0; outage < 20; outage++) {   // an outage every ten minutes
    r.onLost(t);
    for (uint32_t u = t; u < t + 5 * kMin; u += 1000) {
      if (r.noticeDue(u)) {
        r.noteNoticeSent(u);
        told++;
      }
    }
    r.onRecovered();
    t += 10 * kMin;
  }
  TEST_ASSERT_EQUAL(1, told);   // 200 minutes < 6 h: the first outage only
  r.onLost(1000 + SdRecovery::kNoticeEveryMs + 1000);
  TEST_ASSERT_TRUE(r.noticeDue(1000 + SdRecovery::kNoticeEveryMs + 1000 + SdRecovery::kNoticeGraceMs));
}

// The owner-facing words follow the copy rules: printable ASCII, no em dash, the device
// is "it" (never I/me/my), says what happened and what happens to new data.
static void test_notice_copy() {
  const std::string s = nimbus::sdOutageNotice();
  for (unsigned char c : s) TEST_ASSERT_TRUE_MESSAGE(c >= 0x20 && c < 0x7F, "non-ASCII in the notice");
  TEST_ASSERT_TRUE(s.find("memory card") != std::string::npos);
  TEST_ASSERT_TRUE(s.find("held") != std::string::npos);
  for (const char* w : {" I ", " me ", " my ", "I'm", "!"}) TEST_ASSERT_TRUE(s.find(w) == std::string::npos);
  TEST_ASSERT_TRUE(s.size() < 300);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_no_remount_unless_lost_and_memory_ok);
  RUN_TEST(test_remount_cadence);
  RUN_TEST(test_remount_across_millis_wrap);
  RUN_TEST(test_flush_cadence);
  RUN_TEST(test_owed_vectors_once_per_recovery);
  RUN_TEST(test_one_notice_per_outage_after_the_grace);
  RUN_TEST(test_flapping_card_is_rate_limited);
  RUN_TEST(test_notice_copy);
  return UNITY_END();
}
