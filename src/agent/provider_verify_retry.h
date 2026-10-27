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
//
// Row 43: a low-memory deferral also re-arms on heap RECOVERY. The gate input is the
// resting largest block (the minimum of the last idle samples, heap_watch), so once
// the device rests above the floor again the verify runs on the next pump pass rather
// than up to 16 min later - at most once per kRecoverGapMs, so a block hovering at the
// floor cannot turn into a verify storm.
struct DeferralRetry {
  static const uint32_t kFirstDelayMs  = 120000u;   // 2 min - first retry after a defer
  static const uint32_t kMaxDelayMs    = 960000u;   // 16 min - steady cap once backed off
  static const uint32_t kRecoverGapMs  = 30000u;    // at most one recovery retry per 30 s

  bool     armed     = false;   // a low-memory deferral is outstanding
  bool     recovery  = false;   // the next heap recovery may fire a retry early
  uint32_t delayMs   = 0;       // current backoff step (0 until the first defer)
  uint32_t nextAtMs  = 0;       // earliest the next retry may fire
  uint32_t firstAtMs = 0;       // when this deferral streak began (drives the health line)
  uint32_t lastAtMs  = 0;       // when the last deferral was recorded

  // A verify came back low-memory-deferred at `now`: grow the backoff and re-arm
  // (the backoff and the recovery trigger both).
  void onDeferred(uint32_t now) {
    if (!armed) firstAtMs = now;
    uint32_t next = delayMs ? delayMs * 2 : kFirstDelayMs;
    delayMs  = next > kMaxDelayMs ? kMaxDelayMs : next;
    nextAtMs = now + delayMs;
    lastAtMs = now;
    armed    = true;
    recovery = true;
  }

  // Memory was sufficient (or a definitive verdict landed): stop retrying.
  void clear() { armed = false; recovery = false; delayMs = 0; nextAtMs = 0; firstAtMs = 0; lastAtMs = 0; }

  // Is a retry DUE now? Armed, past the backoff, and the device is free to run one:
  // never during a turn (the turn holds the TLS slot), never offline, never while a
  // verify is already queued.
  bool due(uint32_t now, bool inTurn, bool online, bool busy) const {
    if (!armed || inTurn || !online || busy) return false;
    return (int32_t)(now - nextAtMs) >= 0;
  }

  // Due early because the heap recovered: the resting largest block is back at the
  // gate's floor, at least kRecoverGapMs after the deferral, once per deferral.
  bool dueOnRecovery(uint32_t now, bool inTurn, bool online, bool busy, uint32_t restingLargest,
                     uint32_t floor) const {
    if (!armed || !recovery || inTurn || !online || busy) return false;
    return restingLargest >= floor && (uint32_t)(now - lastAtMs) >= kRecoverGapMs;
  }
  // The pump fired the recovery retry (a new deferral re-arms it).
  void onRecoveryFired() { recovery = false; }

  // Has this deferral persisted past `windowMs` without clearing? (the Memory
  // health row says so once a provider is stuck this long).
  bool stuckFor(uint32_t now, uint32_t windowMs) const {
    return armed && (int32_t)(now - (firstAtMs + windowMs)) >= 0;
  }
};

// What a low-memory deferral does (CUM-575). The retry is armed for EVERY provider,
// already-verified ones included: a verify is also how the model list and the router
// prices are refreshed, and a refresh deferred on a loaded device used to be dropped
// for good (the boot-time Cumulo price refill never landed). The displayed state of a
// verified provider stays verified, so the low-memory number is stored (and shown)
// only for a provider that is not.
struct LowMemoryDefer {
  bool armRetry;      // schedule the self-retry (backoff + heap recovery)
  bool storeNumber;   // persist the measured block for the badge's hover text
};
inline LowMemoryDefer onLowMemoryDefer(bool wasVerified) { return {true, !wasVerified}; }

// A verify the memory gate deferred still gets its provider's model list (the refresh needs
// one connection's memory only). Only a key not verified yet (just saved: the web marks it
// -1) forces that refresh past the hourly window, since a new key may see another list. A
// verified key's deferral keeps the hour: on a heap that never reaches the verify's floor
// (Lumi: 7,156 B against 8,000 B) the deferral retry fires every 16 min for good, and a
// forced refresh each time refetched the whole list on the Telegram task (HFM review).
inline bool deferredVerifyForcesRefresh(bool wasVerified) { return !wasVerified; }
// A refresh-only run whose list GET answered 200 serves what the deferral was waiting for (the
// key works, the list is fresh): it ends the deferral, so the retry stops re-firing.
inline bool refreshEndsDeferral(bool refreshOnly, int8_t result) { return refreshOnly && result == 1; }
// One hard budget for a refresh run on the Telegram poll task (it holds Telegram and every
// turn while it runs): each wait is the smaller of its own cap and what is left before
// `untilMs`, 0 once the budget is spent. Wrap-safe.
inline uint32_t inlineWaitMs(uint32_t nowMs, uint32_t untilMs, uint32_t capMs) {
  const int32_t left = (int32_t)(untilMs - nowMs);
  if (left <= 0) return 0;
  return (uint32_t)left < capMs ? (uint32_t)left : capMs;
}

// Router prices after an upgrade (per-model prices, 2026-10 bench): the Cumulo
// router's per-model prices are harvested only by a Cumulo verify, so a device that
// upgraded with a verified Cumulo key priced every Cumulo model at a built-in
// estimate until someone pressed Verify. Boot arms ONE Cumulo verify, deferred like
// the other boot checks; the main-loop pump fires it through the same request path
// (the self-deleting verify task, no new task) when the device is online and no turn
// runs, and only while it is still wanted. A start the device could not make is
// tried again on a slow clock, a few times at most. RAM-only: every boot asks again
// while the prices are missing.
struct RouterPriceRefill {
  static const uint8_t  kMaxTries = 3;
  static const uint32_t kRetryMs  = 120000u;   // 2 min between starts that failed

  bool     armed = false;
  uint8_t  tries = 0;
  uint32_t atMs  = 0;

  // What to do once due. `priceState`: 1 = no router price stored, 0 = stored,
  // -1 = flash storage not loaded yet (cannot tell). Start the verify for a verified
  // Cumulo key with no stored price; Later when it cannot tell yet; else Done.
  enum class Next : uint8_t { Start, Done, Later };
  static Next decide(bool hasKey, bool verified, int8_t priceState) {
    if (!hasKey || !verified || priceState == 0) return Next::Done;
    return priceState < 0 ? Next::Later : Next::Start;
  }
  void arm(uint32_t now, uint32_t delayMs) {
    armed = true;
    tries = 0;
    atMs  = now + delayMs;
  }
  // Due now: armed, past its time, online, no turn in flight (the pump checks the
  // verify slot itself before it asks).
  bool due(uint32_t now, bool inTurn, bool online) const {
    if (!armed || inTurn || !online) return false;
    return (int32_t)(now - atMs) >= 0;
  }
  // The verify started, or it is no longer wanted: done for this boot.
  void done() { armed = false; }
  // The verify could not start (or storage could not tell yet): try again later, or
  // give up after kMaxTries.
  void startFailed(uint32_t now) {
    if (++tries >= kMaxTries) { armed = false; return; }
    atMs = now + kRetryMs;
  }
};

}  // namespace provider_verify
}  // namespace agent
