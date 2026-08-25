#include <unity.h>

#include "nimbus/wifi/setup_ap.h"

using nimbus::wifi::decideSetupAp;
using nimbus::wifi::SetupApAct;
using nimbus::wifi::SetupApInputs;

void setUp() {}
void tearDown() {}

// A device mid-onboarding, orchestrator, TFT, nothing settled yet.
static SetupApInputs base() {
  SetupApInputs in;
  in.orchestrator = true;
  in.tftBoard = true;
  in.staConnected = false;
  in.apAddressed = true;   // AP up on first boot
  in.onboarded = false;    // wizard not finished
  in.handoffGrace = false;
  in.msSinceJoin = 0;      // no join in flight
  in.joinGraceMs = 30000;
  return in;
}

// Notifier keeps the radio off - the setup AP does not exist to reconcile.
static void test_notifier_never_touches_the_radio() {
  SetupApInputs in = base();
  in.orchestrator = false;
  for (int sta = 0; sta < 2; ++sta)
    for (int ap = 0; ap < 2; ++ap) {
      in.staConnected = sta;
      in.apAddressed = ap;
      TEST_ASSERT_EQUAL(SetupApAct::None, decideSetupAp(in));
    }
}

// Nothing reachable (STA down, AP address gone) -> bring the setup AP straight back.
static void test_restore_when_nothing_reachable() {
  SetupApInputs in = base();
  in.staConnected = false;
  in.apAddressed = false;
  TEST_ASSERT_EQUAL(SetupApAct::RestoreAp, decideSetupAp(in));
}

// Restore is board-independent: an eink / non-TFT orchestrator board recovers too
// (the old reconcile was TFT-gated and never self-healed there).
static void test_restore_is_not_tft_gated() {
  SetupApInputs in = base();
  in.tftBoard = false;
  in.staConnected = false;
  in.apAddressed = false;
  TEST_ASSERT_EQUAL(SetupApAct::RestoreAp, decideSetupAp(in));
}

// STA joined and reachable + AP still up on a TFT -> shed the AP beacon (white-screen).
static void test_drop_ap_on_tft_after_handoff() {
  SetupApInputs in = base();
  in.staConnected = true;
  in.apAddressed = true;
  in.handoffGrace = false;
  TEST_ASSERT_EQUAL(SetupApAct::DropAp, decideSetupAp(in));
}

// ...but never during the handoff grace window.
static void test_no_drop_during_handoff_grace() {
  SetupApInputs in = base();
  in.staConnected = true;
  in.apAddressed = true;
  in.handoffGrace = true;
  TEST_ASSERT_EQUAL(SetupApAct::None, decideSetupAp(in));
}

// A non-TFT board has no white-screen risk, so it keeps both interfaces up.
static void test_no_drop_on_non_tft() {
  SetupApInputs in = base();
  in.tftBoard = false;
  in.staConnected = true;
  in.apAddressed = true;
  TEST_ASSERT_EQUAL(SetupApAct::None, decideSetupAp(in));
}

// The crux of CUM-190: a wrong password churns the join past the grace while the
// wizard is unfinished -> protect the AP (stop the STA starving its beacons), no reboot.
static void test_protect_ap_when_join_starves_it_during_onboarding() {
  SetupApInputs in = base();
  in.staConnected = false;
  in.apAddressed = true;     // AP "looks up" but is being starved
  in.onboarded = false;
  in.msSinceJoin = 31000;    // past the 30 s grace
  TEST_ASSERT_EQUAL(SetupApAct::ProtectAp, decideSetupAp(in));
}

// A join still inside the grace is a normal slow join - leave it alone.
static void test_no_protect_before_grace_elapses() {
  SetupApInputs in = base();
  in.staConnected = false;
  in.apAddressed = true;
  in.onboarded = false;
  in.msSinceJoin = 5000;     // still joining
  TEST_ASSERT_EQUAL(SetupApAct::None, decideSetupAp(in));
}

// No join in flight (msSinceJoin == 0) is never treated as a stalled join.
static void test_no_protect_without_a_join_in_flight() {
  SetupApInputs in = base();
  in.staConnected = false;
  in.apAddressed = true;
  in.onboarded = false;
  in.msSinceJoin = 0;        // sentinel: nothing pending
  TEST_ASSERT_EQUAL(SetupApAct::None, decideSetupAp(in));
}

// Once onboarding is COMPLETE the slow-retry policy owns a stalled join, not this
// watchdog - protecting the AP here would fight it.
static void test_no_protect_after_onboarding_complete() {
  SetupApInputs in = base();
  in.staConnected = false;
  in.apAddressed = true;
  in.onboarded = true;       // wizard done
  in.msSinceJoin = 60000;
  TEST_ASSERT_EQUAL(SetupApAct::None, decideSetupAp(in));
}

// The action never contradicts itself: exactly one of restore/protect/drop can win,
// and a fresh first boot (AP up, nothing else) is quiescent.
static void test_first_boot_is_quiescent() {
  SetupApInputs in = base();   // sta down, ap up, no join, unfinished onboarding
  TEST_ASSERT_EQUAL(SetupApAct::None, decideSetupAp(in));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_notifier_never_touches_the_radio);
  RUN_TEST(test_restore_when_nothing_reachable);
  RUN_TEST(test_restore_is_not_tft_gated);
  RUN_TEST(test_drop_ap_on_tft_after_handoff);
  RUN_TEST(test_no_drop_during_handoff_grace);
  RUN_TEST(test_no_drop_on_non_tft);
  RUN_TEST(test_protect_ap_when_join_starves_it_during_onboarding);
  RUN_TEST(test_no_protect_before_grace_elapses);
  RUN_TEST(test_no_protect_without_a_join_in_flight);
  RUN_TEST(test_no_protect_after_onboarding_complete);
  RUN_TEST(test_first_boot_is_quiescent);
  return UNITY_END();
}
