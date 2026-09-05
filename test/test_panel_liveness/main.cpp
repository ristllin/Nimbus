// test_panel_liveness - honest colour-panel controller liveness.
//
// The health/status "Display (color touch): ok, up" was hardwired to the boot
// begin() result and LIED while the controller was off the SPI bus: the owner's
// nimbus-light showed a black glass while every readback pegged all-ones
// (TFTID id=0xFFFFFF, healthy()==0), yet the row still read "ok" with zero live
// measurement. These pin the nimbus-side detector that reads the controller id
// register (readReg(0x04, 3)) and trips ONLY on the persistent not-answering
// signature - never on a plausible id (a live controller) and never on a lone
// glitched read. They also pin the unprobed-state verdict mapping so "not
// measured" can never render as a false "healthy".
#include <unity.h>

#include "nimbus/display/panel_controller.h"

using nimbus::display::idLooksDead;
using nimbus::display::PanelControllerLiveness;
using nimbus::display::PanelStatus;
using nimbus::display::panelStatus;

void setUp() {}
void tearDown() {}

// The per-read signature: every bit of the register width pegged one way (the
// owner's 0xFFFFFF, or all-zeros) is not-answering; a plausible mixed-bit id is a
// live controller.
static void test_id_looks_dead_signature() {
  // RDDID is 3 bytes. The owner's board read 0xFFFFFF: MISO idle-high / off-bus.
  TEST_ASSERT_TRUE(idLooksDead(0xFFFFFF, 3));   // all-ones: not answering
  TEST_ASSERT_TRUE(idLooksDead(0x000000, 3));   // all-zeros: nothing driving MISO
  TEST_ASSERT_FALSE(idLooksDead(0x009341, 3));  // a plausible ILI9341-ish id: alive
  TEST_ASSERT_FALSE(idLooksDead(0x000041, 3));  // low but non-zero: a real answer
  TEST_ASSERT_FALSE(idLooksDead(0xFF0000, 3));  // one byte pegged, rest not: mixed
  // The mask follows the width: 0xFFFF is all-ones for a 2-byte read but a normal
  // mixed value for a 3-byte read, and must not be misread as dead at width 3.
  TEST_ASSERT_TRUE(idLooksDead(0xFFFF, 2));
  TEST_ASSERT_FALSE(idLooksDead(0xFFFF, 3));
  // Widths >= 4 clamp to 32 bits; a full 0xFFFFFFFF is dead, one bit off is not.
  TEST_ASSERT_TRUE(idLooksDead(0xFFFFFFFFu, 4));
  TEST_ASSERT_FALSE(idLooksDead(0xFFFFFFFEu, 4));
  // Nothing read at all (width < 1) is no answer.
  TEST_ASSERT_TRUE(idLooksDead(0x1234, 0));
}

// A live controller answering plausibly, poll after poll, never trips - not even
// across a long uptime.
static void test_live_controller_never_trips() {
  PanelControllerLiveness live(3);
  for (int i = 0; i < 100; i++)
    TEST_ASSERT_FALSE(live.update(/*didRead=*/true, 0x009341, 3));
  TEST_ASSERT_FALSE(live.notResponding());
}

// A busy render bus (read skipped) is NO NEW EVIDENCE: it neither trips nor
// clears, so a panel that is actively being blitted to can never look dead.
static void test_skipped_reads_hold_the_verdict() {
  PanelControllerLiveness live(3);
  for (int i = 0; i < 100; i++)
    TEST_ASSERT_FALSE(live.update(/*didRead=*/false, 0xFFFFFF, 3));
  TEST_ASSERT_FALSE(live.notResponding());
  TEST_ASSERT_EQUAL_UINT16(0, live.deadStreak());
}

// The dead/absent controller: persistent all-ones reads trip only after the
// debounce window, then stay tripped.
static void test_not_answering_trips_after_debounce() {
  PanelControllerLiveness live(3);
  TEST_ASSERT_FALSE(live.update(true, 0xFFFFFF, 3));  // 1
  TEST_ASSERT_FALSE(live.update(true, 0xFFFFFF, 3));  // 2
  TEST_ASSERT_TRUE(live.update(true, 0xFFFFFF, 3));   // 3 -> not responding
  TEST_ASSERT_TRUE(live.update(true, 0xFFFFFF, 3));   // stays tripped
  TEST_ASSERT_TRUE(live.notResponding());
}

// A lone glitched not-answering read does not trip, and a single plausible answer
// resets the streak so the debounce restarts from scratch.
static void test_single_glitch_does_not_trip_and_life_resets() {
  PanelControllerLiveness live(3);
  live.update(true, 0xFFFFFF, 3);  // one silent read
  TEST_ASSERT_FALSE(live.notResponding());
  live.update(true, 0x009341, 3);  // a plausible id: a sign of life resets it
  TEST_ASSERT_EQUAL_UINT16(0, live.deadStreak());
  live.update(true, 0xFFFFFF, 3);  // must climb from scratch again
  live.update(true, 0xFFFFFF, 3);
  TEST_ASSERT_FALSE(live.notResponding());            // only two since reset
  TEST_ASSERT_TRUE(live.update(true, 0xFFFFFF, 3));   // third trips
}

// After tripping, the controller answering again (panel reseated, or a transient
// cleared) clears the fault immediately - the report never stays stale.
static void test_recovery_clears() {
  PanelControllerLiveness live(3);
  for (int i = 0; i < 5; i++) live.update(true, 0xFFFFFF, 3);
  TEST_ASSERT_TRUE(live.notResponding());
  TEST_ASSERT_FALSE(live.update(true, 0x009341, 3));  // a real answer clears it
  TEST_ASSERT_FALSE(live.notResponding());
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
  // Probe OFF, controller reads the all-ones signature: the owner's exact case.
  // Caught as a fault independent of the pixel probe.
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

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_id_looks_dead_signature);
  RUN_TEST(test_live_controller_never_trips);
  RUN_TEST(test_skipped_reads_hold_the_verdict);
  RUN_TEST(test_not_answering_trips_after_debounce);
  RUN_TEST(test_single_glitch_does_not_trip_and_life_resets);
  RUN_TEST(test_recovery_clears);
  RUN_TEST(test_unprobed_mapping_never_false_healthy);
  RUN_TEST(test_probed_mapping_reflects_content);
  return UNITY_END();
}
