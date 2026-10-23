// test_speaker_reply - the speaker's answer for a spoken reply (nimbus/speaker_reply.h).
// The class: a reply counts as spoken only after the sound task opened the speaker for
// it; a driver that would not start, a replaced clip and a clip never queued all read
// as failed, with a reason, and a later report never turns a failure into success.
#include <unity.h>

#include <cstdint>
#include <string>
#include <type_traits>

#include "nimbus/speaker_reply.h"

using namespace nimbus::sfx;

void setUp() {}
void tearDown() {}

static void test_a_clip_that_opens_the_speaker_is_spoken() {
  ReplyTicket t{};
  const uint32_t a = t.queue();
  TEST_ASSERT_NOT_EQUAL(0, a);
  TEST_ASSERT_TRUE(t.state(a) == ReplyState::Queued);
  TEST_ASSERT_FALSE(settled(t.state(a)));
  t.playing(a);
  TEST_ASSERT_TRUE(started(t.state(a)));
  t.done(a);
  TEST_ASSERT_TRUE(t.state(a) == ReplyState::Done);
  TEST_ASSERT_TRUE(started(t.state(a)));
  TEST_ASSERT_EQUAL_STRING("", t.why(a).c_str());
}

// The 22 Oct incident: the driver could not start. Reported, never swallowed.
static void test_a_driver_that_will_not_start_is_reported() {
  ReplyTicket t{};
  const uint32_t a = t.queue();
  t.failed(a, speakerOpenFailReason(7156).c_str());
  TEST_ASSERT_TRUE(settled(t.state(a)));
  TEST_ASSERT_FALSE(started(t.state(a)));
  TEST_ASSERT_EQUAL_STRING("the audio driver could not start (largest free DMA memory block 7156 B)",
                           t.why(a).c_str());
  TEST_ASSERT_EQUAL_STRING(
      "could not play on the speaker: the audio driver could not start (largest free DMA memory block 7156 B)",
      speakerFailText(t.why(a)).c_str());
  // Later reports never turn the failure into success.
  t.playing(a);
  t.done(a);
  TEST_ASSERT_TRUE(t.state(a) == ReplyState::Failed);
  ReplyTicket blank{};
  const uint32_t b = blank.queue();
  blank.failed(b, "");
  TEST_ASSERT_EQUAL_STRING("the clip did not play", blank.why(b).c_str());
}

static void test_replaced_and_unqueued_clips_are_not_spoken() {
  ReplyTicket t{};
  const uint32_t a = t.queue();
  const uint32_t b = t.queue();
  TEST_ASSERT_NOT_EQUAL(a, b);
  t.playing(b);
  TEST_ASSERT_TRUE(t.state(a) == ReplyState::Failed);
  TEST_ASSERT_EQUAL_STRING("a newer spoken reply replaced it", t.why(a).c_str());
  t.failed(a, "late report for the old clip");   // ignored: it is not the newest
  TEST_ASSERT_TRUE(t.state(b) == ReplyState::Playing);
  TEST_ASSERT_TRUE(t.state(0) == ReplyState::Failed);
  TEST_ASSERT_EQUAL_STRING("the clip was not queued", t.why(0).c_str());
}

// Zero-initialized (how the device keeps it, in PSRAM) it tracks no reply.
static void test_a_zeroed_ticket_tracks_nothing() {
  ReplyTicket t{};
  TEST_ASSERT_TRUE(t.state(0) == ReplyState::Failed);
  TEST_ASSERT_TRUE(t.state(1) == ReplyState::Failed);
  TEST_ASSERT_EQUAL_STRING("the clip was not queued", t.why(0).c_str());
  t.playing(0);
  t.done(0);
  TEST_ASSERT_TRUE(t.state(0) == ReplyState::Failed);
  TEST_ASSERT_TRUE(std::is_trivially_default_constructible<ReplyTicket>::value);
}

static void test_tickets_are_distinct_and_never_zero() {
  ReplyTicket t{};
  uint32_t last = 0;
  for (int i = 0; i < 64; i++) {
    const uint32_t k = t.queue();
    TEST_ASSERT_NOT_EQUAL(0, k);
    TEST_ASSERT_TRUE(k > last);
    last = k;
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_clip_that_opens_the_speaker_is_spoken);
  RUN_TEST(test_a_driver_that_will_not_start_is_reported);
  RUN_TEST(test_replaced_and_unqueued_clips_are_not_spoken);
  RUN_TEST(test_a_zeroed_ticket_tracks_nothing);
  RUN_TEST(test_tickets_are_distinct_and_never_zero);
  return UNITY_END();
}
