#include "nimbus/power/battery_model.h"

namespace nimbus::power {

// Parse "mv:pct,mv:pct,..." (high-mV first) into curve points. Returns the count
// (>=2) or 0 on any malformed / non-monotonic input. No allocation, no <string>.
int parseCurveCsv(const char* csv, LiIonCurvePoint* out, int maxN) {
  if (!csv || !out || maxN < 2) return 0;
  int count = 0;
  const char* p = csv;
  while (*p && count < maxN) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    if (*p < '0' || *p > '9') return 0;
    long mv = 0;
    while (*p >= '0' && *p <= '9') { mv = mv * 10 + (*p - '0'); if (mv > 60000) return 0; p++; }
    if (*p != ':') return 0;
    p++;
    if (*p < '0' || *p > '9') return 0;
    long pct = 0;
    while (*p >= '0' && *p <= '9') { pct = pct * 10 + (*p - '0'); if (pct > 1000) return 0; p++; }
    if (pct > 100) return 0;
    // strictly descending mV, non-increasing pct (matches socForCurve's shape).
    if (count > 0 && (mv >= out[count - 1].mv || pct > out[count - 1].pct)) return 0;
    out[count].mv = (uint16_t)mv;
    out[count].pct = (uint8_t)pct;
    count++;
    while (*p == ' ') p++;
    if (*p && *p != ',') return 0;   // junk between points
  }
  if (*p) { while (*p == ' ' || *p == ',') p++; if (*p) return 0; }  // trailing junk / overflow
  return count >= 2 ? count : 0;
}

const char* chargeStateStr(ChargeState s) {
  switch (s) {
    case ChargeState::Discharging: return "discharging";
    case ChargeState::Charging:    return "charging";
    case ChargeState::Full:        return "full";
    case ChargeState::External:    return "external";
    case ChargeState::Unknown:     break;
  }
  return "unknown";
}

// Trend-honest vocabulary for HUMAN/AGENT-facing surfaces (owner 2026-07-16: the
// agents were asserting "charging" / "on external power" - but there is NO
// charge-detect hardware; the ChargeState is INFERRED from the voltage trend and
// can be wrong the moment a load changes). chargeStateStr stays for machine wire
// compat (/api/state feeds the battery-lab tooling); everything a person or the
// model READS uses this instead, which only claims what the ADC actually saw.
const char* trendStr(ChargeState s) {
  switch (s) {
    case ChargeState::Discharging: return "v-falling";
    case ChargeState::Charging:    return "v-rising";
    case ChargeState::Full:        return "v-high-stable";
    case ChargeState::External:    return "v-stable";
    case ChargeState::Unknown:     break;
  }
  return "unknown";
}

void BatteryModel::learnFullAnchor(uint16_t cmv) {
  // Only anchor to a plausibly-full per-cell reading; a degraded/half pack sitting
  // flat is External, not Full (guarded by nearMax()'s kCalKneeMv floor upstream).
  // Reject == knee too: percentFor()/estimate().calibrated treat fullAnchorMv_ <=
  // kCalKneeMv as uncalibrated (the stretch band would be zero-width), so accepting
  // exactly-knee would report success yet apply no correction (prism 2026-07-12).
  if (cmv <= kCalKneeMv) return;
  if (fullAnchorMv_ == 0) fullAnchorMv_ = cmv;
  else fullAnchorMv_ = uint16_t((uint32_t(fullAnchorMv_) * 3 + cmv) / 4);   // gentle EWMA
  if (fullAnchorMv_ > kCalFullMv) fullAnchorMv_ = kCalFullMv;               // a truly-linear board reads 4200
}

uint16_t BatteryModel::correctedCellMv(uint16_t cmv) const {
  // The ONE top-band correction, shared by percentFor() and the reported voltage so
  // they can never disagree (the whole point: the device used to say "100 %" and
  // "7.9 V" about the same full 8.4 V pack - the percent was stretched, the volts
  // weren't). Uncalibrated, or below the ADC's linear knee: the reading is faithful.
  if (fullAnchorMv_ <= kCalKneeMv || cmv <= kCalKneeMv) return cmv;
  // Stretch the compressed top band (knee..anchor) back onto (knee..trueFull).
  if (cmv >= fullAnchorMv_) return kCalFullMv;
  return uint16_t(kCalKneeMv + uint32_t(cmv - kCalKneeMv) * (kCalFullMv - kCalKneeMv) /
                                   (fullAnchorMv_ - kCalKneeMv));
}

uint8_t BatteryModel::percentFor(uint16_t cmv) const {
  // A custom curve (if configured) wins; otherwise the chemistry curve. Li-ion is
  // the default and keeps the top-band ADC stretch (correctedCellMv) so a full 2S
  // reads 100%; the LiFePO4 band sits below the knee, so the stretch is inert there
  // and the curve is read directly. This is the only chemistry-aware SoC point.
  if (customN_ > 0) return socForCurve(correctedCellMv(cmv), custom_, customN_);
  if (chem_ == Chemistry::LiFePO4) return socForCurve(cmv, kLiFePO4Curve, kLiFePO4CurveN);
  return liIonPercent(correctedCellMv(cmv));
}

bool BatteryModel::setCustomCurve(const LiIonCurvePoint* pts, int n) {
  if (!pts || n < 2 || n > kMaxCurvePoints) return false;
  // Must be high-mV first, strictly descending in mV, pct within 0..100 and non-
  // increasing - the same shape socForCurve interpolates over.
  for (int i = 0; i < n; i++) {
    if (pts[i].pct > 100) return false;
    if (i > 0 && (pts[i].mv >= pts[i - 1].mv || pts[i].pct > pts[i - 1].pct)) return false;
  }
  for (int i = 0; i < n; i++) custom_[i] = pts[i];
  customN_ = n;
  return true;
}

// Infer charge state from the voltage trend (reference-point hysteresis). Updates
// state_, the running max, the latest cell mV, and - on a confirmed full plateau -
// the calibration anchor. Never touches the discharge-analytics fields.
void BatteryModel::classifyTrend(uint32_t nowMs, const Sample& s) {
  if (!s.valid) { state_ = ChargeState::Unknown; haveRef_ = false; flatTiming_ = false; return; }

  const uint16_t cmv = cellMv(s);
  lastCellMv_ = cmv;
  haveSample_ = true;
  if (cmv > maxCellMv_) maxCellMv_ = cmv;

  // A wired VBUS pin (if it ever lands) is authoritative.
  if (s.onExternalPower) {
    haveRef_ = false;
    flatTiming_ = false;
    if (nearMax(cmv)) { state_ = ChargeState::Full; learnFullAnchor(cmv); }
    else state_ = s.charging ? ChargeState::Charging : ChargeState::External;
    return;
  }

  if (!haveRef_) {   // first sample: seed the reference, wait for a trend
    haveRef_ = true;
    refMv_ = cmv;
    refMs_ = nowMs;
    flatTiming_ = true;
    flatSinceMs_ = nowMs;
    return;
  }

  const int32_t dMv = int32_t(cmv) - int32_t(refMv_);
  if (dMv <= -int32_t(kTrendHystMv)) {          // confirmed decline
    state_ = ChargeState::Discharging;
    refMv_ = cmv; refMs_ = nowMs;
    flatTiming_ = false;
  } else if (dMv >= int32_t(kTrendHystMv)) {    // confirmed rise
    state_ = ChargeState::Charging;
    refMv_ = cmv; refMs_ = nowMs;
    flatTiming_ = false;
  } else {                                      // within hysteresis: flat
    if (!flatTiming_) { flatTiming_ = true; flatSinceMs_ = nowMs; }
    if (nowMs - flatSinceMs_ >= kFlatHoldMs) {
      // Flat for too long to be a live discharge (a loaded pack always sags):
      // it's on external power - full if pinned near the top, else just external.
      if (nearMax(cmv)) { state_ = ChargeState::Full; learnFullAnchor(cmv); }
      else state_ = ChargeState::External;
      // Restart the flat window but KEEP refMv_ as the cumulative trend anchor.
      // Re-anchoring refMv_=cmv here would reset the ±hysteresis measure every
      // sample, so a pack draining slower than kTrendHystMv per sample (light load,
      // long TelemetryPeriodS) would accumulate no drop and stay pinned in External
      // forever, never re-detecting Discharging (prism 2026-07-12). Leaving refMv_
      // put lets a slow cumulative decline eventually cross -kTrendHystMv -> Discharging.
      flatTiming_ = false;
    }
  }
}

void BatteryModel::resetLearned() {
  learnedRate_ = 0.0f;
  segments_ = 0;
  baselineBandMs_ = 0;
  baselineRuntimeSec_ = 0;
  health_ = 100;
  runtimeHealth_ = 100;
  fastRate_ = 0.0f;
  fastValid_ = false;
  have_ = false;
  inSegment_ = false;
  bandTiming_ = false;
  fullCycleTiming_ = false;
  // fullAnchorMv_ deliberately KEPT: it is a human's BATTCAL assertion, not learning.
}

void BatteryModel::update(uint32_t nowMs, const Sample& s, bool artificialLoad) {
  classifyTrend(nowMs, s);

  // The discharge analytics only run while we're actually on battery and draining
  // UNDER A REPRESENTATIVE LOAD. A hard-external sample, an inferred charge/full/
  // external state, no telemetry, or a synthetic drain-harness load all drop the live
  // trend (so a fresh unplug starts clean) but KEEP learned state.
  const bool discharging = s.valid && !s.onExternalPower && !s.charging &&
                           !artificialLoad &&
                           (state_ == ChargeState::Discharging || state_ == ChargeState::Unknown);
  if (!discharging) {
    have_ = false;
    fastValid_ = false;
    inSegment_ = false;
    bandTiming_ = false;
    return;
  }

  const uint16_t cmv = cellMv(s);
  // ⚠ USE THE CALIBRATED SCALE. s.percent is the RAW curve lookup (liIonPercent, no
  // top-band stretch); estimate() REPORTS percentFor() (stretched). Feeding the
  // analytics the raw scale while reporting the calibrated one put two different
  // percent scales in one sentence: a full pack shows "100 %" but minsToEmpty was
  // computed from the raw 73 % (live: 340 min = 73/12.87*60 while the UI said 100 %),
  // and the raw scale's steep top band inflated the rate EWMA on top. Both the
  // numerator and the rate must live on the scale the user is shown.
  const uint8_t pct = percentFor(cmv);

  // First discharging sample after (re)start: seed, nothing to differentiate yet.
  if (!have_) {
    have_ = true;
    lastMs_ = nowMs;
    lastPct_ = pct;
    // open a discharge segment reference
    inSegment_ = true;
    segStartMs_ = nowMs;
    segStartPct_ = pct;
    bandTiming_ = false;
    // if we start already inside the band, begin timing from now
    if (cmv < kBandHiMv && cmv > kBandLoMv) { bandTiming_ = true; bandStartMs_ = nowMs; }
    // Runtime-grounded health: only time a FULL-ish cycle (discharge starting above the
    // health band, i.e. near full) so a mid-pack unplug doesn't fake a short "capacity".
    if (cmv >= kBandHiMv) { fullCycleTiming_ = true; fullCycleStartMs_ = nowMs; }
    return;
  }

  // ---- fast discharge-rate EWMA (%/hr), WINDOWED ----------------------------
  // The old form differentiated integer percent over a single telemetry tick:
  // one 1% step in a 30 s tick reads as 120 %/hr, and near full the BATTCAL
  // stretch turns mV wobble into exactly such steps - the live "1.47 h at 91%"
  // (implied 2.2 A on an idle desk). The window folds only when it holds real
  // signal: enough TIME that 1% granularity is small, or enough DROP that the
  // rate is genuine. A rise (charge blip) discards the window.
  const uint32_t dtMs = nowMs - lastMs_;
  if (pct > lastPct_) {
    lastMs_ = nowMs;               // rising: restart the window, learn nothing
    lastPct_ = pct;
  } else if (dtMs >= kRateMinWindowMs ||
             (uint8_t(lastPct_ - pct) >= kRateMinDropPct && dtMs >= 1000)) {
    const float dPct = float(lastPct_ - pct);
    const float hrs = float(dtMs) / 3600000.0f;
    float inst = hrs > 0.0f ? dPct / hrs : 0.0f;         // %/hr, >= 0
    if (inst > kRateMaxPctHr) inst = kRateMaxPctHr;      // physics ceiling
    if (!fastValid_) { fastRate_ = inst; fastValid_ = true; }
    else fastRate_ = fastRate_ + kFastAlpha * (inst - fastRate_);
    lastMs_ = nowMs;
    lastPct_ = pct;
  }

  // ---- health band timing (kBandHiMv -> kBandLoMv) --------------------------
  if (!bandTiming_ && cmv < kBandHiMv && cmv > kBandLoMv) {
    bandTiming_ = true;
    bandStartMs_ = nowMs;
  } else if (bandTiming_ && cmv <= kBandLoMv) {
    const uint32_t span = nowMs - bandStartMs_;
    bandTiming_ = false;
    if (span > 0) {
      if (baselineBandMs_ == 0) {
        baselineBandMs_ = span;          // first traversal defines "as-new"
        health_ = 100;
      } else {
        // Faster traversal than baseline => less usable capacity => lower health.
        float h = 100.0f * float(span) / float(baselineBandMs_);
        if (h > 100.0f) h = 100.0f;      // a slower (colder/lighter-load) run isn't "extra" health
        if (h < 1.0f) h = 1.0f;
        health_ = uint8_t(h + 0.5f);
      }
    }
  }

  // ---- runtime-grounded health: close a full-ish cycle at the low mark ------
  if (fullCycleTiming_ && cmv <= kBandLoMv) {
    const uint32_t sec = (nowMs - fullCycleStartMs_) / 1000u;
    fullCycleTiming_ = false;
    if (sec > 0) {
      if (baselineRuntimeSec_ == 0) {
        baselineRuntimeSec_ = sec;         // first full cycle defines "as-new"
        runtimeHealth_ = 100;
      } else {
        float h = 100.0f * float(sec) / float(baselineRuntimeSec_);
        if (h > 100.0f) h = 100.0f;        // a slower (colder/lighter) run isn't "extra"
        if (h < 1.0f) h = 1.0f;
        runtimeHealth_ = uint8_t(h + 0.5f);
      }
    }
  }

  // ---- close a discharge SEGMENT at the low mark -> feed the learned rate ----
  if (inSegment_ && cmv <= kBandLoMv) {
    const uint32_t segMs = nowMs - segStartMs_;
    const float segHrs = float(segMs) / 3600000.0f;
    if (segHrs > 0.01f && pct < segStartPct_) {
      const float segRate = float(segStartPct_ - pct) / segHrs;         // %/hr avg
      if (segments_ == 0) learnedRate_ = segRate;
      else learnedRate_ = learnedRate_ + kLearnAlpha * (segRate - learnedRate_);
      if (segments_ < 0xFFFF) segments_++;
    }
    // reopen a fresh segment from here (continued discharge below the low mark)
    segStartMs_ = nowMs;
    segStartPct_ = pct;
  }
}

bool BatteryModel::calibrateFullNow() {
  if (!haveSample_ || lastCellMv_ <= kCalKneeMv) return false;  // no plausible full reading yet
                                                                // (== knee is inert; see percentFor)
  fullAnchorMv_ = lastCellMv_;
  if (fullAnchorMv_ > kCalFullMv) fullAnchorMv_ = kCalFullMv;
  if (lastCellMv_ > maxCellMv_) maxCellMv_ = lastCellMv_;
  state_ = ChargeState::Full;
  return true;
}

BatteryEstimate BatteryModel::estimate() const {
  BatteryEstimate e;
  // Prefer the RUNTIME-grounded health once a baseline exists (seeded from the drain
  // campaign or self-learned over a full cycle) - it reflects real capacity, not just a
  // mid-band traversal. Fall back to the fast band-traversal proxy until then.
  e.healthPct = (baselineRuntimeSec_ && runtimeHealth_) ? runtimeHealth_ : health_;
  e.segments = segments_;
  e.chargeState = state_;
  e.calibrated = fullAnchorMv_ > kCalKneeMv;
  e.onExternalPower =
      state_ == ChargeState::Charging || state_ == ChargeState::Full || state_ == ChargeState::External;
  if (haveSample_) {
    e.percent = (state_ == ChargeState::Full) ? 100 : percentFor(lastCellMv_);
    // Same correction, same pack: a calibrated full 2S reports 8400 mV here while
    // Sample.millivolts stays the raw ~7913 the ADC actually produced.
    e.millivoltsTrue = uint16_t(uint32_t(correctedCellMv(lastCellMv_)) * cells_);
  }

  // A time-to-empty is meaningful only while genuinely discharging. NOTE: a not-
  // yet-folded rate window (!fastValid_) is NOT a reason to show nothing - the
  // measured-load fallback below covers exactly that (the old gate here returned
  // early and the fallback was dead code - caught by its own regression test).
  if (state_ != ChargeState::Discharging || !have_) return e;

  // Blend the responsive fast rate with the history-learned rate. Trust in the
  // learned term grows with completed segments (cap at ~0.6 so recent behaviour
  // always has a say) - the self-improving part.
  float rate = fastValid_ ? fastRate_ : 0.0f;
  if (fastValid_ && segments_ > 0 && learnedRate_ > 0.0f) {
    float w = 0.15f * float(segments_);
    if (w > 0.6f) w = 0.6f;
    rate = (1.0f - w) * fastRate_ + w * learnedRate_;
  }
  e.valid = true;
  // Physics clamp on the BLENDED rate too (a poisoned learnedRate_ from old NVS
  // must not survive the fold guard): nothing this device can do exceeds ~48 %/hr.
  if (rate > kRateMaxPctHr) rate = kRateMaxPctHr;
  e.ratePctPerHr = rate;
  if (rate > 0.05f) {
    // minutes to reach 0% from the last observed percent.
    float mins = (float(lastPct_) / rate) * 60.0f;
    if (mins < 0.0f) mins = 0.0f;
    if (mins > 100000.0f) mins = 100000.0f;   // clamp absurd flat-rate projections
    e.minutesToEmpty = int32_t(mins + 0.5f);
  } else {
    // No trustworthy observed rate yet (fresh boot / windows still filling):
    // project from the MEASURED load model instead of showing nothing - typical
    // use ≈ 200 mA on the owner-measured 3500 mAh: remaining% × 3500 / 200.
    const float mins =
        (float(lastPct_) / 100.0f) * float(capacityMah_) / float(kTypicalLoadMa) * 60.0f;
    e.minutesToEmpty = int32_t(mins + 0.5f);
    e.ratePctPerHr = 100.0f * float(kTypicalLoadMa) / float(capacityMah_);  // ≈5.7
  }
  return e;
}

BatteryModelState BatteryModel::save() const {
  BatteryModelState st;
  st.learnedRatePctPerHr = learnedRate_;
  st.segments = segments_;
  st.baselineBandMs = baselineBandMs_;
  st.healthPct = health_;
  st.fullAnchorCellMv = fullAnchorMv_;
  st.baselineRuntimeSec = baselineRuntimeSec_;
  return st;
}

void BatteryModel::load(const BatteryModelState& st) {
  learnedRate_ = st.learnedRatePctPerHr;
  segments_ = st.segments;
  baselineBandMs_ = st.baselineBandMs;
  health_ = st.healthPct ? st.healthPct : 100;
  fullAnchorMv_ = st.fullAnchorCellMv;
  baselineRuntimeSec_ = st.baselineRuntimeSec;
  // Loading a seeded/persisted baseline means health should immediately reflect it; the
  // stored healthPct doubles as the last runtime health when a baseline exists.
  if (baselineRuntimeSec_) runtimeHealth_ = health_ ? health_ : 100;
}

}  // namespace nimbus::power
