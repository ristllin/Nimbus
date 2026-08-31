#pragma once
#include <ArduinoJson.h>
#include <cstdint>

#include "solide/touch_liveness.h"   // solide::touch::Health (portable, cstdint-only)

// ============================================================================
// Soak-observability fields for the LAN status surface (GET /api/state).
//
// CUM-257: the LAN-driven soak legs (CUM-243 ring backstop, CUM-248 touch
// liveness) want uptime + counters observable over HTTP without a serial open
// (a CDC open resets the board and so destroys the very run being measured).
// This helper centralizes those fields so buildState() emits them from one
// place and a host test can assert their shape and monotonicity directly - the
// device seam only supplies the live values.
//
// Portable on purpose: no Arduino, only ArduinoJson (host-safe, header-only)
// and the driver's portable Health snapshot. Every touch counter is monotonic
// except consecutiveFailures (the live failure streak, 0 when healthy); on a
// resistive board every touch field stays zero.
// ============================================================================
namespace nimbus::net {

// Fill the soak-observability fields onto the /api/state root object.
//   uptimeMs          - millis() since boot (monotonic until the ~49.7-day wrap)
//   ringBackstopFires - CUM-11 belt-and-braces ring backstop clears (stays 0 healthy)
//   touch             - capacitive touch liveness/recovery snapshot (all-zero on resistive)
inline void soakStateInto(JsonObject o, uint32_t uptimeMs, uint32_t ringBackstopFires,
                          const solide::touch::Health& touch) {
  o["uptimeMs"] = uptimeMs;
  // CUM-11: how many times a belt-and-braces ring backstop had to clear a stuck
  // arc. The primary edge clears a healthy wake-up's arc, so this stays 0; a
  // nonzero value flags a real wedge. The 24 h wake-up soak asserts it stays flat.
  o["ringBackstopFires"] = ringBackstopFires;
  // CUM-248: FT6336U touch controller liveness. The sleep/wake soak reads these
  // over LAN to confirm the controller recovers from a silent I2C dropout instead
  // of going dead until a power-cycle. failures/recoveries/busClears/hardResets are
  // monotonic; consecFailures is the current live streak (0 when comms is healthy).
  JsonObject th = o["touch"].to<JsonObject>();
  th["failures"]       = touch.failures;
  th["recoveries"]     = touch.recoveries;
  th["busClears"]      = touch.busClears;
  th["hardResets"]     = touch.hardResets;
  th["consecFailures"] = touch.consecutiveFailures;
  th["lastRecoveryMs"] = touch.lastRecoveryMs;
  th["degraded"]       = touch.degraded;
}

}  // namespace nimbus::net
