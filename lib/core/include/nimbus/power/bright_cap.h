#pragma once
#include <cstdint>

// bright_cap - the global LED-brightness safety cap (owner feature, 2026-07-17).
//
// A run at ~75% brightness once physically damaged an e-ink panel (heat - the ring
// sits against the shell/screen). The measured thermal ceiling from the battery
// study is 150/255 (~59%): runs at that level hold the die ~50-52 °C with zero
// guard trips. Above it the risk is CATASTROPHIC-but-slow: shell/panel damage,
// not an instant fault, so software must default to refusing it.
//
//   cap      = 153 (60% of 255) unless overridden
//   override = the OWNER (web UI) or the AI (config action, which is REQUIRED by
//              its schema docs to tell the owner + weigh the risk) lifts the cap
//              to the full 255. The on-device ThermalGuard stays armed regardless
//              - the override removes the *static* cap, never the dynamic guard.
//
// Portable + trivially testable; the device applies it at every brightness writer
// (ring plan, wake-reveal envelope, drain load, model lights action).

namespace nimbus::power {

constexpr uint8_t kBrightCap = 153;   // 60% of 255 - the measured-safe ceiling

inline uint8_t clampBright(uint8_t b, bool overrideCap) {
  if (overrideCap) return b;                  // owner/AI accepted the risk: full range
  return b > kBrightCap ? kBrightCap : b;
}

}  // namespace nimbus::power
