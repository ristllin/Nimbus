#pragma once
#include <cstdint>

// thermal_guard - pure trip/resume/abort logic for sustained high LED loads.
//
// Added after a thermal failure of an e-ink panel (2026-07-15): the battery-drain campaign pinned
// the 45-LED ring at solid white ~75% brightness (~10 W) right next to the
// SSD1680 panel; ~15 minutes of that killed a screen. The only thermometer on
// this hardware is the ESP32-S3's internal DIE sensor - a proxy for enclosure
// heat (the die self-heats ~10-20C above ambient with WiFi up), so the guard
// uses BOTH an absolute die ceiling and a rise-over-baseline delta: the delta
// catches "the enclosure is cooking" even when the absolute number still looks
// tame, and the absolute cap catches a hot start.
//
// Pure + host-tested (test_thermal_guard): the caller samples the die sensor at
// its own cadence and feeds temps in; this decides. No clocks, no Arduino.
//
// Lifecycle:  arm(baseline) -> Armed
//   Armed:   temp >= absC OR temp >= baseline+riseC  -> Trip   (load OFF)
//   Tripped: temp <= tripAt - hystC                  -> Resume (load back, REDUCED)
//   trips > maxTrips                                 -> Abort  (op cancelled, stays off)
// disarm() returns to Idle (no decisions).

namespace nimbus::power {

class ThermalGuard {
 public:
  enum class Decision : uint8_t { None = 0, Trip, Resume, Abort };

  struct Config {
    float   absC     = 65.0f;  // absolute die ceiling
    float   riseC    = 18.0f;  // rise over the armed baseline
    float   hystC    = 8.0f;   // cooldown below the trip temperature to resume
    uint8_t maxTrips = 3;      // trips beyond this -> Abort (something is wrong)
  };

  ThermalGuard() = default;                              // default Config
  explicit ThermalGuard(const Config& c) : cfg_(c) {}    // (no `= {}` default arg -
                                                         // ill-formed for a nested
                                                         // class with NSDMIs)

  void arm(float baselineC) {
    armed_ = true; tripped_ = false; aborted_ = false;
    baseline_ = baselineC; trips_ = 0; tripAt_ = 0.0f;
  }
  void disarm() { armed_ = false; tripped_ = false; }

  // Feed a die-temperature sample; act on the returned decision ONCE (edges,
  // not levels - None means "keep doing what you're doing").
  Decision onTemp(float c) {
    if (!armed_ || aborted_) return Decision::None;
    if (!tripped_) {
      if (c >= cfg_.absC || c >= baseline_ + cfg_.riseC) {
        ++trips_;
        if (trips_ > cfg_.maxTrips) { aborted_ = true; armed_ = false; return Decision::Abort; }
        tripped_ = true;
        tripAt_ = c;
        return Decision::Trip;
      }
      return Decision::None;
    }
    // Tripped: resume only after a real cooldown below the trip point (and never
    // above the absolute ceiling).
    if (c <= tripAt_ - cfg_.hystC && c < cfg_.absC) {
      tripped_ = false;
      return Decision::Resume;
    }
    return Decision::None;
  }

  bool    armed()   const { return armed_; }
  bool    tripped() const { return tripped_; }
  bool    aborted() const { return aborted_; }
  uint8_t trips()   const { return trips_; }
  float   baseline() const { return baseline_; }

 private:
  Config  cfg_;
  bool    armed_ = false, tripped_ = false, aborted_ = false;
  float   baseline_ = 0.0f, tripAt_ = 0.0f;
  uint8_t trips_ = 0;
};

}  // namespace nimbus::power
