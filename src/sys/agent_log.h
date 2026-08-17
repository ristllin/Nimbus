#pragma once
#include <Arduino.h>
#include <stdarg.h>

// agent_log - the device-side logging seam for the Orchestrator subsystem.
//
// Nuage-Solide's agent code logs via sys/logbuf (a ring buffer with secret
// redaction exposed on an auth-gated /logs endpoint). Nimbus has the portable
// core::LogRing (lib/core/logring.h) with the same redaction contract but no
// device glue wired yet for the agent path. To keep this port COMPILE-CLEAN and
// self-contained, agent code logs through alog()/alogf() here, which currently
// forward to Serial. When the Nimbus sys/logbuf glue lands, point these at it
// (and register the API keys / bot token as secrets so they are redacted) - every
// call site already routes through this one seam.
//
// NOTE (live-gated): agent logs can echo provider error bodies. Before shipping,
// wire alog/alogf to core::LogRing and addSecret() the OpenAI/Anthropic/Mistral/
// custom keys + Telegram token so a key can never leak to the log surface.
namespace agent {

// RAM log ring so agent diagnostics are readable over HTTP (GET /api/log) WITHOUT
// opening the USB serial - on this S3, opening serial drops the WiFi STA, which
// would mask any network-dependent failure (STT/turns) behind a false "no network".
// Fixed byte ring, no heap in the write path; a portMUX guards cross-task writes
// (orchestrator poll task + main task both log).
namespace logring {
constexpr size_t kCap = 1280;   // small on purpose: this + agentLogTail's snapshot are
                                // static internal RAM, which is the scarce pool on the S3
inline char         g_buf[kCap];
inline size_t       g_total = 0;   // total bytes ever written (mod kCap = head)
inline portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

inline void put(const char* s) {
  portENTER_CRITICAL(&g_mux);
  for (; *s; ++s) { g_buf[g_total % kCap] = *s; ++g_total; }
  g_buf[g_total % kCap] = '\n'; ++g_total;
  portEXIT_CRITICAL(&g_mux);
}
}  // namespace logring

// Snapshot the ring (oldest->newest) into a String. Copies under the lock into a
// static buffer, then builds the String outside the critical section (no heap under
// the spinlock). TODO(secrets): the ring can echo provider error bodies, so it may hold
// provider keys. /api/log is token-gated, which bounds the exposure to a caller who
// already holds the token - redact at the source before any wider exposure.
inline String agentLogTail() {
  static char snap[logring::kCap + 1];
  size_t n;
  portENTER_CRITICAL(&logring::g_mux);
  const size_t total = logring::g_total;
  const size_t start = total > logring::kCap ? total - logring::kCap : 0;
  n = total - start;
  for (size_t i = 0; i < n; ++i) snap[i] = logring::g_buf[(start + i) % logring::kCap];
  portEXIT_CRITICAL(&logring::g_mux);
  snap[n] = 0;
  return String(snap);
}

inline void alog(const char* msg) {
  Serial.print("[agent] ");
  Serial.println(msg);
  logring::put(msg);
}

inline void alogf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print("[agent] ");
  Serial.println(buf);
  logring::put(buf);
}

}  // namespace agent
