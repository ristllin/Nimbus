#include <unity.h>

#include "nimbus/cloud/relay_liveness.h"

using nimbus::cloud::HeartbeatLiveness;
using nimbus::cloud::kMaxMissedHeartbeatAcks;

void setUp() {}
void tearDown() {}

// The band the device clamps the relay heartbeat into keeps the cadence sane; the
// tracker's default trip count is the release-gate bound.
static void test_defaults_are_sane() {
  TEST_ASSERT_EQUAL_UINT8(2, kMaxMissedHeartbeatAcks);
  HeartbeatLiveness live;
  TEST_ASSERT_FALSE(live.dead());  // fresh tracker is never dead
}

// A zero interval or zero maxMissed must fall back to safe values, never schedule a
// ping every tick or a link that can never trip.
static void test_reset_zero_args_fall_back() {
  HeartbeatLiveness live;
  live.reset(1000, /*interval=*/0, /*maxMissed=*/0);
  TEST_ASSERT_TRUE(live.intervalMs > 0);
  TEST_ASSERT_TRUE(live.maxMissedAcks > 0);
  // First ping is one (defaulted) interval out, not immediately due.
  TEST_ASSERT_FALSE(live.duePing(1000));
  TEST_ASSERT_TRUE(live.duePing(1000 + live.intervalMs));
}

// HALF-OPEN: the relay session is gone but the device's TCP never errored, so no pong
// ever comes back. Drive the exact loop shape runOnlineLoop uses (arm, then on each due
// ping send + notePingSent + check dead) against a synthetic clock, and assert the
// device declares the link dead within 2 heartbeat periods - the CUM-191 gate bound.
static void test_half_open_trips_within_two_periods() {
  const uint32_t interval = 24000;  // 4/5 of the 30s default heartbeat
  HeartbeatLiveness live;
  uint32_t now = 100000;
  live.reset(now, interval);

  int pingsSent = 0;
  uint32_t deadAt = 0;
  bool dead = false;
  // Step the clock in 1s ticks; NO pong is ever delivered (half-open).
  for (uint32_t t = now; t <= now + 5 * interval && !dead; t += 1000) {
    if (live.duePing(t)) {
      pingsSent++;
      live.notePingSent(t);
      if (live.dead()) { dead = true; deadAt = t; }
    }
  }
  TEST_ASSERT_TRUE(dead);
  TEST_ASSERT_EQUAL_INT(2, pingsSent);            // tripped on the 2nd unacked ping
  TEST_ASSERT_TRUE(deadAt - now <= 2 * interval); // within 2 heartbeat periods
  TEST_ASSERT_EQUAL_UINT8(2, live.outstanding);
}

// HEALTHY: a pong lands after every ping. The tracker must never declare the link dead,
// no matter how long it runs.
static void test_healthy_link_never_trips() {
  const uint32_t interval = 24000;
  HeartbeatLiveness live;
  uint32_t now = 0;
  live.reset(now, interval);

  bool dead = false;
  for (uint32_t t = now; t <= now + 100 * interval && !dead; t += 1000) {
    if (live.duePing(t)) {
      live.notePingSent(t);
      if (live.dead()) { dead = true; break; }
      live.notePong();  // relay answers within the cadence
    }
  }
  TEST_ASSERT_FALSE(dead);
  TEST_ASSERT_EQUAL_UINT8(0, live.outstanding);
}

// A single unanswered ping followed by a recovered pong must NOT trip: the miss count
// resets the moment an ack lands, so a lone late/dropped pong does not force a redial.
static void test_one_miss_then_recovery_resets() {
  const uint32_t interval = 24000;
  HeartbeatLiveness live;
  uint32_t now = 0;
  live.reset(now, interval);

  // Ping #1 goes unacked.
  uint32_t t = now + interval;
  TEST_ASSERT_TRUE(live.duePing(t));
  live.notePingSent(t);
  TEST_ASSERT_FALSE(live.dead());
  TEST_ASSERT_EQUAL_UINT8(1, live.outstanding);

  // A pong finally arrives (before the 2nd ping would trip) -> count clears.
  live.notePong();
  TEST_ASSERT_EQUAL_UINT8(0, live.outstanding);

  // Ping #2 with a prompt pong -> still healthy, never dead.
  t = now + 2 * interval;
  TEST_ASSERT_TRUE(live.duePing(t));
  live.notePingSent(t);
  TEST_ASSERT_FALSE(live.dead());
  live.notePong();
  TEST_ASSERT_FALSE(live.dead());
}

// duePing uses a signed difference so a millis() rollover near 2^32 does not stall the
// heartbeat (the next ping is still recognized as due just after the wrap).
static void test_due_ping_is_wrap_safe() {
  HeartbeatLiveness live;
  const uint32_t interval = 24000;
  const uint32_t nearWrap = 0xFFFFFFFFu - 1000;  // 1s before rollover
  live.reset(nearWrap, interval);
  // nextPingAt wrapped past zero; a now-clock that has also wrapped past it is due.
  TEST_ASSERT_FALSE(live.duePing(nearWrap));
  TEST_ASSERT_TRUE(live.duePing(nearWrap + interval));  // 23000 after wrap
}

// A higher trip threshold tolerates more consecutive misses before declaring dead.
static void test_threshold_is_configurable() {
  const uint32_t interval = 10000;
  HeartbeatLiveness live;
  uint32_t now = 0;
  live.reset(now, interval, /*maxMissed=*/3);

  int pings = 0;
  bool dead = false;
  for (uint32_t t = now; t <= now + 6 * interval && !dead; t += 1000) {
    if (live.duePing(t)) {
      pings++;
      live.notePingSent(t);
      if (live.dead()) dead = true;
    }
  }
  TEST_ASSERT_TRUE(dead);
  TEST_ASSERT_EQUAL_INT(3, pings);  // trips on the 3rd unacked ping, not the 2nd
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_defaults_are_sane);
  RUN_TEST(test_reset_zero_args_fall_back);
  RUN_TEST(test_half_open_trips_within_two_periods);
  RUN_TEST(test_healthy_link_never_trips);
  RUN_TEST(test_one_miss_then_recovery_resets);
  RUN_TEST(test_due_ping_is_wrap_safe);
  RUN_TEST(test_threshold_is_configurable);
  return UNITY_END();
}
