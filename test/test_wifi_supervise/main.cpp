#include <unity.h>

#include <cstdint>

#include "nimbus/wifi/supervise.h"

using nimbus::wifi::decideSupervise;
using nimbus::wifi::SuperviseAct;
using nimbus::wifi::SuperviseDecision;
using nimbus::wifi::SuperviseInputs;

void setUp() {}
void tearDown() {}

// A tiny driver that threads the seam's carried state (engaged + kickOwed) across ticks
// exactly as src/net/wifi_link.cpp does, so a test reads like the live loop: decideSupervise
// is pure, and the flag it returns is stored back for the next tick.
struct Sim {
  bool     engaged      = false;
  bool     kickOwed     = false;
  uint32_t downSinceMs  = 1;    // link down since t=1 (non-zero sentinel)
  int      rebeginCount = 0;

  SuperviseDecision tick(uint32_t now, int known, bool manualHold, uint32_t msSinceJoin,
                         uint32_t apStations, bool onboarded = true) {
    SuperviseInputs in;
    in.onboarded   = onboarded;
    in.engaged     = engaged;
    in.knownCount  = known;
    in.manualHold  = manualHold;
    in.msSinceJoin = msSinceJoin;
    in.apStations  = apStations;
    in.nowMs       = now;
    in.downSinceMs = downSinceMs;
    in.kickOwed    = kickOwed;
    const SuperviseDecision d = decideSupervise(in);
    kickOwed = d.kickOwed;
    if (d.fireRebegin) ++rebeginCount;
    switch (d.act) {
      case SuperviseAct::Engage: engaged = true; break;
      case SuperviseAct::BowOut: engaged = false; break;
      case SuperviseAct::Drive:      /* stays engaged */ break;
      case SuperviseAct::WaitGrace:  /* nothing */ break;
    }
    return d;
  }
};

// ---- byte-identical guards (must pass on the pre-fix and post-fix logic alike) --------

// Single saved network, never engaged: bow out with NO kick (nothing to fail over to).
static void test_single_network_bowout_no_kick_unchanged() {
  Sim s;
  const SuperviseDecision d = s.tick(9000, /*known=*/1, false, 0, 0);
  TEST_ASSERT_EQUAL(SuperviseAct::BowOut, d.act);
  TEST_ASSERT_FALSE(d.fireRebegin);
  TEST_ASSERT_FALSE(s.kickOwed);
}

// First run / no saved networks: bow out, quiescent.
static void test_first_run_no_networks_quiescent() {
  Sim s;
  const SuperviseDecision d = s.tick(9000, /*known=*/0, false, 0, 0);
  TEST_ASSERT_EQUAL(SuperviseAct::BowOut, d.act);
  TEST_ASSERT_FALSE(d.fireRebegin);
  TEST_ASSERT_FALSE(s.kickOwed);
}

// Two networks, link just dropped: hold for the core's fast-reconnect grace, then engage.
static void test_core_grace_before_engage() {
  Sim s;
  s.downSinceMs = 1000;
  const SuperviseDecision d1 = s.tick(2000, 2, false, 0, 0);   // 1 s down < 8 s grace
  TEST_ASSERT_EQUAL(SuperviseAct::WaitGrace, d1.act);
  const SuperviseDecision d2 = s.tick(9500, 2, false, 0, 0);   // 8.5 s down
  TEST_ASSERT_EQUAL(SuperviseAct::Engage, d2.act);
}

// Engaged and eligible: keep driving the failover cycle.
static void test_engaged_eligible_drives() {
  Sim s;
  s.engaged = true;
  const SuperviseDecision d = s.tick(9000, 2, false, 0, 0);
  TEST_ASSERT_EQUAL(SuperviseAct::Drive, d.act);
}

// ---- the CUM-294 fix: an owed kick is never consumed by a blocked edge ----------------

// SHAPE 1: mid-failover the owner's phone associates to the setup AP (kick correctly
// skipped), the owner forgets down to one saved network and disconnects. The blocked edge
// must NOT be consumed forever - the re-begin fires once the AP client leaves.
static void test_shape1_forget_to_one_after_ap_client() {
  Sim s;
  s.engaged = true;                                            // failing over across 2 nets
  const SuperviseDecision d1 = s.tick(12000, 2, false, 0, /*ap=*/1);
  TEST_ASSERT_EQUAL(SuperviseAct::BowOut, d1.act);
  TEST_ASSERT_FALSE(d1.fireRebegin);                           // correctly skipped
  TEST_ASSERT_TRUE(s.kickOwed);                                // ...but remembered
  const SuperviseDecision d2 = s.tick(13000, /*known=*/1, false, 0, /*ap=*/1);
  TEST_ASSERT_FALSE(d2.fireRebegin);                           // still blocked, still owed
  TEST_ASSERT_TRUE(s.kickOwed);
  const SuperviseDecision d3 = s.tick(14000, 1, false, 0, /*ap=*/0);
  TEST_ASSERT_TRUE(d3.fireRebegin);                            // owed kick finally fires
  TEST_ASSERT_FALSE(s.kickOwed);
  TEST_ASSERT_EQUAL(1, s.rebeginCount);
}

// SHAPE 2 (live-reproduced 2026-09-02): a manual join to a hotspot that then vanished. The
// old bow-out kick required msSinceJoin == 0, which a never-landed join never reached, so
// the kick was consumed forever. msSinceJoin GROWS (nothing re-arms it); once it ages past
// its bounded window the hold releases and the owed kick fires the hand-back to the core.
static void test_shape2_manual_join_died_then_owed_kick_fires() {
  Sim s;
  s.engaged = true;                                          // failing over across 2 nets
  // The owner hit Connect: a manual credential test is in flight (young). The supervisor
  // correctly stands aside AND defers its re-begin edge rather than consuming it.
  const SuperviseDecision d1 = s.tick(12000, /*known=*/2, false, /*msSinceJoin=*/3000, 0);
  TEST_ASSERT_EQUAL(SuperviseAct::BowOut, d1.act);
  TEST_ASSERT_FALSE(d1.fireRebegin);
  TEST_ASSERT_TRUE(s.kickOwed);
  // The joined network never comes up and the list is down to that one entry; msSinceJoin
  // has aged past its window (it grows, it is never pinned). The hold releases.
  const SuperviseDecision d2 = s.tick(40000, /*known=*/1, false, /*msSinceJoin=*/25000, 0);
  TEST_ASSERT_TRUE(d2.fireRebegin);                          // owed kick hands back to core
  TEST_ASSERT_FALSE(s.kickOwed);
  TEST_ASSERT_EQUAL(1, s.rebeginCount);
}

// COUNTER-TEST (the regression the outage-clock version would have shipped): a manual join
// begun LATE in a long outage must keep its full bounded window. The hold is keyed to the
// JOIN clock (msSinceJoin), not the outage age, so the supervisor never stomps the network
// the owner just asked for - even when the link has been down far longer than any window.
static void test_manual_join_late_in_outage_keeps_its_window() {
  Sim s;
  s.downSinceMs = 1;   // link down since t=1 (a very long outage)
  const SuperviseDecision d = s.tick(/*now=*/300000, /*known=*/2, false,
                                     /*msSinceJoin=*/2000, 0);
  TEST_ASSERT_EQUAL(SuperviseAct::BowOut, d.act);   // stands aside, does not engage over it
  TEST_ASSERT_FALSE(d.fireRebegin);
}

// CLASS RULE: whatever blocker deferred the kick (escape-hatch hold, manual join, or a
// setup-AP client), the owed kick fires EXACTLY once when that blocker clears with the
// link still down and a network to hand back. A blocker with no guard here would fail.
static void test_owed_kick_fires_for_every_blocker_class() {
  enum { HOLD = 0, MANUAL = 1, APCLIENT = 2 };
  for (int b = HOLD; b <= APCLIENT; ++b) {
    Sim s;
    s.engaged = true;
    const bool     hold = (b == HOLD);
    const uint32_t mj   = (b == MANUAL) ? 3000u : 0u;
    const uint32_t ap   = (b == APCLIENT) ? 1u : 0u;
    // Blocked engaged->disengaged edge, dropped to one network (nothing to fail over to).
    const SuperviseDecision d1 = s.tick(9000, /*known=*/1, hold, mj, ap);
    TEST_ASSERT_EQUAL(SuperviseAct::BowOut, d1.act);
    TEST_ASSERT_FALSE(d1.fireRebegin);        // deferred
    TEST_ASSERT_TRUE(s.kickOwed);             // remembered
    s.tick(9500, 1, hold, mj, ap);            // still blocked
    TEST_ASSERT_TRUE(s.kickOwed);             // not consumed
    const SuperviseDecision d3 = s.tick(10000, 1, false, 0, 0);   // blocker clears
    TEST_ASSERT_TRUE(d3.fireRebegin);
    TEST_ASSERT_FALSE(s.kickOwed);
    TEST_ASSERT_EQUAL(1, s.rebeginCount);     // exactly once, per blocker class
  }
}

// No-thrash: once the owed kick fires it is not re-issued while the link stays down with a
// single saved network - one hand-back to the core, not a storm of WiFi.begin() calls.
static void test_kick_fires_once_no_thrash() {
  Sim s;
  s.engaged = true;
  s.tick(9000, 1, false, 0, /*ap=*/1);        // blocked edge, owed
  s.tick(10000, 1, false, 0, 0);              // fires once
  TEST_ASSERT_EQUAL(1, s.rebeginCount);
  for (uint32_t now = 11000; now <= 30000; now += 1000)
    s.tick(now, 1, false, 0, 0);
  TEST_ASSERT_EQUAL(1, s.rebeginCount);       // still exactly once
}

// Engaging to drive the failover ourselves makes any owed kick moot (we take the radio).
static void test_engage_clears_owed_kick() {
  Sim s;
  s.engaged = true;
  s.tick(9000, 1, false, 0, /*ap=*/1);        // owe a kick (known=1, AP client)
  TEST_ASSERT_TRUE(s.kickOwed);
  // A second network reappears, nothing blocking, link still down past grace -> engage.
  const SuperviseDecision d = s.tick(10000, /*known=*/2, false, 0, 0);
  TEST_ASSERT_EQUAL(SuperviseAct::Engage, d.act);
  TEST_ASSERT_FALSE(s.kickOwed);
}

// CLASS INVARIANT over the full (engaged, kickOwed, known, blocker) cross-product: a
// re-begin may fire ONLY on the ineligible branch, ONLY when a kick is due (engaged or
// owed, with a network to hand back), and ONLY when nothing is blocking; a due-but-blocked
// kick is always remembered, never dropped; and with no network there is no kick but the
// debt is kept. The `fired > 0` guard proves the sweep is not vacuously passing.
static void test_kick_invariants_over_cross_product() {
  int fired = 0;
  for (int engaged = 0; engaged < 2; ++engaged)
    for (int owed = 0; owed < 2; ++owed)
      for (int known = 0; known < 3; ++known)
        for (int blk = 0; blk < 4; ++blk) {   // 0 none, 1 hold, 2 manual join, 3 AP client
          SuperviseInputs in;
          in.onboarded   = true;
          in.engaged     = engaged;
          in.kickOwed    = owed;
          in.knownCount  = known;
          in.manualHold  = (blk == 1);
          in.msSinceJoin = (blk == 2) ? 3000u : 0u;
          in.apStations  = (blk == 3) ? 1u : 0u;
          in.nowMs       = 30000;
          in.downSinceMs = 1;                 // grace long past
          const SuperviseDecision d = decideSupervise(in);
          const bool blocked = (blk != 0);
          const bool due = (engaged || owed) && known >= 1;
          if (d.fireRebegin) {
            TEST_ASSERT_EQUAL(SuperviseAct::BowOut, d.act);
            TEST_ASSERT_TRUE(due);
            TEST_ASSERT_FALSE(blocked);
            ++fired;
          }
          if (due && blocked) TEST_ASSERT_TRUE(d.kickOwed);      // remembered, not dropped
          if (known == 0 && owed) {                              // no network: keep owing
            TEST_ASSERT_FALSE(d.fireRebegin);
            TEST_ASSERT_TRUE(d.kickOwed);
          }
        }
  TEST_ASSERT_TRUE(fired > 0);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_single_network_bowout_no_kick_unchanged);
  RUN_TEST(test_first_run_no_networks_quiescent);
  RUN_TEST(test_core_grace_before_engage);
  RUN_TEST(test_engaged_eligible_drives);
  RUN_TEST(test_shape1_forget_to_one_after_ap_client);
  RUN_TEST(test_shape2_manual_join_died_then_owed_kick_fires);
  RUN_TEST(test_manual_join_late_in_outage_keeps_its_window);
  RUN_TEST(test_owed_kick_fires_for_every_blocker_class);
  RUN_TEST(test_kick_fires_once_no_thrash);
  RUN_TEST(test_engage_clears_owed_kick);
  RUN_TEST(test_kick_invariants_over_cross_product);
  return UNITY_END();
}
