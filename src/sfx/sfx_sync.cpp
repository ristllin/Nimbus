#include "sfx_sync.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <memory>

#include "../agent/agent_config.h"
#include "../sys/agent_log.h"
#include "../agent/memory_subsystem.h"  // memory::haveSd()
#include "../sys/net_util.h"          // tlsClose (RST close, no TIME_WAIT)
#include "../sys/ps_json.h"           // PsramJsonAllocator
#include "../sys/tls_arbiter.h"
#include "nimbus/fault.h"
#include "nimbus/sfx_map.h"
#include "nimbus/sfx_paths.h"
#include "sound_fx.h"

namespace sfxsync {

namespace {

constexpr uint32_t kHttpTimeoutMs   = 30000;
constexpr uint32_t kIdleSleepMs     = 10 * 60 * 1000;  // set complete / gate closed
constexpr uint32_t kActiveSleepMs   = 4000;            // between download batches
constexpr uint32_t kFirstCheckMs    = 25000;           // let boot + WiFi settle first
constexpr uint32_t kBackoffBaseMs   = 60 * 1000;       // 1 -> 2 -> 4 ... max 16 min
constexpr int      kFilesPerWake    = 4;               // bounded work per wake
constexpr int      kPerFileRetries  = 3;
// Measured live 2026-07-11: this firmware rests at ~26-27 KB internal (audio-panel
// tone buffer + sfx tasks) and TLS runs fine there with PSRAM-backed mbedTLS
// (tool-loop rounds verified at 27-30 KB). Higher floors keep the gate
// permanently shut; the sync's own internal cost is a 1 KB stream buffer (the
// manifest body lives in PSRAM - an internal-heap String here OOM'd the device
// to heapMin=84 bytes, measured live).
constexpr uint32_t kHeapFloor       = 20000;           // don't sync under memory pressure

char     g_status[24] = "idle";
int      g_fails = 0;         // consecutive whole-pass failures (drives backoff)

void setStatus(const char* s) { strlcpy(g_status, s, sizeof(g_status)); }

// Rollover-safe "now is before `deadline`" (unsigned wrap at 49.7 days). The
// naive `millis() < deadline` gives a zero-length window right at the wrap.
bool before(uint32_t deadline) { return (int32_t)(millis() - deadline) < 0; }

bool gateOpen() {
  return agent::memory::haveSd() && !nimbus::fault::active(nimbus::fault::SD) &&
         WiFi.status() == WL_CONNECTED && ESP.getFreeHeap() > kHeapFloor;
}

// ---- HTTPS GET (HTTP/1.0 + close, arbiter-slotted) --------------------------
// Body bytes stream through `sink(data,len)`; returns the HTTP status code,
// 0 on transport failure, -1 when the arbiter slot is busy.
using ByteSink = bool (*)(const uint8_t*, size_t, void*);

int httpsGet(const char* path, ByteSink sink, void* ctx, int32_t* lenOut) {
  if (!agent::arbiter::acquireWork(10000)) return -1;
  WiFiClientSecure client;
  tlsSetup(client);
  client.setHandshakeTimeout(12);
  client.setConnectionTimeout(kHttpTimeoutMs);  // F25: real socket bound (setTimeout inert)
  bool connected = false;
  for (int a = 0; a < 2 && !connected; a++) {
    if (client.connect(SFX_SYNC_HOST, 443)) { connected = true; break; }
    tlsClose(client);
    if (a < 1) vTaskDelay(pdMS_TO_TICKS(400));
  }
  if (!connected) {
    agent::arbiter::releaseWork();
    return 0;
  }
  String req = String("GET ") + path + " HTTP/1.0\r\n"
             + "Host: " SFX_SYNC_HOST "\r\n"
             + "Connection: close\r\n\r\n";
  client.print(req);

  const uint32_t deadline = millis() + kHttpTimeoutMs;
  int code = 0;
  String status;
  while (before(deadline)) {
    if (client.available()) { char c = client.read(); if (c == '\n') break; if (c != '\r') status += c; }
    else if (!client.connected() && !client.available()) break;
    else delay(2);
  }
  int sp = status.indexOf(' ');
  if (sp > 0 && (int)status.length() >= sp + 4) code = status.substring(sp + 1, sp + 4).toInt();

  int32_t contentLen = -1;
  String line;
  bool headersDone = false;
  while (!headersDone && before(deadline)) {
    if (client.available()) {
      char c = client.read();
      if (c == '\n') {
        if (line.length() == 0) headersDone = true;
        else if (line.startsWith("Content-Length:") || line.startsWith("content-length:"))
          contentLen = line.substring(15).toInt();
        line = "";
      } else if (c != '\r') line += c;
    } else if (!client.connected() && !client.available()) break;
    else delay(2);
  }
  if (lenOut) *lenOut = contentLen;

  bool sinkOk = headersDone;
  if (headersDone && code == 200 && sink) {
    uint8_t buf[1024];
    while (before(deadline)) {
      int n = client.read(buf, sizeof(buf));
      if (n > 0) {
        if (!sink(buf, (size_t)n, ctx)) { sinkOk = false; break; }
      } else if (!client.connected() && !client.available()) {
        break;   // clean EOF (Connection: close)
      } else {
        delay(2);
      }
    }
  }
  tlsClose(client);
  agent::arbiter::releaseWork();
  if (!sinkOk) return 0;
  return code;
}

// ---- download one file to SD with streaming SHA-256 -------------------------
struct DlCtx {
  File                   f;
  mbedtls_sha256_context sha;
  size_t                 bytes = 0;
};

bool dlSink(const uint8_t* d, size_t n, void* vctx) {
  DlCtx* c = (DlCtx*)vctx;
  if (c->f.write(d, n) != n) return false;   // SD full / yanked - abort cleanly
  mbedtls_sha256_update(&c->sha, d, n);
  c->bytes += n;
  return true;
}

void hex64(const unsigned char* digest, char* out) {
  static const char* k = "0123456789abcdef";
  for (int i = 0; i < 32; i++) { out[i * 2] = k[digest[i] >> 4]; out[i * 2 + 1] = k[digest[i] & 0xF]; }
  out[64] = 0;
}

// Ensure every parent directory of `path` exists ("/sfx/general/x.wav" -> /sfx, /sfx/general).
void mkParents(const String& path) {
  for (int i = 1; i < (int)path.length(); i++) {
    if (path[i] == '/') SD.mkdir(path.substring(0, i));
  }
}

// Download <base>/<repoPath> to `local`, verify sha256+size, atomic rename.
bool fetchFile(const char* repoPath, const String& local, size_t wantBytes,
               const char* wantSha) {
  const String tmp = "/sfx/.tmp";
  mkParents(local);
  SD.remove(tmp);
  DlCtx ctx;
  ctx.f = SD.open(tmp, FILE_WRITE);
  if (!ctx.f) return false;
  mbedtls_sha256_init(&ctx.sha);
  mbedtls_sha256_starts(&ctx.sha, 0 /* SHA-256, not 224 */);
  String url = String(SFX_SYNC_BASE "/") + repoPath;
  int32_t contentLen = -1;
  const int code = httpsGet(url.c_str(), &dlSink, &ctx, &contentLen);
  ctx.f.close();
  unsigned char digest[32];
  mbedtls_sha256_finish(&ctx.sha, digest);
  mbedtls_sha256_free(&ctx.sha);
  char sha[65];
  hex64(digest, sha);
  const bool ok = code == 200 && ctx.bytes == wantBytes && strcmp(sha, wantSha) == 0;
  if (!ok) {
    agent::alogf("sfxsync: %s FAILED http=%d bytes=%u/%u", repoPath, code,
                 (unsigned)ctx.bytes, (unsigned)wantBytes);
    SD.remove(tmp);
    return false;
  }
  SD.remove(local);           // FAT rename won't overwrite
  return SD.rename(tmp, local);
}

// ---- manifest handling -------------------------------------------------------
// The manifest body accumulates in a fixed PSRAM buffer - NEVER an internal-heap
// String (20+ KB of realloc churn at a ~26 KB resting heap drove heapMin to 84
// bytes, measured live 2026-07-11).
struct PsBuf { char* p; size_t cap; size_t len; };
bool psSink(const uint8_t* d, size_t n, void* vctx) {
  PsBuf* b = (PsBuf*)vctx;
  if (b->len + n >= b->cap) return false;   // manifest sanity cap
  memcpy(b->p + b->len, d, n);
  b->len += n;
  return true;
}

// The SD path a manifest entry lands at ("sd/general/boot-0.wav" -> "/sfx/general/boot-0.wav").
String localPathFor(const char* repoPath) {
  return String("/sfx/") + (repoPath + 3);
}

// The manifest is fetched over setInsecure() TLS (owner-accepted, docs/security.md),
// so every field is hostile input. Path vetting (clean `sd/<...>` only, no
// "..", filesystem-safe chars, never the owner's `sd/custom/` pool) is the
// shared, host-tested nimbus::sfx::safeRepoPath (lib/core nimbus/sfx_paths.h).
// Anything unsafe is skipped, not written.

uint32_t localManifestVersion() {
  File f = SD.open("/sfx/manifest.json", FILE_READ);
  if (!f) return 0;
  JsonDocument filter;
  filter["version"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f, DeserializationOption::Filter(filter));
  f.close();
  return err ? 0 : doc["version"].as<uint32_t>();
}

// One full sync pass, bounded to kFilesPerWake downloads. Returns:
//   0 = set complete (verified)   1 = made progress / more to do   -1 = failure
int syncPass() {
  // 1) fetch the remote manifest into PSRAM (filter the parse to what we use).
  //    The guard frees the buffer on EVERY exit - an early `return` that forgot
  //    the free leaked 96 KB of PSRAM per mid-pass gate-close (WiFi/heap dip).
  constexpr size_t kManifestCap = 96 * 1024;
  std::unique_ptr<char, void (*)(void*)> bodyBuf(
      (char*)heap_caps_malloc(kManifestCap, MALLOC_CAP_SPIRAM), &free);
  if (!bodyBuf) return -1;   // no PSRAM: skip syncing entirely (basic tier stands)
  PsBuf body{bodyBuf.get(), kManifestCap, 0};
  const int code = httpsGet(SFX_SYNC_BASE "/manifest.json", &psSink, &body, nullptr);
  if (code != 200) {
    agent::alogf("sfxsync: manifest http=%d", code);
    return -1;
  }
  body.p[body.len] = 0;
  JsonDocument filter;
  filter["version"] = true;
  JsonObject fo = filter["files"].add<JsonObject>();
  fo["path"] = true; fo["bytes"] = true; fo["sha256"] = true;
  JsonDocument doc(&agent::PsramJsonAllocator::instance());
  const DeserializationError parseErr =
      deserializeJson(doc, (const char*)body.p, DeserializationOption::Filter(filter));
  if (parseErr) return -1;
  const uint32_t remoteVer = doc["version"].as<uint32_t>();

  // 2) diff: collect missing / size-mismatched SD-tier files.
  int total = 0, present = 0, fetched = 0;
  bool failedOne = false;
  for (JsonObjectConst f : doc["files"].as<JsonArrayConst>()) {
    const char* p = f["path"] | "";
    if (!nimbus::sfx::safeRepoPath(p)) continue;  // embedded tier / custom pool / hostile -> skip
    total++;
    const size_t want = f["bytes"] | 0;
    const String local = localPathFor(p);
    // Existing files are revalidated by SIZE, not re-hashed: a same-size but
    // content-changed clip is not re-fetched. Deliberate - a byte-identical
    // length across two distinct lossy-encoded WAVs is a non-event, the
    // download path already hash-verifies (fetchFile), and playback is
    // fail-soft; a full re-hash of ~120 files every pass would block the shared
    // sfx task. Set-level content changes still bump the manifest `version`.
    File ex = SD.open(local, FILE_READ);
    const bool have = ex && (size_t)ex.size() == want;
    if (ex) ex.close();
    if (have) { present++; continue; }
    if (fetched >= kFilesPerWake) continue;    // bounded work per wake
    if (!gateOpen()) return -1;                // conditions changed mid-pass (guard frees)
    snprintf(g_status, sizeof(g_status), "syncing %d/%d", present + fetched, total);
    bool ok = false;
    for (int a = 0; a < kPerFileRetries && !ok; a++) {
      ok = fetchFile(p, local, want, f["sha256"] | "");
      if (!ok) vTaskDelay(pdMS_TO_TICKS(1000u << a));
    }
    if (ok) fetched++;
    else failedOne = true;
  }

  if (present == total) {
    // 3) whole set verified on card: persist the manifest + refresh the player.
    if (localManifestVersion() != remoteVer) {
      File f = SD.open("/sfx/.tmp", FILE_WRITE);
      if (f) {
        f.write((const uint8_t*)body.p, body.len);
        f.close();
        SD.remove("/sfx/manifest.json");
        SD.rename("/sfx/.tmp", "/sfx/manifest.json");
      }
      ::sfx::rescan();
      ::sfx::fire(nimbus::sfx::Ev::SyncDone);
      agent::alogf("sfxsync: set complete (%d files, version %u)", total, (unsigned)remoteVer);
    }
    setStatus("full");
    return 0;
  }
  if (failedOne && fetched == 0) return -1;    // no progress at all this pass
  return 1;                                     // progress - come back soon
}

uint32_t g_nextDueMs = kFirstCheckMs;   // when the next step may run

}  // namespace

void tick() {
  const uint32_t now = millis();
  if ((int32_t)(now - g_nextDueMs) < 0) return;
  if (!gateOpen()) {
    // Keep a completed/errored badge visible; only a fresh device shows idle.
    if (strcmp(g_status, "full") != 0 && strcmp(g_status, "error") != 0)
      setStatus("idle");
    static uint32_t s_lastWhy = 0;
    if (now - s_lastWhy > 5 * 60 * 1000) {   // rate-limited diagnostics
      s_lastWhy = now;
      agent::alogf("sfxsync: gate closed (sd=%d fault=%d sta=%d heap=%u)",
                   (int)agent::memory::haveSd(),
                   (int)nimbus::fault::active(nimbus::fault::SD),
                   (int)(WiFi.status() == WL_CONNECTED),
                   (unsigned)ESP.getFreeHeap());
    }
    g_nextDueMs = now + kIdleSleepMs / 10;   // gate closed: re-check every minute
    return;
  }
  const int r = syncPass();
  if (r == 1) {
    g_fails = 0;
    g_nextDueMs = millis() + kActiveSleepMs;      // mid-sync: keep going briskly
  } else if (r == 0) {
    g_fails = 0;
    g_nextDueMs = millis() + kIdleSleepMs;        // complete: slow re-check cadence
  } else {
    if (++g_fails > 4) g_fails = 4;
    setStatus("error");
    g_nextDueMs = millis() + (kBackoffBaseMs << g_fails);   // 2 -> 16 min
  }
}

const char* statusStr() { return g_status; }

}  // namespace sfxsync
