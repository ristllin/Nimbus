#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdarg.h>

#include <string>
#include <vector>

#include "nimbus/logring.h"   // core::LogRing::redact (portable, host-tested)

// agent_log - the device-side logging seam for the Orchestrator subsystem.
//
// Agent code logs through alog()/alogf() here. Both the ring-visible surface
// (GET /api/log, an auth-gated HTTP endpoint) and Serial are fed from the ONE
// choke point logring::put(), which redacts every line through
// core::LogRing::redact before it is stored. Provider error bodies echoed into a
// log line can carry the device's own keys or an Authorization header, so:
//   - Layer 1 (reliable): the provider keys + Telegram bot token are registered
//     as exact secrets at boot via logring::addSecret() (see main.cpp), so a
//     known key is masked wherever it appears.
//   - Layer 2 (backstop): the "Bearer <tok>", api_key=/"key":"..." and
//     user:pass@host heuristics catch an unregistered secret (e.g. a key set
//     after boot) in the formats a provider error body actually uses.
// This closes the old "the ring can hold provider keys" gap (CUM-73).
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
// Registered exact secrets (provider keys + bot token). Written once at boot on
// the single setup task, then read-only, so the redact() reads below need no
// lock even though put() runs from both the poll task and the main task.
inline std::vector<std::string> g_secrets;

// Register an exact secret to mask in every future log line (>=4 chars; shorter
// values are ignored by redact). Call at boot for each provisioned key/token.
inline void addSecret(const char* s) { if (s && s[0]) g_secrets.emplace_back(s); }
inline void clearSecrets() { g_secrets.clear(); }

inline void put(const char* s) {
  // Redact BEFORE the spinlock: redact() allocates, and no heap may run under
  // the critical section. This is the choke point for the ring-visible /api/log
  // surface, so a provider error body echoing a key never reaches the ring raw.
  std::string red = core::LogRing::redact(s ? s : "", g_secrets);
  const char* r = red.c_str();
  portENTER_CRITICAL(&g_mux);
  for (; *r; ++r) { g_buf[g_total % kCap] = *r; ++g_total; }
  g_buf[g_total % kCap] = '\n'; ++g_total;
  portEXIT_CRITICAL(&g_mux);
}
}  // namespace logring

// Snapshot the ring (oldest->newest) into a String. Copies under the lock into a
// static buffer, then builds the String outside the critical section (no heap under
// the spinlock). Lines were already redacted at the source (logring::put), so the
// ring never holds a registered key or an echoed Bearer/api_key value; /api/log is
// additionally token-gated.
inline String agentLogTail() {
  // PSRAM scratch, allocated OUTSIDE the spinlock (the no-heap-under-lock rule), so
  // the ~1.3 KB snapshot buffer is not a permanent internal-SRAM static (SRAM reclaim).
  char* snap = (char*)heap_caps_malloc(logring::kCap + 1, MALLOC_CAP_SPIRAM);
  if (!snap) snap = (char*)heap_caps_malloc(logring::kCap + 1, MALLOC_CAP_8BIT);
  if (!snap) return String();
  size_t n;
  portENTER_CRITICAL(&logring::g_mux);
  const size_t total = logring::g_total;
  const size_t start = total > logring::kCap ? total - logring::kCap : 0;
  n = total - start;
  for (size_t i = 0; i < n; ++i) snap[i] = logring::g_buf[(start + i) % logring::kCap];
  portEXIT_CRITICAL(&logring::g_mux);
  snap[n] = 0;
  String out(snap);
  heap_caps_free(snap);
  return out;
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
