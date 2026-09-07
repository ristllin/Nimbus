#pragma once
#include <cstdint>

// battery_sense - honest "the battery sense line is not detected" telemetry.
//
// A board that expects a pack (battery monitoring ON) and whose pack ONCE READ
// VALID, but now reads persistently INVALID, is not desk-powered: its sense
// divider has most likely failed OPEN, and reporting "no gauge (desk-powered)"
// hides a real fault (the owner's nimbus-light, whose voltage-sense divider was
// open, read exactly this way). The policy still treats an invalid sample as
// desk-powered for SAFETY - it must never deep-sleep a board it cannot read -
// but the truth is surfaced alongside so the owner can tell an open sense line
// from a genuinely absent pack.
//
// The claim is EVIDENCE-GATED (v4.4.8 shipped without the gate and false-alarmed):
// a solide_s3 used desk-powered with no pack fitted has monitoring ON by default
// (the pack is part of that build, board_power.h) and its floating sense GPIO
// reads invalid forever - a legitimate configuration, not a fault. The degraded
// verdict therefore requires monitoring on AND a debounced invalid streak AND
// everSawValid: proof a pack once produced a valid sample, either this boot or
// via a durable anchor the device seeds at load (see loadBattModel in main.cpp).
// Without that evidence the honest verdict stays "no gauge (desk-powered)".
//
// Pure + host-tested (no Arduino). The device feeds one (monitoringOn, sampleValid)
// outcome per telemetry tick; the debounced verdict is surfaced over HTTP
// (batt.senseMissing) and in the health report. Three rules the tests pin:
//   - it NEVER trips when monitoring is OFF (a genuinely desk-powered board),
//   - it NEVER trips without evidence a pack once worked (a desk-powered solide
//     with monitoring at its pack-expected default), and
//   - it clears IMMEDIATELY when a valid sample arrives (a recovered/plugged pack).

namespace nimbus::power {

// The debounced predicate as a pure value: the sense is missing only when
// monitoring is on AND a pack is known to have once worked AND the invalid
// streak has reached the debounce threshold.
constexpr bool senseMissing(bool monitoringOn, bool everSawValid, uint16_t invalidStreak,
                            uint16_t threshold) {
  return monitoringOn && everSawValid && threshold != 0 && invalidStreak >= threshold;
}

// Stateful debouncer, driven one sample per telemetry tick.
class SenseMissingDetector {
 public:
  // Default 3 ticks: fed on Manager's fixed 30 s sense cadence (senseTelemetryDue,
  // which fires regardless of sample validity - never the valid-gated
  // telemetryDue): three checks 30 s apart, so the fault is claimed about a
  // minute after boot - long past any single glitched read, short enough that a
  // remote owner sees the truth in the same session.
  static constexpr uint16_t kDefaultThreshold = 3;

  SenseMissingDetector() = default;
  explicit SenseMissingDetector(uint16_t threshold)
      : threshold_(threshold ? threshold : 1) {}

  // Fold in this tick's outcome and return the current verdict.
  //   monitoringOn - battery monitoring is enabled (the board expects a pack).
  //   sampleValid  - this tick's Sample.valid.
  bool update(bool monitoringOn, bool sampleValid) {
    if (sampleValid) everSawValid_ = true;
    if (!monitoringOn || sampleValid) invalidStreak_ = 0;
    else if (invalidStreak_ != 0xFFFF) invalidStreak_++;
    missing_ = senseMissing(monitoringOn, everSawValid_, invalidStreak_, threshold_);
    return missing_;
  }

  // Durable evidence a pack once worked on this unit: the device calls this at
  // boot when the persisted battery model proves it (a full-charge anchor or a
  // completed discharge segment, both reachable only through valid samples).
  // One-way by design - evidence never un-happens within a boot.
  void seedEverSawValid() { everSawValid_ = true; }

  bool     missing() const { return missing_; }
  bool     everSawValid() const { return everSawValid_; }
  uint16_t invalidStreak() const { return invalidStreak_; }
  uint16_t threshold() const { return threshold_; }

 private:
  uint16_t threshold_ = kDefaultThreshold;
  uint16_t invalidStreak_ = 0;
  bool     everSawValid_ = false;
  bool     missing_ = false;
};

}  // namespace nimbus::power
