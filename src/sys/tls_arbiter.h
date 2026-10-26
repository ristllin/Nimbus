#pragma once
#include <Arduino.h>

#include "nimbus/held_slot.h"   // WorkSlot: the slot comes back on every path out

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
// The cloud relay keeps its WebSocket session open between requests, but it is not
// outside the rule: its dial (connect, upgrade, hello) takes the slot, and every
// response frame over 8 KB takes it one 8 KB chunk at a time, released between
// chunks so a waiting turn gets it within one chunk (nimbus/cloud/relay_slot.h).
// Heartbeats, acks and small frames take no slot. tests/hil/test_l41_tls_slot_guard.py
// checks every TLS connect in src/ against this rule.
//
// Usage (a guard, so the slot comes back on every path out, an exception included):
//   arbiter::WorkSlot slot(10000);
//   if (!slot) { /* timeout, try later */ }
//   // ... open client, do HTTP, close client ...
//   slot.release();   // or let the guard release it when the scope ends
// A turn catches std::bad_alloc (L24), so code a turn can reach never holds the slot
// by hand: test_held_slot's source guard lists the few non-turn sites that still do.
namespace agent {
namespace arbiter {
void begin();
bool acquireWork(uint32_t timeoutMs = 30000);
void releaseWork();
int  slots();   // configured slot count (0 = arbiter not started)

// One work slot held by a scope guard (nimbus/held_slot.h).
class WorkSlot : public nimbus::HeldSlot {
 public:
  explicit WorkSlot(uint32_t timeoutMs) : nimbus::HeldSlot(&acquireWork, &releaseWork, timeoutMs) {}
};
}  // namespace arbiter
}  // namespace agent
