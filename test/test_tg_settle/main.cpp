#include <unity.h>

#include <cstdint>
#include <random>
#include <string>

#include "nimbus/net/tg_settle.h"

// The settle rule (nimbus/net/tg_settle.h): answer a getUpdates page now, or hold it
// a little for the rest of an album, a burst, or the photo sent right after a text. A
// decision table over every rule (a command, no turn candidate, page full, growth
// restarting the window, the maximum hold, an old date, an unset clock, the text and
// media windows, the shortened last re-poll), then the class properties: a hold never
// outlives kSettleMaxHoldMs, a page that stops growing is answered within its quiet
// window plus one round trip, every tail kind either waits a bounded window or
// dispatches, and a held page that does not grow re-polls at most
// ceil(quiet / repoll) + 1 times.

using nimbus::net::kSettleMaxHoldMs;
using nimbus::net::kSettleQuietMediaMs;
using nimbus::net::kSettleQuietTextMs;
using nimbus::net::kSettleRepollMinMs;
using nimbus::net::kSettleRepollMs;
using nimbus::net::Settle;
using nimbus::net::SettlePage;
using nimbus::net::SettleTail;
using nimbus::net::SettleWhy;
using nimbus::net::TgSettle;

void setUp() {}
void tearDown() {}

static constexpr int64_t kNow = 1792400000;   // a set device clock (Oct 2026)

static SettlePage page(int count, SettleTail tail, int64_t newestDate = kNow, int limit = 10) {
  SettlePage p;
  p.count = count;
  p.limit = limit;
  p.tail = tail;
  p.newestId = 1000 + count;   // one more update = a new newest id
  p.newestDate = newestDate;
  return p;
}

static int dispatchOf(TgSettle& s, int32_t offset, const SettlePage& p, uint32_t at, int64_t epoch = kNow) {
  return (int)s.observe(offset, p, at, epoch);
}

static constexpr int kHold = (int)Settle::Hold;
static constexpr int kDispatch = (int)Settle::Dispatch;

// Commands never wait: a page holding one is answered at first sight, whatever else it
// holds and whatever the timing.
static void test_command_never_waits() {
  for (SettleTail tail : {SettleTail::Text, SettleTail::Media}) {
    TgSettle s;
    SettlePage p = page(2, tail);
    p.command = true;
    TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 50, p, 0));
    TEST_ASSERT_EQUAL_INT((int)SettleWhy::Command, (int)s.why());
    TEST_ASSERT_EQUAL_UINT32(0, s.lastHeldMs());
  }
  // A text held for its window: the command that arrives next ends the hold at once.
  TgSettle s;
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(1, SettleTail::Text), 0));
  SettlePage cmd = page(2, SettleTail::Text);
  cmd.command = true;
  TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 50, cmd, 1200));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::Command, (int)s.why());
}

// No turn candidate from an allowed chat (an edit, a stranger, a sticker): nothing would
// join a turn, so nothing waits.
static void test_no_turn_candidate_dispatches_at_once() {
  TgSettle s;
  TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 50, page(3, SettleTail::None), 0));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::NoTurn, (int)s.why());
}

// Page full: dispatched at first sight, whatever the timing (the item cap of a burst).
static void test_full_page_dispatches_at_once() {
  for (SettleTail tail : {SettleTail::Text, SettleTail::Media}) {
    TgSettle s;
    TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 50, page(10, tail), 0));
    TEST_ASSERT_EQUAL_INT((int)SettleWhy::Full, (int)s.why());
  }
  // A limit=1 fallback poll (an oversized batch) is full with one update.
  TgSettle one;
  TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(one, 50, page(1, SettleTail::Media, kNow, 1), 0));
}

// Growth restarts the quiet window: an album that arrives one photo a second is
// answered quiet ms after its LAST photo, as one page.
static void test_growth_restarts_the_window() {
  TgSettle s;
  uint32_t t = 0;
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(1, SettleTail::Media), t));
  t += 1000;
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(2, SettleTail::Media), t));
  t += 1000;
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(3, SettleTail::Media), t));   // last photo at 2000
  t += 1000;
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(3, SettleTail::Media), t));
  t += 1000;
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(3, SettleTail::Media), t));
  t += 1000;   // 5000: 3000 since the last growth
  TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 50, page(3, SettleTail::Media), t));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::Quiet, (int)s.why());
  TEST_ASSERT_EQUAL_UINT32(5000, s.lastHeldMs());
  TEST_ASSERT_EQUAL_INT(5, s.repolls());
}

// A plain text waits the text window (5 s) for what the owner sends next; a lone text is
// answered at its end.
static void test_lone_text_waits_the_text_window() {
  TEST_ASSERT_EQUAL_UINT32(5000, kSettleQuietTextMs);
  TgSettle s;
  for (uint32_t t = 0; t < kSettleQuietTextMs; t += 1000)
    TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(1, SettleTail::Text), t));
  TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 50, page(1, SettleTail::Text), kSettleQuietTextMs));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::Quiet, (int)s.why());
  TEST_ASSERT_EQUAL_UINT32(kSettleQuietTextMs, s.lastHeldMs());
}

// Text then a photo 1, 3 or 5 s after the text was first seen: the photo grows the page
// inside the text window, the page is held on for the rest of the album (the media
// window), and both are dispatched together.
static void test_text_then_photo_inside_the_window() {
  for (uint32_t gap : {1000u, 3000u, 5000u}) {
    TgSettle s;
    uint32_t t = 0;
    for (; t < gap; t += 500) TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(1, SettleTail::Text), t));
    t = gap;   // the re-poll that sees the photo (the last re-poll of a window lands at its end)
    TEST_ASSERT_EQUAL_INT_MESSAGE(kHold, dispatchOf(s, 50, page(2, SettleTail::Media), t), "the photo joined");
    for (t += 500; t < gap + kSettleQuietMediaMs; t += 500)
      TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(2, SettleTail::Media), t));
    TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 50, page(2, SettleTail::Media), gap + kSettleQuietMediaMs));
    TEST_ASSERT_EQUAL_INT((int)SettleWhy::Quiet, (int)s.why());
  }
}

// Photo then text: the text restarts the window as a text window (a second photo may
// follow it too), so the photo and its text are one page.
static void test_photo_then_text_is_one_page() {
  TgSettle s;
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(1, SettleTail::Media), 0));
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(2, SettleTail::Text), 2000));
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(2, SettleTail::Text), 2000 + kSettleQuietMediaMs));
  TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 50, page(2, SettleTail::Text), 2000 + kSettleQuietTextMs));
  // A photo alone with nothing after it: the media window.
  TgSettle p;
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(p, 50, page(1, SettleTail::Media), 0));
  TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(p, 50, page(1, SettleTail::Media), kSettleQuietMediaMs));
}

// The maximum hold: a page that keeps growing is answered at kSettleMaxHoldMs, never
// later; one that stops growing earlier is answered on its quiet window instead.
static void test_max_hold_bounds_a_growing_page() {
  for (SettleTail tail : {SettleTail::Text, SettleTail::Media}) {
    TgSettle g;
    int32_t id = 1;
    uint32_t at = 0;
    for (; at <= 40000; at += 1000) {
      SettlePage p = page(3, tail);
      p.newestId = ++id;   // a new newest update every poll (other updates acked away)
      if (g.observe(77, p, at, kNow) == Settle::Dispatch) break;
    }
    TEST_ASSERT_EQUAL_UINT32(kSettleMaxHoldMs, at);
    TEST_ASSERT_EQUAL_INT((int)SettleWhy::MaxHold, (int)g.why());
  }
  // Growth up to 9 items (one a second), then silence: quiet ends it at 8 s + 3 s.
  TgSettle s;
  uint32_t t = 0;
  for (int n = 1; t < 30000; t += 1000, ++n)
    if (s.observe(50, page(n < 9 ? n : 9, SettleTail::Media), t, kNow) == Settle::Dispatch) break;
  TEST_ASSERT_EQUAL_UINT32(8000 + kSettleQuietMediaMs, t);
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::Quiet, (int)s.why());
}

// The burst caps: texts every 4 s (each inside the text window) hold the page for at
// most kSettleMaxHoldMs from its first item, and a burst that fills the page is answered
// at once (the item cap).
static void test_burst_caps_items_and_total_wait() {
  TgSettle s;
  uint32_t t = 0;
  int n = 1;
  for (; t <= 60000; t += 1000) {
    if (t % 4000 == 0 && t) ++n;   // one more text every 4 s, for ever
    if (s.observe(50, page(n, SettleTail::Text), t, kNow) == Settle::Dispatch) break;
  }
  TEST_ASSERT_EQUAL_UINT32(kSettleMaxHoldMs, t);
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::MaxHold, (int)s.why());
  TgSettle f;
  int count = 1;
  uint32_t at = 0;
  while (f.observe(60, page(count, SettleTail::Text), at, kNow) == Settle::Hold) {
    ++count;   // a fast burst: one more message every re-poll
    at += 400;
    TEST_ASSERT_TRUE(count <= 10);
  }
  TEST_ASSERT_EQUAL_INT(10, count);
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::Full, (int)f.why());
}

// An old date dispatches at once: the sender finished more than quiet + 2 s ago.
// Exactly quiet + 2 s still holds (Telegram dates are whole seconds).
static void test_old_date_dispatches_at_once() {
  TgSettle s;
  const int64_t media = kSettleQuietMediaMs / 1000 + 2;   // 5 s
  TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 50, page(2, SettleTail::Media, kNow - media - 1), 0));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::OldDate, (int)s.why());
  TgSettle edge;
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(edge, 50, page(2, SettleTail::Media, kNow - media), 0));
  // Text: 5 s + 2 s = 7 s.
  const int64_t text = kSettleQuietTextMs / 1000 + 2;
  TgSettle txt;
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(txt, 50, page(1, SettleTail::Text, kNow - text), 0));
  TgSettle txt8;
  TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(txt8, 50, page(1, SettleTail::Text, kNow - text - 1), 0));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::OldDate, (int)txt8.why());
}

// An unset clock skips the date rule (a stale date is meaningless against it): the
// page still dispatches on its quiet window.
static void test_unset_clock_skips_the_date_rule() {
  for (int64_t clock : {(int64_t)0, (int64_t)86400, nimbus::net::kSettleClockSet - 1}) {
    TgSettle s;
    TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(2, SettleTail::Media, kNow - 3600), 0, clock));
    TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(2, SettleTail::Media, kNow - 3600), 1000, clock));
    TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 50, page(2, SettleTail::Media, kNow - 3600), 3000, clock));
    TEST_ASSERT_EQUAL_INT((int)SettleWhy::Quiet, (int)s.why());
  }
  // A page with no date at all behaves the same with a set clock.
  TgSettle s;
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(1, SettleTail::Media, 0), 0));
}

// An empty page is never held, and a new offset starts a fresh hold.
static void test_empty_page_and_new_offset() {
  TgSettle s;
  TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 50, page(0, SettleTail::None), 0));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::Empty, (int)s.why());
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 50, page(1, SettleTail::Media), 100));
  // The cursor moved (an earlier page was answered): the windows restart there.
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 60, page(1, SettleTail::Media), 2900));
  TEST_ASSERT_EQUAL_INT(kHold, dispatchOf(s, 60, page(1, SettleTail::Media), 5000));
  TEST_ASSERT_EQUAL_INT(kDispatch, dispatchOf(s, 60, page(1, SettleTail::Media), 5900));
}

// The last re-poll of a window lands at its end: the pause is kSettleRepollMs, or the
// rest of the window (or of the maximum hold) when that is shorter, never below
// kSettleRepollMinMs.
static void test_repoll_pause_ends_at_the_window() {
  TgSettle s;
  TEST_ASSERT_EQUAL_UINT32(kSettleRepollMs, s.repollInMs(0));   // not holding
  s.observe(50, page(1, SettleTail::Text), 0, kNow);
  TEST_ASSERT_EQUAL_UINT32(kSettleRepollMs, s.repollInMs(0));
  TEST_ASSERT_EQUAL_UINT32(800, s.repollInMs(4200));
  TEST_ASSERT_EQUAL_UINT32(kSettleRepollMinMs, s.repollInMs(4990));
  TEST_ASSERT_EQUAL_UINT32(kSettleRepollMinMs, s.repollInMs(9000));   // past it: never a spin
  // The maximum hold binds a page that keeps growing.
  TgSettle g;
  int32_t id = 1;
  for (uint32_t t = 0; t <= 14000; t += 1000) {
    SettlePage p = page(2, SettleTail::Text);
    p.newestId = ++id;
    g.observe(70, p, t, kNow);
  }
  TEST_ASSERT_EQUAL_UINT32(600, g.repollInMs(14400));
}

// A lone text paced by repollInMs() plus a round trip of 100-400 ms is answered no
// earlier than the text window and no later than one round trip (plus at most the
// shortest pause) after it: no added latency beyond the window.
static void test_lone_text_answered_at_the_window_end() {
  std::mt19937 rng(5);
  for (int trial = 0; trial < 500; ++trial) {
    TgSettle s;
    uint32_t t = rng() % 100000;
    const uint32_t first = t;
    uint32_t worstRtt = 0;
    while (s.observe(9, page(1, SettleTail::Text), t, 0) == Settle::Hold) {
      const uint32_t rtt = 100 + rng() % 301;
      worstRtt = rtt > worstRtt ? rtt : worstRtt;
      t += s.repollInMs(t) + rtt;
    }
    TEST_ASSERT_TRUE(t - first >= kSettleQuietTextMs);
    TEST_ASSERT_TRUE_MESSAGE(t - first <= kSettleQuietTextMs + kSettleRepollMinMs + worstRtt,
                             "a plain text held beyond its window");
  }
}

// Every tail kind (kSettleTailAll) either dispatches at once or waits a window shorter
// than the maximum hold.
static void test_every_tail_has_a_bounded_rule() {
  for (const SettleTail tail : nimbus::net::kSettleTailAll) {
    const uint32_t quiet = nimbus::net::settleQuietMs(tail);
    TEST_ASSERT_TRUE(quiet < kSettleMaxHoldMs);
    TgSettle s;
    const bool held = s.observe(1, page(1, tail), 0, 0) == Settle::Hold;
    TEST_ASSERT_EQUAL(quiet > 0, held);
  }
}

// Absence of thrash: a held page that does not grow re-polls at most
// ceil(quiet / repoll) + 1 times, for both windows and any re-poll pace at or above
// kSettleRepollMs (a slow round trip only stretches the pace), and also when the last
// pause of a window is shortened.
static void test_repoll_bound_for_a_still_page() {
  std::mt19937 rng(7);
  for (SettleTail tail : {SettleTail::Text, SettleTail::Media}) {
    const uint32_t quiet = nimbus::net::settleQuietMs(tail);
    const int bound = (int)((quiet + kSettleRepollMs - 1) / kSettleRepollMs) + 1;
    for (int trial = 0; trial < 500; ++trial) {
      const bool shortened = trial % 2;
      TgSettle s;
      uint32_t t = rng() % 100000;
      int polls = 0;
      while (s.observe(9, page(2, tail), t, 0) == Settle::Hold) {
        ++polls;
        t += (shortened ? s.repollInMs(t) : kSettleRepollMs) + rng() % 400;
        TEST_ASSERT_TRUE(polls <= bound + 1);
      }
      TEST_ASSERT_TRUE_MESSAGE(s.repolls() <= bound, "a still page re-polled too often");
      TEST_ASSERT_TRUE(s.lastHeldMs() < quiet + kSettleRepollMs + 400);
    }
  }
}

// Property over random arrival patterns: no hold ever exceeds the maximum by more
// than one re-poll interval, and a dispatch always names a reason.
static void test_hold_never_exceeds_the_maximum() {
  std::mt19937 rng(11);
  for (int trial = 0; trial < 2000; ++trial) {
    TgSettle s;
    int count = 1;
    int32_t id = 100;
    uint32_t t = rng();
    SettleTail tail = rng() % 2 ? SettleTail::Text : SettleTail::Media;
    for (;;) {
      if (rng() % 3 == 0 && count < 9) {
        ++count;
        ++id;
        tail = rng() % 2 ? SettleTail::Text : SettleTail::Media;
      }
      SettlePage p = page(count, tail);
      p.newestId = id;
      if (s.observe(1, p, t, 0) == Settle::Dispatch) break;
      t += s.repollInMs(t) + rng() % 300;
    }
    TEST_ASSERT_TRUE(s.lastHeldMs() < kSettleMaxHoldMs + kSettleRepollMs + 300);
    TEST_ASSERT_TRUE(s.why() != SettleWhy::Waiting);
  }
}

// The crossed-answer rule: a message sent before its chat's previous answer went out
// missed that turn; the same second is not counted, and an unknown time never is.
static void test_crossed_answer_rule() {
  using nimbus::net::crossedAnswer;
  TEST_ASSERT_TRUE(crossedAnswer(kNow - 3, kNow));
  TEST_ASSERT_FALSE(crossedAnswer(kNow, kNow));
  TEST_ASSERT_FALSE(crossedAnswer(kNow + 5, kNow));
  TEST_ASSERT_FALSE(crossedAnswer(0, kNow));
  TEST_ASSERT_FALSE(crossedAnswer(kNow, 0));
  // The note is model-facing and starts on its own paragraph.
  TEST_ASSERT_EQUAL_INT(0, std::string(nimbus::net::kCrossedNote).find("\n\n["));
  // The history row keeps what was sent: the note comes off, nothing else does.
  using nimbus::net::withoutCrossedNote;
  const std::string sent = "[image 1/1 ref=00ab] a plate";
  const std::string routed = sent + nimbus::net::kCrossedNote;
  TEST_ASSERT_EQUAL_size_t(sent.size(), withoutCrossedNote(routed.c_str(), routed.size()));
  TEST_ASSERT_EQUAL_size_t(sent.size(), withoutCrossedNote(sent.c_str(), sent.size()));
  TEST_ASSERT_EQUAL_size_t(0, withoutCrossedNote("", 0));
  const std::string inner = std::string(nimbus::net::kCrossedNote) + " and more";   // not at the end
  TEST_ASSERT_EQUAL_size_t(inner.size(), withoutCrossedNote(inner.c_str(), inner.size()));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_command_never_waits);
  RUN_TEST(test_no_turn_candidate_dispatches_at_once);
  RUN_TEST(test_full_page_dispatches_at_once);
  RUN_TEST(test_growth_restarts_the_window);
  RUN_TEST(test_lone_text_waits_the_text_window);
  RUN_TEST(test_text_then_photo_inside_the_window);
  RUN_TEST(test_photo_then_text_is_one_page);
  RUN_TEST(test_max_hold_bounds_a_growing_page);
  RUN_TEST(test_burst_caps_items_and_total_wait);
  RUN_TEST(test_old_date_dispatches_at_once);
  RUN_TEST(test_unset_clock_skips_the_date_rule);
  RUN_TEST(test_empty_page_and_new_offset);
  RUN_TEST(test_repoll_pause_ends_at_the_window);
  RUN_TEST(test_lone_text_answered_at_the_window_end);
  RUN_TEST(test_every_tail_has_a_bounded_rule);
  RUN_TEST(test_repoll_bound_for_a_still_page);
  RUN_TEST(test_hold_never_exceeds_the_maximum);
  RUN_TEST(test_crossed_answer_rule);
  return UNITY_END();
}
