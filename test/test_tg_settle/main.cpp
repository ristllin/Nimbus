#include <unity.h>

#include <cstdint>
#include <random>

#include "nimbus/net/tg_settle.h"

// The settle rule (nimbus/net/tg_settle.h): answer a getUpdates page now, or hold it
// a little for the rest of an album or burst. A decision table over every rule (page
// full, growth restarting the window, the maximum hold, an old date, an unset clock,
// text only), then the class properties: a hold never outlives kSettleMaxHoldMs, a
// page that stops growing is answered within its quiet window plus one re-poll, and a
// held page that does not grow re-polls at most ceil(quiet / repoll) + 1 times.

using nimbus::net::kSettleMaxHoldMs;
using nimbus::net::kSettleQuietMediaMs;
using nimbus::net::kSettleQuietTextMs;
using nimbus::net::kSettleRepollMs;
using nimbus::net::Settle;
using nimbus::net::SettlePage;
using nimbus::net::SettleWhy;
using nimbus::net::TgSettle;

void setUp() {}
void tearDown() {}

static constexpr int64_t kNow = 1792400000;   // a set device clock (Oct 2026)

static SettlePage page(int count, bool media, int64_t newestDate = kNow, int limit = 10) {
  SettlePage p;
  p.count = count;
  p.limit = limit;
  p.hasMedia = media;
  p.newestId = 1000 + count;   // one more update = a new newest id
  p.newestDate = newestDate;
  return p;
}

// Page full: dispatched at first sight, whatever the timing.
static void test_full_page_dispatches_at_once() {
  TgSettle s;
  TEST_ASSERT_EQUAL_INT((int)Settle::Dispatch, (int)s.observe(50, page(10, true), 0, kNow));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::Full, (int)s.why());
  // A limit=1 fallback poll (an oversized batch) is full with one update.
  TgSettle one;
  TEST_ASSERT_EQUAL_INT((int)Settle::Dispatch, (int)one.observe(50, page(1, true, kNow, 1), 0, kNow));
}

// Growth restarts the quiet window: an album that arrives one photo a second is
// answered quiet ms after its LAST photo, as one page.
static void test_growth_restarts_the_window() {
  TgSettle s;
  uint32_t t = 0;
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(50, page(1, true), t, kNow));
  t += 1000;
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(50, page(2, true), t, kNow));
  t += 1000;
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(50, page(3, true), t, kNow));   // last photo at 2000
  t += 1000;
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(50, page(3, true), t, kNow));
  t += 1000;
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(50, page(3, true), t, kNow));
  t += 1000;   // 5000: 3000 since the last growth
  TEST_ASSERT_EQUAL_INT((int)Settle::Dispatch, (int)s.observe(50, page(3, true), t, kNow));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::Quiet, (int)s.why());
  TEST_ASSERT_EQUAL_UINT32(5000, s.lastHeldMs());
  TEST_ASSERT_EQUAL_INT(5, s.repolls());
}

// The maximum hold: a page that keeps growing is answered at kSettleMaxHoldMs, never
// later; one that stops growing earlier is answered on its quiet window instead.
static void test_max_hold_bounds_a_growing_page() {
  TgSettle g;
  int32_t id = 1;
  uint32_t at = 0;
  for (; at <= 40000; at += 1000) {
    SettlePage p = page(3, true);
    p.newestId = ++id;   // a new newest update every poll (other updates acked away)
    if (g.observe(77, p, at, kNow) == Settle::Dispatch) break;
  }
  TEST_ASSERT_EQUAL_UINT32(kSettleMaxHoldMs, at);
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::MaxHold, (int)g.why());
  // Growth up to 9 items (one a second), then silence: quiet ends it at 8 s + 3 s.
  TgSettle s;
  uint32_t t = 0;
  for (int n = 1; t < 30000; t += 1000, ++n)
    if (s.observe(50, page(n < 9 ? n : 9, true), t, kNow) == Settle::Dispatch) break;
  TEST_ASSERT_EQUAL_UINT32(8000 + kSettleQuietMediaMs, t);
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::Quiet, (int)s.why());
}

// An old date dispatches at once: the sender finished more than quiet + 2 s ago.
// Exactly quiet + 2 s still holds (Telegram dates are whole seconds).
static void test_old_date_dispatches_at_once() {
  TgSettle s;
  const int64_t media = kSettleQuietMediaMs / 1000 + 2;   // 5 s
  TEST_ASSERT_EQUAL_INT((int)Settle::Dispatch, (int)s.observe(50, page(2, true, kNow - media - 1), 0, kNow));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::OldDate, (int)s.why());
  TgSettle edge;
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)edge.observe(50, page(2, true, kNow - media), 0, kNow));
  // Text only: 1.5 s + 2 s = 3.5 s.
  TgSettle txt;
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)txt.observe(50, page(1, false, kNow - 3), 0, kNow));
  TgSettle txt4;
  TEST_ASSERT_EQUAL_INT((int)Settle::Dispatch, (int)txt4.observe(50, page(1, false, kNow - 4), 0, kNow));
}

// An unset clock skips the date rule (a stale date is meaningless against it): the
// page still dispatches on its quiet window.
static void test_unset_clock_skips_the_date_rule() {
  for (int64_t clock : {(int64_t)0, (int64_t)86400, nimbus::net::kSettleClockSet - 1}) {
    TgSettle s;
    TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(50, page(2, true, kNow - 3600), 0, clock));
    TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(50, page(2, true, kNow - 3600), 1000, clock));
    TEST_ASSERT_EQUAL_INT((int)Settle::Dispatch, (int)s.observe(50, page(2, true, kNow - 3600), 3000, clock));
    TEST_ASSERT_EQUAL_INT((int)SettleWhy::Quiet, (int)s.why());
  }
  // A page with no date at all behaves the same with a set clock.
  TgSettle s;
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(50, page(1, true, 0), 0, kNow));
}

// Text only: the short window (1.5 s). A lone message waits two re-polls at most.
static void test_text_only_uses_the_short_window() {
  TgSettle s;
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(50, page(1, false), 0, kNow));
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(50, page(1, false), 1000, kNow));
  TEST_ASSERT_EQUAL_INT((int)Settle::Dispatch, (int)s.observe(50, page(1, false), 2000, kNow));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::Quiet, (int)s.why());
  // The same timing with a photo in the page still holds (3 s window).
  TgSettle m;
  m.observe(50, page(1, true), 0, kNow);
  m.observe(50, page(1, true), 1000, kNow);
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)m.observe(50, page(1, true), 2000, kNow));
}

// An empty page is never held, and a new offset starts a fresh hold.
static void test_empty_page_and_new_offset() {
  TgSettle s;
  TEST_ASSERT_EQUAL_INT((int)Settle::Dispatch, (int)s.observe(50, page(0, false), 0, kNow));
  TEST_ASSERT_EQUAL_INT((int)SettleWhy::Empty, (int)s.why());
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(50, page(1, true), 100, kNow));
  // The cursor moved (an earlier page was answered): the windows restart there.
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(60, page(1, true), 2900, kNow));
  TEST_ASSERT_EQUAL_INT((int)Settle::Hold, (int)s.observe(60, page(1, true), 5000, kNow));
  TEST_ASSERT_EQUAL_INT((int)Settle::Dispatch, (int)s.observe(60, page(1, true), 5900, kNow));
}

// Absence of thrash: a held page that does not grow re-polls at most
// ceil(quiet / repoll) + 1 times, for both windows and any re-poll pace at or above
// kSettleRepollMs (a slow round trip only stretches the pace).
static void test_repoll_bound_for_a_still_page() {
  std::mt19937 rng(7);
  for (bool media : {false, true}) {
    const uint32_t quiet = nimbus::net::settleQuietMs(media);
    const int bound = (int)((quiet + kSettleRepollMs - 1) / kSettleRepollMs) + 1;
    for (int trial = 0; trial < 500; ++trial) {
      TgSettle s;
      uint32_t t = rng() % 100000;
      int polls = 0;
      while (s.observe(9, page(2, media), t, 0) == Settle::Hold) {
        ++polls;
        t += kSettleRepollMs + rng() % 400;
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
    const bool media = rng() % 2;
    for (;;) {
      if (rng() % 3 == 0 && count < 9) {
        ++count;
        ++id;
      }
      SettlePage p = page(count, media);
      p.newestId = id;
      if (s.observe(1, p, t, 0) == Settle::Dispatch) break;
      t += kSettleRepollMs + rng() % 300;
    }
    TEST_ASSERT_TRUE(s.lastHeldMs() < kSettleMaxHoldMs + kSettleRepollMs + 300);
    TEST_ASSERT_TRUE(s.why() != SettleWhy::Waiting);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_full_page_dispatches_at_once);
  RUN_TEST(test_growth_restarts_the_window);
  RUN_TEST(test_max_hold_bounds_a_growing_page);
  RUN_TEST(test_old_date_dispatches_at_once);
  RUN_TEST(test_unset_clock_skips_the_date_rule);
  RUN_TEST(test_text_only_uses_the_short_window);
  RUN_TEST(test_empty_page_and_new_offset);
  RUN_TEST(test_repoll_bound_for_a_still_page);
  RUN_TEST(test_hold_never_exceeds_the_maximum);
  return UNITY_END();
}
