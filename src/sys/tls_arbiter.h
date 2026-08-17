#pragma once
#include <Arduino.h>

// Ported verbatim from Nuage-Solide src/tls_arbiter.h (Head Orchestrator v2).
//
// Sequential work-slot arbitration. Telegram runs on its own persistent TLS
// session (core 0, never released). Everything else - agent dispatch, agent poll,
// orchestrator turn, STT/TTS - must take the work slot before opening a
// WiFiClientSecure and release it immediately after the connection closes. This
// keeps total concurrent TLS open to at most 2 (Telegram + one active call), so
// the single mbedTLS arena never has to hold two resident sessions.
//
// v2.0.0: DONE - the slot is a counting semaphore, default 2 concurrent work-TLS
// sessions (NVS `tlsSlots` 1..2, latched at begin(); mbedTLS + malloc churn are
// PSRAM-backed, so the second session's internal cost is lwIP + stack only).
// Total concurrent TLS is now at most 3 (Telegram + 2 work slots); `tlsSlots=1`
// restores the classic single-slot behavior.
//
// Usage:
//   if (!arbiter::acquireWork(10000)) { /* timeout, try later */ }
//   // ... open client, do HTTP, close client ...
//   arbiter::releaseWork();
namespace agent {
namespace arbiter {
void begin();
bool acquireWork(uint32_t timeoutMs = 30000);
void releaseWork();
int  slots();   // configured slot count (0 = arbiter not started)
}  // namespace arbiter
}  // namespace agent
