#include "ota_update.h"

#include <Update.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <nvs.h>

#include <atomic>
#include <memory>
#include <solide/memory.h>

#include "../agent/agent_config.h"
#include "net_util.h"              // tlsSetup (CA bundle / setInsecure) + tlsClose
#include "../agent/store.h"
#include "tls_arbiter.h"
#include "nimbus/ota/ota_logic.h"
#include "ota_pubkey.h"
#include "version.h"

// Device glue over the portable nimbus::ota core. IO + wiring only - every
// decision (versions, manifest validity, eligibility, rollback, idle window)
// is host-tested in lib/core. Structure mirrors the house patterns:
// provider_verify's on-demand self-deleting task, sfx_sync's streamed GET with
// a ByteSink + streaming SHA-256, and the loops_subsystem injected hooks.

namespace otaupd {

using nimbus::ota::State;

#ifndef NIMBUS_OTA_VARIANT
#define NIMBUS_OTA_VARIANT ""
#endif
#ifndef SOLIDE_BOARD
#define SOLIDE_BOARD solide_s3
#endif

// Hard-bind the OTA variant string to the compile-time board pinout. The variant
// is the ONLY thing an OTA manifest matches against, and the image is signed by
// bytes - nothing at runtime re-checks that a "cyd" image was built for the
// Freenove pinout. So enforce it here at build time: a *cyd* variant iff the
// board is freenove_s3. This makes it impossible to accidentally publish a
// solide_s3 image under a cyd slot (or vice-versa) and drive the wrong pin map
// onto an all-in-one PCB. Add a board->variant family to the table when boards grow.
namespace {
constexpr bool ota_streq(const char* a, const char* b) {
  return (*a == *b) && (*a == '\0' || ota_streq(a + 1, b + 1));
}
constexpr bool ota_contains(const char* hay, const char* needle) {
  for (const char* h = hay; *h; ++h) {
    const char* a = h; const char* b = needle;
    while (*a && *b && *a == *b) { ++a; ++b; }
    if (*b == '\0') return true;
  }
  return false;
}
#define NIMBUS_OTA_STR2(x) #x
#define NIMBUS_OTA_STR(x) NIMBUS_OTA_STR2(x)
constexpr bool kBoardIsFreenove = ota_streq(NIMBUS_OTA_STR(SOLIDE_BOARD), "freenove_s3");
constexpr bool kVariantIsCyd    = ota_contains(NIMBUS_OTA_VARIANT, "cyd");
static_assert(NIMBUS_OTA_VARIANT[0] == '\0' || (kBoardIsFreenove == kVariantIsCyd),
              "OTA variant does not match SOLIDE_BOARD: freenove_s3 must use a "
              "*cyd* variant and solide_s3 must not, or a wrong-pinout image "
              "could be delivered to a board.");
}  // namespace

// ---- module state -----------------------------------------------------------

static State  g_state = (NIMBUS_OTA_VARIANT[0] ? State::Idle : State::Unsupported);
static volatile int g_progressPct = -1;
static String g_latestVersion;
static String g_latestNotes;
static char   g_lastError[24] = {0};     // short reason for state Error (fixed buffer:
                                         // lastError() hands out a stable pointer that a
                                         // cross-task reader can hold without a use-after
                                         // -free - a String's c_str() could dangle if the
                                         // OTA task reassigned it mid-read)
static bool   g_wifiEverUp = false;
static bool   g_markedValid = false;     // RAM latch: guard resolved this boot
// Whether the LAST check's fetch actually reached the release server (any HTTP
// status back, incl. 404/5xx) vs. a transport failure (DNS/TLS/timeout). Lets
// checkResult() tell "unreachable" from "reached but bad manifest" so a polled
// /api/ota/check always resolves to a definitive result and never hangs.
static bool   g_lastCheckReached = false;
// Single-flight claim for the ONE check/install task. ATOMIC because requestCheck/
// requestInstall are reachable from TWO tasks in production - the main loop's tick()
// (daily check + hourly auto-install) and the AsyncTCP web task (/api/ota/*). A plain
// bool test-then-set let both win the race and both xTaskCreate an installer writing
// the same OTA partition (2-slot arbiter admits both) -> corrupt flash. compare_
// exchange makes the claim indivisible.
static std::atomic<bool> g_taskRunning{false};
static void setLastError(const char* e) {
  snprintf(g_lastError, sizeof g_lastError, "%s", e ? e : "");
}
static nimbus::ota::ManifestInfo g_manifest;   // last good parse (install uses it)
static bool   g_haveManifest = false;

static EventHook g_hook = nullptr;
static IdleProvider g_idleProvider = nullptr;

#ifdef NIMBUS_TEST
static String g_urlOverride;             // RAM-only (OTAURL console cmd)
static const char* kSimCrashKey = "otaSimCrash";
#endif

static const char* manifestUrl() {
#ifdef NIMBUS_TEST
  if (g_urlOverride.length()) return g_urlOverride.c_str();
#endif
  return OTA_MANIFEST_URL;
}

static void fireEvent(int ev, const char* a, const char* b) {
  if (g_hook) g_hook(ev, a ? a : "", b ? b : "");
}

// ---- boot guard (raw NVS - runs before solide::begin, see header) -----------

static const char* kNs = "solide";

static int32_t rawGetI32(nvs_handle_t h, const char* k, int32_t def) {
  int32_t v = def;
  if (nvs_get_i32(h, k, &v) != ESP_OK) return def;
  return v;
}
static bool rawGetStr(nvs_handle_t h, const char* k, char* buf, size_t cap) {
  size_t len = cap;
  if (nvs_get_str(h, k, buf, &len) != ESP_OK) { buf[0] = '\0'; return false; }
  return true;
}

static const char* runningLabel() {
  const esp_partition_t* p = esp_ota_get_running_partition();
  return p ? p->label : "?";
}

// The device TYPE the typed (schema 2) manifest is matched against. Prefer the
// NVS otaType seeded by the flasher or written by the transition boot; fall back
// to the compile-time build tag so dev + HIL builds (test / test-cyd, which never
// get an otaType) keep resolving. A stored type from the wrong board family is
// refused (resolves to "") so a misseeded NVS can never pull a wrong-pinout image,
// the runtime twin of the compile-time board<->variant static_assert above.
// Resolved once and cached: NVS is stable for the life of a boot and the OTA path
// runs well after nvs init.
static bool nvsGetStr(const char* key, char* buf, size_t cap) {
  nvs_handle_t h;
  buf[0] = '\0';
  if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = cap;
  bool ok = nvs_get_str(h, key, buf, &len) == ESP_OK;
  if (!ok) buf[0] = '\0';
  nvs_close(h);
  return ok;
}

static const char* otaVariant() {
  static char cached[24];
  static bool resolved = false;
  if (resolved) return cached;
  char t[24] = {0};
  nvsGetStr(AKEY_OTA_TYPE, t, sizeof t);
#ifndef NIMBUS_TEST
  if (!t[0]) {
    // Transition boot: a fielded production device that carries no type yet.
    // Derive it once from stored hardware identity and persist it, so a TFT board
    // starts matching "nimbus-tft" (an untyped fallback to the build tag would
    // never match the typed manifest) and e-ink stays untyped -> frozen.
    char scr[8] = {0};
    nvsGetStr(AKEY_SCREEN_MODEL, scr, sizeof scr);
    char derived[24];
    if (nimbus::ota::deriveDeviceType(derived, sizeof derived, kBoardIsFreenove,
                                      strcmp(scr, "tft") == 0)) {
      nvs_handle_t h;
      if (nvs_open(kNs, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, AKEY_OTA_TYPE, derived);
        nvs_commit(h);
        nvs_close(h);
      }
      snprintf(t, sizeof t, "%s", derived);
    }
    // e-ink derives to "" -> left unset -> untyped -> no update (correct).
  }
#endif
  if (t[0] && nimbus::ota::typeAllowedForBoard(t, kBoardIsFreenove))
    snprintf(cached, sizeof cached, "%s", t);
  else if (t[0])
    cached[0] = '\0';                       // misseeded -> untyped -> no update
  else
    snprintf(cached, sizeof cached, "%s", NIMBUS_OTA_VARIANT);  // dev/HIL fallback
  resolved = true;
  return cached;
}

void bootGuard() {
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;  // no NVS: nothing to guard

  if (!rawGetI32(h, AKEY_OTA_PENDING, 0)) {
    g_markedValid = true;  // nothing pending - this boot needs no validation
    // CUM-197: clear a stale "last result". If the record names a different
    // version than the one now running, the current image arrived by some path
    // other than the OTA that wrote it (e.g. a later dev/USB flash), so showing
    // "ok v4.2.0" on a v4.3.0 device is misleading. The next OTA writes a fresh,
    // accurate result. Only touched when there is nothing pending (rollback /
    // abort paths below record their own accurate outcome).
    char lr[40] = {0};
    rawGetStr(h, AKEY_OTA_LASTRES, lr, sizeof lr);
    if (nimbus::ota::lastResultStale(lr, NIMBUS_FW_VERSION)) {
      nvs_set_str(h, AKEY_OTA_LASTRES, "");
      nvs_commit(h);
    }
    nvs_close(h);
    return;
  }

  char prev[17] = {0};
  rawGetStr(h, AKEY_OTA_PREV, prev, sizeof prev);
  if (prev[0] && strcmp(prev, runningLabel()) == 0) {
    // Still on the OLD slot: power died between the NVS-pend write and the
    // otadata flip (or the flip failed). The update never happened - disarm.
    nvs_set_i32(h, AKEY_OTA_PENDING, 0);
    nvs_set_i32(h, AKEY_OTA_BOOTS, 0);
    nvs_set_str(h, AKEY_OTA_LASTRES, "aborted-preflip");
#ifdef NIMBUS_TEST
    nvs_set_i32(h, kSimCrashKey, 0);
#endif
    nvs_commit(h);
    nvs_close(h);
    g_markedValid = true;
    return;
  }

  // Fresh image, unvalidated: burn one attempt BEFORE any risky bring-up.
  int32_t boots = rawGetI32(h, AKEY_OTA_BOOTS, 0) + 1;
  nvs_set_i32(h, AKEY_OTA_BOOTS, boots);
  nvs_commit(h);

  if (nimbus::ota::shouldRollback(true, (uint8_t)boots)) {
    const esp_partition_t* p =
        prev[0] ? esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                           ESP_PARTITION_SUBTYPE_ANY, prev)
                : nullptr;
    nvs_set_i32(h, AKEY_OTA_PENDING, 0);
    nvs_set_i32(h, AKEY_OTA_BOOTS, 0);
#ifdef NIMBUS_TEST
    nvs_set_i32(h, kSimCrashKey, 0);
#endif
    if (p && esp_ota_set_boot_partition(p) == ESP_OK) {
      nvs_set_str(h, AKEY_OTA_LASTRES, "rollback " NIMBUS_FW_VERSION);
      nvs_commit(h);
      nvs_close(h);
      esp_restart();  // boots the previous, untouched image
    }
    // Previous slot unfindable (label wiped?) - nothing to flip to; keep
    // running this image rather than reboot-looping.
    nvs_set_str(h, AKEY_OTA_LASTRES, "rollback-lost");
    nvs_commit(h);
    nvs_close(h);
    g_markedValid = true;
    return;
  }

#ifdef NIMBUS_TEST
  if (rawGetI32(h, kSimCrashKey, 0)) {
    // Synthetic bad image for the HIL rollback drill: die before mark-valid.
    // The NEXT boot re-enters bootGuard and burns another attempt.
    nvs_close(h);
    abort();
  }
#endif
  nvs_close(h);
  // Pending, attempts remain: run normally; tick() marks valid once healthy.
}

// ---- HTTPS GET with cross-host redirect follow ------------------------------
// GitHub release-asset URLs 302 to objects.githubusercontent.com (sometimes via
// the versioned URL first), so unlike sfx_sync's fixed-host httpsGet this one
// parses full URLs and follows Location. HTTP/1.0 + Connection: close (no
// chunked encoding); each hop is a fresh TLS connection through tlsSetup().

using ByteSink = bool (*)(const uint8_t*, size_t, void*);

static bool before(uint32_t deadline) { return (int32_t)(millis() - deadline) < 0; }

struct UrlParts {
  String host;
  uint16_t port = 443;
  String path;
};

static bool splitUrl(const String& url, UrlParts& out) {
  if (!url.startsWith("https://")) return false;
  int hostStart = 8;
  int slash = url.indexOf('/', hostStart);
  String hostPort = (slash < 0) ? url.substring(hostStart) : url.substring(hostStart, slash);
  out.path = (slash < 0) ? "/" : url.substring(slash);
  int colon = hostPort.indexOf(':');
  if (colon >= 0) {
    out.host = hostPort.substring(0, colon);
    out.port = (uint16_t)hostPort.substring(colon + 1).toInt();
    if (!out.port) return false;
  } else {
    out.host = hostPort;
    out.port = 443;
  }
  return out.host.length() > 0;
}

// One GET; if the server answers 30x, *redirect gets the Location and the call
// returns -2. Returns the final HTTP code, 0 on transport error.
static int httpsGetOnce(const UrlParts& u, ByteSink sink, void* ctx,
                        uint32_t deadline, String* redirect) {
  WiFiClientSecure client;
  tlsSetup(client);
  client.setHandshakeTimeout(12);
  client.setConnectionTimeout(15000);  // F25: real socket bound (setTimeout inert)
  bool connected = false;
  for (int a = 0; a < 2 && !connected; a++) {
    if (client.connect(u.host.c_str(), u.port)) { connected = true; break; }
    tlsClose(client);
    if (a < 1) vTaskDelay(pdMS_TO_TICKS(400));
  }
  if (!connected) return 0;

  String req = String("GET ") + u.path + " HTTP/1.0\r\n" +
               "Host: " + u.host + "\r\n" +
               "User-Agent: nimbus-ota/" NIMBUS_FW_VERSION "\r\n" +
               "Connection: close\r\n\r\n";
  client.print(req);

  // Status line.
  int code = 0;
  String line;
  while (before(deadline)) {
    if (client.available()) {
      char c = client.read();
      if (c == '\n') break;
      if (c != '\r') line += c;
    } else if (!client.connected() && !client.available()) break;
    else delay(2);
  }
  int sp = line.indexOf(' ');
  if (sp > 0 && (int)line.length() >= sp + 4)
    code = line.substring(sp + 1, sp + 4).toInt();

  // Headers: Location for redirects (Content-Length informational only - the
  // manifest's size field is the authoritative byte count for the firmware).
  String location;
  line = "";
  bool headersDone = false;
  while (!headersDone && before(deadline)) {
    if (client.available()) {
      char c = client.read();
      if (c == '\n') {
        if (line.length() == 0) headersDone = true;
        else {
          String low = line; low.toLowerCase();
          if (low.startsWith("location:")) {
            location = line.substring(9);
            location.trim();
          }
        }
        line = "";
      } else if (c != '\r') line += c;
    } else if (!client.connected() && !client.available()) break;
    else delay(2);
  }
  if (!headersDone) { tlsClose(client); return 0; }

  if (code >= 300 && code < 400 && location.length()) {
    tlsClose(client);
    if (redirect) {
      if (location.startsWith("https://")) *redirect = location;
      else if (location.startsWith("/"))   *redirect = "https://" + u.host + location;
      else return 0;  // relative-path redirects don't occur here; refuse
    }
    return -2;
  }

  bool sinkOk = true;
  if (code == 200 && sink) {
    uint8_t buf[1024];
    while (before(deadline)) {
      int n = client.read(buf, sizeof(buf));
      if (n > 0) {
        if (!sink(buf, (size_t)n, ctx)) { sinkOk = false; break; }
      } else if (!client.connected() && !client.available()) {
        break;  // clean EOF (Connection: close)
      } else {
        delay(2);
      }
    }
  }
  tlsClose(client);
  return sinkOk ? code : 0;
}

static int httpsGetUrl(const String& url, ByteSink sink, void* ctx,
                       uint32_t timeoutMs, int maxRedirects = 4) {
  const uint32_t deadline = millis() + timeoutMs;
  String cur = url;
  for (int hop = 0; hop <= maxRedirects; hop++) {
    UrlParts u;
    if (!splitUrl(cur, u)) return 0;
    String redirect;
    int code = httpsGetOnce(u, sink, ctx, deadline, &redirect);
    if (code == -2) { cur = redirect; continue; }
    return code;
  }
  return 0;  // redirect loop
}

// ---- task-spawn gates -------------------------------------------------------
// Internal heap only (the ~266 KB pool): the 10 KB task stack + lwIP transients
// must fit with margin above the ~24 KB TLS/lwIP danger zone. mbedTLS's big
// buffers are PSRAM-routed (main.cpp), so these floors are the honest cost.

static const uint32_t kSpawnFreeInternal = 24000;
static const uint32_t kSpawnLargestBlock = 16000;

// Non-claim gates only (variant/wifi/heap). The busy check is the atomic
// g_taskRunning claim in requestCheck/requestInstall, done BEFORE these.
static bool spawnGates(const char** why) {
  if (!NIMBUS_OTA_VARIANT[0]) { *why = "unsupported"; return false; }
  if (WiFi.status() != WL_CONNECTED) { *why = "no-wifi"; return false; }
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kSpawnFreeInternal ||
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kSpawnLargestBlock) {
    *why = "low-heap";
    return false;
  }
  *why = "";
  return true;
}

// ---- manifest fetch (shared by check + install) -----------------------------

struct PsBuf {
  char*  p = nullptr;
  size_t cap = 0;
  size_t len = 0;
};
static bool psSink(const uint8_t* d, size_t n, void* ctx) {
  PsBuf* b = (PsBuf*)ctx;
  if (b->len + n > b->cap) return false;  // oversized manifest - refuse
  memcpy(b->p + b->len, d, n);
  b->len += n;
  return true;
}

static const size_t kManifestCap = 8 * 1024;

static bool verifySignature(const nimbus::ota::ManifestInfo& m);  // fwd

// Fetch + parse + AUTHENTICATE into g_manifest. Returns nullptr on success, else
// a static reason ("fetch"/"schema"/"variant"/"sig-fail"/...). Caller holds the
// arbiter slot. The ECDSA check runs HERE (over the manifest's signed version+
// variant+sha) so an unauthentic manifest can never drive "available"/the owner
// notification/auto-install - not just the final install (closes the spoofed-
// notification + downgrade-arming gap). The download's actual sha is separately
// checked against this signed sha at install time.
static const char* fetchManifest() {
  PsBuf b;
  b.p = (char*)heap_caps_malloc(kManifestCap, MALLOC_CAP_SPIRAM);
  if (!b.p) b.p = (char*)malloc(kManifestCap);
  if (!b.p) return "alloc";
  b.cap = kManifestCap;
  std::unique_ptr<char, decltype(&free)> guard(b.p, &free);

  int code = httpsGetUrl(manifestUrl(), psSink, &b, 30000);
  g_lastCheckReached = (code > 0);   // any HTTP status back == server reached
  // 404 = no release / no manifest asset published yet - NOT an error, just
  // "nothing to update" (the common state before the first release exists). A
  // real transport failure (0) or 5xx stays an error worth retrying/surfacing.
  if (code == 404) return "no-release";
  if (code != 200 || b.len == 0) return "fetch";

  const char* err = nullptr;
  nimbus::ota::ManifestInfo m;
  if (!nimbus::ota::parseManifest(b.p, b.len, otaVariant(), m, &err))
    return err ? err : "schema";
  if (!verifySignature(m)) return "sig-fail";
  g_manifest = m;
  g_haveManifest = true;
  return nullptr;
}

// ---- check task -------------------------------------------------------------

static void finishCheck() {
  using nimbus::ota::Eligibility;
  Eligibility e = nimbus::ota::eligibility(NIMBUS_FW_VERSION, g_manifest);
  g_latestVersion = g_manifest.version;
  g_latestNotes = g_manifest.notes;
  if (e == Eligibility::Newer || e == Eligibility::BlockedMinVersion) {
    g_state = State::Available;
    // Owner notify (Telegram in Orch mode), once per version.
    if (agent::store::otaNotifiedVersion() != g_latestVersion) {
      agent::store::setOtaNotifiedVersion(g_latestVersion);
      fireEvent(EvAvailable, g_manifest.version, g_manifest.notes);
    }
  } else {
    g_state = State::UpToDate;
  }
}

static void checkTask(void*) {
  g_state = State::Checking;
  g_lastCheckReached = false;   // set true iff fetchManifest reaches the server
  const char* err = "arbiter";
  if (agent::arbiter::acquireWork(10000)) {
    err = fetchManifest();
    agent::arbiter::releaseWork();
  }
  if (err && (strcmp(err, "no-release") == 0 || strcmp(err, "variant") == 0)) {
    // no-release: nothing published yet. variant: this device TYPE has no image
    // in the typed manifest (untyped/e-ink, or a family with no build). Both mean
    // "no update for this device" - a settled state, never a scary Error.
    g_state = State::UpToDate;
  } else if (err) {
    setLastError(err);
    g_state = State::Error;
  } else {
    finishCheck();
  }
  g_taskRunning = false;
  vTaskDelete(nullptr);
}

bool requestCheck(const char** whyOut) {
  const char* why = "";
  bool expected = false;
  if (!g_taskRunning.compare_exchange_strong(expected, true)) {
    if (whyOut) *whyOut = "busy";   // an OTA task is already running (single-flight)
    return false;
  }
  if (!spawnGates(&why)) {          // "unsupported" / "no-wifi" / "low-heap"
    g_taskRunning = false;
    if (whyOut) *whyOut = why;
    return false;
  }
  if (!nimbus::ota::canCheck(g_state)) {   // a check/install is mid-flight
    g_taskRunning = false;
    if (whyOut) *whyOut = "in-progress";
    return false;
  }
  if (xTaskCreate(checkTask, "otacheck", 16384, nullptr, 1, nullptr) != pdPASS) {
    g_taskRunning = false;
    if (whyOut) *whyOut = "low-heap";
    return false;
  }
  if (whyOut) *whyOut = "";
  return true;
}

// ---- install task -----------------------------------------------------------

struct DlCtx {
  mbedtls_sha256_context sha;
  size_t bytes = 0;
  size_t total = 0;
  bool   writeFail = false;
};

static bool dlSink(const uint8_t* d, size_t n, void* ctx) {
  DlCtx* c = (DlCtx*)ctx;
  if (Update.write((uint8_t*)d, n) != n) { c->writeFail = true; return false; }
  mbedtls_sha256_update(&c->sha, d, n);
  c->bytes += n;
  if (c->total) {
    int pct = (int)((uint64_t)c->bytes * 100 / c->total);
    g_progressPct = pct > 100 ? 100 : pct;
  }
  return true;
}

// Verify the ECDSA signature over the canonical message against the trust
// anchors (include/ota_pubkey.h). mbedTLS's allocations ride the PSRAM-backed
// allocator main.cpp installs.
static bool verifySignature(const nimbus::ota::ManifestInfo& m) {
  char msg[192];
  size_t n = nimbus::ota::buildSigMessage(msg, sizeof msg, m.version,
                                          otaVariant(), m.v.shaHex);
  if (!n) return false;
  uint8_t hash[32];
  mbedtls_sha256((const unsigned char*)msg, n, hash, 0);
  for (int i = 0; i < kOtaPubKeyCount; i++) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_key(&pk, (const uint8_t*)kOtaPubKeys[i],
                                         strlen(kOtaPubKeys[i]) + 1);
    if (rc == 0)
      rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash,
                             m.v.sig, m.v.sigLen);
    mbedtls_pk_free(&pk);
    if (rc == 0) return true;
  }
  return false;
}

static bool g_installDry = false;
static bool g_installForce = false;

static void failInstall(const char* why) {
  if (Update.isRunning()) Update.abort();
  g_progressPct = -1;
  setLastError(why);
  g_state = State::Error;
  agent::store::setOtaLastResult(String(why) + " " +
                                 (g_haveManifest ? g_manifest.version : "?"));
  fireEvent(EvInstallFail, why, g_haveManifest ? g_manifest.version : "");
}

static void installTask(void*) {
  fireEvent(EvInstallStart, g_installDry ? "dry" : "real", "");
  g_state = State::Checking;

  const char* err = "arbiter";
  if (!agent::arbiter::acquireWork(10000)) {
    failInstall("arbiter");
    g_taskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  // Freshness: re-fetch the manifest so a just-published release supersedes a
  // stale Available state, then stream the binary into the inactive slot.
  err = fetchManifest();
  if (err) {
    agent::arbiter::releaseWork();
    failInstall(err);
    g_taskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  // Re-assert eligibility on the RE-FETCHED manifest for non-force installs.
  // Without this a MITM could swap the just-approved release for an authentically
  // -signed OLDER one between check and install (both pass sha+sig) to force a
  // downgrade - the "auto == strictly newer" guarantee must hold on the manifest
  // we actually install, not the one the check saw. force (authenticated web UI)
  // still allows an explicit downgrade.
  if (!g_installForce &&
      !nimbus::ota::autoEligible(
          nimbus::ota::eligibility(NIMBUS_FW_VERSION, g_manifest))) {
    agent::arbiter::releaseWork();
    failInstall("not-newer");
    g_taskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  g_state = State::Downloading;
  g_progressPct = 0;

  DlCtx ctx;
  ctx.total = g_manifest.v.size;
  mbedtls_sha256_init(&ctx.sha);
  mbedtls_sha256_starts(&ctx.sha, 0);

  bool ok = Update.begin(g_manifest.v.size, U_FLASH);
  int code = 0;
  if (ok) {
    // ~3 MB at WiFi pace: allow up to 8 minutes before declaring a stall.
    code = httpsGetUrl(String(g_manifest.v.url), dlSink, &ctx, 480000);
  }
  agent::arbiter::releaseWork();

  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx.sha, digest);
  mbedtls_sha256_free(&ctx.sha);

  if (!ok) { failInstall("slot"); g_taskRunning = false; vTaskDelete(nullptr); return; }
  if (ctx.writeFail) { failInstall("flash-write"); g_taskRunning = false; vTaskDelete(nullptr); return; }
  if (code != 200 || ctx.bytes != g_manifest.v.size) {
    failInstall("download");
    g_taskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  g_state = State::Verifying;
  if (memcmp(digest, g_manifest.v.sha256, 32) != 0) {
    failInstall("sha-fail");
    g_taskRunning = false;
    vTaskDelete(nullptr);
    return;
  }
  if (!verifySignature(g_manifest)) {
    failInstall("sig-fail");
    g_taskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  if (g_installDry) {
    Update.abort();
    g_progressPct = -1;
    agent::store::setOtaLastResult(String("dryrun ok ") + g_manifest.version);
    g_state = State::Available;  // verified - still installable for real
    g_taskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  // COMMIT ORDER (do not reorder - the rollback guard depends on it):
  // 1. arm the NVS guard while still running the OLD image;
  // 2. Update.end(true) validates the image header and flips otadata;
  // 3. restart. Power loss after (1) but before (2) is the "aborted-preflip"
  // path bootGuard disarms; after (2) the guard is already armed.
  agent::store::setOtaPrevSlot(runningLabel());
  agent::store::setOtaBootCount(0);
  agent::store::setOtaPending(1);
  agent::store::setOtaLastResult(String("installing ") + g_manifest.version);
  // Persist the release notes ACROSS the reboot this install causes - the RAM
  // copy (g_latestNotes) dies with it, and post-update is exactly when the owner
  // (and the model) ask "what changed?". Consumed + cleared at EvValidated.
  agent::store::setOtaPendingNotes(String(g_manifest.version) + "|" + g_manifest.notes);

  if (!Update.end()) {
    // Flip failed - disarm the guard, keep running the old image.
    agent::store::setOtaPending(0);
    failInstall("commit");
    g_taskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  g_state = State::ReadyToReboot;
  g_progressPct = 100;
  fireEvent(EvRebooting, g_manifest.version, "");
  // Give the main loop time to paint the e-ink "restarting" frame (~2.2 s panel).
  vTaskDelay(pdMS_TO_TICKS(3500));
  esp_restart();
}

bool requestInstall(bool dryRun, bool force, const char** whyOut) {
  // Atomic single-flight claim FIRST, so a web-UI install and the main-loop
  // auto-install can't both pass the gates and both write the OTA slot. Release
  // it on every refusal path below.
  bool expected = false;
  if (!g_taskRunning.compare_exchange_strong(expected, true)) {
    if (whyOut) *whyOut = "busy";
    return false;
  }
  auto refuse = [&](const char* w) { if (whyOut) *whyOut = w; g_taskRunning = false; return false; };
  const char* why;
  if (!spawnGates(&why)) return refuse(why);
  if (!nimbus::ota::canInstall(g_state)) {
    // force allows Same/Older/minVersion-blocked installs, but only from a
    // settled state with a manifest already seen (check first).
    if (!(force && g_haveManifest &&
          (g_state == State::UpToDate || g_state == State::Available)))
      return refuse("not-available");
  }
  if (g_haveManifest && !force) {
    using nimbus::ota::Eligibility;
    Eligibility e = nimbus::ota::eligibility(NIMBUS_FW_VERSION, g_manifest);
    if (e != Eligibility::Newer)
      return refuse((e == Eligibility::BlockedMinVersion) ? "min-version" : "not-newer");
  }
  // Battery / health gate (manual path): an interrupted write on a dying pack
  // can brick a slot. force bypasses it - that is the "Install anyway, I am
  // charging" escape hatch the UI offers, since the board has no VBUS sense.
  if (!force && g_idleProvider) {
    nimbus::ota::IdleSnapshot snap;
    if (g_idleProvider(snap)) {
      nimbus::ota::InstallGateInput gi;
      gi.battMonEnabled  = snap.battMonEnabled;
      gi.onExternalPower = snap.onExternalPower;
      gi.battPct         = snap.battPct;
      gi.healthPct       = snap.healthPct;
      nimbus::ota::InstallGate g = nimbus::ota::installGate(gi);
      if (g != nimbus::ota::InstallGate::Allowed)
        return refuse(nimbus::ota::installGateStr(g));  // need-power / need-recalibrate
    }
  }
  g_installDry = dryRun;
  g_installForce = force;
  if (xTaskCreate(installTask, "otainstall", 16384, nullptr, 1, nullptr) != pdPASS) {
    g_taskRunning = false;
    if (whyOut) *whyOut = "task";
    return false;
  }
  if (whyOut) *whyOut = "";
  return true;
}

// ---- mark-valid + scheduling ------------------------------------------------

static void markValid() {
  using namespace agent;
  store::setOtaPending(0);
  store::setOtaBootCount(0);
  store::setOtaLastResult(String("ok ") + NIMBUS_FW_VERSION);
  g_markedValid = true;
  fireEvent(EvValidated, NIMBUS_FW_VERSION, "");
}

static const uint32_t kFirstCheckMs = 2 * 60 * 1000;      // settle first
static const uint32_t kCheckEveryMs = 24 * 60 * 60 * 1000;  // daily
static const uint32_t kAutoEveryMs = 60 * 60 * 1000;      // idle-window retry

void tick() {
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < 1000) return;  // ~1 Hz
  lastMs = now;

  if (WiFi.status() == WL_CONNECTED) g_wifiEverUp = true;

  if (!g_markedValid && agent::store::otaPending() &&
      nimbus::ota::bootHealthy(now / 1000, g_wifiEverUp)) {
    markValid();
  }

#ifndef NIMBUS_TEST
  // Background check cadence: first at ~2 min, then daily. Silent on failure
  // (next cadence retries); requestCheck's gates keep it cheap.
  //
  // Test builds never poll or auto-install on their own: their trust anchor
  // includes the COMMITTED test keypair (test/ota_test_key.pem), so anyone can
  // sign a manifest a test build accepts. Explicit console drills (OTACHECK /
  // OTAAPPLY / OTASIM, plus OTAURL to point at a bench server) still work -
  // an operator at the console is the authorization.
  static uint32_t nextCheckMs = kFirstCheckMs;
  if (NIMBUS_OTA_VARIANT[0] && now >= nextCheckMs) {
    nextCheckMs = now + kCheckEveryMs;
    if (nimbus::ota::canCheck(g_state)) requestCheck();
  }

  // Auto-install (owner opt-in): hourly, only in a genuinely idle window.
  static uint32_t nextAutoMs = kAutoEveryMs;
  if (agent::store::otaAutoUpdate() && g_state == State::Available &&
      now >= nextAutoMs) {
    nextAutoMs = now + kAutoEveryMs;
    nimbus::ota::IdleSnapshot snap;
    if (g_idleProvider && g_idleProvider(snap) &&
        nimbus::ota::autoInstallAllowed(snap) && g_haveManifest &&
        nimbus::ota::autoEligible(
            nimbus::ota::eligibility(NIMBUS_FW_VERSION, g_manifest))) {
      requestInstall(false, false, nullptr);
    }
  }
#endif  // NIMBUS_TEST
}

// ---- accessors / hooks ------------------------------------------------------

void setEventHook(EventHook h) { g_hook = h; }
void setIdleProvider(IdleProvider p) { g_idleProvider = p; }

bool installing() {
  return g_state == State::Downloading || g_state == State::Verifying ||
         g_state == State::ReadyToReboot;
}

const char* statusStr() { return nimbus::ota::stateStr(g_state); }
const char* checkResultStr() {
  return nimbus::ota::checkResultStr(
      nimbus::ota::checkResult(g_state, g_lastCheckReached));
}
const char* lastError() { return g_lastError; }
int progressPct() { return g_progressPct; }
String latestSeen() { return g_latestVersion; }
String latestNotes() { return g_latestNotes; }
String lastResult() { return agent::store::otaLastResult(); }
const char* runningSlot() { return runningLabel(); }  // "app0"/"app1" - flip observability

// ---- HIL seams --------------------------------------------------------------

#ifdef NIMBUS_TEST
void simArm(const String& prevLabel) {
  agent::store::setOtaPending(1);
  agent::store::setOtaBootCount(0);
  agent::store::setOtaPrevSlot(prevLabel);
  agent::store::setOtaLastResult(String("sim-arm ") + prevLabel);
  g_markedValid = false;  // let tick() exercise the real mark-valid path
}
void simCrash(bool on) { solide::memory::setInt(kSimCrashKey, on ? 1 : 0); }
void simClear() {
  agent::store::setOtaPending(0);
  agent::store::setOtaBootCount(0);
  agent::store::setOtaPrevSlot("");
  solide::memory::setInt(kSimCrashKey, 0);
}
void setManifestUrl(const String& url) { g_urlOverride = url; }
#endif

}  // namespace otaupd
