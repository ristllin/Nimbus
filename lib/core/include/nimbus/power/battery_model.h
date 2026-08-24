#pragma once
#include <cstdint>

#include "nimbus/power/power_monitor.h"

// battery_model - portable battery ANALYTICS on top of the raw power::Sample
// stream (voltage/percent only; no coulomb counter on this board). Four jobs:
//
//   1. CHARGE STATE - the board has no VBUS sense pin, so the raw Sample can't
//      tell "on the charger" from "on battery". This model owns the time series,
//      so it infers the state from the VOLTAGE TREND (reference-point hysteresis,
//      immune to ADC dither): a pack under the device's load always DECLINES;
//      a rising trend is charging; a sustained flat plateau at the top is full.
//      Only the Discharging state produces a time-to-empty - that killed the bug
//      where ADC dither on a full-on-charger pack faked a discharge estimate.
//
//   2. TIME-TO-EMPTY - a discharge-rate estimate that blends a FAST EWMA (recent
//      %/hr, responsive) with a HISTORY-LEARNED rate (the EWMA of each completed
//      discharge segment's average %/hr). Early on it trusts the fast term; as
//      completed segments accumulate it leans on the learned rate, so the
//      estimate gets steadier and more accurate the longer the device runs -
//      the "simple self-improving formula" (persist load()/save() across reboots
//      and the learning survives; the device mirrors it to NVS/SD).
//
//   3. HEALTH / DEGRADATION - a worn 18650 traverses the usable per-cell voltage
//      band faster under the same desk load. We time the band from kBandHiMv to
//      kBandLoMv on each discharge; the FIRST complete traversal sets the
//      baseline, later ones give health% = observedSpan / baseline (a LiitoKala
//      INR18650-35E ~3500 mAh cell frames "as-new"; the metric is relative, so
//      it needs no absolute mAh).
//
//   4. TOP-END CALIBRATION - the ESP32-S3 ADC (11 dB) compresses above ~2.45 V,
//      so a full 2S pack (2.625 V at the ÷3.2 divider node) READS LOW (~7.9 V for
//      a true 8.4 V → ~72 % instead of 100 %). When the model confirms a full
//      plateau it LEARNS that per-cell node voltage as the 100 % anchor (or the
//      owner asserts it via calibrateFullNow()); percentFor() then stretches the
//      compressed top band so full reads 100 % and discharge-from-full is smooth.
//      Below the knee the ADC is accurate, so the low/mid range is untouched.
//
// Pure + host-tested: no clock, no I/O - the caller passes nowMs and the sample.

namespace nimbus::power {

enum class ChargeState : uint8_t {
  Unknown = 0,   // too few samples to tell
  Discharging,   // confirmed voltage decline - the only state with a time estimate
  Charging,      // confirmed voltage rise
  Full,          // plateau at the top of the observed range (on the charger, topped off)
  External,      // flat for too long to be a live discharge (plugged in, not near full)
};
const char* chargeStateStr(ChargeState s);
// Trend-honest form for human/agent-facing surfaces: "v-rising"/"v-falling"/
// "v-stable"/"v-high-stable" - states only what the ADC measured, never a
// charging/plugged-in claim (no charge-detect hardware exists to back one).
const char* trendStr(ChargeState s);

struct BatteryEstimate {
  bool        valid = false;          // a discharge time-to-empty is available
  int32_t     minutesToEmpty = -1;    // -1 = unknown (charging / external / no trend yet)
  float       ratePctPerHr = 0.0f;    // discharge rate used for the estimate (>= 0)
  uint8_t     healthPct = 100;        // 100 = as-new; < 100 = capacity fade observed
  uint16_t    segments = 0;           // completed discharge segments learned from
  uint8_t     percent = 0;            // calibrated SoC (top-end corrected); 0 = no sample yet
  // Pack mV with the SAME top-band correction as `percent` - so the two agree about
  // the same pack (a full 2S reads 8400 + 100 %, not 7913 + 100 %). 0 = no sample.
  // The RAW Sample.millivolts stays the wire/lab value; this is the display value.
  uint16_t    millivoltsTrue = 0;
  ChargeState chargeState = ChargeState::Unknown;
  bool        onExternalPower = false;// inferred: charging | full | external
  bool        calibrated = false;     // a full-charge anchor has been learned/set
};

// Persisted learning - the device saves this to NVS (small) and/or SD, and
// restores it at boot so accuracy carries across reboots + improves over life.
struct BatteryModelState {
  float    learnedRatePctPerHr = 0.0f;  // history EWMA of per-segment average rate
  uint16_t segments = 0;                // completed discharge segments seen
  uint32_t baselineBandMs = 0;          // as-new band-traversal time (0 = not set)
  uint8_t  healthPct = 100;
  uint16_t fullAnchorCellMv = 0;        // learned per-cell mV that reads as 100 % (0 = uncalibrated)
  // As-new full-cycle runtime (seconds) for RUNTIME-grounded health. 0 = not set yet;
  // the drain campaign (the Battery Lab tooling) seeds this from the measured median so
  // health % (and capacityMah = health × 3500) is real-capacity-referenced from day one.
  uint32_t baselineRuntimeSec = 0;
};

class BatteryModel {
 public:
  // Per-cell reference voltages for the health band + the "empty enough" mark
  // that closes a discharge segment. Defaults suit a 3.0-4.2 V Li-ion cell.
  static constexpr uint16_t kBandHiMv = 3900;   // segment/band start (below this, discharging)
  static constexpr uint16_t kBandLoMv = 3550;   // band end + segment close
  static constexpr float    kFastAlpha = 0.30f; // fast-EWMA weight per sample
  static constexpr float    kLearnAlpha = 0.25f;// history-EWMA weight per segment

  // Charge-state trend + top-end calibration.
  static constexpr uint16_t kTrendHystMv = 8;     // confirmed rise/drop threshold (> ADC dither)
  static constexpr uint16_t kFullNearMv  = 40;    // "near the observed max" for Full
  static constexpr uint32_t kFlatHoldMs  = 10u * 60u * 1000u;  // flat this long => not discharging
  static constexpr uint16_t kCalKneeMv   = 3800;  // ADC accurate at/below this per-cell mV
  static constexpr uint16_t kCalFullMv   = 4200;  // true per-cell mV at 100 %

  // ---- rate-sanity + fallback constants (MEASURED, 2026-07-17 study) ----------
  // I(b) = 150 + 5.95·b mA at the pack (two full discharge runs, owner-measured
  // 3500 mAh capacity). Typical use ≈ the ring lit ~1% duty -> ~200 mA.
  static constexpr uint16_t kCapacityMah    = 3500;  // owner-measured (analyzer)
  static constexpr uint16_t kTypicalLoadMa  = 200;   // measured I(10) = 209
  // Physics ceiling on any believable discharge rate: full-white ring = 150 +
  // 5.95*255 ≈ 1667 mA -> ~48 %/hr. Anything above is measurement noise, never
  // a real load (live-caught: "1.47 h at 91%" = an implied 2.2 A on an idle desk).
  static constexpr float    kRateMaxPctHr   = 48.0f;
  // Rate-window folding: integer percent over one 30-300 s tick is granularity
  // noise (one 1% step in 30 s reads as 120 %/hr). Fold the EWMA only when the
  // window holds enough SIGNAL: >= kRateMinWindowMs elapsed, or >= kRateMinDropPct
  // accumulated (fast real drains fold sooner; bench wobble folds slow + small).
  static constexpr uint32_t kRateMinWindowMs = 10u * 60u * 1000u;
  static constexpr uint8_t  kRateMinDropPct  = 3;

  // cells = series count (pack mV / cells = per-cell mV). Matches Board.batt.cells.
  explicit BatteryModel(uint8_t cells = 1) : cells_(cells ? cells : 1) {}

  // Feed one sample. nowMs is a monotonic ms clock (millis() on device).
  // artificialLoad: the TEST-only drain/storage harness is driving a synthetic load
  // (LED bank at up to ~1.5 A). The analytics MUST NOT learn from it - the rate EWMA,
  // the band baseline and the full-cycle runtime baseline all describe how THIS pack
  // behaves in NORMAL use, and a drain campaign at ~10x the idle draw would teach the
  // model that "discharging" means ~17 %/hr, so a freshly-charged device would predict
  // hours instead of days FOREVER (the baselines persist to NVS). Live-caught: after a
  // 5.75 h / 609 mA curve run, Board 2 projected ~4-6 h from full. Passing true here
  // parks the analytics exactly like a non-discharging sample: live trend dropped,
  // learned state preserved untouched.
  void update(uint32_t nowMs, const Sample& s, bool artificialLoad = false);

  // Discard everything LEARNED from observation, keeping the full-charge anchor (that
  // comes from BATTCAL - a human asserting a full pack - not from the analytics). For
  // recovering a model already poisoned by a drain campaign; also lets the harness
  // clean up after itself.
  void resetLearned();

  BatteryEstimate estimate() const;

  // Owner asserts "the pack is full right now": anchor 100 % to the latest
  // reading (used by the BATTCAL console cmd / web button for instant, verifiable
  // calibration). No-op until at least one plausible sample has been seen.
  bool calibrateFullNow();

  ChargeState chargeState() const { return state_; }
  uint16_t    fullAnchorMv() const { return fullAnchorMv_; }

  // The per-cell mV a linear ADC WOULD have reported - i.e. the reading with the
  // calibrated top-band stretch applied. percentFor() is exactly liIonPercent() of
  // this, so the voltage the device shows and the percent it shows are the same
  // statement (a full pack reads 8.40 V AND 100 %, not 7.9 V AND 100 %).
  // ⚠ NOT a substitute for Sample.millivolts, which stays RAW on purpose: the
  // Battery Lab stores raw and applies its own (finer, per-device, multimeter-
  // referenced) correction - pre-correcting the wire would double-correct it.
  uint16_t    correctedCellMv(uint16_t cmv) const;

  BatteryModelState save() const;
  void load(const BatteryModelState& st);

  void setCells(uint8_t cells) { cells_ = cells ? cells : 1; }
  // Pack capacity (mAh) - physical, owner-configurable (a different pack). Drives
  // the measured-load time-to-empty fallback + the capacity=health×capacity readout.
  // Default kCapacityMah until set; 0 is ignored (keeps the last good value).
  void setCapacityMah(uint16_t mah) { if (mah) capacityMah_ = mah; }
  uint16_t capacityMah() const { return capacityMah_; }

  // Battery chemistry - selects the per-cell voltage->SoC curve percentFor() uses.
  // Default LiIonLipo reproduces the shipped behaviour exactly (kLiIonCurve + the
  // top-band ADC stretch). LiFePO4 uses kLiFePO4Curve and, since its band sits below
  // the Li-ion knee, the top-band stretch is naturally inert. The discharge-rate /
  // health analytics stay Li-ion-tuned; only the reported SoC follows the chemistry.
  void setChemistry(Chemistry c) { chem_ = c; }
  Chemistry chemistry() const { return chem_; }

  // Optional owner custom curve (high-mV first, strictly descending). When set it
  // overrides the chemistry curve in percentFor(). Rejects n outside [2,
  // kMaxCurvePoints]; returns whether it was accepted. clearCustomCurve() reverts
  // to the chemistry default.
  bool setCustomCurve(const LiIonCurvePoint* pts, int n);
  void clearCustomCurve() { customN_ = 0; }
  int  customCurveN() const { return customN_; }

 private:
  uint16_t cellMv(const Sample& s) const { return uint16_t(s.millivolts / cells_); }
  bool     nearMax(uint16_t cmv) const {
    return maxCellMv_ && cmv >= kCalKneeMv && cmv + kFullNearMv >= maxCellMv_;
  }
  void     learnFullAnchor(uint16_t cmv);
  uint8_t  percentFor(uint16_t cmv) const;
  void     classifyTrend(uint32_t nowMs, const Sample& s);

  uint8_t  cells_ = 1;
  uint16_t capacityMah_ = kCapacityMah;   // runtime-configurable pack capacity
  Chemistry chem_ = Chemistry::LiIonLipo; // SoC curve selector (default = shipped)
  LiIonCurvePoint custom_[kMaxCurvePoints]; // owner custom curve (customN_>0 => active)
  int      customN_ = 0;
  // live discharge trend
  bool     have_ = false;         // a prior valid discharging sample exists
  uint32_t lastMs_ = 0;
  uint8_t  lastPct_ = 0;
  float    fastRate_ = 0.0f;      // fast EWMA, %/hr
  bool     fastValid_ = false;
  // segment accumulation (from the last kBandHiMv crossing)
  bool     inSegment_ = false;
  uint32_t segStartMs_ = 0;
  uint8_t  segStartPct_ = 0;
  uint32_t bandStartMs_ = 0;      // when cell first dropped below kBandHiMv this segment
  bool     bandTiming_ = false;
  // charge-state trend
  ChargeState state_ = ChargeState::Unknown;
  bool     haveRef_ = false;
  uint32_t refMs_ = 0;
  uint16_t refMv_ = 0;            // per-cell mV at the trend reference point
  bool     flatTiming_ = false;
  uint32_t flatSinceMs_ = 0;
  uint16_t maxCellMv_ = 0;        // highest per-cell mV observed
  uint16_t lastCellMv_ = 0;       // latest per-cell mV (for percentFor)
  bool     haveSample_ = false;
  uint16_t fullAnchorMv_ = 0;     // learned per-cell mV that reads 100 % (0 = uncalibrated)
  // learned state
  float    learnedRate_ = 0.0f;
  uint16_t segments_ = 0;
  uint32_t baselineBandMs_ = 0;
  uint8_t  health_ = 100;
  // Runtime-grounded health: time a full-ish discharge (started above the health band)
  // down to the band-low close. First cycle sets the baseline; later cycles give
  // health = 100 × observed/baseline (shorter runtime = less capacity = lower health).
  // Preferred over band health once baselineRuntimeSec_ is set (seeded or self-learned).
  uint32_t fullCycleStartMs_    = 0;
  bool     fullCycleTiming_     = false;
  uint32_t baselineRuntimeSec_  = 0;   // 0 = not set
  uint8_t  runtimeHealth_       = 0;   // 0 = not computed yet (fall back to band health)
};

}  // namespace nimbus::power
