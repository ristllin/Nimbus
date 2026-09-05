#pragma once
#include <cstdint>

// resistive_touch - honest liveness for a resistive (XPT2046) touch controller.
//
// The capacitive (FT6336U) path has its own liveness watchdog in the driver
// (solide::touch::Health), but on a resistive board every Health field stays zero,
// so the health/status surface reported "touch: ok, 0 failures" even when the
// controller was dead - which is exactly what happened on the owner's nimbus-light
// (a dead XPT2046 whose SPI MISO was stuck high, so every raw read pegged at the
// 12-bit all-ones value, 4095). "touch up" from a begin() that succeeded at boot
// says nothing about a controller that died afterwards.
//
// This is the nimbus-side detector: it reads solide::touch::readRaw (no pinned-
// driver change) and trips only on the persistent stuck-high signature. It is
// deliberately narrow so real input never reads as dead:
//   - a normal idle (readRaw reports nothing touching) is a sign of LIFE - the
//     controller answered "no finger" - and clears the streak;
//   - a genuine press yields plausible x/y (well inside the ADC range) and clears
//     the streak;
//   - ONLY every axis pegged at the all-ones sentinel across a debounce window
//     (the MISO-stuck-high pattern) is treated as a dead controller.
//
// Pure + host-tested (no Arduino). The device feeds one raw read per liveness poll.

namespace nimbus::display {

// A single raw XPT2046 reading matches the dead-controller (MISO stuck high)
// signature when EVERY axis is pegged at the 12-bit all-ones sentinel. A real
// press never pegs both position axes at once (x/y map the touch coordinate, far
// below full scale), and an idle reads a low z - so requiring all three ruling
// out both. `sentinel` defaults to 4095 (0xFFF).
constexpr bool rawLooksDead(uint16_t x, uint16_t y, uint16_t z, uint16_t sentinel = 4095) {
  return x >= sentinel && y >= sentinel && z >= sentinel;
}

// Debounced resistive-touch liveness. Driven one raw read per poll.
class ResistiveTouchLiveness {
 public:
  // Default 4 consecutive stuck reads: enough to ignore a lone glitched read,
  // short at the device's ~2 s liveness cadence (~8 s of confirmed-stuck comms).
  static constexpr uint16_t kDefaultThreshold = 4;

  ResistiveTouchLiveness() = default;
  explicit ResistiveTouchLiveness(uint16_t threshold)
      : threshold_(threshold ? threshold : 1) {}

  // Fold in one poll and return the current verdict.
  //   gotRaw - readRaw() returned true (it thinks something is touching). A false
  //            here is a live controller reporting no finger: a sign of life.
  //   x,y,z  - the raw ADC counts from that read (only meaningful when gotRaw).
  bool update(bool gotRaw, uint16_t x, uint16_t y, uint16_t z) {
    if (gotRaw && rawLooksDead(x, y, z)) {
      if (deadStreak_ != 0xFFFF) deadStreak_++;
    } else {
      deadStreak_ = 0;  // no touch, or a real press: the controller is alive
    }
    degraded_ = deadStreak_ >= threshold_;
    return degraded_;
  }

  bool     degraded() const { return degraded_; }
  uint16_t deadStreak() const { return deadStreak_; }
  uint16_t threshold() const { return threshold_; }

 private:
  uint16_t threshold_ = kDefaultThreshold;
  uint16_t deadStreak_ = 0;
  bool     degraded_ = false;
};

}  // namespace nimbus::display
