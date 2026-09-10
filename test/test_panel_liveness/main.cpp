// test_panel_liveness - honest colour-panel controller liveness.
//
// The health/status "Display (color touch): ok, up" was hardwired to the boot
// begin() result and LIED while the controller was off the SPI bus: the owner's
// nimbus-light showed a black glass while healthy() read false (TFTHEALTH
// healthy=0), yet the row still read "ok" with zero live measurement. These pin
// the nimbus-side detector, which is driven by the driver's RDDST-based
// healthy() read (NOT the controller id / RDDID): it trips ONLY on a persistent
// not-answering signature - never on a live panel and never on a lone glitched
// read. They also pin the unprobed-state verdict mapping so "not measured" can
// never render as a false "healthy".
//
// Regression under test (the reason the source changed): the id-based (RDDID)
// verdict reported a healthy Freenove / CYD panel as "not responding", because
// such a panel returns RDDID 0x000000 while fully working and visibly rendering.
// The verdict now follows healthy() (RDDST), which reads true on that same panel.
#include <unity.h>

#include "nimbus/display/panel_controller.h"

using nimbus::display::PanelControllerLiveness;
using nimbus::display::PanelStatus;
using nimbus::display::panelStatus;

void setUp() {}
void tearDown() {}

// The RETIRED RDDID predicate, reproduced here ONLY to prove the exact case it
// got wrong. Production no longer ships this: it keyed "dead" on an all-zeros OR
// all-ones controller id, which is why a healthy Freenove (RDDID 0x000000) read
// as dead. The live detector below is driven by healthy() instead and gets it
// right. Do not wire anything to this - it exists to lock in the regression.
static bool retiredRddidLooksDead(uint32_t id, int nbytes) {
  if (nbytes < 1) return true;
  const uint32_t mask =
      (nbytes >= 4) ? 0xFFFFFFFFu : ((1u << (static_cast<unsigned>(nbytes) * 8u)) - 1u);
  const uint32_t v = id & mask;
  return v == 0u || v == mask;
}

// THE regression case, and the one that would FAIL under the old RDDID logic.
// A healthy Freenove / CYD panel: healthy() reads true (RDDST mirrors the mode we
// wrote) while its RDDID reads 0x000000. The retired id-based verdict called that
// panel dead; the live detector, fed the healthy() stream, reports it responding
// across a long uptime.
static void test_freenove_rddid_zero_reads_responding() {
  // Proof the source had to change: the retired predicate marks this working
  // panel's id as "dead", contradicting reality.
  constexpr uint32_t kFreenoveRddid = 0x000000;   // healthy Freenove, verified on hardware
  TEST_ASSERT_TRUE(retiredRddidLooksDead(kFreenoveRddid, 3));  // the old bug: false positive

  // The live detector follows healthy() (RDDST), which is true on this panel, so
  // it never trips - the working Freenove reads as responding.
  PanelControllerLiveness live(3);
  for (int i = 0; i < 100; i++)
    TEST_ASSERT_FALSE(live.update(/*didRead=*/true, /*healthy=*/true));
  TEST_ASSERT_FALSE(live.notResponding());
  TEST_ASSERT_EQUAL_UINT16(0, live.unhealthyStreak());
}

// A live controller answering healthy, poll after poll, never trips - not even
// across a long uptime.
static void test_healthy_stream_never_trips() {
  PanelControllerLiveness live(3);
  for (int i = 0; i < 100; i++)
    TEST_ASSERT_FALSE(live.update(/*didRead=*/true, /*healthy=*/true));
  TEST_ASSERT_FALSE(live.notResponding());
}

// A busy render bus (read skipped) is NO NEW EVIDENCE: it neither trips nor
// clears, so a panel that is actively being blitted to can never look dead - even
// if a stale healthy=false is passed alongside didRead=false.
static void test_skipped_reads_hold_the_verdict() {
  PanelControllerLiveness live(3);
  for (int i = 0; i < 100; i++)
    TEST_ASSERT_FALSE(live.update(/*didRead=*/false, /*healthy=*/false));
  TEST_ASSERT_FALSE(live.notResponding());
  TEST_ASSERT_EQUAL_UINT16(0, live.unhealthyStreak());
}

// The dead/absent controller - the real case tf-hon2 was built for. A genuinely
// disconnected panel reads healthy() false on every idle poll (the owner's board
// read TFTHEALTH healthy=0); the debounce trips it after the window, then it
// stays tripped.
static void test_disconnected_trips_after_debounce() {
  PanelControllerLiveness live(3);
  TEST_ASSERT_FALSE(live.update(true, /*healthy=*/false));  // 1
  TEST_ASSERT_FALSE(live.update(true, /*healthy=*/false));  // 2
  TEST_ASSERT_TRUE(live.update(true, /*healthy=*/false));   // 3 -> not responding
  TEST_ASSERT_TRUE(live.update(true, /*healthy=*/false));   // stays tripped
  TEST_ASSERT_TRUE(live.notResponding());
}

// A lone glitched unhealthy read does not trip, and a single healthy answer
// resets the streak so the debounce restarts from scratch.
static void test_single_glitch_does_not_trip_and_health_resets() {
  PanelControllerLiveness live(3);
  live.update(true, /*healthy=*/false);  // one unhealthy read
  TEST_ASSERT_FALSE(live.notResponding());
  live.update(true, /*healthy=*/true);   // a healthy answer: a sign of life resets it
  TEST_ASSERT_EQUAL_UINT16(0, live.unhealthyStreak());
  live.update(true, /*healthy=*/false);  // must climb from scratch again
  live.update(true, /*healthy=*/false);
  TEST_ASSERT_FALSE(live.notResponding());           // only two since reset
  TEST_ASSERT_TRUE(live.update(true, /*healthy=*/false));  // third trips
}

// After tripping, the controller answering healthy again (panel reseated, or a
// transient cleared) clears the fault immediately - the report never stays stale.
static void test_recovery_clears() {
  PanelControllerLiveness live(3);
  for (int i = 0; i < 5; i++) live.update(true, /*healthy=*/false);
  TEST_ASSERT_TRUE(live.notResponding());
  TEST_ASSERT_FALSE(live.update(true, /*healthy=*/true));  // a healthy read clears it
  TEST_ASSERT_FALSE(live.notResponding());
  TEST_ASSERT_EQUAL_UINT16(0, live.unhealthyStreak());
}

// A threshold of 1 trips on the first unhealthy read (the constructor floors 0 to
// 1 so a mis-configured detector can never be un-trippable).
static void test_threshold_one_and_zero_floor() {
  PanelControllerLiveness one(1);
  TEST_ASSERT_TRUE(one.update(true, /*healthy=*/false));
  PanelControllerLiveness zero(0);
  TEST_ASSERT_EQUAL_UINT16(1, zero.threshold());
  TEST_ASSERT_TRUE(zero.update(true, /*healthy=*/false));
}

// The unprobed-state mapping - the core of the fix. "Not measured" must never
// render as a healthy "ok", and a genuinely present panel must never read as a
// false fault.
static void test_unprobed_mapping_never_false_healthy() {
  // Probe OFF (the shipped default), controller answering: present but the pixels
  // are not confirmed. Must be Unverified, NOT Ok - the old code emitted true here.
  TEST_ASSERT_EQUAL(PanelStatus::Unverified,
                    panelStatus(/*notResponding=*/false, /*probed=*/false, /*contentOk=*/false));
  // The contentOk argument is meaningless when the probe is off and must not
  // upgrade the verdict to Ok.
  TEST_ASSERT_EQUAL(PanelStatus::Unverified,
                    panelStatus(false, false, true));
  // Probe OFF, controller not answering: the owner's exact case. Caught as a
  // fault independent of the pixel probe.
  TEST_ASSERT_EQUAL(PanelStatus::NotResponding,
                    panelStatus(/*notResponding=*/true, false, false));
}

static void test_probed_mapping_reflects_content() {
  // Probe ON and content matched: confirmed Ok.
  TEST_ASSERT_EQUAL(PanelStatus::Ok, panelStatus(false, /*probed=*/true, /*contentOk=*/true));
  // Probe ON but content diverged: a fault, not a healthy claim.
  TEST_ASSERT_EQUAL(PanelStatus::NotResponding,
                    panelStatus(false, true, false));
  // Not-responding always wins, even with the probe on and a stale contentOk=true.
  TEST_ASSERT_EQUAL(PanelStatus::NotResponding, panelStatus(true, true, true));
}

// scrok (CUM-388): the machine-readable "screen confirmed up and answering" bit
// that the STATUS field and the boot signal both report. Its truth table must
// match the health "screen" row exactly (kOk == bound && !fault && !notResponding),
// so the flasher's guard and the health row cannot ever disagree.
static void test_screen_responding_matches_health_ok() {
  using nimbus::display::screenResponding;
  // The one healthy case: bound at boot, not fault-injected, controller answering.
  TEST_ASSERT_TRUE(screenResponding(/*boundOk=*/true, /*fault=*/false, /*notResp=*/false));
  // A wrong-variant flash: bound blindly, but the controller never answers -> 0.
  TEST_ASSERT_FALSE(screenResponding(true, false, /*notResp=*/true));
  // Boot bring-up failed (dead panel / wrong scrModel) -> not confirmed up.
  TEST_ASSERT_FALSE(screenResponding(/*boundOk=*/false, false, false));
  // Fault-injected absent (test FAULT screen on): simulated-absent is never "up",
  // which is exactly how the on-device negative path drives scrok=0.
  TEST_ASSERT_FALSE(screenResponding(true, /*fault=*/true, false));
  // notResponding can never be masked by boundOk, and fault can never be masked by
  // a stale not-responding=false: any one disqualifier forces 0.
  TEST_ASSERT_FALSE(screenResponding(false, true, true));
}

// The boot one-shot signal classifies the same signals into the three outcomes,
// so a genuinely-dead panel is surfaced without a false variant claim.
static void test_boot_panel_signal_classifies_three_outcomes() {
  using nimbus::display::BootPanelSignal;
  using nimbus::display::bootPanelSignal;
  // Bound + answering -> the normal healthy boot (PANEL scrok=1).
  TEST_ASSERT_EQUAL(int(BootPanelSignal::Responding),
                    int(bootPanelSignal(/*boundOk=*/true, /*notResp=*/false)));
  // Bound but silent -> the wrong-variant signature (loud line + scrok=0).
  TEST_ASSERT_EQUAL(int(BootPanelSignal::NotResponding),
                    int(bootPanelSignal(true, /*notResp=*/true)));
  // begin() failed -> surfaced as a hint, distinct from the variant claim. A dead
  // panel that never bound reads InitFailed even if the (unmeasured) liveness bit
  // has not latched, so the honest hint always wins over a hard variant claim.
  TEST_ASSERT_EQUAL(int(BootPanelSignal::InitFailed),
                    int(bootPanelSignal(/*boundOk=*/false, /*notResp=*/false)));
  TEST_ASSERT_EQUAL(int(BootPanelSignal::InitFailed),
                    int(bootPanelSignal(false, true)));
}

// End to end from the debounced detector: a not-responding stream drives scrok to
// 0, and a healthy stream keeps it 1 - the same PanelControllerLiveness the device
// feeds, so the pure predicates are exercised against the real verdict source.
static void test_scrok_follows_the_debounced_verdict() {
  using nimbus::display::screenResponding;
  PanelControllerLiveness live(3);
  // Healthy stream: never trips, scrok stays 1.
  for (int i = 0; i < 10; i++) live.update(/*didRead=*/true, /*healthy=*/true);
  TEST_ASSERT_TRUE(screenResponding(true, false, live.notResponding()));
  // Controller goes silent: below threshold scrok holds 1, at threshold it drops.
  live.update(true, false);
  live.update(true, false);
  TEST_ASSERT_TRUE(screenResponding(true, false, live.notResponding()));  // streak 2 < 3
  live.update(true, false);
  TEST_ASSERT_FALSE(screenResponding(true, false, live.notResponding())); // streak 3 -> 0
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_freenove_rddid_zero_reads_responding);
  RUN_TEST(test_healthy_stream_never_trips);
  RUN_TEST(test_skipped_reads_hold_the_verdict);
  RUN_TEST(test_disconnected_trips_after_debounce);
  RUN_TEST(test_single_glitch_does_not_trip_and_health_resets);
  RUN_TEST(test_recovery_clears);
  RUN_TEST(test_threshold_one_and_zero_floor);
  RUN_TEST(test_unprobed_mapping_never_false_healthy);
  RUN_TEST(test_probed_mapping_reflects_content);
  RUN_TEST(test_screen_responding_matches_health_ok);
  RUN_TEST(test_boot_panel_signal_classifies_three_outcomes);
  RUN_TEST(test_scrok_follows_the_debounced_verdict);
  return UNITY_END();
}
