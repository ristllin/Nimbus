#include <unity.h>

#include "nimbus/power/power_manager.h"
#include "nimbus/power/power_monitor.h"
#include "nimbus/profile.h"

using namespace nimbus;
using namespace nimbus::power;

void setUp() {}
void tearDown() {}

// NullMonitor: never any battery data -> the manager stays completely inert and
// the device runs as desk-powered.
static void test_null_monitor_inert() {
  NullMonitor mon;
  Selector sel;
  sel.setUser(ProfileId::Balanced);
  Manager m(&mon, &sel);
  for (uint32_t t = 0; t < 300000; t += 30000) {
    ManagerActions a = m.tick(t);
    TEST_ASSERT_FALSE(a.warnT1);
    TEST_ASSERT_FALSE(a.shutdownT2);
    TEST_ASSERT_FALSE(a.telemetryDue);
  }
  // VBUS defaults present (Null reports external power) -> Desk auto.
  TEST_ASSERT_TRUE(m.onVbus());
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(sel.resolve()));
}

// A discharge curve drives the selector and raises the right actions.
static void test_discharge_drives_selector_and_actions() {
  SimMonitor mon;
  Selector sel;
  sel.setUser(ProfileId::Balanced);
  Manager m(&mon, &sel);
  uint32_t t = 0;
  auto step = [&](uint8_t pct, bool ext) {
    mon.setBattery(pct, ext);
    t += 2000;  // exceed the VBUS debounce between steps
    return m.tick(t);
  };

  // Healthy on battery: user profile stands (no forced, no VBUS).
  ManagerActions a = step(80, false);
  TEST_ASSERT_FALSE(a.warnT1);
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(sel.resolve()));

  // Cross T1 (20%): warn + force Battery Saver.
  a = step(20, false);
  TEST_ASSERT_TRUE(a.warnT1);
  TEST_ASSERT_TRUE(a.profileChanged);
  TEST_ASSERT_TRUE(m.forced());
  TEST_ASSERT_EQUAL(int(ProfileId::BatterySaver), int(sel.resolve()));

  // Cross T2 (8%): request shutdown.
  a = step(8, false);
  TEST_ASSERT_TRUE(a.shutdownT2);

  // Plug in: VBUS debounces, clears T1, auto-Desk.
  mon.setBattery(8, true);
  m.tick(t + 100);            // raw flip
  a = m.tick(t + 2000);      // debounce elapsed
  TEST_ASSERT_TRUE(a.clearedT1);
  TEST_ASSERT_TRUE(m.onVbus());
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(sel.resolve()));
}

// Telemetry fires on the first valid sample, then on cadence.
static void test_telemetry_cadence() {
  SimMonitor mon;
  Selector sel;
  Manager m(&mon, &sel);
  m.setTelemetryPeriodMs(60000);

  mon.setBattery(50, true);
  ManagerActions a = m.tick(1000);
  TEST_ASSERT_TRUE(a.telemetryDue);          // first valid sample

  a = m.tick(30000);
  TEST_ASSERT_FALSE(a.telemetryDue);          // within period

  a = m.tick(61001);
  TEST_ASSERT_TRUE(a.telemetryDue);           // period elapsed
}

// Forced (T1) beats VBUS in the resolved profile even when both are set.
static void test_forced_beats_vbus() {
  SimMonitor mon;
  Selector sel;
  sel.setUser(ProfileId::Balanced);
  Manager m(&mon, &sel);
  // On external power but critically low: charging clears T-latches per policy,
  // so to see forced+vbus together we use a low battery with external NOT yet
  // debounced as charging clearing - instead assert the selector precedence
  // directly.
  sel.setForced(true);
  sel.setVbus(true);
  TEST_ASSERT_EQUAL(int(ProfileId::BatterySaver), int(sel.resolve()));
}


// The owner can turn OFF the automatic battery-mode switch (it is SHIPPED
// behaviour, so the core default stays true and only the device layer defaults it
// off/on for the owner). Turning it off must leave the mode alone while keeping
// every protection intact: the warn action, and the T2 shutdown request.
static void test_auto_saver_gate_leaves_protection_intact() {
  SimMonitor mon;
  Selector sel;
  sel.setUser(ProfileId::Desk);
  Manager m(&mon, &sel);
  m.setAutoSaverOnLow(false);
  uint32_t t = 0;
  auto step = [&](uint8_t pct, bool ext) {
    mon.setBattery(pct, ext);
    t += 2000;
    return m.tick(t);
  };

  ManagerActions a = step(80, false);
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(sel.resolve()));

  // Cross T1: the WARNING still fires and the policy still latches...
  a = step(20, false);
  TEST_ASSERT_TRUE_MESSAGE(a.warnT1, "the low-battery warning was suppressed");
  TEST_ASSERT_TRUE_MESSAGE(m.forced(), "the policy latch was suppressed");
  // ...but the owner's battery mode is left alone.
  TEST_ASSERT_EQUAL_MESSAGE(int(ProfileId::Desk), int(sel.resolve()),
      "the battery mode was switched even though the owner turned that off");

  // T2 deep-sleep protection is NOT opt-out-able.
  a = step(8, false);
  TEST_ASSERT_TRUE_MESSAGE(a.shutdownT2, "T2 shutdown was suppressed - protection must be unconditional");

  // Re-enabling takes effect on the next tick with T1 already latched (the
  // level-triggered device-side re-resolve depends on this).
  m.setAutoSaverOnLow(true);
  a = step(8, false);
  TEST_ASSERT_EQUAL_MESSAGE(int(ProfileId::BatterySaver), int(sel.resolve()),
      "re-enabling the switch did not take effect while T1 was already latched");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_null_monitor_inert);
  RUN_TEST(test_discharge_drives_selector_and_actions);
  RUN_TEST(test_telemetry_cadence);
  RUN_TEST(test_forced_beats_vbus);
  RUN_TEST(test_auto_saver_gate_leaves_protection_intact);
  return UNITY_END();
}
