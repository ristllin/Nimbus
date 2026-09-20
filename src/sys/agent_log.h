#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdarg.h>

#include <string>
#include <vector>

#include "nimbus/logring.h"     // core::LogRing::redact (portable, host-tested)
#include "nimbus/log_sinks.h"   // core::emitRedacted (portable, host-tested two-sink seam)
#include "errlog.h"             // nimbus::errlog::Level + persistsDurably (durable routing, CUM-409)
#include "errlog_fs.h"          // durable SD/flash sink (CUM-401) - fed the SAME redacted line

// agent_log - the device-side logging seam for the Orchestrator subsystem.
//
// Agent code logs through alog()/alogf() here. Both the ring-visible surface
// (GET /api/log, an auth-gated HTTP endpoint) and Serial are fed from the ONE
// choke point core::emitRedacted(), which redacts every line ONCE through
// core::LogRing::redact and hands that same masked line to both sinks - so serial
// can never print a value the ring redacted away (the F4 serial-bypass, CUM-281).
// Provider error bodies echoed into a log line can carry the device's own keys or
// an Authorization header, so:
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

// Append an ALREADY-redacted line to the ring. Only byte copies run here, so this is safe
// inside the no-heap portMUX critical section (core::emitRedacted does the allocating
// redaction before calling this). The ring-visible /api/log surface therefore never holds
// a raw provider key or echoed Bearer value.
inline void store(const std::string& red) {
  const char* r = red.c_str();
  portENTER_CRITICAL(&g_mux);
  for (; *r; ++r) { g_buf[g_total % kCap] = *r; ++g_total; }
  g_buf[g_total % kCap] = '\n'; ++g_total;
  portEXIT_CRITICAL(&g_mux);
}
}  // namespace logring

// Snapshot the ring (oldest->newest) into a String. Copies under the lock into a
// static buffer, then builds the String outside the critical section (no heap under
// the spinlock). Lines were already redacted at the source (core::emitRedacted), so the
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

// All sinks are fed by core::emitRedacted, which redacts ONCE and hands the same masked
// line to the Serial writer, the RAM ring AND the durable sink. Serial therefore prints
// the redacted line, never the raw msg - printing `msg` would leak a sign-in/pairing code
// or an echoed key over USB serial while the ring stayed clean (the F4 serial-bypass,
// CUM-281). By the same single-redaction property, the durable SD/flash log (errlog,
// CUM-401) can only ever store the identical masked line - there is no second, un-redacted
// path into it.
inline void logSerial(const std::string& red) {
  Serial.print("[agent] ");
  Serial.println(red.c_str());
}

// The RAM-ring + durable-sink writer. logring::store() copies bytes under its own no-heap
// portMUX critical section and RETURNS before errlog::append() runs, so nothing below runs
// inside that spinlock. errlog::append() is non-blocking by contract: it records to its own
// RAM tail under a leaf spinlock and only best-effort-persists to the card under a SHORT
// timed lock (skipping, never stalling, if the card is contended), so a log call from the
// WDT-guarded loop or an AsyncTCP handler is always cheap.
//
// Durable routing (CUM-409): the RAM ring + Serial always get the line; the durable FS
// sink gets it only when errlog::persistsDurably(lvl, cat) says so - warn/error and
// category-tagged key events persist, routine info stays RAM-only. This bounds the
// internal-flash write wear CUM-401 caused by persisting EVERY line on the caller task.
inline void logPersist(const std::string& red, const char* cat, nimbus::errlog::Level lvl) {
  logring::store(red);
  if (nimbus::errlog::persistsDurably(lvl, cat))
    nimbus::errlog::append(red, cat);
}

// Level of a bare alog()/alogf(): durable, so no existing call site loses durable capture
// when CUM-409 lands (bare alog was durable under CUM-401). Use alogi()/alogif() for the
// routine info stream that should stay in the RAM ring + Serial only.
inline void alog(const char* msg) {
  core::emitRedacted(msg, logring::g_secrets, logSerial,
                     [](const std::string& red) { logPersist(red, nullptr, nimbus::errlog::Level::Warn); });
}

inline void alogf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  core::emitRedacted(buf, logring::g_secrets,
                     logSerial, [](const std::string& red) { logPersist(red, nullptr, nimbus::errlog::Level::Warn); });
}

// Info level: the full verbose stream stays in the RAM ring (GET /api/log) + Serial, but
// is NOT written to the durable FS log (CUM-409). Use this for routine, high-frequency
// diagnostics whose durability would only add flash wear.
inline void alogi(const char* msg) {
  core::emitRedacted(msg, logring::g_secrets, logSerial,
                     [](const std::string& red) { logPersist(red, nullptr, nimbus::errlog::Level::Info); });
}

inline void alogif(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  core::emitRedacted(buf, logring::g_secrets, logSerial,
                     [](const std::string& red) { logPersist(red, nullptr, nimbus::errlog::Level::Info); });
}

// Explicit warn/error: same three sinks, durable. Semantic aliases of the durable default
// so a caller can state severity at the site (and so an info-defaulted future never drops
// a genuine error from the durable log).
inline void alogw(const char* msg) {
  core::emitRedacted(msg, logring::g_secrets, logSerial,
                     [](const std::string& red) { logPersist(red, nullptr, nimbus::errlog::Level::Warn); });
}

inline void alogwf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  core::emitRedacted(buf, logring::g_secrets, logSerial,
                     [](const std::string& red) { logPersist(red, nullptr, nimbus::errlog::Level::Warn); });
}

inline void aloge(const char* msg) {
  core::emitRedacted(msg, logring::g_secrets, logSerial,
                     [](const std::string& red) { logPersist(red, nullptr, nimbus::errlog::Level::Error); });
}

inline void alogef(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  core::emitRedacted(buf, logring::g_secrets, logSerial,
                     [](const std::string& red) { logPersist(red, nullptr, nimbus::errlog::Level::Error); });
}

// Category-tagged variants: same three sinks, but the durable log line carries a short
// class tag (nimbus::errlog::cat::*) so a retrieval reader can grep the failure classes
// CUM-401 targets (mem/provider/relay/ota/panel/nvs/storage/net). The RAM ring + Serial
// are unchanged (the tag is a durable-log concept), so /api/log stays byte-for-byte as is.
inline void alogc(const char* cat, const char* msg) {
  core::emitRedacted(msg, logring::g_secrets, logSerial,
                     [cat](const std::string& red) { logPersist(red, cat, nimbus::errlog::Level::Warn); });
}

inline void alogcf(const char* cat, const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  core::emitRedacted(buf, logring::g_secrets, logSerial,
                     [cat](const std::string& red) { logPersist(red, cat, nimbus::errlog::Level::Warn); });
}

}  // namespace agent
