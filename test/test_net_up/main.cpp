// CUM-504 - the network-up latch and the SNTP kick schedule (nimbus/wifi/net_up.h).
//
// The bug: SNTP was started only from a WiFi.onEvent(GOT_IP) handler registered ~10 s
// into setup(), after net::begin() had already started the station. A fast join fired
// GOT_IP before the handler existed and the clock never synced. These tests drive fake
// event timelines through the same tick order the device seam uses (src/net/wifi_link.cpp:
// drain the disconnect latch, THEN read the radio, then run the hook) and assert the class
// rule: once the latch is armed and the station holds an IP, SNTP is kicked within one
// tick, whatever order the join and the arming happened in.
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "nimbus/wifi/net_up.h"

using nimbus::wifi::NetEdges;
using nimbus::wifi::NetUpLatch;
using nimbus::wifi::SntpKick;
using nimbus::wifi::sntpWhyName;

void setUp() {}
void tearDown() {}

namespace {

// What the Arduino core holds: the live link level, plus the disconnect latch the event
// task sets. GOT_IP is deliberately not modeled as an input to the fix: the seam never
// needs to have seen it.
struct Radio {
  bool linked      = false;
  bool discLatched = false;
  void join() { linked = true; }
  void drop() { linked = false; discLatched = true; }
  void retryNoise() { discLatched = true; }   // the core re-emits DISCONNECTED while retrying
};

// The device seam (wifi_link::tick + the main.cpp hook + loops::sntpTick), in order.
struct Seam {
  NetUpLatch latch;
  SntpKick   sntp;
  bool       armed   = false;   // the hook is installed (setup() finished)
  int        ups     = 0;
  int        downs   = 0;
  int        retries = 0;
  std::vector<uint32_t> kicks;

  void tick(uint32_t now, Radio& r, bool synced) {
    if (!armed) return;   // no tick before the hook: an edge is never consumed unheard
    const bool disc = r.discLatched;   // drain the event latch FIRST...
    r.discLatched = false;
    const NetEdges e = latch.tick(r.linked, disc);   // ...then read the radio
    if (e.down) ++downs;
    if (e.up) ++ups;
    const SntpKick::Why w = sntp.tick(now, latch.up(), e.up, synced);
    if (w == SntpKick::Why::None) return;
    kicks.push_back(now);
    if (w == SntpKick::Why::Retry) ++retries;
  }
};

// The PRE-FIX behavior, kept as a counter-model: SNTP is kicked only from a GOT_IP
// callback, and a callback sees only the events dispatched after it was registered.
struct EventOnlyModel {
  bool subscribed = false;
  int  kicks      = 0;
  void gotIp() {
    if (subscribed) ++kicks;
  }
};

// Run a boot: the station joins at joinMs, the hook is armed at armMs, ticks every stepMs
// until endMs. Returns the time of the first kick, or UINT32_MAX if none.
uint32_t firstKickAt(uint32_t joinMs, uint32_t armMs, uint32_t stepMs, uint32_t endMs) {
  Radio r;
  Seam s;
  for (uint32_t t = 0; t <= endMs; t += stepMs) {
    if (t >= joinMs && !r.linked) r.join();
    if (t >= armMs) s.armed = true;
    s.tick(t, r, /*synced=*/false);
    if (!s.kicks.empty()) return s.kicks.front();
  }
  return UINT32_MAX;
}

}  // namespace

// ---- the latch: every (wasUp, staLinked, sawDisc) case ------------------------------

static void test_latch_truth_table_all_cases() {
  for (int wasUp = 0; wasUp <= 1; ++wasUp)
    for (int linked = 0; linked <= 1; ++linked)
      for (int disc = 0; disc <= 1; ++disc) {
        NetUpLatch l;
        if (wasUp) l.tick(true, false);   // bring it up first
        const NetEdges e = l.tick(linked != 0, disc != 0);
        const bool wantDown = wasUp && (!linked || disc);
        const bool wantUp   = linked && (!wasUp || disc);
        char msg[64];
        snprintf(msg, sizeof msg, "wasUp=%d linked=%d disc=%d", wasUp, linked, disc);
        TEST_ASSERT_EQUAL_MESSAGE(wantDown, e.down, msg);
        TEST_ASSERT_EQUAL_MESSAGE(wantUp, e.up, msg);
        TEST_ASSERT_EQUAL_MESSAGE(linked != 0, l.up(), msg);   // verdict == radio truth
      }
}

// ---- the three timelines the issue names ----------------------------------------------

// IP before arm (the bench-B boot): the station joined at 2 s, the hook only exists at
// 11 s. The first tick must report up and kick SNTP. The event-only counter-model, fed the
// same timeline, never kicks: this is the bug, and this test fails on the old design.
static void test_ip_before_arm_kicks_on_first_tick() {
  Radio r;
  Seam s;
  EventOnlyModel old;
  r.join();
  old.gotIp();   // GOT_IP fires at the join; nobody is subscribed yet
  old.subscribed = true;   // the pre-fix main.cpp handler registered ~10 s later
  s.armed = true;
  s.tick(11000, r, false);
  TEST_ASSERT_EQUAL(1, s.ups);
  TEST_ASSERT_EQUAL(1, (int)s.kicks.size());
  TEST_ASSERT_EQUAL_UINT32(11000, s.kicks[0]);
  TEST_ASSERT_EQUAL(0, old.kicks);   // the pre-fix design: never synced
}

// IP after arm (the slow join that always worked): nothing while down, one kick at join.
static void test_ip_after_arm_kicks_at_join() {
  Radio r;
  Seam s;
  s.armed = true;
  for (uint32_t t = 0; t < 5000; t += 100) s.tick(t, r, false);
  TEST_ASSERT_EQUAL(0, (int)s.kicks.size());
  r.join();
  s.tick(5000, r, false);
  TEST_ASSERT_EQUAL(1, s.ups);
  TEST_ASSERT_EQUAL(1, (int)s.kicks.size());
  TEST_ASSERT_EQUAL_UINT32(5000, s.kicks[0]);
}

// Disconnect + reconnect that both land between two ticks: both polls read "up", only the
// latched disconnect shows a new session. SNTP is re-kicked on the reconnect even though
// the clock is already synced (it re-syncs whatever drifted while offline).
static void test_reconnect_between_ticks_is_a_new_session() {
  Radio r;
  Seam s;
  s.armed = true;
  r.join();
  s.tick(0, r, false);
  s.tick(2000, r, true);   // synced
  r.drop();
  r.join();                // rejoined before the next tick
  s.tick(60000, r, true);
  TEST_ASSERT_EQUAL(1, s.downs);
  TEST_ASSERT_EQUAL(2, s.ups);
  TEST_ASSERT_EQUAL(2, (int)s.kicks.size());
  TEST_ASSERT_EQUAL_UINT32(60000, s.kicks[1]);
}

// A drop the poll sees, the core's retry noise while down, then a rejoin: one down, one
// new up, and the noise never fakes a session.
static void test_drop_then_retry_noise_then_rejoin() {
  Radio r;
  Seam s;
  s.armed = true;
  r.join();
  s.tick(0, r, true);
  r.drop();
  s.tick(1000, r, true);
  for (uint32_t t = 2000; t < 30000; t += 1000) {
    r.retryNoise();
    s.tick(t, r, true);
  }
  r.join();
  s.tick(30000, r, true);
  TEST_ASSERT_EQUAL(1, s.downs);
  TEST_ASSERT_EQUAL(2, s.ups);
  TEST_ASSERT_EQUAL(2, (int)s.kicks.size());
}

// Auth retries before the first successful join latch DISCONNECTED events: the first join
// is still exactly one up edge.
static void test_disc_noise_before_first_join_is_one_up() {
  Radio r;
  Seam s;
  s.armed = true;
  for (uint32_t t = 0; t < 8000; t += 500) {
    r.retryNoise();
    s.tick(t, r, false);
  }
  r.retryNoise();
  r.join();
  s.tick(8000, r, false);
  TEST_ASSERT_EQUAL(0, s.downs);
  TEST_ASSERT_EQUAL(1, s.ups);
  TEST_ASSERT_EQUAL(1, (int)s.kicks.size());
}

// ---- the class property over the boot window ------------------------------------------

// For every join time in a 20 s boot window and every place setup() might arm the hook,
// SNTP is kicked within one tick of "armed AND linked". The event-only counter-model
// misses every join that beats the arming.
static void test_kick_within_one_tick_for_any_join_arm_order() {
  const uint32_t step = 50;
  const uint32_t arms[] = {0, 5000, 10000, 12000};
  int oldMisses = 0;
  int cases = 0;
  for (uint32_t arm : arms) {
    for (uint32_t join = 0; join <= 20000; join += 250) {
      const uint32_t ready = join > arm ? join : arm;
      const uint32_t at = firstKickAt(join, arm, step, 30000);
      char msg[64];
      snprintf(msg, sizeof msg, "join=%u arm=%u", (unsigned)join, (unsigned)arm);
      TEST_ASSERT_NOT_EQUAL_MESSAGE(UINT32_MAX, at, msg);
      TEST_ASSERT_TRUE_MESSAGE(at >= ready && at - ready <= step, msg);
      if (join < arm) ++oldMisses;   // GOT_IP before subscribe: the old design never kicks
      ++cases;
    }
  }
  TEST_ASSERT_TRUE(oldMisses > 0);   // the window really contains the bug's orderings
  TEST_ASSERT_TRUE(cases > 300);
}

// ---- absence of thrash -------------------------------------------------------------------

// A stable, synced link for an hour of 100 ms ticks: one edge, one kick, nothing else.
static void test_stable_link_never_rekicks() {
  Radio r;
  Seam s;
  s.armed = true;
  r.join();
  for (uint32_t t = 0; t <= 3600000; t += 100) s.tick(t, r, t >= 3000);
  TEST_ASSERT_EQUAL(1, s.ups);
  TEST_ASSERT_EQUAL(0, s.downs);
  TEST_ASSERT_EQUAL(1, (int)s.kicks.size());
}

// A link flapping every second for two minutes: edges track every flap, but SNTP restarts
// stay at least kMinGapMs apart.
static void test_flapping_link_kicks_stay_spaced() {
  Radio r;
  Seam s;
  s.armed = true;
  for (uint32_t t = 0; t <= 120000; t += 100) {
    if (t % 1000 == 0) {
      if (r.linked) r.drop(); else r.join();
    }
    s.tick(t, r, false);
  }
  TEST_ASSERT_EQUAL(61, s.ups);   // a join on every even second, 0..120 s
  TEST_ASSERT_TRUE((int)s.kicks.size() <= 120000 / (int)SntpKick::kMinGapMs + 1);
  for (size_t i = 1; i < s.kicks.size(); ++i)
    TEST_ASSERT_TRUE(s.kicks[i] - s.kicks[i - 1] >= SntpKick::kMinGapMs);
}

// ---- the SNTP schedule ---------------------------------------------------------------------

// An up edge inside the gap is owed, not dropped: it fires once the gap has passed.
static void test_up_edge_inside_gap_is_owed_not_dropped() {
  SntpKick k;
  TEST_ASSERT_EQUAL(SntpKick::Why::LinkUp, k.tick(0, true, true, true));
  TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(3000, true, true, true));
  for (uint32_t t = 3100; t < SntpKick::kMinGapMs; t += 100)
    TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(t, true, false, true));
  TEST_ASSERT_EQUAL(SntpKick::Why::LinkUp, k.tick(SntpKick::kMinGapMs, true, false, true));
  TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(SntpKick::kMinGapMs + 100, true, false, true));
}

// An owed kick whose link dropped again is not fired into a dead link; the next up edge
// re-arms it.
static void test_down_drops_the_owed_kick() {
  SntpKick k;
  TEST_ASSERT_EQUAL(SntpKick::Why::LinkUp, k.tick(0, true, true, true));
  TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(3000, true, true, true));   // owed
  for (uint32_t t = 5000; t < 30000; t += 500)
    TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(t, false, false, true));
  TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(30000, true, false, true));  // owed kick gone
  TEST_ASSERT_EQUAL(SntpKick::Why::LinkUp, k.tick(31000, true, true, true));
}

// Up and never synced: kicks at 0, then a doubling backstop capped at kMaxRetryMs. The first
// backstop waits out lwIP's own retry and server rotation, and the count stays bounded.
static void test_unsynced_backstop_doubles_to_the_cap() {
  Radio r;
  Seam s;
  s.armed = true;
  r.join();
  for (uint32_t t = 0; t <= 7200000; t += 1000) s.tick(t, r, false);
  TEST_ASSERT_TRUE(s.kicks.size() >= 3);
  TEST_ASSERT_EQUAL_UINT32(0, s.kicks[0]);
  TEST_ASSERT_EQUAL_UINT32(SntpKick::kFirstRetryMs, s.kicks[1]);
  TEST_ASSERT_TRUE(s.kicks[1] >= 60000);   // never restarts lwIP inside its own first minute
  uint32_t prevGap = 0;
  for (size_t i = 1; i < s.kicks.size(); ++i) {
    const uint32_t gap = s.kicks[i] - s.kicks[i - 1];
    TEST_ASSERT_TRUE(gap >= prevGap);                 // never speeds up
    TEST_ASSERT_TRUE(gap <= SntpKick::kMaxRetryMs);   // never goes quiet past the cap
    prevGap = gap;
  }
  TEST_ASSERT_EQUAL_UINT32(SntpKick::kMaxRetryMs, prevGap);   // reached the ceiling
  TEST_ASSERT_TRUE((int)s.kicks.size() <= 7200000 / (int)SntpKick::kMaxRetryMs + 7);
  TEST_ASSERT_EQUAL((int)s.kicks.size() - 1, s.retries);
}

// The backstop stops the moment the clock is real.
static void test_backstop_stops_once_synced() {
  Radio r;
  Seam s;
  s.armed = true;
  r.join();
  for (uint32_t t = 0; t <= 3600000; t += 1000) s.tick(t, r, t >= 600000);
  TEST_ASSERT_EQUAL(3, (int)s.kicks.size());   // 0, 3 min, 9 min; synced at 10 min
  TEST_ASSERT_EQUAL_UINT32(540000, s.kicks[2]);
}

// No link, no kick, however long the clock stays unsynced.
static void test_never_kicks_while_down() {
  Radio r;
  Seam s;
  s.armed = true;
  for (uint32_t t = 0; t <= 3600000; t += 1000) s.tick(t, r, false);
  TEST_ASSERT_EQUAL(0, (int)s.kicks.size());
}

// A caller that never feeds an up edge (a broken seam) still gets a kick the moment the
// link is up and the clock is unsynced: a missed edge can never again mean "never".
static void test_missed_edge_still_kicks() {
  SntpKick k;
  TEST_ASSERT_EQUAL(SntpKick::Why::LinkUp, k.tick(500, true, false, false));
  TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(1500, true, false, false));
  TEST_ASSERT_EQUAL(SntpKick::Why::Retry, k.tick(500 + SntpKick::kFirstRetryMs, true, false, false));
}

// ...but a synced clock with no edge is left alone (nothing to fix, no traffic).
static void test_synced_without_edge_is_quiet() {
  SntpKick k;
  for (uint32_t t = 0; t <= 600000; t += 1000)
    TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(t, true, false, true));
}

// millis() wraps every ~49.7 days; the gap and backstop math must not stall across it.
static void test_millis_wrap() {
  SntpKick k;
  const uint32_t t0 = 0xFFFFF000u;   // 4096 ms before the wrap
  TEST_ASSERT_EQUAL(SntpKick::Why::LinkUp, k.tick(t0, true, true, false));
  TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(t0 + 10000u, true, false, false));
  TEST_ASSERT_EQUAL(SntpKick::Why::Retry,
                    k.tick(t0 + SntpKick::kFirstRetryMs, true, false, false));   // wrapped
}

// An owner request (Sync now) kicks at the next tick with a live link, obeys the gap,
// restarts the backstop, and is dropped (not hoarded) while the link is down.
static void test_owner_request() {
  SntpKick k;
  TEST_ASSERT_EQUAL(SntpKick::Why::LinkUp, k.tick(0, true, true, false));
  k.request();
  TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(4000, true, false, false));   // inside the gap
  TEST_ASSERT_EQUAL(SntpKick::Why::Requested, k.tick(SntpKick::kMinGapMs, true, false, false));
  // The backstop restarted from the request, not from the first kick.
  TEST_ASSERT_EQUAL(SntpKick::Why::None,
                    k.tick(SntpKick::kFirstRetryMs + 1000, true, false, false));
  TEST_ASSERT_EQUAL(SntpKick::Why::Retry,
                    k.tick(SntpKick::kMinGapMs + SntpKick::kFirstRetryMs, true, false, false));
  // A request on a synced clock still kicks (the owner asked).
  k.request();
  TEST_ASSERT_EQUAL(SntpKick::Why::Requested, k.tick(900000, true, false, true));
  // A request while the link is down is dropped; the link coming back kicks as LinkUp.
  k.request();
  TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(1000000, false, false, true));
  TEST_ASSERT_EQUAL(SntpKick::Why::None, k.tick(1001000, true, false, true));
  TEST_ASSERT_EQUAL(SntpKick::Why::LinkUp, k.tick(1002000, true, true, true));
}

static void test_why_names() {
  TEST_ASSERT_EQUAL_STRING("none", sntpWhyName(SntpKick::Why::None));
  TEST_ASSERT_EQUAL_STRING("link up", sntpWhyName(SntpKick::Why::LinkUp));
  TEST_ASSERT_EQUAL_STRING("retry", sntpWhyName(SntpKick::Why::Retry));
  TEST_ASSERT_EQUAL_STRING("sync now", sntpWhyName(SntpKick::Why::Requested));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_latch_truth_table_all_cases);
  RUN_TEST(test_ip_before_arm_kicks_on_first_tick);
  RUN_TEST(test_ip_after_arm_kicks_at_join);
  RUN_TEST(test_reconnect_between_ticks_is_a_new_session);
  RUN_TEST(test_drop_then_retry_noise_then_rejoin);
  RUN_TEST(test_disc_noise_before_first_join_is_one_up);
  RUN_TEST(test_kick_within_one_tick_for_any_join_arm_order);
  RUN_TEST(test_stable_link_never_rekicks);
  RUN_TEST(test_flapping_link_kicks_stay_spaced);
  RUN_TEST(test_up_edge_inside_gap_is_owed_not_dropped);
  RUN_TEST(test_down_drops_the_owed_kick);
  RUN_TEST(test_unsynced_backstop_doubles_to_the_cap);
  RUN_TEST(test_backstop_stops_once_synced);
  RUN_TEST(test_never_kicks_while_down);
  RUN_TEST(test_missed_edge_still_kicks);
  RUN_TEST(test_synced_without_edge_is_quiet);
  RUN_TEST(test_millis_wrap);
  RUN_TEST(test_owner_request);
  RUN_TEST(test_why_names);
  return UNITY_END();
}
