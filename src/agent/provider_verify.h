#pragma once
#include <Arduino.h>

// provider_verify - one authenticated GET /v1/models per provider to prove an
// API key works; the web UI gates the model dropdowns on the cached verdict
// ("only enable options that are actively verified"). Ported from Nuage-Solide
// src/provider_verify.{h,cpp} with one structural change:
//
// Nuage ran the verify from the Telegram poll task (the only TLS-safe spot on
// the no-PSRAM board). Nimbus must verify in BOTH operating modes - in Notifier
// mode no poll task exists - so begin() spawns a tiny dedicated task that idles
// on the one-slot queue and runs each verify under the TLS arbiter (which
// serializes it against orchestrator turns; the persistent Telegram session is
// not arbited and simply coexists, same as Nuage). The task is watchdog-free,
// so the up-to-~35 s acquire+handshake+read worst case cannot trip the F12 loop
// watchdog the way an inline main-loop verify would (8 s panic).
//
// Results land in store::setVerify: 1 = verified (HTTP 200), 0 = rejected
// (401/403), -1 = couldn't verify (no key / connect failed / TLS slot busy -
// transient, retryable). The web handler only ENQUEUEs (request) and the UI
// polls /api/orch until the provider's verify timestamp bumps.
namespace agent {
namespace provider_verify {

// Spawn the verify task. Call once from setup() (either mode, after WiFi init).
void begin();

// Enqueue a verify for `provider` ("openai" | "anthropic" | "mistral"). One
// slot: returns false if a verify is already pending (caller reports "busy").
bool request(const String& provider);

// True while a verify is queued or running.
bool pending();

// The machine-readable reason for `provider`'s LAST verify outcome, for the web
// UI badge (CUM-77 x1 §4; contract in lanes/L1/PROGRESS.md). One of:
//   nocredits | router_outdated | low-memory | connectfail | tlsbusy | "" (none)
// Empty when verified (result 1), plainly rejected (0), or a generic transient.
// Emitted into the provider objects of /api/state, /api/orch and /api/models as
// `vfyReason`. verify===1 overrides it (the badge shows verified regardless).
String reason(const String& provider);

// The measured largest contiguous INTERNAL block (bytes) from `provider`'s last
// low-memory deferral, and the fixed gate it had to clear. The web UI shows both in
// the deferred pill's hover ("largest free block 11 KB, needs 8 KB"). deferMax8 is
// 0 for a provider that never deferred; deferFloor is a compile-time constant.
uint32_t deferMax8(const String& provider);
uint32_t deferFloor();

// Re-arm pump for low-memory deferrals (CUM-447). Call once per main-loop pass: it
// re-requests a verify for any provider whose deferral backoff is due, provided the
// device is free (online, no turn in flight, no verify already queued). Reuses the
// existing self-deleting, TLS-arbited verify task - no new task, no new TLS slot.
// The Verify button still forces one immediately via request().
void pumpRetry();

// True while at least one provider has stayed low-memory-deferred past the retry
// window (the Memory health row surfaces this in one line).
bool anyDeferredStuck();

}  // namespace provider_verify
}  // namespace agent
