#include <unity.h>

#include "nimbus/power/battery_model.h"
#include "nimbus/power/power_monitor.h"   // liIonPercent / liIonCellMvForPct (curve)

using namespace nimbus::power;

void setUp() {}
void tearDown() {}

// ⚠ dis(mv, pct) lets a test state mv and pct INDEPENDENTLY, which the real device
// can never do: AdcBatteryMonitor::sample() sets percent = liIonPercent(cellMv), so
// the two are always consistent by construction (src/hw/power_battery_adc.cpp). The
// model now derives the percent from the voltage itself (on the CALIBRATED scale it
// also reports), so tests that need a specific SoC must pick the voltage that MEANS
// that SoC - disPct() does it via the curve's own inverse.
static Sample disPct(uint8_t pct);

// Build a valid on-battery sample for a 1S cell at the given per-cell mV/percent.
static Sample dis(uint16_t mv, uint8_t pct) {
  Sample s;
  s.valid = true;
  s.onExternalPower = false;
  s.charging = false;
  s.millivolts = mv;
  s.percent = pct;
  return s;
}

// A sample at the voltage that genuinely MEANS this SoC (curve inverse) - the only
// mv/pct pairing the hardware can actually produce.
static Sample disPct(uint8_t pct) { return dis(liIonCellMvForPct(pct), pct); }

static const uint32_t MIN = 60000;   // one minute in ms
static const uint32_t HR = 3600000;

// A steady 10%/hr discharge should project ~ (pct/10)*60 minutes to empty.
static void test_time_to_empty_from_rate() {
  BatteryModel m(1);
  uint32_t t = 0;
  // A steady ~10%/hr discharge. Voltages come from the curve inverse; the model
  // derives the SoC from them, and the curve's integer math means a step can land
  // +-1%, so use hour-scale steps where that rounding is noise rather than signal.
  m.update(t, disPct(80));
  t += 1 * HR; m.update(t, disPct(70));
  t += 1 * HR; m.update(t, disPct(60));
  BatteryEstimate e = m.estimate();
  TEST_ASSERT_TRUE(e.valid);
  TEST_ASSERT_EQUAL(int(ChargeState::Discharging), int(e.chargeState));
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 10.0f, e.ratePctPerHr);   // ~10%/hr
  // ~60% / ~10%/hr = ~6 h = ~360 min.
  TEST_ASSERT_INT32_WITHIN(70, 360, e.minutesToEmpty);
}

// Charging / external power yields no time estimate and doesn't corrupt state.
static void test_external_power_no_estimate() {
  BatteryModel m(1);
  uint32_t t = 0;
  m.update(t, dis(3800, 80));
  t += 6 * MIN; m.update(t, dis(3790, 79));
  Sample ext = dis(3900, 80); ext.onExternalPower = true;
  t += 6 * MIN; m.update(t, ext);
  BatteryEstimate e = m.estimate();
  TEST_ASSERT_FALSE(e.valid);
  TEST_ASSERT_EQUAL_INT32(-1, e.minutesToEmpty);
  TEST_ASSERT_TRUE(e.onExternalPower);
}

// Cells: a 2S pack's per-cell mV = pack/2 drives the band/health logic.
static void test_cells_divide_pack_voltage() {
  BatteryModel m(2);
  uint32_t t = 0;
  // pack 7800 mV => 3900/cell (band start). Walk down the band to close it.
  m.update(t, dis(7900, 60));
  t += 30 * MIN; m.update(t, dis(7800, 55));   // 3900/cell -> band timing starts
  t += 30 * MIN; m.update(t, dis(7100, 40));   // 3550/cell -> band closes, baseline set
  TEST_ASSERT_EQUAL_UINT8(100, m.estimate().healthPct);   // first traversal = as-new
}

// Degradation: a second discharge that crosses the band FASTER than the baseline
// reports reduced health; persistence carries the baseline across a reboot.
static void test_health_degrades_on_faster_band() {
  BatteryModel m(1);
  uint32_t t = 0;
  // Baseline traversal: 3900 -> 3550 over 60 min.
  m.update(t, dis(3950, 70));
  t += 20 * MIN; m.update(t, dis(3899, 60));   // enter band
  t += 60 * MIN; m.update(t, dis(3550, 30));   // close band -> baseline = 60 min
  BatteryModelState st = m.save();
  TEST_ASSERT_EQUAL_UINT8(100, st.healthPct);
  TEST_ASSERT_TRUE(st.baselineBandMs > 0);

  // Reboot: a fresh model restores the baseline, then a 30-min traversal (half)
  // => ~50% health.
  BatteryModel m2(1);
  m2.load(st);
  uint32_t u = 0;
  m2.update(u, dis(3950, 70));
  u += 5 * MIN; m2.update(u, dis(3899, 62));   // enter band
  u += 30 * MIN; m2.update(u, dis(3550, 40));  // close band in 30 min = half baseline
  uint8_t h = m2.estimate().healthPct;
  TEST_ASSERT_INT_WITHIN(6, 50, h);
}

// Learning: completed segments feed a history rate, and save()/load() persist it
// (segments counter grows; learned rate becomes non-zero).
static void test_segment_learning_persists() {
  BatteryModel m(1);
  uint32_t t = 0;
  m.update(t, disPct(60));                 // seg opens (first sample)
  t += 2 * HR; m.update(t, disPct(20));    // cross low mark -> segment closes (40% / 2h = 20%/hr)
  BatteryModelState st = m.save();
  TEST_ASSERT_EQUAL_UINT16(1, st.segments);
  TEST_ASSERT_FLOAT_WITHIN(3.0f, 20.0f, st.learnedRatePctPerHr);

  BatteryModel m2(1);
  m2.load(st);
  TEST_ASSERT_EQUAL_UINT16(1, m2.estimate().segments);
}

// No telemetry -> invalid estimate, health defaults to as-new.
static void test_invalid_sample() {
  BatteryModel m(1);
  Sample none;   // valid=false
  m.update(0, none);
  BatteryEstimate e = m.estimate();
  TEST_ASSERT_FALSE(e.valid);
  TEST_ASSERT_EQUAL_UINT8(100, e.healthPct);
}

// ---- charge-state trend + calibration (the full-on-charger bug) --------------

// A rising voltage is charging: no time-to-empty, state = Charging.
static void test_rising_voltage_is_charging() {
  BatteryModel m(2);
  uint32_t t = 0;
  m.update(t, dis(7600, 50));                 // cell 3800
  t += 5 * MIN; m.update(t, dis(7700, 55));   // +50 mV cell -> rising
  t += 5 * MIN; m.update(t, dis(7800, 60));
  BatteryEstimate e = m.estimate();
  TEST_ASSERT_EQUAL(int(ChargeState::Charging), int(e.chargeState));
  TEST_ASSERT_FALSE(e.valid);
  TEST_ASSERT_EQUAL_INT32(-1, e.minutesToEmpty);
  TEST_ASSERT_TRUE(e.onExternalPower);
}

// THE REGRESSION: a full pack on the charger reads a flat voltage while the raw
// percent DITHERS (72<->73). This must NOT be read as a discharge; after the flat
// hold it is classified Full and reads 100% - not a bogus "278 min to empty".
static void test_full_plateau_not_faked_as_discharge() {
  BatteryModel m(2);
  uint32_t t = 0;
  // ~3940 mV/cell (a true-full 8.4 V pack that the S3 ADC under-reads), voltage
  // jittering < hysteresis while the percent dithers. 2-min cadence for 24 min.
  const uint16_t packs[] = {7882, 7878, 7884, 7880, 7878, 7882, 7880, 7884,
                            7878, 7880, 7882, 7878, 7880};
  const uint8_t  pcts[]  = {73, 72, 73, 72, 72, 73, 72, 73, 72, 72, 73, 72, 72};
  for (int i = 0; i < 13; i++) { m.update(t, dis(packs[i], pcts[i])); t += 2 * MIN; }
  BatteryEstimate e = m.estimate();
  TEST_ASSERT_FALSE(e.valid);                                  // no discharge estimate
  TEST_ASSERT_EQUAL_INT32(-1, e.minutesToEmpty);
  TEST_ASSERT_EQUAL(int(ChargeState::Full), int(e.chargeState));
  TEST_ASSERT_TRUE(e.onExternalPower);
  TEST_ASSERT_EQUAL_UINT8(100, e.percent);                     // full reads 100%, not 72%
}

// Owner asserts full -> instant calibration; anchor persists across a reboot and
// the corrected curve reads 100% at the (under-read) full voltage.
static void test_calibrate_full_now_persists() {
  BatteryModel m(2);
  m.update(0, dis(7880, 72));            // cell 3940, under-read full
  TEST_ASSERT_EQUAL_UINT8(72, m.estimate().percent);   // uncalibrated: raw curve
  TEST_ASSERT_TRUE(m.calibrateFullNow());
  BatteryEstimate e = m.estimate();
  TEST_ASSERT_EQUAL_UINT8(100, e.percent);
  TEST_ASSERT_TRUE(e.calibrated);

  BatteryModelState st = m.save();
  TEST_ASSERT_TRUE(st.fullAnchorCellMv >= 3900);
  BatteryModel m2(2);
  m2.load(st);
  m2.update(0, dis(7880, 72));
  TEST_ASSERT_EQUAL_UINT8(100, m2.estimate().percent);   // calibration survived reboot
}

// Calibration only stretches the compressed TOP band; below the ADC knee the
// reading is untouched (the ADC is accurate there).
static void test_calibration_leaves_low_range_intact() {
  BatteryModel m(2);
  m.update(0, dis(7880, 72));
  TEST_ASSERT_TRUE(m.calibrateFullNow());   // anchor at cell 3940
  // A low pack (cell 3500 = pack 7000) is below the knee: still ~15%, not inflated.
  m.update(1000, dis(7000, 15));
  uint8_t p = m.estimate().percent;
  TEST_ASSERT_INT_WITHIN(4, 15, p);
}

// An AUTO-learned full anchor (a flat plateau at the top, no owner BATTCAL) sets a
// persistable fullAnchorMv() > knee - the value the device now persists the MOMENT
// it changes (main.cpp save-on-anchor-change), so an auto-calibration survives
// reboot instead of being lost unless a discharge segment happens to close. That
// lost-auto-anchor gap is why two identical boards on the same power drifted apart
// (one showing "full", one "76%").
static void test_auto_plateau_anchor_is_persistable() {
  BatteryModel m(2);
  uint32_t t = 0;
  const uint16_t packs[] = {7882, 7878, 7884, 7880, 7878, 7882, 7880, 7884,
                            7878, 7880, 7882, 7878, 7880};
  const uint8_t  pcts[]  = {73, 72, 73, 72, 72, 73, 72, 73, 72, 72, 73, 72, 72};
  for (int i = 0; i < 13; i++) { m.update(t, dis(packs[i], pcts[i])); t += 2 * MIN; }
  TEST_ASSERT_EQUAL(int(ChargeState::Full), int(m.estimate().chargeState));
  const uint16_t anchor = m.fullAnchorMv();
  TEST_ASSERT_TRUE(anchor > BatteryModel::kCalKneeMv);     // a real, correction-applying anchor
  BatteryModelState st = m.save();
  TEST_ASSERT_EQUAL_UINT16(anchor, st.fullAnchorCellMv);   // present in the persisted blob
  BatteryModel m2(2);
  m2.load(st);
  m2.update(0, dis(7880, 72));
  TEST_ASSERT_EQUAL_UINT8(100, m2.estimate().percent);     // survived the reboot
}

// Regression (prism 2026-07-12): a pack draining SLOWER than the hysteresis per
// sample must not get pinned in External by the flat-hold branch - the cumulative
// drop from the trend anchor must still flip it back to Discharging (the old code
// re-anchored refMv_ on every flat-hold fire, collapsing the cumulative window).
static void test_slow_discharge_not_pinned_external() {
  BatteryModel m(1);
  uint32_t t = 0;
  m.update(t, dis(3700, 45));                 // seed mid-SoC (below cal knee)
  t += 6 * MIN; m.update(t, dis(3698, 45));   // sub-hysteresis wobble...
  t += 6 * MIN; m.update(t, dis(3696, 44));   // ...~12 min flat -> fires External
  TEST_ASSERT_EQUAL(int(ChargeState::External), int(m.estimate().chargeState));
  t += 6 * MIN; m.update(t, dis(3694, 44));
  t += 6 * MIN; m.update(t, dis(3690, 43));   // cumulative -10 mV from the 3700 anchor
  TEST_ASSERT_EQUAL(int(ChargeState::Discharging), int(m.estimate().chargeState));
}

static void test_charge_state_strings() {
  TEST_ASSERT_EQUAL_STRING("discharging", chargeStateStr(ChargeState::Discharging));
  TEST_ASSERT_EQUAL_STRING("charging", chargeStateStr(ChargeState::Charging));
  TEST_ASSERT_EQUAL_STRING("full", chargeStateStr(ChargeState::Full));
  TEST_ASSERT_EQUAL_STRING("external", chargeStateStr(ChargeState::External));
  TEST_ASSERT_EQUAL_STRING("unknown", chargeStateStr(ChargeState::Unknown));
}

// battery-measurement: runtime-grounded health. A full-ish cycle (starting above the band)
// down to the low mark sets the as-new baseline; a half-length cycle after a reboot reports
// ~50% health, and it's preferred over the band-traversal proxy.
static void test_runtime_health_grounds_on_full_cycle() {
  BatteryModel m(1);
  uint32_t t = 0;
  m.update(t, dis(3950, 90));                  // start above band -> full-cycle timing on
  t += 60 * MIN; m.update(t, dis(3800, 60));   // discharging
  t += 60 * MIN; m.update(t, dis(3540, 30));   // <=3550 -> full cycle closes (120 min baseline)
  BatteryModelState st = m.save();
  TEST_ASSERT_TRUE(st.baselineRuntimeSec > 0);
  TEST_ASSERT_EQUAL_UINT8(100, st.healthPct);

  BatteryModel m2(1);
  m2.load(st);                                 // reboot restores the runtime baseline
  uint32_t u = 0;
  m2.update(u, dis(3950, 90));
  u += 30 * MIN; m2.update(u, dis(3800, 55));
  u += 30 * MIN; m2.update(u, dis(3540, 25));  // full cycle in 60 min = half the baseline
  TEST_ASSERT_INT_WITHIN(8, 50, m2.estimate().healthPct);
}

// The new baselineRuntimeSec field round-trips through save()/load().
static void test_battmodel_state_roundtrips_runtime() {
  BatteryModelState st;
  st.baselineRuntimeSec = 9450; st.healthPct = 88; st.segments = 3;
  BatteryModel m(1);
  m.load(st);
  BatteryModelState out = m.save();
  TEST_ASSERT_EQUAL_UINT32(9450, out.baselineRuntimeSec);
  TEST_ASSERT_EQUAL_UINT8(88, m.estimate().healthPct);   // seeded health honored
}

// liIonCellMvForPct is the monotone inverse of liIonPercent (used by STORAGE targeting).
static void test_liion_curve_inverse() {
  TEST_ASSERT_TRUE(liIonCellMvForPct(100) > liIonCellMvForPct(50));
  TEST_ASSERT_TRUE(liIonCellMvForPct(50) > liIonCellMvForPct(0));
  for (uint8_t p = 10; p <= 90; p += 20)
    TEST_ASSERT_INT_WITHIN(3, p, liIonPercent(liIonCellMvForPct(p)));  // round-trips
  TEST_ASSERT_INT_WITHIN(60, 3800, liIonCellMvForPct(55));             // ~storage target
}


// ── the drain harness must not teach the model (live-caught on Board 2) ─────────
// A 5.75 h / 609 mA curve run taught the rate EWMA ~17 %/hr and defined the "as-new"
// baselines from a synthetic load, so a FULL pack then projected ~4-6 h - and all of
// it persists to NVS. update()'s artificialLoad flag parks the analytics.
static nimbus::power::Sample drainSample(uint16_t packMv, uint8_t pct) {
  nimbus::power::Sample s;
  s.valid = true;
  s.millivolts = packMv;
  s.percent = pct;
  s.onExternalPower = false;
  s.charging = false;
  return s;
}

void test_artificial_load_does_not_teach_the_rate(void) {
  nimbus::power::BatteryModel m(2);
  uint32_t t = 0;
  for (int pct = 100; pct >= 0; pct -= 2) {
    m.update(t, drainSample(uint16_t(8000 - (100 - pct) * 24), uint8_t(pct)),
             /*artificialLoad=*/true);
    t += 10u * 60u * 1000u;
  }
  const auto st = m.save();
  TEST_ASSERT_EQUAL_FLOAT(0.0f, st.learnedRatePctPerHr);   // learned NOTHING
  TEST_ASSERT_EQUAL_UINT32(0, st.baselineBandMs);          // no fake "as-new" band
  TEST_ASSERT_EQUAL_UINT32(0, st.baselineRuntimeSec);      // no fake "as-new" runtime
  TEST_ASSERT_EQUAL_UINT16(0, st.segments);
}

void test_normal_discharge_still_learns(void) {
  nimbus::power::BatteryModel m(2);   // the guard must not break what it protects
  uint32_t t = 0;
  for (int pct = 100; pct >= 0; pct -= 2) {
    m.update(t, drainSample(uint16_t(8000 - (100 - pct) * 24), uint8_t(pct)), false);
    t += 10u * 60u * 1000u;
  }
  TEST_ASSERT_TRUE(m.save().baselineRuntimeSec > 0);
}

void test_reset_learned_keeps_the_battcal_anchor(void) {
  nimbus::power::BatteryModel m(2);
  uint32_t t = 0;
  for (int pct = 100; pct >= 0; pct -= 2) {
    m.update(t, drainSample(uint16_t(8000 - (100 - pct) * 24), uint8_t(pct)), false);
    t += 10u * 60u * 1000u;
  }
  m.update(t, drainSample(7942, 100), false);
  m.calibrateFullNow();                                    // human asserts a full pack
  const uint16_t anchor = m.fullAnchorMv();
  TEST_ASSERT_TRUE(m.save().baselineRuntimeSec > 0);
  m.resetLearned();
  const auto st = m.save();
  TEST_ASSERT_EQUAL_UINT32(0, st.baselineRuntimeSec);      // learning gone
  TEST_ASSERT_EQUAL_FLOAT(0.0f, st.learnedRatePctPerHr);
  TEST_ASSERT_EQUAL_UINT16(anchor, m.fullAnchorMv());      // the ASSERTION survives
}

// ⚠ THE TWO-SCALES BUG (live-caught on Board 1, 2026-07-16): estimate() REPORTS
// percentFor() (calibrated) but computed minutesToEmpty from the RAW liIonPercent
// scale. On a calibrated board a FULL pack showed "100 %" while the time was derived
// from the raw 73 % -> "100 %, 5.7 h left" - two percent scales in one sentence, and
// the owner's "4h for a full battery even on desktop". The numerator and the reported
// percent must be the same number.
void test_time_to_empty_uses_the_percent_it_reports(void) {
  BatteryModel m(2);
  // Calibrate like a real board: the S3 ADC under-reads a full 2S pack, so the owner
  // BATTCALs at full and the anchor stretches the compressed top band.
  uint32_t t = 0;
  // dis() takes the PACK mV; a 2S model divides by cells. Board 2's real full-pack
  // reading was 7942 raw => 3971 mV/cell (the ADC under-read of a true 8.40 V pack).
  m.update(t, dis(7942, 0));
  TEST_ASSERT_TRUE(m.calibrateFullNow());
  TEST_ASSERT_TRUE(m.fullAnchorMv() > BatteryModel::kCalKneeMv);
  // now discharge steadily (60 mV/pack per hour = 30 mV/cell)
  for (int i = 1; i <= 4; i++) {
    t += 1 * HR;
    m.update(t, dis(uint16_t(7942 - i * 60), 0));
  }
  BatteryEstimate e = m.estimate();
  TEST_ASSERT_TRUE(e.valid);
  TEST_ASSERT_TRUE(e.calibrated);
  TEST_ASSERT_TRUE(e.ratePctPerHr > 0.05f);
  // THE INVARIANT: minutes == (the percent SHOWN / the rate SHOWN) * 60.
  const int32_t implied = int32_t((float(e.percent) / e.ratePctPerHr) * 60.0f + 0.5f);
  TEST_ASSERT_INT32_WITHIN(2, implied, e.minutesToEmpty);
}

// The device used to say "100 %" and "7.9 V" about the same full 8.4 V pack: BATTCAL
// stretched the percent but never the volts. correctedCellMv() is now the ONE transform
// behind both, so they are the same statement.
void test_corrected_voltage_and_percent_agree(void) {
  BatteryModel m(2);
  m.update(0, dis(7913, 0));               // Board 1's real full-pack raw reading
  TEST_ASSERT_TRUE(m.calibrateFullNow());  // owner: "it IS full"
  BatteryEstimate e = m.estimate();
  TEST_ASSERT_EQUAL_UINT8(100, e.percent);
  TEST_ASSERT_EQUAL_UINT16(BatteryModel::kCalFullMv * 2, e.millivoltsTrue);  // 8400
  // (percentFor is now literally liIonPercent(correctedCellMv) - one transform, so
  // agreement is by construction rather than something a test has to police.)
}

// Below the ADC's linear knee there is nothing to correct: the reading is faithful and
// must pass through untouched (a stretch there would INVENT error).
void test_corrected_voltage_passthrough_below_knee(void) {
  BatteryModel m(2);
  m.update(0, dis(7913, 0));
  TEST_ASSERT_TRUE(m.calibrateFullNow());
  for (uint16_t cmv = 2900; cmv <= BatteryModel::kCalKneeMv; cmv += 50)
    TEST_ASSERT_EQUAL_UINT16(cmv, m.correctedCellMv(cmv));
}

// An UNCALIBRATED device must not invent a correction - raw in, raw out.
void test_uncalibrated_voltage_is_untouched(void) {
  BatteryModel m(2);
  m.update(0, dis(7913, 0));               // no calibrateFullNow()
  BatteryEstimate e = m.estimate();
  TEST_ASSERT_FALSE(e.calibrated);
  TEST_ASSERT_EQUAL_UINT16(7912, e.millivoltsTrue);   // 3956*2, i.e. the raw reading
}

// ── "1.47 h at 91%" (owner, live 2026-07-17) ───────────────────────────────────
// The rate was an integer-percent derivative over ONE telemetry tick: a 1% step in
// a 30 s tick reads as 120 %/hr, and near full the BATTCAL stretch turns mV wobble
// into exactly such steps. Implied draw was 2.2 A on an idle desk - physically
// impossible (full-white ring = ~1.67 A). Windowed folding + a physics ceiling.
// ── configurable capacity (owner feature 2026-07-17: reclaimed ~500 mAh packs) ──
// A 500 mAh pack must estimate ~7x shorter than a 3500 mAh one at the same load.
void test_capacity_scales_the_time_estimate(void) {
  auto minsAt = [](uint16_t cap) {
    BatteryModel m(2);
    m.setCapacityMah(cap);
    uint32_t t = 0;
    m.update(t, dis(7660, 0));                    // establish Discharging, no folded rate
    for (int i = 1; i <= 3; i++) { t += 60000; m.update(t, dis(uint16_t(7660 - i*12), 0)); }
    return m.estimate().minutesToEmpty;
  };
  const int32_t big = minsAt(3500), small = minsAt(500);
  TEST_ASSERT_TRUE(big > 0 && small > 0);
  // 3500/500 = 7x - allow slack for the shared per-cent starting point
  TEST_ASSERT_TRUE(big > small * 5);
  TEST_ASSERT_TRUE(big < small * 9);
}

void test_capacity_setter_ignores_zero(void) {
  BatteryModel m(2);
  m.setCapacityMah(500);
  m.setCapacityMah(0);                            // 0 = keep the last good value
  TEST_ASSERT_EQUAL_UINT16(500, m.capacityMah());
}

void test_bench_wobble_cannot_fabricate_a_huge_rate(void) {
  BatteryModel m(2);
  uint32_t t = 0;
  // seed at ~full, then 30 s ticks where the ADC wobbles one calibrated % down
  // every few ticks - the bench reality that produced "1.47 h at 91%"
  m.update(t, dis(7900, 0));
  for (int i = 1; i <= 40; i++) {
    t += 30000;
    // ADC dither: dip 10 mV, recover, dip, recover - NET ~zero (a desk, not a
    // drain). The old code folded each dip's step into the EWMA and ignored the
    // recoveries (pct <= lastPct only), fabricating a steady huge rate.
    const uint16_t mv = uint16_t(7900 - ((i % 4 == 1) ? 10 : 0));
    m.update(t, dis(mv, 0));
  }
  BatteryEstimate e = m.estimate();
  if (e.valid && e.ratePctPerHr > 0.05f) {
    TEST_ASSERT_TRUE(e.ratePctPerHr <= BatteryModel::kRateMaxPctHr);
    // and the projection must be HOURS at ~90+%, never ~1.5 h
    TEST_ASSERT_TRUE(e.minutesToEmpty < 0 || e.minutesToEmpty > 300);
  }
}

void test_fresh_boot_projects_from_the_measured_load(void) {
  // No observed rate yet: 91% must read ~15.9 h (91% x 3500 mAh / 200 mA), not
  // "estimating..." and never 1.47 h.
  BatteryModel m(2);
  uint32_t t = 0;
  // establish Discharging with two gentle real drops (calibrated ~91%)
  m.update(t, dis(7660, 0));
  for (int i = 1; i <= 3; i++) { t += 60000; m.update(t, dis(uint16_t(7660 - i * 12), 0)); }
  BatteryEstimate e = m.estimate();
  TEST_ASSERT_TRUE(e.valid);
  TEST_ASSERT_TRUE(e.minutesToEmpty > 0);
  // remaining% x 3500/200 x 60: anything from ~10 h up is sane here; the exact
  // percent depends on the curve, the FLOOR is what the bug violated
  TEST_ASSERT_TRUE(e.minutesToEmpty > 600);
  TEST_ASSERT_TRUE(e.minutesToEmpty < 40 * 60);
}

void test_real_drain_rate_still_learns_through_the_window(void) {
  // A genuine ~17 %/hr drain (the measured 1%-ring run scale) must still be
  // learned - the window folds on accumulated drop, not only on elapsed time.
  BatteryModel m(2);
  uint32_t t = 0;
  m.update(t, dis(7660, 0));
  // ~1%/3.5min decline for 45 min (calibrated-scale steps via real mV decline)
  for (int i = 1; i <= 13; i++) { t += 210000; m.update(t, dis(uint16_t(7660 - i * 14), 0)); }
  BatteryEstimate e = m.estimate();
  TEST_ASSERT_TRUE(e.valid);
  TEST_ASSERT_TRUE(e.ratePctPerHr > 3.0f);           // it IS learning
  TEST_ASSERT_TRUE(e.ratePctPerHr <= BatteryModel::kRateMaxPctHr);
}

// --- CUM-63: chemistry + custom curve --------------------------------------

// The default chemistry reproduces the shipped Li-ion curve exactly (regression pin).
static void test_default_chemistry_is_liion() {
  BatteryModel m(1);
  TEST_ASSERT_EQUAL_INT((int)Chemistry::LiIonLipo, (int)m.chemistry());
  // socForCurve over the Li-ion table == liIonPercent at a few points.
  TEST_ASSERT_EQUAL_UINT8(liIonPercent(3900), socForCurve(3900, kLiIonCurve, kLiIonCurveN));
  TEST_ASSERT_EQUAL_UINT8(100, liIonPercent(4200));
  TEST_ASSERT_EQUAL_UINT8(0, liIonPercent(3200));
}

// The LiFePO4 curve is monotonic and hits its endpoints; a mid-plateau voltage
// reads very differently than it would on the Li-ion curve (proves selection works).
static void test_lifepo4_curve_endpoints_and_monotonic() {
  TEST_ASSERT_EQUAL_UINT8(100, socForCurve(3400, kLiFePO4Curve, kLiFePO4CurveN));
  TEST_ASSERT_EQUAL_UINT8(0, socForCurve(2500, kLiFePO4Curve, kLiFePO4CurveN));
  uint8_t prev = 100;
  for (uint16_t mv = 3400; mv >= 2500; mv -= 10) {
    uint8_t p = socForCurve(mv, kLiFePO4Curve, kLiFePO4CurveN);
    TEST_ASSERT_TRUE(p <= prev);   // non-increasing as voltage falls
    prev = p;
  }
  // 3.30 V/cell: near-empty on Li-ion (~2%), mid-pack on LiFePO4 (~60%).
  TEST_ASSERT_TRUE(socForCurve(3300, kLiFePO4Curve, kLiFePO4CurveN) > 40);
  TEST_ASSERT_TRUE(liIonPercent(3300) < 10);
}

// A model set to LiFePO4 reports the LiFePO4 SoC for a resting sample.
static void test_model_lifepo4_reports_curve_soc() {
  BatteryModel m(1);
  m.setChemistry(Chemistry::LiFePO4);
  TEST_ASSERT_EQUAL_INT((int)Chemistry::LiFePO4, (int)m.chemistry());
  Sample s = dis(3300, 0);   // 3.30 V/cell resting
  m.update(0, s);
  uint8_t p = m.estimate().percent;
  TEST_ASSERT_TRUE(p > 40 && p <= 70);   // LiFePO4 mid-pack, not ~2% (Li-ion)
}

// parseCurveCsv: valid, and every malformed / non-monotonic form rejected.
static void test_parse_curve_csv() {
  LiIonCurvePoint out[kMaxCurvePoints];
  int n = parseCurveCsv("4200:100,3700:50,3200:0", out, kMaxCurvePoints);
  TEST_ASSERT_EQUAL_INT(3, n);
  TEST_ASSERT_EQUAL_UINT16(4200, out[0].mv);
  TEST_ASSERT_EQUAL_UINT8(100, out[0].pct);
  TEST_ASSERT_EQUAL_UINT8(0, out[2].pct);
  // rejects: <2 points, pct>100, non-descending mv, increasing pct, junk, empty
  TEST_ASSERT_EQUAL_INT(0, parseCurveCsv("4200:100", out, kMaxCurvePoints));
  TEST_ASSERT_EQUAL_INT(0, parseCurveCsv("4200:150,3200:0", out, kMaxCurvePoints));
  TEST_ASSERT_EQUAL_INT(0, parseCurveCsv("3200:0,4200:100", out, kMaxCurvePoints));   // ascending mv
  TEST_ASSERT_EQUAL_INT(0, parseCurveCsv("4200:50,3700:80", out, kMaxCurvePoints));   // rising pct
  TEST_ASSERT_EQUAL_INT(0, parseCurveCsv("4200:100;3200:0", out, kMaxCurvePoints));   // junk sep
  TEST_ASSERT_EQUAL_INT(0, parseCurveCsv("", out, kMaxCurvePoints));
  TEST_ASSERT_EQUAL_INT(0, parseCurveCsv("abc", out, kMaxCurvePoints));
}

// A custom curve overrides the chemistry curve; invalid shapes are rejected.
static void test_custom_curve_overrides() {
  BatteryModel m(1);
  LiIonCurvePoint pts[3] = {{4000, 100}, {3600, 50}, {3200, 0}};
  TEST_ASSERT_TRUE(m.setCustomCurve(pts, 3));
  TEST_ASSERT_EQUAL_INT(3, m.customCurveN());
  Sample s = dis(3600, 0);
  m.update(0, s);
  TEST_ASSERT_EQUAL_UINT8(50, m.estimate().percent);   // custom curve says 50% at 3.60 V
  m.clearCustomCurve();
  TEST_ASSERT_EQUAL_INT(0, m.customCurveN());
  // rejected shapes leave the previous state untouched (return false)
  LiIonCurvePoint bad[2] = {{3200, 0}, {4000, 100}};   // ascending
  TEST_ASSERT_FALSE(m.setCustomCurve(bad, 2));
  TEST_ASSERT_FALSE(m.setCustomCurve(pts, 1));          // too few
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_time_to_empty_from_rate);
  RUN_TEST(test_external_power_no_estimate);
  RUN_TEST(test_cells_divide_pack_voltage);
  RUN_TEST(test_health_degrades_on_faster_band);
  RUN_TEST(test_segment_learning_persists);
  RUN_TEST(test_invalid_sample);
  RUN_TEST(test_rising_voltage_is_charging);
  RUN_TEST(test_full_plateau_not_faked_as_discharge);
  RUN_TEST(test_calibrate_full_now_persists);
  RUN_TEST(test_calibration_leaves_low_range_intact);
  RUN_TEST(test_auto_plateau_anchor_is_persistable);
  RUN_TEST(test_slow_discharge_not_pinned_external);
  RUN_TEST(test_charge_state_strings);
  RUN_TEST(test_runtime_health_grounds_on_full_cycle);
  RUN_TEST(test_battmodel_state_roundtrips_runtime);
  RUN_TEST(test_liion_curve_inverse);
  RUN_TEST(test_capacity_scales_the_time_estimate);
  RUN_TEST(test_capacity_setter_ignores_zero);
  RUN_TEST(test_bench_wobble_cannot_fabricate_a_huge_rate);
  RUN_TEST(test_fresh_boot_projects_from_the_measured_load);
  RUN_TEST(test_real_drain_rate_still_learns_through_the_window);
  RUN_TEST(test_corrected_voltage_and_percent_agree);
  RUN_TEST(test_corrected_voltage_passthrough_below_knee);
  RUN_TEST(test_uncalibrated_voltage_is_untouched);
  RUN_TEST(test_time_to_empty_uses_the_percent_it_reports);
  RUN_TEST(test_artificial_load_does_not_teach_the_rate);
  RUN_TEST(test_normal_discharge_still_learns);
  RUN_TEST(test_reset_learned_keeps_the_battcal_anchor);
  RUN_TEST(test_default_chemistry_is_liion);
  RUN_TEST(test_lifepo4_curve_endpoints_and_monotonic);
  RUN_TEST(test_model_lifepo4_reports_curve_soc);
  RUN_TEST(test_parse_curve_csv);
  RUN_TEST(test_custom_curve_overrides);
  return UNITY_END();
}
