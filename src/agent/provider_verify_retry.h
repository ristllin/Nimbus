#pragma once
#include <cstdint>

// provider_verify_retry - the PURE policy behind a low-memory-deferred verify's
// self-retry (CUM-447). No Arduino, no hardware, no globals, so the host suite
// (test/test_provider_verify_retry) drives it with a synthetic clock. The device
// seam in provider_verify.cpp owns one DeferralRetry per provider and calls these
// from the verify task (onDeferred/clear) and the main-loop pump (due).
namespace agent {
namespace provider_verify {

// The reason token a verify records when the largest contiguous INTERNAL block is
// under the gate: shared by the verify task (to label the deferral) and the host
// test (to pin the class). Empty when memory is sufficient. Kept here, beside the
// backoff, so the token and the number always move together.
inline const char* deferReason(uint32_t max8, uint32_t floor) {
  return max8 < floor ? "low-memory" : "";
}

// Bounded exponential backoff for a deferred verify. Delays grow 2, 4, 8, 16 min
// then hold at a 16 min cap (a few tries per hour), and a run of memory-sufficient
// attempts clears it. RAM-only: a reboot resets the streak (the provider re-probes
// on the next verify, harmless), the same trade the W3b capProbe timer makes.
struct DeferralRetry {
  static const uint32_t kFirstDelayMs = 120000u;   // 2 min - first retry after a defer
  static const uint32_t kMaxDelayMs   = 960000u;   // 16 min - steady cap once backed off

  bool     armed     = false;   // a low-memory deferral is outstanding
  uint32_t delayMs   = 0;       // current backoff step (0 until the first defer)
  uint32_t nextAtMs  = 0;       // earliest the next retry may fire
  uint32_t firstAtMs = 0;       // when this deferral streak began (drives the health line)

  // A verify came back low-memory-deferred at `now`: grow the backoff and re-arm.
  void onDeferred(uint32_t now) {
    if (!armed) firstAtMs = now;
    uint32_t next = delayMs ? delayMs * 2 : kFirstDelayMs;
    delayMs  = next > kMaxDelayMs ? kMaxDelayMs : next;
    nextAtMs = now + delayMs;
    armed    = true;
  }

  // Memory was sufficient (or a definitive verdict landed): stop retrying.
  void clear() { armed = false; delayMs = 0; nextAtMs = 0; firstAtMs = 0; }

  // Is a retry DUE now? Armed, past the backoff, and the device is free to run one:
  // never during a turn (the turn holds the TLS slot), never offline, never while a
  // verify is already queued.
  bool due(uint32_t now, bool inTurn, bool online, bool busy) const {
    if (!armed || inTurn || !online || busy) return false;
    return (int32_t)(now - nextAtMs) >= 0;
  }

  // Has this deferral persisted past `windowMs` without clearing? (the Memory
  // health row says so once a provider is stuck this long).
  bool stuckFor(uint32_t now, uint32_t windowMs) const {
    return armed && (int32_t)(now - (firstAtMs + windowMs)) >= 0;
  }
};

}  // namespace provider_verify
}  // namespace agent
