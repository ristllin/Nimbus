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

}  // namespace provider_verify
}  // namespace agent
