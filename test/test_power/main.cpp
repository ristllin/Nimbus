#include <unity.h>

#include "nimbus/power/power_monitor.h"
#include "nimbus/power/power_policy.h"
#include "nimbus/power/bright_cap.h"
#include "nimbus/power/board_power.h"
#include "nimbus/power/battery_sense.h"

using namespace nimbus::power;

// A valid on-battery sample by PACK mV (the voltage-T2 world's step()).
static nimbus::power::Sample mvSample(uint16_t packMv, uint8_t pct = 50) {
  nimbus::power::Sample s;
  s.valid = true;
  s.millivolts = packMv;
  s.percent = pct;
  s.onExternalPower = false;
  s.charging = false;
  return s;
}


void setUp() {}
void tearDown() {}

static void test_null_monitor_is_desk_powered() {
  NullMonitor m;
  Sample s = m.sample();
  TEST_ASSERT_FALSE(s.valid);
  TEST_ASSERT_TRUE(s.onExternalPower);
}

static void test_liion_pack_plausibility_rejects_a_floating_divider() {
  // Live fresh-board regression: an unwired GPIO4 floated to a fake 3020 mV
  // pack and sent a USB-powered device into low-battery deep sleep during setup.
  TEST_ASSERT_FALSE(plausibleLiIonPackMv(0, 2));
  TEST_ASSERT_FALSE(plausibleLiIonPackMv(3020, 2));
  TEST_ASSERT_FALSE(plausibleLiIonPackMv(9200, 2));
  TEST_ASSERT_TRUE(plausibleLiIonPackMv(5000, 2));
  TEST_ASSERT_TRUE(plausibleLiIonPackMv(5574, 2));  // measured dangerous pack
  TEST_ASSERT_TRUE(plausibleLiIonPackMv(8400, 2));
  TEST_ASSERT_TRUE(plausibleLiIonPackMv(2500, 1));
  TEST_ASSERT_TRUE(plausibleLiIonPackMv(4500, 1));
  TEST_ASSERT_FALSE(plausibleLiIonPackMv(3700, 0));
}

// CUM-15 lying-knob class: the low-battery preferences (lbRing / lbSaver) are
// live exactly when monitoring is on AND the sample reads valid - all four
// combos asserted, so a regression in either input fails by name instead of
// re-shipping toggles that toast success with zero effect.
static void test_batt_settings_live_all_four_combos() {
  TEST_ASSERT_FALSE(battSettingsLive(false, false));  // opted out, nothing sensed
  TEST_ASSERT_FALSE(battSettingsLive(false, true));   // opted out: user intent wins
  TEST_ASSERT_FALSE(battSettingsLive(true, false));   // promised, but no readable pack
  TEST_ASSERT_TRUE(battSettingsLive(true, true));     // monitoring a real pack
}

static void test_invalid_samples_change_nothing() {
  Policy p;
  Sample s;  // valid=false, onExternalPower=true
  for (uint32_t t = 0; t < 10000; t += 500) {
    PolicyEvents ev = p.update(s, t);
    TEST_ASSERT_FALSE(ev.enterT1);
    TEST_ASSERT_FALSE(ev.enterT2);
  }
  TEST_ASSERT_FALSE(p.forcedBatterySaver());
  TEST_ASSERT_FALSE(p.shutdownRequested());
  TEST_ASSERT_TRUE(p.onVbus());  // seeded from first sample
}

// Full scripted discharge: 100% -> T1 -> partial recovery inside hysteresis ->
// exit above hysteresis -> back down through T1 into T2.
static void test_discharge_through_thresholds_with_hysteresis() {
  // SimMonitor is 1S-scaled (3300-4200 mV), so give T2 a 1S floor: percent drives
  // T1 above it, voltage drives T2 below it - both FSMs genuinely exercised.
  Policy p(([]{ PolicyConfig c; c.t2PackMv = 3000; return c; })());
  SimMonitor m;
  uint32_t t = 0;
  auto step = [&](uint8_t pct) {
    m.setBattery(pct, /*external=*/false);
    t += 1000;
    return p.update(m.sample(), t);
  };

  TEST_ASSERT_FALSE(step(100).enterT1);
  TEST_ASSERT_FALSE(step(50).enterT1);
  TEST_ASSERT_FALSE(step(21).enterT1);

  PolicyEvents ev = step(20);     // exactly T1
  TEST_ASSERT_TRUE(ev.enterT1);
  TEST_ASSERT_TRUE(p.forcedBatterySaver());

  ev = step(23);                  // inside hysteresis band (< 25): stay forced
  TEST_ASSERT_FALSE(ev.exitT1);
  TEST_ASSERT_TRUE(p.forcedBatterySaver());

  ev = step(25);                  // at T1+hyst: release
  TEST_ASSERT_TRUE(ev.exitT1);
  TEST_ASSERT_FALSE(p.forcedBatterySaver());

  ev = step(19);                  // re-enter
  TEST_ASSERT_TRUE(ev.enterT1);

  // T2: three consecutive below-floor VOLTAGE samples (the debounce)
  ev = p.update(mvSample(2900, 19), t += 1000);
  TEST_ASSERT_FALSE(ev.enterT2);
  ev = p.update(mvSample(2900, 19), t += 1000);
  TEST_ASSERT_FALSE(ev.enterT2);
  ev = p.update(mvSample(2900, 19), t += 1000);
  TEST_ASSERT_TRUE(ev.enterT2);
  TEST_ASSERT_TRUE(p.shutdownRequested());
  TEST_ASSERT_TRUE(p.forcedBatterySaver());

  ev = step(30);                  // T2 is latched: recovery alone doesn't clear it
  TEST_ASSERT_TRUE(p.shutdownRequested());
}

// Jumping straight below T2 must fire both edges in one update.
static void test_t2_implies_t1() {
  // A voltage-T2 firing on a pack whose PERCENT never crossed T1 must still
  // force Battery Saver (T2 implies T1) in the same update.
  Policy p(([]{ PolicyConfig c; c.t2PackMv = 3000; return c; })());
  uint32_t t = 0;
  p.update(mvSample(2900, 50), t += 1000);
  p.update(mvSample(2900, 50), t += 1000);
  PolicyEvents ev = p.update(mvSample(2900, 50), t += 1000);   // 3rd: debounced
  TEST_ASSERT_TRUE(ev.enterT1);
  TEST_ASSERT_TRUE(ev.enterT2);
}

static void test_external_power_clears_latches() {
  Policy p(([]{ PolicyConfig c; c.t2PackMv = 3000; return c; })());
  SimMonitor m;
  uint32_t t0 = 0;
  for (int i = 0; i < 3; i++) p.update(mvSample(2900, 5), t0 += 300);
  TEST_ASSERT_TRUE(p.shutdownRequested());
  m.setBattery(5, false);
  p.update(m.sample(), 1000);

  // Plug in: VBUS raw flips, debounce runs, then latches clear.
  m.setBattery(5, /*external=*/true);
  PolicyEvents ev = p.update(m.sample(), 2000);
  TEST_ASSERT_FALSE(ev.vbusConnected);          // debounce not elapsed
  ev = p.update(m.sample(), 3600);              // 1600ms > 1500ms debounce
  TEST_ASSERT_TRUE(ev.vbusConnected);
  TEST_ASSERT_TRUE(ev.exitT1);
  TEST_ASSERT_FALSE(p.forcedBatterySaver());
  TEST_ASSERT_FALSE(p.shutdownRequested());
  TEST_ASSERT_TRUE(p.onVbus());
}

static void test_charging_flag_clears_latches_without_vbus_flag() {
  Policy p;
  SimMonitor m;
  m.setBattery(10, false);
  p.update(m.sample(), 1000);
  TEST_ASSERT_TRUE(p.forcedBatterySaver());
  m.setBattery(10, /*external=*/false, /*charging=*/true);
  PolicyEvents ev = p.update(m.sample(), 2000);
  TEST_ASSERT_TRUE(ev.exitT1);
  TEST_ASSERT_FALSE(p.forcedBatterySaver());
}

// A VBUS blip shorter than the debounce window must not produce events.
static void test_vbus_debounce_filters_blips() {
  Policy p;
  SimMonitor m;
  m.setBattery(50, false);
  p.update(m.sample(), 0);
  TEST_ASSERT_FALSE(p.onVbus());

  m.setBattery(50, true);                       // blip on
  p.update(m.sample(), 100);
  m.setBattery(50, false);                      // off again before 1500ms
  PolicyEvents ev = p.update(m.sample(), 800);
  TEST_ASSERT_FALSE(ev.vbusConnected);
  ev = p.update(m.sample(), 5000);
  TEST_ASSERT_FALSE(ev.vbusConnected);
  TEST_ASSERT_FALSE(p.onVbus());

  // Sustained connect fires exactly one event.
  m.setBattery(50, true);
  p.update(m.sample(), 6000);
  ev = p.update(m.sample(), 8000);
  TEST_ASSERT_TRUE(ev.vbusConnected);
  ev = p.update(m.sample(), 9000);
  TEST_ASSERT_FALSE(ev.vbusConnected);

  // Sustained disconnect fires the opposite edge.
  m.setBattery(50, false);
  p.update(m.sample(), 10000);
  ev = p.update(m.sample(), 12000);
  TEST_ASSERT_TRUE(ev.vbusDisconnected);
}

// liIonPercent: the per-cell SoC curve used by the ADC battery monitor.
static void test_liion_percent_curve() {
  TEST_ASSERT_EQUAL_UINT8(100, liIonPercent(4200));  // full
  TEST_ASSERT_EQUAL_UINT8(100, liIonPercent(4300));  // above full -> clamp 100
  TEST_ASSERT_EQUAL_UINT8(0,   liIonPercent(3200));  // empty
  TEST_ASSERT_EQUAL_UINT8(0,   liIonPercent(3000));  // below empty -> clamp 0
  TEST_ASSERT_EQUAL_UINT8(80,  liIonPercent(4000));  // exact knot
  TEST_ASSERT_EQUAL_UINT8(42,  liIonPercent(3700));  // exact knot
  // monotonic non-increasing as voltage drops
  uint8_t prev = 100;
  for (uint16_t mv = 4200; mv >= 3200; mv -= 25) {
    uint8_t p = liIonPercent(mv);
    TEST_ASSERT_TRUE(p <= prev);
    prev = p;
  }
  // interpolated midpoint between 4000(80) and 3900(68) ~ 74 at 3950
  uint8_t mid = liIonPercent(3950);
  TEST_ASSERT_TRUE(mid > 68 && mid < 80);
}


// ── voltage-grounded T2 (owner feature 2026-07-17) ─────────────────────────────
// The pack's own BMS let it fall to 5574 mV live (measured); the percent scale
// read 0% with ~33% really left. So T2 now triggers on MEASURED pack mV: default
// 6000 = ~10% real SoC from the study's curve, debounced, overridable.
// (mvSample moved above the first test - see top of file)

void test_t2_fires_on_voltage_after_debounce(void) {
  nimbus::power::PolicyConfig c;   // defaults: t2PackMv=6000, consec=3
  nimbus::power::Policy p(c);
  uint32_t t = 0;
  // percent says 50% the whole time - voltage is what matters now
  for (int i = 0; i < 2; i++) {
    auto ev = p.update(mvSample(5990), t += 60000);
    TEST_ASSERT_FALSE(ev.enterT2);            // 1st + 2nd below: not yet
  }
  auto ev = p.update(mvSample(5985), t += 60000);
  TEST_ASSERT_TRUE(ev.enterT2);               // 3rd consecutive: fire
  TEST_ASSERT_TRUE(p.shutdownRequested());
}

void test_t2_debounce_resets_on_recovery(void) {
  nimbus::power::Policy p({});
  uint32_t t = 0;
  p.update(mvSample(5990), t += 60000);
  p.update(mvSample(5990), t += 60000);
  p.update(mvSample(6400), t += 60000);       // an LED-sag dip that recovered
  auto ev = p.update(mvSample(5990), t += 60000);
  TEST_ASSERT_FALSE(ev.enterT2);              // counter restarted - 1 of 3
  TEST_ASSERT_FALSE(p.shutdownRequested());
}

void test_t2_override_skips_and_clears(void) {
  nimbus::power::Policy p({});
  uint32_t t = 0;
  for (int i = 0; i < 3; i++) p.update(mvSample(5990), t += 60000);
  TEST_ASSERT_TRUE(p.shutdownRequested());    // latched
  p.setT2Override(true);                      // owner/AI accepts the risk
  TEST_ASSERT_FALSE(p.shutdownRequested());   // cleared immediately
  for (int i = 0; i < 5; i++) p.update(mvSample(5500), t += 60000);
  TEST_ASSERT_FALSE(p.shutdownRequested());   // and never re-fires while overridden
  p.setT2Override(false);
  for (int i = 0; i < 3; i++) p.update(mvSample(5500), t += 60000);
  TEST_ASSERT_TRUE(p.shutdownRequested());    // protection re-arms when cleared
}

void test_t2_zero_truly_disables(void) {
  // "0 = off" must mean OFF. (It used to fall back to the legacy percent-T2 -
  // an UNDEBOUNCED trigger on the scale the study proved reads 0% with a third
  // of the pack left, i.e. "off" was secretly MORE aggressive. Review finding.)
  nimbus::power::PolicyConfig c;
  c.t2PackMv = 0;
  nimbus::power::Policy p(c);
  uint32_t t = 0;
  for (int i = 0; i < 5; i++) {
    auto ev = p.update(mvSample(5200, /*pct=*/1), t += 60000);   // dire by every scale
    TEST_ASSERT_FALSE(ev.enterT2);
  }
  TEST_ASSERT_FALSE(p.shutdownRequested());
}

void test_bright_cap_clamps_unless_overridden(void) {
  using nimbus::power::clampBright;
  using nimbus::power::kBrightCap;
  TEST_ASSERT_EQUAL_UINT8(100, clampBright(100, false));
  TEST_ASSERT_EQUAL_UINT8(kBrightCap, clampBright(255, false));   // capped at 60%
  TEST_ASSERT_EQUAL_UINT8(kBrightCap, clampBright(154, false));
  TEST_ASSERT_EQUAL_UINT8(255, clampBright(255, true));           // override: full
  TEST_ASSERT_EQUAL_UINT8(10, clampBright(10, true));
}


// ── post-sleep wake hysteresis (owner: "drop a few % then wake above a bar") ───
// Rested-EMPTY packs measured 6918-6992 mV - above the 6000 sleep floor - so a
// same-threshold wake test oscillates forever. The wake bar sits above rest.
void test_rested_empty_pack_does_not_stay_awake(void) {
  using nimbus::power::stayAwakeAfterSleep;
  // the measured rested-empty voltages: must go BACK to sleep
  TEST_ASSERT_FALSE(stayAwakeAfterSleep(6918, 6000, 7200, false));
  TEST_ASSERT_FALSE(stayAwakeAfterSleep(6992, 6000, 7200, false));
  // a genuinely recovered / charging pack stays up
  TEST_ASSERT_TRUE(stayAwakeAfterSleep(7250, 6000, 7200, false));
  TEST_ASSERT_TRUE(stayAwakeAfterSleep(6500, 6000, 7200, true));   // charger wins
  TEST_ASSERT_FALSE(stayAwakeAfterSleep(0, 6000, 7200, false));    // no reading
}

void test_wake_default_is_the_owners_6500(void) {
  // OWNER'S CALL (2026-07-17): default 6500 sits BELOW the measured rested-empty
  // band (6918-6992), deliberately trading deep-discharge wear for bottom-end
  // runtime - so a rested-empty pack DOES stay awake at the default. This test
  // pins that as a DECISION, not an accident; 7200 remains the strict setting.
  TEST_ASSERT_EQUAL_UINT16(6500, nimbus::power::kWakeMvDefault);
  using nimbus::power::stayAwakeAfterSleep;
  TEST_ASSERT_TRUE(stayAwakeAfterSleep(6918, 6000, nimbus::power::kWakeMvDefault, false));
  TEST_ASSERT_FALSE(stayAwakeAfterSleep(6400, 6000, nimbus::power::kWakeMvDefault, false));
}

void test_wake_bar_never_below_sleep_threshold(void) {
  using nimbus::power::stayAwakeAfterSleep;
  // misconfigured wakeMv below sleepMv must not create a stay-awake-while-
  // below-the-sleep-floor contradiction
  TEST_ASSERT_FALSE(stayAwakeAfterSleep(5900, 6000, 5000, false));
  TEST_ASSERT_TRUE(stayAwakeAfterSleep(6000, 6000, 0, false));     // bar clamps to sleep
}


// ── configurable divider math (owner feature: 220/100 vs 270/120 boards) ───────
// The store computes dividerX100 = (Rtop+Rbot)/Rbot*100; the ADC multiplies the
// node mV by it. This pins the two real board configs.
static uint16_t dividerX100(uint32_t rtop, uint32_t rbot) {
  uint32_t d = (uint64_t(rtop + rbot) * 100) / (rbot ? rbot : 1);
  return d < 100 ? 100 : (d > 2000 ? 2000 : (uint16_t)d);
}
void test_divider_x100_for_both_real_boards(void) {
  TEST_ASSERT_EQUAL_UINT16(320, dividerX100(220000, 100000));   // baked-in default
  TEST_ASSERT_EQUAL_UINT16(325, dividerX100(270000, 120000));   // the ACTUAL boards
  // a full 8.40 V pack on the node: /3.25 boards read the node at 2585 mV; scaling
  // back with the CORRECT 325 recovers 8402 vs the wrong 320 giving 8272 (~1.6% low)
  const uint32_t nodeMv = 2585;
  TEST_ASSERT_INT_WITHIN(5, 8401, int(nodeMv * dividerX100(270000,120000) / 100));
  TEST_ASSERT_INT_WITHIN(5, 8272, int(nodeMv * 320 / 100));     // the on-device bug
}
// ── AlertGate (field bug 2026-08-11: "spamming 0% non stop") ─────────────────
// The Policy's enterT1 edge legitimately re-fires - these tests pin that the
// owner PING has its own memory regardless of how many edges arrive.

static constexpr uint32_t kEp = 1786400000;   // a sane 2026 epoch

// Every call models one battery-telemetry tick: (calibratedPct, discharging, epoch).
static void test_alert_gate_pings_once_per_episode() {
  AlertGate g;
  // Above the 20% threshold: a discharging pack stays silent all the way down.
  TEST_ASSERT_FALSE(g.shouldPing(93, true, kEp - 7200));
  TEST_ASSERT_FALSE(g.shouldPing(45, true, kEp - 3600));  // ~ the raw-T1-edge point
  TEST_ASSERT_FALSE(g.shouldPing(21, true, kEp - 60));
  TEST_ASSERT_TRUE(g.shouldPing(20, true, kEp));          // crossing 20%: ping
  // Later ticks in the same episode (still low, still discharging): silent.
  TEST_ASSERT_FALSE(g.shouldPing(18, true, kEp + 300));
  TEST_ASSERT_FALSE(g.shouldPing(9, true, kEp + 3600));
  TEST_ASSERT_EQUAL_UINT32(kEp, g.persistEpoch());
}

static void test_alert_gate_charging_at_low_pct_never_pings() {
  AlertGate g;
  // 15% but charging (or externally powered): good news, not an alert.
  TEST_ASSERT_FALSE(g.shouldPing(15, false, kEp));
  TEST_ASSERT_FALSE(g.shouldPing(2, false, kEp + 60));
  TEST_ASSERT_EQUAL_UINT32(0, g.persistEpoch());  // nothing recorded
}

static void test_alert_gate_survives_the_wake_sniff_reboot_loop() {
  // The T2 sleep wakes every 5 minutes for a charger sniff; each wake is a
  // FULL boot whose first telemetry tick sees a low discharging pack. The
  // persisted epoch must keep it silent.
  uint32_t persisted = 0;
  int pings = 0;
  for (int wake = 0; wake < 24; wake++) {         // 2 hours of 5-min wakes
    AlertGate g(persisted);                       // fresh boot, persisted state
    if (g.shouldPing(8, true, kEp + (uint32_t)wake * 300)) pings++;
    persisted = g.persistEpoch();
  }
  TEST_ASSERT_EQUAL_MESSAGE(1, pings,
      "the wake-sniff loop must produce exactly ONE owner ping, not one per wake");
}

static void test_alert_gate_rearms_on_external_power() {
  AlertGate g;
  TEST_ASSERT_TRUE(g.shouldPing(19, true, kEp));
  g.noteExternalPower();                          // charged up
  TEST_ASSERT_EQUAL_UINT32(0, g.persistEpoch());  // caller persists the re-arm
  TEST_ASSERT_TRUE(g.shouldPing(19, true, kEp + 600));  // next discharge: news again
}

static void test_alert_gate_cooldown_expiry_repings() {
  AlertGate g;
  TEST_ASSERT_TRUE(g.shouldPing(20, true, kEp));
  TEST_ASSERT_FALSE(g.shouldPing(12, true, kEp + AlertGate::kDefaultCooldownS - 1));
  TEST_ASSERT_TRUE(g.shouldPing(12, true, kEp + AlertGate::kDefaultCooldownS));
}

static void test_alert_gate_unsynced_clock() {
  // Pre-SNTP boot (epoch ~0): a prior persisted ping suppresses (can't compute
  // a cooldown, assume within it); a never-pinged gate allows exactly one.
  AlertGate prior(kEp);
  TEST_ASSERT_FALSE(prior.shouldPing(10, true, 42));  // wake boot before SNTP: silent
  AlertGate fresh;
  TEST_ASSERT_TRUE(fresh.shouldPing(10, true, 42));   // genuine first alert
  TEST_ASSERT_FALSE(fresh.shouldPing(9, true, 43));   // but only one per boot
}

// ── per-cell sleep/wake thresholds (CUM-202) ─────────────────────────────────
// The sleep + wake mV are PACK voltages; their defaults and clamp ceilings scale
// with the series-cell count so a 1S board is not judged against a 2S floor.
void test_percell_threshold_seeds(void) {
  using namespace nimbus::power;
  // 2S reproduces the historical constants EXACTLY (no behaviour change on the Solide).
  TEST_ASSERT_EQUAL_UINT16(6000, sleepMvDefaultFor(2));
  TEST_ASSERT_EQUAL_UINT16(6800, sleepMvCeilFor(2));
  TEST_ASSERT_EQUAL_UINT16(6500, wakeMvDefaultFor(2));
  TEST_ASSERT_EQUAL_UINT16(7600, wakeMvCeilFor(2));
  TEST_ASSERT_EQUAL_UINT16(kWakeMvDefault, wakeMvDefaultFor(2));
  // 1S halves them - all below a full 1S cell (~4200 mV).
  TEST_ASSERT_EQUAL_UINT16(3000, sleepMvDefaultFor(1));
  TEST_ASSERT_EQUAL_UINT16(3400, sleepMvCeilFor(1));
  TEST_ASSERT_EQUAL_UINT16(3250, wakeMvDefaultFor(1));
  TEST_ASSERT_EQUAL_UINT16(3800, wakeMvCeilFor(1));
  // 0 cells is treated as 1 (never divide-by / multiply-by zero).
  TEST_ASSERT_EQUAL_UINT16(sleepMvDefaultFor(1), sleepMvDefaultFor(0));
}

// The core CUM-202 defect: a full 1S pack must NOT trip the voltage-grounded T2.
void test_1s_full_pack_does_not_insta_sleep(void) {
  using namespace nimbus::power;
  // The 1S default sleep threshold, exactly as the store now seeds it.
  Policy p(PolicyConfig{20, 8, 5, 1500, sleepMvDefaultFor(1), 3, false});
  uint32_t t = 0;
  // A full 1S cell reads ~4200 mV pack; hammer it well past the debounce count.
  for (int i = 0; i < 10; i++) {
    auto ev = p.update(mvSample(4200, 100), t += 60000);
    TEST_ASSERT_FALSE(ev.enterT2);
  }
  TEST_ASSERT_FALSE(p.shutdownRequested());
  // And a genuinely empty 1S cell (~3000 mV) still protects after the debounce.
  Policy q(PolicyConfig{20, 8, 5, 1500, sleepMvDefaultFor(1), 3, false});
  uint32_t u = 0;
  for (int i = 0; i < 2; i++) TEST_ASSERT_FALSE(q.update(mvSample(2990, 2), u += 60000).enterT2);
  TEST_ASSERT_TRUE(q.update(mvSample(2990, 2), u += 60000).enterT2);
  TEST_ASSERT_TRUE(q.shutdownRequested());
}

// Documents the bug being fixed: the OLD 2S default (6000) on a 1S pack was an
// unconditional sleep - a full 4200 mV pack is below 6000 and sleeps after debounce.
void test_2s_default_would_have_slept_a_full_1s_pack(void) {
  using namespace nimbus::power;
  Policy bad(PolicyConfig{20, 8, 5, 1500, /*t2PackMv=*/6000, 3, false});
  uint32_t t = 0;
  bad.update(mvSample(4200, 100), t += 60000);
  bad.update(mvSample(4200, 100), t += 60000);
  auto ev = bad.update(mvSample(4200, 100), t += 60000);
  TEST_ASSERT_TRUE(ev.enterT2);            // the pre-fix behaviour: full pack, asleep
  TEST_ASSERT_TRUE(bad.shutdownRequested());
}

// ── explicit per-board battMon default (CUM-202) ─────────────────────────────
// Pins the shipped policy so it cannot drift back into a fragile proxy: the
// Solide S3 ships with a pack (ON); the Freenove/all-in-one CYD treats a battery
// as an optional add-on (OFF, opt-in). An unknown board is OFF (the safe default).
void test_board_battmon_defaults(void) {
  using nimbus::power::battMonDefaultForBoard;
  TEST_ASSERT_TRUE(battMonDefaultForBoard("solide_s3"));     // hand-built 2S, shipped ON
  TEST_ASSERT_FALSE(battMonDefaultForBoard("freenove_s3"));  // all-in-one, opt-in
  TEST_ASSERT_FALSE(battMonDefaultForBoard("some_future_board"));  // unknown -> safe OFF
  TEST_ASSERT_FALSE(battMonDefaultForBoard(nullptr));        // null -> safe OFF
  // constexpr: the policy is resolved at compile time (no runtime cost on device).
  static_assert(nimbus::power::battMonDefaultForBoard("solide_s3"), "Solide S3 defaults ON");
  static_assert(!nimbus::power::battMonDefaultForBoard("freenove_s3"), "Freenove defaults OFF");
  // Both shipped boards are present in the table (a new board must add a row).
  bool haveSolide = false, haveFreenove = false;
  for (const auto& e : nimbus::power::kBoardBattMonDefaults) {
    if (nimbus::power::boardSlugEq(e.boardSlug, "solide_s3")) haveSolide = true;
    if (nimbus::power::boardSlugEq(e.boardSlug, "freenove_s3")) haveFreenove = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(haveSolide, "solide_s3 must have an explicit battMon default row");
  TEST_ASSERT_TRUE_MESSAGE(haveFreenove, "freenove_s3 must have an explicit battMon default row");
}

// FIX 3: a battery board whose sense line has failed OPEN reads persistently
// invalid, which the SAFETY policy treats as desk-powered - so an open sense line
// and a genuinely absent pack look identical over the wire. These pin the honest
// predicate (monitoringOn AND debounced-invalid) that lets them be told apart.

// The pure predicate: missing only when monitoring is on and the streak has
// reached the threshold.
static void test_sense_missing_predicate() {
  using nimbus::power::senseMissing;
  TEST_ASSERT_FALSE(senseMissing(false, 99, 3));  // monitoring off: never
  TEST_ASSERT_FALSE(senseMissing(true, 2, 3));    // streak below threshold
  TEST_ASSERT_TRUE(senseMissing(true, 3, 3));     // streak at threshold
  TEST_ASSERT_TRUE(senseMissing(true, 9, 3));     // streak past threshold
  TEST_ASSERT_FALSE(senseMissing(true, 9, 0));    // zero threshold disables
}

// The critical false-positive guard: a genuinely desk-powered board (monitoring
// OFF) reads invalid forever and must NEVER be reported as a sense fault.
static void test_sense_missing_never_trips_when_monitoring_off() {
  nimbus::power::SenseMissingDetector d(3);
  for (int i = 0; i < 50; i++) TEST_ASSERT_FALSE(d.update(/*monOn=*/false, /*valid=*/false));
  TEST_ASSERT_FALSE(d.missing());
}

// It does not flip on a single invalid sample; it trips only after the debounce
// window of consecutive invalid samples while monitoring is on.
static void test_sense_missing_needs_debounce_then_trips() {
  nimbus::power::SenseMissingDetector d(3);
  TEST_ASSERT_FALSE(d.update(true, false));  // 1
  TEST_ASSERT_FALSE(d.update(true, false));  // 2
  TEST_ASSERT_TRUE(d.update(true, false));   // 3 -> tripped
  TEST_ASSERT_TRUE(d.update(true, false));   // stays tripped
}

// It clears IMMEDIATELY when a valid sample arrives (a pack plugged in / sense
// recovered) - no lingering false fault.
static void test_sense_missing_clears_immediately_on_valid() {
  nimbus::power::SenseMissingDetector d(3);
  d.update(true, false);
  d.update(true, false);
  TEST_ASSERT_TRUE(d.update(true, false));   // tripped
  TEST_ASSERT_FALSE(d.update(true, true));   // one valid sample clears it at once
  TEST_ASSERT_EQUAL_UINT16(0, d.invalidStreak());
  // And turning monitoring off also clears immediately.
  d.update(true, false); d.update(true, false); d.update(true, false);
  TEST_ASSERT_TRUE(d.missing());
  TEST_ASSERT_FALSE(d.update(false, false));
}

// The default threshold debounces more than one tick (no single-sample trip).
static void test_sense_missing_default_threshold() {
  nimbus::power::SenseMissingDetector d;   // default threshold
  TEST_ASSERT_TRUE(d.threshold() >= 2);
  TEST_ASSERT_FALSE(d.update(true, false));  // first invalid never trips
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_liion_percent_curve);
  RUN_TEST(test_board_battmon_defaults);
  RUN_TEST(test_percell_threshold_seeds);
  RUN_TEST(test_1s_full_pack_does_not_insta_sleep);
  RUN_TEST(test_2s_default_would_have_slept_a_full_1s_pack);
  RUN_TEST(test_null_monitor_is_desk_powered);
  RUN_TEST(test_liion_pack_plausibility_rejects_a_floating_divider);
  RUN_TEST(test_batt_settings_live_all_four_combos);
  RUN_TEST(test_invalid_samples_change_nothing);
  RUN_TEST(test_discharge_through_thresholds_with_hysteresis);
  RUN_TEST(test_t2_implies_t1);
  RUN_TEST(test_external_power_clears_latches);
  RUN_TEST(test_charging_flag_clears_latches_without_vbus_flag);
  RUN_TEST(test_vbus_debounce_filters_blips);
  RUN_TEST(test_t2_fires_on_voltage_after_debounce);
  RUN_TEST(test_t2_debounce_resets_on_recovery);
  RUN_TEST(test_t2_override_skips_and_clears);
  RUN_TEST(test_t2_zero_truly_disables);
  RUN_TEST(test_bright_cap_clamps_unless_overridden);
  RUN_TEST(test_rested_empty_pack_does_not_stay_awake);
  RUN_TEST(test_wake_bar_never_below_sleep_threshold);
  RUN_TEST(test_wake_default_is_the_owners_6500);
  RUN_TEST(test_divider_x100_for_both_real_boards);
  RUN_TEST(test_alert_gate_pings_once_per_episode);
  RUN_TEST(test_alert_gate_charging_at_low_pct_never_pings);
  RUN_TEST(test_alert_gate_survives_the_wake_sniff_reboot_loop);
  RUN_TEST(test_alert_gate_rearms_on_external_power);
  RUN_TEST(test_alert_gate_cooldown_expiry_repings);
  RUN_TEST(test_alert_gate_unsynced_clock);
  RUN_TEST(test_sense_missing_predicate);
  RUN_TEST(test_sense_missing_never_trips_when_monitoring_off);
  RUN_TEST(test_sense_missing_needs_debounce_then_trips);
  RUN_TEST(test_sense_missing_clears_immediately_on_valid);
  RUN_TEST(test_sense_missing_default_threshold);
  return UNITY_END();
}
