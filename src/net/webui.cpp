// webui - see webui.h. Adapted from Nuage-Solide src/webui.cpp. Originally
// gutted to a config-only surface; the Orchestrator control surface (provider
// keys + verify gating + routing + memory panel) was re-added 2026-07 (plan
// ROUND 3 Part A), ported from Nuage's SETTINGS_HTML + /verify + /status.
//
// Orchestrator config threading: unlike the Config overrides (staged, because
// nimbus::Config is not thread-safe), the orchestrator settings are NVS-backed
// (agent::store) and NVS is internally mutexed - so the /api/orch handler
// reads/writes the store directly on the AsyncTCP task, exactly as Nuage's
// webui did. Live orchestrator state (model memory, jobs) crosses tasks only
// through the crash-safe accessors in agent/orchestrator.h.
//
// SECURITY: API keys and the Telegram token are WRITE-ONLY on this surface -
// /api/orch reports presence booleans, never echoes a secret. Every /api route
// (GET and POST) is gated on the per-device web token (webAuthOk); the only
// pre-auth surfaces are the static shell "/", "/logo.svg", and (unprovisioned
// only) the AP token handout + first-run /savewifi.
#include "webui.h"
#include "nimbus/orch/connectors_wire.h"   // /api/test/connwire grid seam

#include "version.h"  // NIMBUS_FW_VERSION/BUILD -> /api/state fw/build
#include "wifi_portal.h"
#include "wifi_store.h"   // /api/wifi - the known-networks list (the stored password
                          // is read here ONLY to hand it back to the radio, never out)
#include "web_pages.h"
#include "web/ui_logo.h"   // the dotted-ring mark served at GET /logo.svg
#include "nimbus_config.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Esp.h>
#include <esp_heap_caps.h>  // heap_caps breakdown for /api/state mem{} (docs/memory-model.md)
#include <LittleFS.h>              // fs free/used for the memory panel
#include <solide/storage.h>        // SD card free/total (orch data store)
#include <solide/memory.h>
#include <solide/audio.h>          // mic/speaker diagnostics (VU meter, beep, loopback)
#include <math.h>                  // sqrtf/sinf for the audio diagnostics
#include "nimbus/fault.h"
#include "nimbus/orch/budget.h"    // deriveBudget - "auto (currently N)" effective caps
#include "nimbus/orch/compact.h"   // modelCtxTokens (window table)
#include "nimbus/power/bright_cap.h"          // resilience: simulated mic/speaker faults
#include "nimbus/power/power_monitor.h"       // battery chemistry + custom curve parse (config)

#include "../agent/agent_config.h"
#include "../agent/memory_subsystem.h"
#include "../sys/config_nvs.h"          // device identity (devName, P2)
#include "../hw/tft_out.h"                     // /api/screenshot - the bytes on the glass
#include <solide/display_tft.h>                 // ...and the panel geometry it advertises
#include <solide/board.h>                       // board identity + panel-fixed capability
#include "nimbus/touch_cal.h"                 // tchCal validation (shared with the console)
#include "solide/touch.h"                     // live-apply a new calibration
#include "../sys/ota_update.h"          // /api/ota/* + /api/state ota fields
#include "../agent/health.h"            // /api/health - device self-diagnosis (P5)
#include "../agent/telegram.h"          // /api/telegram - allowlist + approval (P8)
#include "ble_notifier.h"               // /api/health - BLE status (Notifier mode)
#include "nimbus/theme.h"
#include "nimbus/status_style.h"  // /api/themes roles[] - the legend single source               // /api/themes - palette source of truth
#include "../sfx/sound_fx.h"
#include "../sfx/sfx_sync.h"
#include "../hw/selftest.h"        // /api/selftest - device health-check engine
#include "../agent/orchestrator.h"
#include "../agent/loops_subsystem.h"   // /api/loops -> Local Loops
#include "../agent/adapters/tts_voices.h"
#include "../agent/provider_verify.h"
#include "../agent/store.h"
#include "relay_client.h"                // cloud tunnel status + control
#include "../agent/connectors.h"           // /api/connectors - known catalog + host
#include "web_memory.h"
#include "web_files.h"                   // E1: /api/files* artifact-store routes
#include "web_skills.h"                  // P2: /api/skills* dynamic-skill CRUD
#include "../agent/files_subsystem.h"      // E1: files::stats for /api/state

using nimbus::Param;
using nimbus::ProfileId;

namespace nimbus::net {

static AsyncWebServer s_server(80);
static WebConfig      s_wc;

// Is `id` on the Telegram allowlist RIGHT NOW? Reads NVS rather than
// telegram::isAllowed(), which serves the poll task's cached g_allowlist and so
// lags a just-added chat by up to a ~50 s long-poll. Both callers need the fresh
// answer to stay fail-closed: promoting an id the UI already shows allow-listed,
// and refusing to delete a tenant row whose absence would silently re-grant
// access.
static bool onFreshAllowlist(const String& id) {
  const String al = agent::store::telegramAllowlist();
  for (int s = 0; s < (int)al.length();) {
    int e = al.indexOf(',', s); if (e < 0) e = al.length();
    String t = al.substring(s, e); t.trim();
    if (t.length() && t == id) return true;
    s = e + 1;
  }
  return false;
}

// Repeated 401s -> show the Config QR on the panel (P3). Bumped by webAuthOk on
// the AsyncTCP task (single aligned writes); the QR re-show guard is main-task
// state inside consumeAuthQrRequest().
static volatile uint8_t  s_authFails       = 0;
static volatile uint32_t s_authFirstFailMs = 0;

// Per-device web auth (prism): a state-changing request must carry the device token
// (X-Nimbus-Token header OR ?t= param), constant-time compared vs store::webAuthToken().
// The owner obtains it via the Config QR. Closes the unauthenticated config/CSRF surface
// - a cross-site form can't know the per-device token, so this doubles as CSRF defence.
bool webAuthOk(::AsyncWebServerRequest* r) {
  String want = agent::store::webAuthToken();
  String got;
  if (r->hasHeader("X-Nimbus-Token"))    got = r->getHeader("X-Nimbus-Token")->value();
  else if (r->hasParam("t"))             got = r->getParam("t")->value();          // query
  else if (r->hasParam("t", true))       got = r->getParam("t", true)->value();    // form
  bool ok = !(got.length() == 0 || got.length() != want.length());
  if (ok) {
    uint8_t diff = 0;                     // constant-time compare (no early-out on mismatch)
    for (size_t i = 0; i < want.length(); i++) diff |= (uint8_t)(got[i] ^ want[i]);
    ok = (diff == 0);
  }
  // 401->QR breadcrumbs (P3): a burst of failures usually means the owner is on
  // a token-less page - count them in a 60 s window; any authed request resets.
  if (ok) {
    s_authFails = 0;
  } else {
    const uint32_t now = millis();
    if (s_authFails == 0 || now - s_authFirstFailMs > 60000UL) {
      s_authFails = 1;
      s_authFirstFailMs = now;
    } else if (s_authFails < 255) {
      s_authFails = uint8_t(s_authFails + 1);
    }
  }
  return ok;
}

// Reject an unauthenticated state-changing request with 401. Returns true if it blocked.
static bool authBlocked(::AsyncWebServerRequest* r) {
  if (webAuthOk(r)) return false;
  r->send(401, "application/json",
          "{\"error\":\"Signed-in link required. Scan the Sign-in QR; first-time setup signs in automatically.\"}");
  return true;
}

// ---- curated override params (plan §5) -------------------------------------
// The subset the config page exposes as inputs; the rest stay profile-driven.
// `kind` drives the input widget on the client ("num" | "bool" | "posture").
struct ParamMeta { Param param; const char* kind; };
static const ParamMeta kParams[] = {
  { Param::Posture,          "posture" },   // ring level (Dark/Calm/Full)
  { Param::RingBrightness,   "num"     },
  { Param::AttnHoldMs,       "num"     },   // how long a needs-you CTA holds (ms)
  { Param::EpdCoalesceMs,    "num"     },
  { Param::DwellMs,          "num"     },
  { Param::TelemetryPeriodS, "num"     },
  { Param::TgLowBattPing,    "bool"    },   // Orchestrator low-battery Telegram alert
                                            // (now wired: lowBatteryPing gates on it)
};
static constexpr int kParamMetaCount = sizeof(kParams) / sizeof(kParams[0]);

// Deferred-apply staging. The /api/config handler runs on the AsyncTCP task
// (core 0); the Config it mutates is read by the main loop (core 1) with no
// atomicity on has_/val_. So EVERY mutation - profile, mode AND single-param
// overrides - is staged here from the handler and applied in loopWeb() on the
// main task, so the Config is only ever written from one task. This also gives
// a clean write-then-apply order (stage -> loopWeb writes -> onChanged reads).
static portMUX_TYPE s_cfgMux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool    s_dirty     = false;  // any mutation -> fire onChanged
static volatile bool    s_ringRefresh = false; // theme (or other ring-affecting) change -> refreshRing
static volatile bool    s_ledConfirm  = false; // web action succeeded -> LED blip (P3)
// TFT first-run handoff: saving credentials must not let the main loop tear the
// setup AP down before the browser learns the exact, token-bearing LAN URL.
// AsyncTCP only sets these aligned one-shot flags; loop() consumes them.
static volatile bool    s_wifiJoinStarted = false;
static volatile bool    s_wifiHandoffReady = false;
static volatile bool    s_haveProf  = false;
static volatile int     s_pendProf  = 0;
static volatile bool    s_haveMode  = false;
static volatile int     s_pendMode  = 0;
// POST /api/onboard/restart - non-destructive restart staged to loopWeb() (the
// main task); ESP.restart() inline on the AsyncTCP task is unsafe (mode rule).
static volatile bool    s_onbRestartPending = false;
// POST /api/preview staging - same single-writer-on-apply pattern as profile/
// mode above, just fired through onPreview instead of mutating Config/Selector
// (a preview never persists and never touches g_cfg).
static volatile bool    s_havePreview = false;
static volatile int     s_pendPreview = 0;
static volatile int     s_pendPreviewStatus = -1;
// POST /api/battcal staging - owner asserts a full pack. The battery model + its
// NVS save are main-task-owned, so the anchor is applied in loopWeb() (main task),
// never from the AsyncTCP handler.
static volatile bool    s_battCalPending = false;
static volatile bool    s_battResetPending = false;  // POST /api/battreset -> main task
static volatile bool    s_battHwPending = false;     // battery hardware (divider/capacity) -> main task
// Battery drain/storage staging (battery-measurement) - same main-task-apply pattern.
static volatile bool    s_storagePending = false;
static volatile int     s_storagePct     = 0;
// POST /api/wifi publishap|resume staging. Both re-point the radio AND (when the
// AP has to be re-asserted) restart the captive DNS server, which the main task
// pumps every loop in process() - so they are applied in loopWeb(), never from
// the AsyncTCP handler. One slot, so clicking both in quick succession lands the
// LAST intent rather than an arbitrary interleaving. 0 = none, 1 = publish, 2 = resume.
static volatile int8_t  s_wifiLinkAction = 0;
#ifdef NIMBUS_TEST
static volatile bool    s_drainPending = false, s_drainOn = false, s_drainDeep = false;
static volatile int     s_drainBright  = -1;   // battlab: per-run LED load; -1 = firmware default
static volatile int     s_drainTtl     = -1;   // battlab host dead-man seconds; -1 = default, 0 = off
#endif

// Pending per-curated-param override edits, staged by applyParam() on the
// AsyncTCP task and drained in loopWeb() on the main task. Action per param:
enum class PendOv : uint8_t { None = 0, Set, Clear };
static volatile PendOv  s_pendOv[kParamMetaCount] = {};
static volatile int32_t s_pendOvVal[kParamMetaCount] = {};

// ---- config lock (see webui.h) ---------------------------------------------
void lockConfig()   { portENTER_CRITICAL(&s_cfgMux); }
void unlockConfig() { portEXIT_CRITICAL(&s_cfgMux); }

// ---- helpers ----------------------------------------------------------------
// Stream the config page as the ordered concatenation of its PROGMEM fragments
// (CONFIG_HTML_PARTS, P4 split) with a chunked response: the callback copies
// straight from flash into the TCP buffer as the socket drains - no RAM copy
// of the ~170 KB page ever exists. Fragment lengths are measured once.
static void sendConfigPage(AsyncWebServerRequest* r) {
  static size_t s_len[CONFIG_HTML_PART_COUNT] = {0};
  static bool   s_measured = false;
  if (!s_measured) {
    for (size_t p = 0; p < CONFIG_HTML_PART_COUNT; p++)
      s_len[p] = strlen_P(CONFIG_HTML_PARTS[p]);
    s_measured = true;
  }
  AsyncWebServerResponse* res = r->beginChunkedResponse(
      "text/html",
      [](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
        size_t off = index;  // absolute offset into the concatenated page
        for (size_t p = 0; p < CONFIG_HTML_PART_COUNT; p++) {
          if (off < s_len[p]) {
            size_t take = s_len[p] - off;
            if (take > maxLen) take = maxLen;
            memcpy_P(buf, CONFIG_HTML_PARTS[p] + off, take);
            return take;
          }
          off -= s_len[p];
        }
        return 0;  // past the last fragment: response complete
      });
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

static void buildState(String& out) {
  JsonDocument d;
  const int mode = s_wc.currentMode ? s_wc.currentMode()
                                    : (int)solide::memory::getInt("mode", 0);
  d["fw"]    = NIMBUS_FW_VERSION;
  d["build"] = NIMBUS_FW_BUILD;
  d["mode"] = mode;
  d["jobs"] = s_wc.activeJobs ? s_wc.activeJobs() : 0;
  // Memory picture. heap = free INTERNAL RAM right now (this is what turns/TLS draw
  // on and why it fluctuates); heapTotal = size of that internal pool; heapMin = the
  // lowest it has ever dropped to (headroom check). PSRAM is the 8 MB external pool
  // (TLS is routed here). fs = LittleFS flash (vectors, voice clips, config blob).
  d["heap"]       = (uint32_t)ESP.getFreeHeap();
  d["heapTotal"]  = (uint32_t)ESP.getHeapSize();
  d["heapMin"]    = (uint32_t)ESP.getMinFreeHeap();
  d["psramFree"]  = (uint32_t)ESP.getFreePsram();
  d["psramTotal"] = (uint32_t)ESP.getPsramSize();
  // Per-capability breakdown so future agents MEASURE the internal-vs-PSRAM split
  // instead of conflating (docs/memory-model.md). intFree/intLargest = the scarce
  // internal pool + its largest contiguous block (what a TLS handshake needs);
  // dmaFree = DMA-capable internal (WiFi/I2S/SPI); spiLargest = PSRAM contiguous.
  JsonObject mem = d["mem"].to<JsonObject>();
  mem["intFree"]    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  mem["intLargest"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  mem["dmaFree"]    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
  mem["spiFree"]    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  mem["spiLargest"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  // Worst free stack the tg_poll task (which runs the whole turn + tool loop) has
  // ever had, in bytes. This is the headroom over a canary overflow - the recurring
  // crash(panic). Watch it: if it approaches 0 the stack needs raising.
  mem["pollStackMin"] = (uint32_t)agent::telegram::pollStackMinFree();
  // The AsyncTCP service task's current free stack (this handler runs ON it, so
  // its own high-water is the relevant number). Confirms the halved 8 KB has room.
  if (TaskHandle_t at = xTaskGetHandle("async_tcp"))
    mem["asyncStackMin"] = (uint32_t)uxTaskGetStackHighWaterMark(at);  // bytes (uint8_t StackType_t)
  {
    int rsm = nimbus::relay::stackMinFree();
    if (rsm >= 0) mem["relayStackMin"] = rsm;  // cloud relay task (Orchestrator only)
  }
  // Cloud tunnel (cumulo-nimbus) status. Present in both modes; reports disabled in
  // Notifier (the relay task only spawns in Orchestrator).
  nimbus::relay::statusInto(d["cloud"].to<JsonObject>());
  // Data store for orchestrator vectors/episodic: the SD card (16 GB+) when mounted,
  // else internal LittleFS flash. Reported as doubles (bytes) - a 16 GB card overflows
  // uint32 - with a label so the UI shows the right device + adaptive units.
  d["fsTotal"]    = (uint32_t)LittleFS.totalBytes();   // internal flash (firmware/config)
  d["fsUsed"]     = (uint32_t)LittleFS.usedBytes();
  const bool sd = solide::storage::available();
  d["storeSD"]    = sd;
  d["storeLabel"] = sd ? "SD card" : "internal flash";
  d["storeTotal"] = sd ? (double)solide::storage::cardSizeMB() * 1048576.0 : (double)LittleFS.totalBytes();
  d["storeFree"]  = sd ? (double)solide::storage::freeMB()   * 1048576.0
                       : (double)(LittleFS.totalBytes() - LittleFS.usedBytes());
  // Named-unit twins (owner: storeFree/Total read ambiguously vs sdprobe's MB
  // fields). The unnamed originals stay in BYTES for the page's sz() formatter.
  if (sd) {
    d["storeTotalMB"] = (uint32_t)solide::storage::cardSizeMB();
    d["storeFreeMB"]  = (uint32_t)solide::storage::freeMB();
  }

  // HAL health (solide::begin per-subsystem) - which peripherals came up at boot.
  // Null accessor => reported all-up (can't tell). Lets STATUS/dashboards flag a
  // dead display/leds/storage/input without a camera.
  const uint8_t hm = s_wc.halMask ? s_wc.halMask() : 0x1F;
  JsonObject hal = d["hal"].to<JsonObject>();
  hal["display"] = (hm & 1) != 0;  hal["leds"] = (hm & 2) != 0;  hal["storage"] = (hm & 4) != 0;
  hal["memory"]  = (hm & 8) != 0;  hal["input"] = (hm & 16) != 0;
  // Memory-tier degradation (cheap accessors, no engine lock): the EFFECTIVE SD
  // state (physical mount AND not fault-injected) + the LittleFS-full pause flag.
  // memSd can read false while storeSD reads true - that is a simulated SD loss.
  d["memSd"]        = agent::memory::haveSd();
  d["memFlashFull"] = agent::memory::flashFull();
  d["sdLost"]       = agent::memory::sdLost();   // demoted mid-run (card was present at boot)
  // Injected resilience faults (nimbus::fault). Always 0 in production (only the
  // NIMBUS_TEST console / POST /api/fault can set them); surfaced so the HIL suite
  // can confirm an injection took and the degraded path is the one under test.
  const uint16_t fmask = nimbus::fault::mask();
  JsonObject fj = d["faults"].to<JsonObject>();
  for (uint8_t i = 0; i < nimbus::fault::COUNT; i++)
    fj[nimbus::fault::name(nimbus::fault::Cap(i))] = (fmask & (1u << i)) != 0;

  power::Sample b = s_wc.power ? s_wc.power->sample() : power::Sample{};
  JsonObject batt = d["batt"].to<JsonObject>();
  batt["valid"]       = b.valid;
  batt["millivolts"]  = b.millivolts;   // pack mV (the analytics/history feed)
  // Charge state, corrected SoC and charge flags all come from the BatteryModel:
  // it owns the time series, so it infers charging/full/discharging from the
  // voltage trend and calibrates the ADC's under-read top band. The raw driver
  // percent/onExternalPower are NOT authoritative on a board with no VBUS pin.
  if (s_wc.batteryEstimate) {
    const power::BatteryEstimate e = s_wc.batteryEstimate();
    batt["percent"]     = e.percent;
    // mvTrue: pack mV with the SAME top-band correction as `percent`, so the two
    // agree about one pack. `millivolts` above stays RAW on purpose - the Battery
    // Lab stores raw and applies its own per-device, multimeter-referenced
    // correction; pre-correcting the wire would double-correct it there.
    if (e.millivoltsTrue) batt["mvTrue"] = e.millivoltsTrue;
    batt["health"]      = e.healthPct;
    batt["segments"]    = e.segments;
    batt["chargeState"] = power::chargeStateStr(e.chargeState);
    batt["onExtPower"]  = e.onExternalPower;
    batt["charging"]    = e.chargeState == power::ChargeState::Charging;
    batt["full"]        = e.chargeState == power::ChargeState::Full;
    batt["calibrated"]  = e.calibrated;
    // Protection state (owner feature): the low-batt sleep threshold + the two
    // risk overrides, so every surface (web/AI/HIL) can see what is armed.
    batt["sleepMv"]   = agent::store::sleepMv();
    batt["wakeMv"]    = agent::store::wakeMv();
    batt["sleepOvr"]  = agent::store::sleepOvr();
    batt["brightOvr"] = agent::store::brightOvr();
    batt["brightCap"] = agent::store::brightOvr() ? 255 : nimbus::power::kBrightCap;
    // Battery hardware config (owner-set; boards differ). rtop/rbot are the divider
    // resistors, capMah the pack capacity - they drive voltage + estimations.
    batt["rtop"]   = agent::store::battRtop();
    batt["rbot"]   = agent::store::battRbot();
    batt["capMah"] = agent::store::battCapMah();
    batt["chem"]   = agent::store::battChem();     // "liion" | "lifepo4"
    batt["cells"]  = agent::store::battCellsOvr();  // 0 = board default
    batt["curve"]  = agent::store::battCurve();      // "" = chemistry default
    if (e.valid) {
      batt["minsToEmpty"] = e.minutesToEmpty;   // only while discharging
      batt["ratePctHr"]   = e.ratePctPerHr;
    }
    // Absolute capacity grounded in the externally-measured 3500 mAh nominal ×
    // runtime-referenced health (battery-measurement). 0 until health is learned.
    batt["capacityMah"] = int(e.healthPct) * agent::store::battCapMah() / 100;
  } else {
    batt["percent"]     = b.percent;
    batt["onExtPower"]  = b.onExternalPower;
    batt["charging"]    = b.charging;
  }
  // Drain/storage campaign state (battery-measurement). The host poller reads restingMv /
  // restingAgeS to log the calibration-grade resting curve; drainActive/deep confirm the
  // load is armed. Always present (false / 0 on a production build with no drain armed).
  if (s_wc.drainState) {
    bool da = false, dd = false, sa = false; uint16_t rmv = 0; uint32_t rage = 0;
    s_wc.drainState(da, dd, sa, rmv, rage);
    batt["drainActive"]   = da;
    batt["drainDeep"]     = dd;
    batt["storageActive"] = sa;
    batt["restingMv"]     = rmv;
    batt["restingAgeS"]   = rage;
  }
  // Thermal guard (fried-panel fix): die temperature + trip state - the campaign
  // harness logs dieTempC alongside every voltage sample now.
  if (s_wc.thermalState) {
    float dieC = 0; bool trip = false, abrt = false; uint8_t trips = 0;
    s_wc.thermalState(dieC, trip, trips, abrt);
    batt["dieTempC"]      = int(dieC * 10 + 0.5f) / 10.0;   // one decimal
    batt["thermalTrip"]   = trip;
    batt["thermalTrips"]  = trips;
    batt["thermalAbort"]  = abrt;
  }
  // battlab: the active per-run LED load + the firmware's compile-time divider
  // assumption, so the host tool's resistor correction is self-describing
  // (mv_true = mv_reported * ratio_device / (dividerX100/100)).
  if (s_wc.drainBright) batt["drainBright"] = s_wc.drainBright();
  if (s_wc.drainTtlLeftS) batt["drainTtlLeftS"] = s_wc.drainTtlLeftS();
  batt["dividerX100"] = agent::store::battDividerX100();   // CONFIGURED, not the compile default
  // ⚠ UNCONDITIONAL, unlike the protection knobs nested in `if (batteryEstimate)`
  // above: these are preferences, and a board with no pack fitted must still be
  // able to see and set them (the web UI hides the whole battery TELEMETRY section
  // when there is no valid reading).
  batt["lbRing"]  = agent::store::lowBattRing();
  batt["lbSaver"] = agent::store::lowBattSaver();
  // Battery monitoring on/off; default is board-derived (all-in-one boards, which
  // have no e-paper option, treat a battery as opt-in and default this OFF).
  batt["battMon"] = agent::store::battMon(solide::board().epd.sck >= 0);

  // E1 artifact store presence (SD /mem/files): the web UI's Files section +
  // the campaign harness read this.
  {
    agent::files::StorageTruth t = agent::files::storageTruth();
    JsonObject fl = d["files"].to<JsonObject>();
    fl["present"]     = t.present;
    fl["unsupported"] = t.unsupported;   // CUM-7: mounted card < 1 GB
    fl["count"]       = t.files;
    fl["bytes"]       = (unsigned long long)t.used;
    fl["quota"]       = (unsigned long long)t.quota;       // card - 512 MB reserve
    fl["cardFree"]    = (unsigned long long)t.cardFree;    // free-on-card
  }

  bool sta = staConnected();
  d["sta"]   = sta;
  // First-run gate: the web UI shows the setup wizard overlay while this is true
  // (cleared when the wizard finishes, or by an NVS-wipe factory reset).
  d["needsOnboarding"] = !agent::store::onboarded();
  d["staIp"] = sta ? staIp() : "";
  d["apIp"]  = apIp();
  d["mdns"]  = mdnsName();
  // Device identity (P2): the user-visible name + the live setup-AP SSID it
  // derives ("<name>-setup"). Renames apply on the next reboot.
  d["devName"] = sys::deviceName();
  // Device clock + timezone, for the UI badge/banner and browser-side "ago"/
  // "in" math (memory rows, routine next-runs). `local` is rendered HERE via
  // localtime_r - the browser must never parse a POSIX TZ string. Predicate is
  // the STRICT one (memory::clockSynced, >=2020): it implies the loops gate, so
  // the UI can never claim "synced" while dreaming would still be blocked.
  {
    JsonObject ck = d["clock"].to<JsonObject>();
    const time_t now = time(nullptr);
    ck["synced"] = agent::memory::clockSynced();
    ck["epoch"]  = (uint64_t)now;
    ck["tz"]     = agent::store::deviceTz();
    struct tm lt;
    localtime_r(&now, &lt);
    char buf[20];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    ck["local"] = buf;
  }
  // Which display panel this device is running (hardware identity, applied at
  // boot) - lets the dashboard and the HIL suites tell the two configs apart.
  d["scrModel"] = agent::store::screenModel();
  // The driver actually BOUND at boot. Differs from scrModel (the stored
  // preference) after a display change that hasn't been restarted into yet - the
  // onboarding wizard reads this to know a restart is still pending.
  {
    int b = agent::store::bootScreenIsTft();
    if (b >= 0) d["scrBoot"] = b ? "tft" : "eink";
  }
  // Board pinout identity (compile-time) + whether the panel is fixed. An
  // all-in-one board has no e-paper option (epd.sck < 0), so its scrModel cannot
  // change - the UI locks the selector and the setter rejects a switch.
#ifndef SOLIDE_BOARD
#define SOLIDE_BOARD solide_s3
#endif
#define NIMBUS_BSTR2(x) #x
#define NIMBUS_BSTR(x) NIMBUS_BSTR2(x)
  d["board"] = NIMBUS_BSTR(SOLIDE_BOARD);
  d["scrFixed"] = (solide::board().epd.sck < 0);
  // Capacitive touch reports pixel coordinates, so the resistive min/max
  // calibration is meaningless - the UI hides that field on such a board.
  d["touchCap"] = (solide::board().touchKind == solide::TouchKind::CapacitiveI2c);
  // Idle minutes before the screen rests. Surfaced because it is an owner
  // setting the web UI edits, yet it was not readable back from any endpoint -
  // so its per-panel default (e-ink 60, colour 10, because a backlight is the
  // largest continuous draw) could not be confirmed on a device.
  d["saverMin"] = agent::store::saverMin();
  // Panel watchdog counters, on HTTP because reading them over the console
  // RESETS this board (DTR/RTS drive EN) and so destroys the very fault being
  // measured. A white screen could not be diagnosed live without this.
  //   panelOk   - do the panel's registers still hold what we wrote?
  //   panelHeal - times the registers were found wrong and re-asserted
  //   panelPaint- unconditional watchdog repaints (covers the pixel-loss mode
  //               that no register reveals)
  if (agent::store::screenIsTft()) {
    d["panelOk"]    = nimbus::hw::tft::probeEnabled() ? nimbus::hw::tft::panelConfigOk() : true;
    d["panelHeal"]  = nimbus::hw::tft::healCount();
    d["panelPaint"] = nimbus::hw::tft::repaintCount();
    // The backlight is the difference between "black screen" and "white screen":
    // 0 means the panel is intentionally resting, non-zero means it is lit and
    // any blankness is the panel's own doing. Without this the two look the same
    // from here.
    d["panelBacklight"] = nimbus::hw::tft::backlight();
    // ⚠ backlight is the REQUESTED level; this says whether it is driveable at
    // all. A dark panel reporting backlight=100 with panelBlOk=false means the
    // light was never actually driven.
    d["panelBlOk"] = solide::display_tft::backlightAttached();
    // Content, not configuration: does the panel still hold the pixels we sent?
    // ⚠ Only probe when explicitly enabled - /api/state must not be able to
    // provoke the fault just by being polled.
    d["panelProbe"]   = nimbus::hw::tft::probeEnabled();
    d["panelPixOk"]   = nimbus::hw::tft::probeEnabled() ? nimbus::hw::tft::panelContentOk(4) : true;
    d["panelPixLost"] = nimbus::hw::tft::contentLostCount();
    // ⚠ The two that separate "we asked" from "it happened". repaintCount only
    // counts watchdog TRIGGERS; renderAndPush then drops the push if the driver
    // is still busy. A render task that died or wedged leaves busy=true forever,
    // so every push is silently dropped while the framebuffer, the registers and
    // the backlight all look perfect - which is exactly what a blank panel with
    // healthy diagnostics looks like.
    d["panelTask"] = solide::display_tft::taskAlive();
    d["panelBusy"] = solide::display_tft::busy();
  }
  d["tchCal"]   = agent::store::touchCal();
  d["apSsid"]  = apSsid();
  d["rssi"]  = sta ? rssi() : 0;

  const ProfileId user = s_wc.selector ? s_wc.selector->user() : ProfileId::Balanced;
  const ProfileId eff  = s_wc.selector ? s_wc.selector->resolve() : user;
  d["profile"]              = (int)user;
  d["effectiveProfile"]     = (int)eff;
  d["effectiveProfileName"] = nimbus::profileName(eff);

  // Snapshot every Config-derived value under the lock so the menu (main task)
  // can't tear a has_/val_ pair mid-read. Keep the critical section tiny - no
  // JSON/heap work inside it - then serialize from the snapshot below.
  int32_t posture = 0;
  int32_t pv[kParamMetaCount] = {};
  bool    pOv[kParamMetaCount] = {};
  if (s_wc.config) {
    lockConfig();
    posture = s_wc.config->effective(Param::Posture);
    for (int i = 0; i < kParamMetaCount; i++) {
      pv[i]  = s_wc.config->effective(kParams[i].param);
      pOv[i] = s_wc.config->hasOverride(kParams[i].param);
    }
    unlockConfig();
  }
  d["posture"] = posture;

  JsonArray arr = d["params"].to<JsonArray>();
  for (int i = 0; i < kParamMetaCount; i++) {
    Param p = kParams[i].param;
    nimbus::ParamMeta pm = nimbus::paramMeta(p);
    JsonObject o = arr.add<JsonObject>();
    o["key"]        = (int)p;
    o["name"]       = nimbus::paramName(p);
    o["label"]      = nimbus::paramLabel(p);        // human title
    o["desc"]       = nimbus::paramDescription(p);  // plain-English help
    o["kind"]       = kParams[i].kind;
    o["value"]      = pv[i];
    o["def"]        = nimbus::presetValue(user, p);
    o["overridden"] = pOv[i];
    o["min"]        = pm.min;                        // allowed range for the UI
    o["max"]        = pm.max;
    o["step"]       = pm.step;
  }
  // OTA firmware update (device-level, both modes) - the Device-tab panel and
  // the HIL suite read these; progress rides the 3 s /api/state poll.
  d["ota"]      = otaupd::statusStr();     // idle/checking/available/downloading/...
  d["otaResult"]= otaupd::checkResultStr(); // definitive check outcome: pending/
                                            // up-to-date/new-version/unreachable/failed
  d["otaPct"]   = otaupd::progressPct();   // -1 unless downloading
  d["otaLatest"]= otaupd::latestSeen();    // newest release seen ("" = none)
  d["otaNotes"] = otaupd::latestNotes();
  d["otaErr"]   = otaupd::lastError();
  d["lastOta"]  = otaupd::lastResult();    // persisted outcome (ok/rollback/...)
  d["autoUpd"]  = agent::store::otaAutoUpdate();
  d["otaSlot"]  = otaupd::runningSlot();    // running app slot (app0/app1) - install flip proof
  serializeJson(d, out);
}

// Stage a single-param override / clear (runs on the AsyncTCP task). The Config
// is NOT touched here - the pending edit is drained onto the Config in loopWeb()
// on the main task, so has_/val_ is only ever written from one core. Returns
// true if the field matched a curated param (so the handler can mark it dirty).
static bool applyParam(const String& name, const String& value) {
  if (!s_wc.config) return false;
  // Revert-to-defaults (owner 2026-07-18): stage a Clear on EVERY curated param;
  // the main-task drain drops all overrides under the config lock and the
  // s_dirty -> onChanged() path persists once. The active battery mode then
  // runs on its pure reference preset.
  if (name == "revert_overrides" && (value == "1" || value == "true")) {
    for (int i = 0; i < kParamMetaCount; i++) s_pendOv[i] = PendOv::Clear;
    return true;
  }
  bool touched = false;
  for (int i = 0; i < kParamMetaCount; i++) {
    Param p = kParams[i].param;
    String pk = String("p_") + String((int)p);
    String ck = String("clr_") + String((int)p);
    if (name == pk) {
      s_pendOvVal[i] = (int32_t)value.toInt();
      s_pendOv[i]    = PendOv::Set;
      touched = true;
    } else if (name == ck && (value == "1" || value == "true")) {
      s_pendOv[i] = PendOv::Clear;
      touched = true;
    }
  }
  return touched;
}

// ---- orchestrator control surface (plan ROUND 3 Part A) ----------------------

// The three verifiable providers and their model choice lists (agent_config.h).
struct ProvMeta { const char* name; const char* choices; };
static const ProvMeta kProviders[] = {
  { "openai",    OPENAI_MODEL_CHOICES  },
  { "anthropic", ANT_MODEL_CHOICES     },
  { "mistral",   MISTRAL_MODEL_CHOICES },
};
static constexpr int kProvCount = sizeof(kProviders) / sizeof(kProviders[0]);

static bool isKnownProvider(const String& p) {
  for (int i = 0; i < kProvCount; i++)
    if (p == kProviders[i].name) return true;
  return p == "custom";
}

// Sanitize a comma-separated priority list: keep only known provider tokens,
// drop duplicates/whitespace. Returns "" if nothing valid survives (caller
// then ignores the field rather than persisting an empty routing list).
static String sanitizePriority(const String& in) {
  String out;
  int start = 0;
  while (start < (int)in.length()) {
    int comma = in.indexOf(',', start);
    if (comma < 0) comma = in.length();
    String tok = in.substring(start, comma);
    tok.trim(); tok.toLowerCase();
    if (isKnownProvider(tok) && (out.indexOf(tok) < 0)) {
      if (out.length()) out += ",";
      out += tok;
    }
    start = comma + 1;
  }
  return out;
}

// A model value is accepted if it is empty (= reset to provider default) or a
// member of that provider's choice list - the "verified options only" contract
// is enforced client-side by gating the dropdown and server-side right here.
static bool modelInChoices(const char* choices, const String& v) {
  if (!v.length()) return true;
  String all(choices);
  int start = 0;
  while (start < (int)all.length()) {
    int comma = all.indexOf(',', start);
    if (comma < 0) comma = all.length();
    if (v == all.substring(start, comma)) return true;
    start = comma + 1;
  }
  return false;
}

static void buildOrchState(String& out) {
  JsonDocument d;
  const int mode = s_wc.currentMode ? s_wc.currentMode()
                                    : (int)solide::memory::getInt("mode", 0);
  d["running"]  = (mode == 1);
  d["hasTg"]    = agent::store::telegramToken().length() > 0;
  d["tgVerify"] = agent::store::verifyResult("telegram");   // 1 ok / 0 rejected / -1 unknown
  d["tgVts"]    = agent::store::verifyTs("telegram");       // 0 => never verified
  d["tgBot"]    = agent::store::tgBotName();                // @username from getMe ("" = unknown)
  d["tgAllow"]  = agent::store::telegramAllowlist();
  d["orchHost"] = agent::store::orchHost();
  d["provPrio"] = agent::store::providerPriority();
  d["subPrio"]  = agent::store::subPriority();
  d["orchLoop"] = agent::store::orchToolLoop();
  d["midFail"]  = agent::store::midTurnFailover();
  d["orchTrace"] = agent::store::orchTrace();   // glass-box trace capture (A4)
  d["ttsOn"]    = agent::store::ttsEnabled();   // "Voice replies" toggle (P2.5)
  d["tgLive"]   = agent::telegram::enabled();   // poll task actually running with a
                                                // token (false + hasTg => reboot needed)
  d["loopRounds"]   = agent::store::orchLoopRounds();
  d["loopRescap"]   = agent::store::orchLoopResultCap();   // 0 = auto (derived per turn)
  d["loopTotcap"]   = agent::store::orchLoopTotalCap();    // 0 = auto
  // Local Loops governor cap overrides (0 = no override, using the caps.h default).
  d["loopMaxCnt"]   = agent::store::loopCapMaxCount();
  d["loopMinIvl"]   = agent::store::loopCapMinIntervalS();
  d["loopFires"]    = agent::store::loopCapFiresPerDay();
  d["loopTokens"]   = agent::store::loopCapTokensPerDay();
  d["loopDevTok"]   = agent::store::loopCapDevTokensPerDay();
  d["loopDevFir"]   = agent::store::loopCapDevFiresWindow();
  {
    // Effective derived caps so the UI can render "auto (currently N)" - same
    // derivation the engine applies at turn time (budget.h anchor invariant).
    nimbus::orch::BudgetOverrides bov;
    bov.toolResultCap = agent::store::orchLoopResultCap();
    bov.toolTotalCap = agent::store::orchLoopTotalCap();
    const String h = agent::store::resolvedOrchHost();
    const auto b = nimbus::orch::deriveBudget(
        nimbus::orch::modelCtxTokens(h.c_str(), agent::store::orchModel(h).c_str()), bov);
    d["loopRescapEff"] = (int)b.toolResultBytes;
    d["loopTotcapEff"] = (int)b.toolTotalBytes;
    d["ctxTokens"] = b.ctxTokens;
  }
  d["compactKB"]    = agent::store::compactAtKB();
  d["tlsSlots"]     = agent::store::tlsSlots();
  d["tlsVerify"]    = agent::store::tlsVerify();   // validate provider certs (default ON)
  d["capProbe"]     = agent::store::capProbe();     // capability validation mode (W3b): 0 off / 1 passive / 2 active
  d["fetchPol"]     = agent::store::fetchPolicy();  // W18 URL downloads: 0 off / 1 approve / 2 scan / 3 yolo
  d["modInbound"]   = agent::store::modInbound();   // CUM-69 moderation gates (non-admin only; paid per item)
  d["modOutbound"]  = agent::store::modOutbound();
  d["modInjection"] = agent::store::modInjection();
  d["capProbeH"]    = agent::store::capProbeHours(); // active re-verify interval (hours)
  d["loopDeadline"] = agent::store::orchLoopDeadlineS();
  // Token usage (Phase-0 TokenUsage seam) - real billed in/out tokens, last turn +
  // running session total. Honest counts only; no fabricated cost (needs a price table).
  {
    auto su = agent::orchestrator::sessionUsage();
    auto lu = agent::orchestrator::lastTurnUsage();
    JsonObject u = d["usage"].to<JsonObject>();
    u["turns"]   = agent::orchestrator::turnCount();
    u["sessIn"]  = su.promptTokens;
    u["sessOut"] = su.completionTokens;
    u["lastIn"]  = lu.promptTokens;
    u["lastOut"] = lu.completionTokens;
    // Per-provider monthly ledger (owner: budget per provider). The store hands back
    // a JSON array string (built under its own mutex); parse-and-copy it into the doc
    // so the Usage pane can render count-vs-budget bars per provider.
    {
      String pj = agent::store::providerUsageJson();
      JsonDocument pd;
      if (deserializeJson(pd, pj) == DeserializationError::Ok && pd.is<JsonArray>())
        u["byProvider"] = pd.as<JsonArray>();   // deep-copied into d
    }
  }
  d["sfxLvlN"]  = agent::store::sfxLevelNotif();
  d["sfxLvlO"]  = agent::store::sfxLevelOrch();
  d["sfxTheme"] = agent::store::sfxTheme();
  d["sfxVol"]   = agent::store::sfxVolume();
  d["sfxTier"]  = ::sfx::tierStr();
  d["sfxSync"]  = sfxsync::statusStr();
  d["sttProv"]  = agent::store::sttProvider();
  d["ttsProv"]  = agent::store::ttsProvider();
  d["ttsVoice"] = agent::store::ttsVoice();
  d["theme"]    = agent::store::theme();
  d["scrModel"] = agent::store::screenModel();
  d["scrFlip"]  = agent::store::tftFlip();   // display mounted 180 deg (TFT only)
  d["directive"]= agent::store::sysPrompt();
  d["verifyPending"] = agent::provider_verify::pending();

  JsonObject cust = d["cust"].to<JsonObject>();
  cust["base"]   = agent::store::customBase();
  cust["conv"]   = agent::store::customConv();
  cust["model"]  = agent::store::customModel();
  cust["hasKey"] = agent::store::customKey().length() > 0;

  JsonObject provs = d["providers"].to<JsonObject>();
  for (int i = 0; i < kProvCount; i++) {
    const char* p = kProviders[i].name;
    JsonObject o = provs[p].to<JsonObject>();
    bool hasKey =
        (i == 0) ? agent::store::hasOpenaiKey()
      : (i == 1) ? agent::store::hasAnthropicKey()
                 : agent::store::hasMistralKey();
    o["hasKey"]    = hasKey;
    o["verify"]    = agent::store::verifyResult(p);
    o["vts"]       = agent::store::verifyTs(p);
    o["orchModel"] = agent::store::orchModel(p);
    o["subModel"]  = agent::store::subModel(p);
    // Live-harvested list first (verify pass reads /v1/models), static fallback.
    {
      String dyn = agent::store::modelChoices(p);
      o["choices"] = dyn.length() ? dyn : String(kProviders[i].choices);
    }
  }

  d["hasTav"]    = agent::store::hasTavilyKey();   // web-search tool configured
  d["tavVerify"] = agent::store::verifyResult("tavily");   // 1 ok / 0 rejected / -1 unknown
  d["tavVts"]    = agent::store::verifyTs("tavily");

  // Live orchestrator state via the cross-task-safe accessors (both are inert
  // empties in Notifier mode - see orchestrator.h).
  d["mem"]  = agent::orchestrator::memorySnapshot();
  d["jobs"] = serialized(agent::orchestrator::sessionsJson());

  serializeJson(d, out);
}

// Apply one POSTed orchestrator field. Key writes invalidate that provider's
// verify cache (result -1, ts 0 = "never verified") so a swapped key can't ride
// a stale "verified" badge. Returns true if the field was recognized.
static bool applyOrchField(const String& n, const String& v, bool& cfgDirty) {
  // provider keys (set on non-empty; explicit clr_* to clear)
  if (n == "oaiKey")  { if (v.length()) { agent::store::setOpenaiKey(v);    agent::store::setVerify("openai", -1, 0); }    return true; }
  if (n == "antKey")  { if (v.length()) { agent::store::setAnthropicKey(v); agent::store::setVerify("anthropic", -1, 0); } return true; }
  if (n == "mistKey") { if (v.length()) { agent::store::setMistralKey(v);   agent::store::setVerify("mistral", -1, 0); }   return true; }
  if (n == "clr_oaiKey")  { agent::store::setOpenaiKey("");    agent::store::setVerify("openai", -1, 0);    return true; }
  if (n == "clr_antKey")  { agent::store::setAnthropicKey(""); agent::store::setVerify("anthropic", -1, 0); return true; }
  if (n == "clr_mistKey") { agent::store::setMistralKey("");   agent::store::setVerify("mistral", -1, 0);   return true; }
  // Tavily web-search key (enables the web.search tool at the next boot/registration)
  if (n == "tavKey")     {
    if (v.length()) {
      agent::store::setTavilyKey(v);
      // Verify-on-save like every other capability (owner: "unverified? verified but
      // no confirmation?"): one minimal real /search proves the key does work.
      agent::store::setVerify("tavily", -1, 0);
      agent::provider_verify::request("tavily");
    }
    return true;
  }
  if (n == "clr_tavKey") { agent::store::setTavilyKey(""); agent::store::setVerify("tavily", -1, 0); return true; }

  // custom endpoint
  if (n == "custBase")  { agent::store::setCustomBase(v);  return true; }
  if (n == "custKey")   { if (v.length()) agent::store::setCustomKey(v); return true; }
  if (n == "clr_custKey") { agent::store::setCustomKey(""); return true; }
  if (n == "custConv")  {
    if (v == "openai" || v == "mistral" || v == "anthropic") agent::store::setCustomConv(v);
    return true;
  }
  if (n == "custModel") { agent::store::setCustomModel(v); return true; }

  // Per-provider budget (owner: "limit budget per provider"). Composite value
  // "provider:tokenLimit:callLimit:resetDay" - one field, four correlated knobs.
  // 0 limit = unlimited; resetDay 1..28. Provider allowlisted to the known set so a
  // stray value can't seed an arbitrary NVS ledger entry.
  if (n == "budget") {
    int c1 = v.indexOf(':');
    if (c1 <= 0) return true;
    String prov = v.substring(0, c1);
    int c2 = v.indexOf(':', c1 + 1);
    int c3 = (c2 > 0) ? v.indexOf(':', c2 + 1) : -1;
    if (c2 <= 0 || c3 <= 0) return true;
    if (!(isKnownProvider(prov) || prov == "tavily" || prov == "custom")) return true;
    uint64_t tl = strtoull(v.substring(c1 + 1, c2).c_str(), nullptr, 10);
    uint32_t cl = (uint32_t)strtoul(v.substring(c2 + 1, c3).c_str(), nullptr, 10);
    // W16: optional 5th field - the $ ceiling in CENTS ("prov:tok:call:rd:cents").
    int c4 = v.indexOf(':', c3 + 1);
    String rdStr = (c4 > 0) ? v.substring(c3 + 1, c4) : v.substring(c3 + 1);
    int rd = rdStr.toInt();
    if (rd < 1) rd = 1; if (rd > 28) rd = 28;
    uint64_t cents = (c4 > 0) ? strtoull(v.substring(c4 + 1).c_str(), nullptr, 10) : 0;
    agent::store::setProviderBudget(prov, tl, cl, (uint8_t)rd, cents);
    return true;
  }
  // Per-provider price rates for $ estimates. Same composite pattern as budget:
  // "provider:centsPerMIn:centsPerMOut:centsPerKCalls". Since W16 the rates ALSO
  // arm the $ budget ceiling (estCents >= centsLimit refuses the provider), so
  // they are enforcement inputs, not display-only; 0 = unset.
  if (n == "rates") {
    int c1 = v.indexOf(':');
    if (c1 <= 0) return true;
    String prov = v.substring(0, c1);
    int c2 = v.indexOf(':', c1 + 1);
    int c3 = (c2 > 0) ? v.indexOf(':', c2 + 1) : -1;
    if (c2 <= 0 || c3 <= 0) return true;
    if (!(isKnownProvider(prov) || prov == "tavily" || prov == "custom")) return true;
    uint32_t ri = (uint32_t)strtoul(v.substring(c1 + 1, c2).c_str(), nullptr, 10);
    uint32_t ro = (uint32_t)strtoul(v.substring(c2 + 1, c3).c_str(), nullptr, 10);
    uint32_t rc = (uint32_t)strtoul(v.substring(c3 + 1).c_str(), nullptr, 10);
    agent::store::setProviderRates(prov, ri, ro, rc);
    return true;
  }

  // Conversation reset (benchmark isolation + owner "start fresh"): drops the
  // provider-side history chain so the next turn starts clean. State-only - no
  // TLS, safe on the AsyncTCP task.
  if (n == "convReset")     { agent::orchestrator::requestConvReset(); return true; }  // staged on tg_poll (prism B)
  if (n == "clearConv")     { if (v == "1") agent::orchestrator::requestConvClear(); return true; }  // /clear: drop conversation + active task, keep memory/files

  // routing (HUMAN-only surface - this handler is exactly that)
  if (n == "orchHost") {
    if (!v.length() || isKnownProvider(v)) agent::store::setOrchHost(v);
    return true;
  }
  if (n == "provPrio") {
    String s = sanitizePriority(v);
    if (s.length()) agent::store::setProviderPriority(s);
    return true;
  }
  // Only mistral/openai do on-device voice, and only if a key is configured -
  // reject an unconfigured provider rather than store a voice setting that can
  // never run (the onboarding UI also gates this, this is the backstop).
  if (n == "sttProv") {
    if ((v == "mistral" || v == "openai") && agent::store::providerHasKey(v))
      agent::store::setSttProvider(v);
    return true;
  }
  if (n == "ttsProv") {
    if ((v == "mistral" || v == "openai") && agent::store::providerHasKey(v))
      agent::store::setTtsProvider(v);
    return true;
  }
  if (n == "ttsVoice") {   // free-form voice id/slug; validated against the provider on use
    if (v.length() <= 40) agent::store::setTtsVoice(v);
    return true;
  }
  if (n == "theme") {      // LED colour theme (resolver clamps unknown -> default)
    if (v.length() <= 16) { agent::store::setTheme(v); s_ringRefresh = true; }
    return true;
  }
  if (n == "subPrio") {
    String s = sanitizePriority(v);
    if (s.length()) agent::store::setSubPriority(s);
    return true;
  }

  // per-provider model picks - accepted only from the verified choice list
  for (int i = 0; i < kProvCount; i++) {
    const char* p = kProviders[i].name;
    // Validate against the EFFECTIVE list (live-harvested first, static fallback)
    // so a freshly-released model the harvest surfaced is actually selectable.
    String eff = agent::store::modelChoices(p);
    if (!eff.length()) eff = kProviders[i].choices;
    if (n == String("orchM_") + p) {
      if (modelInChoices(eff.c_str(), v)) agent::store::setOrchModel(p, v);
      return true;
    }
    if (n == String("subM_") + p) {
      if (modelInChoices(eff.c_str(), v)) agent::store::setSubModel(p, v);
      return true;
    }
  }

  // directive + memory + telegram + tts
  if (n == "sysPrompt") { agent::store::setSysPrompt(v); cfgDirty = true; return true; }
  if (n == "clearMem")  { if (v == "1") agent::orchestrator::requestMemoryClear(); return true; }
  if (n == "tgToken")   {
    if (v.length()) {
      agent::store::setTelegramToken(v);
      // Verify-on-save (owner: "first run a tiny verification"): reset the verdict and
      // enqueue a getMe check on the arbited verify task. The UI polls the verdict.
      agent::store::setVerify("telegram", -1, 0);
      agent::provider_verify::request("telegram");
      // Take the new token LIVE (owner 2026-07-16: after replacing the token the poll
      // task kept fighting the OLD bot with 409s - it latched the token at boot). The
      // stored getUpdates offset belongs to the old bot, so reset it in NVS too (covers
      // Notifier mode, where no poll task is running to drain the staged swap).
      agent::store::setTelegramOffset(0);
      agent::telegram::applyToken(v);
    }
    return true;
  }
  if (n == "clr_tgToken") {
    agent::store::setTelegramToken("");
    agent::store::setVerify("telegram", -1, 0);
    agent::store::setTelegramOffset(0);
    agent::telegram::applyToken("");   // stop polling the old bot immediately
    return true;
  }
  if (n == "tgAllow")   { agent::store::setTelegramAllowlist(v);
                          // Hot-reload (P2.6): the poll task re-reads at its loop top -
                          // an id added here works IMMEDIATELY (used to need a reboot,
                          // unlike /api/telegram/add which always reloaded).
                          agent::telegram::reloadAllowlist(); return true; }
  // Head multi-turn tool-use loop (P6: default ON, the turn path). HUMAN-only surface -
  // the model's config action can't reach these (not whitelisted benign keys).
  if (n == "orchLoop")     { agent::store::setOrchToolLoop(v == "1" || v == "true"); return true; }
  if (n == "midFail")      { agent::store::setMidTurnFailover(v == "1" || v == "true"); return true; }
  if (n == "orchTrace")    { agent::store::setOrchTrace(v == "1" || v == "true"); return true; }
  if (n == "ttsOn")        { agent::store::setTtsEnabled(v == "1" || v == "true"); return true; }
  if (n == "loopRounds")   { agent::store::setOrchLoopRounds(v.toInt()); return true; }
  if (n == "loopRescap")   { agent::store::setOrchLoopResultCap(v.toInt()); return true; }  // per-tool-result byte cap (dialable for stress)
  if (n == "loopTotcap")   { agent::store::setOrchLoopTotalCap(v.toInt()); return true; }   // cumulative tool-output byte budget
  // Local Loops governor caps (routines) - owner may only TIGHTEN; reloadCaps() applies live.
  if (n == "loopMaxCnt")   { agent::store::setLoopCapMaxCount(v.toInt()); agent::loops::reloadCaps(); return true; }
  if (n == "loopMinIvl")   { agent::store::setLoopCapMinIntervalS(v.toInt()); agent::loops::reloadCaps(); return true; }
  if (n == "loopFires")    { agent::store::setLoopCapFiresPerDay(v.toInt()); agent::loops::reloadCaps(); return true; }
  if (n == "loopTokens")   { agent::store::setLoopCapTokensPerDay(v.toInt()); agent::loops::reloadCaps(); return true; }
  if (n == "loopDevTok")   { agent::store::setLoopCapDevTokensPerDay(v.toInt()); agent::loops::reloadCaps(); return true; }
  if (n == "loopDevFir")   { agent::store::setLoopCapDevFiresWindow(v.toInt()); agent::loops::reloadCaps(); return true; }
  if (n == "compactKB")    { agent::store::setCompactAtKB((uint16_t)v.toInt()); return true; }
  if (n == "tlsSlots")     { agent::store::setTlsSlots(v.toInt()); return true; }   // latched at boot (arbiter::begin)
  if (n == "tlsVerify")    { agent::store::setTlsVerify(v == "1" || v == "true"); return true; }   // live: next TLS connect honours it
  if (n == "capProbe")     { agent::store::setCapProbe(v.toInt()); return true; }   // W3b: 0 off / 1 passive / 2 active
  if (n == "fetchPol")     { agent::store::setFetchPolicy(v.toInt()); return true; } // W18: 0 off/1 approve/2 scan/3 yolo
  if (n == "modInbound")   { agent::store::setModInbound(v == "1" || v == "true"); return true; }   // CUM-69 gates (non-admin)
  if (n == "modOutbound")  { agent::store::setModOutbound(v == "1" || v == "true"); return true; }
  if (n == "modInjection") { agent::store::setModInjection(v == "1" || v == "true"); return true; }
  if (n == "capProbeH")    { agent::store::setCapProbeHours(v.toInt()); return true; }   // active re-verify interval (hours)
  if (n == "loopDeadline") { agent::store::setOrchLoopDeadlineS(v.toInt()); return true; }
  // Sound cues: per-mode sound levels + shared sound theme (clamped/validated).
  // constrain BEFORE the uint8_t cast - a raw (uint8_t)"260" wraps to 4 then clamps
  // to 3, and "256" wraps to 0 (silence), both reporting success on garbage input.
  if (n == "sfxLvlN")   { agent::store::setSfxLevelNotif((uint8_t)constrain(v.toInt(), 0, 3)); ::sfx::refreshConfig(); return true; }
  if (n == "sfxLvlO")   { agent::store::setSfxLevelOrch((uint8_t)constrain(v.toInt(), 0, 3));  ::sfx::refreshConfig(); return true; }
  if (n == "sfxVol")    { agent::store::setSfxVolume((uint8_t)constrain(v.toInt(), 0, 100));   ::sfx::refreshConfig(); return true; }
  if (n == "sfxTheme")  {
    if (v == "pulse") {
      agent::store::setSfxTheme(v);
      ::sfx::refreshConfig();
    }
    return true;
  }
  if (n == "tchCal")    {
    // Touch calibration, measured per unit (see nimbus/touch_cal.h). Validated
    // with the SAME parser the console uses so the two surfaces cannot drift,
    // and applied live - recalibrating should not need a restart.
    if (v.length() == 0) {
      agent::store::setTouchCal("");
      // Restore the driver defaults LIVE. Persisting alone left g_cal mapping
      // through the discarded calibration until a restart while the page said
      // "Saved" - and this is the recovery path an owner reaches for right
      // after mis-calibrating. A default-constructed Calibration IS the default.
      solide::touch::setCalibration(solide::touch::Calibration{});
      return true;
    }
    nimbus::touch::Cal c;
    // ⚠ Return FALSE on a bad value. Returning true made the endpoint answer
    // {"ok":true} and the page toast "Saved" for a calibration the device had
    // just rejected - telling the owner a change happened that did not.
    if (!nimbus::touch::parseCal(std::string(v.c_str()), c)) return false;
    agent::store::setTouchCal(v);
    solide::touch::Calibration sc;
    sc.minX = c.minX; sc.maxX = c.maxX;
    sc.minY = c.minY; sc.maxY = c.maxY;
    sc.swapXY = c.swapXY; sc.invertX = c.invertX; sc.invertY = c.invertY;
    solide::touch::setCalibration(sc);
    return true;
  }
  if (n == "scrModel")  {
    // Which panel is fitted. Persist only - the display and input drivers are
    // bound once at boot, so this deliberately has NO live side effect; the UI
    // tells the owner to restart. Reject unknown slugs (returning true for a
    // value we dropped would report a change that never happened).
    // A fixed-panel board (no e-paper option) can only be "tft"; reject a switch
    // to eink rather than brick the display until the next reflash.
    if (solide::board().epd.sck < 0 && v != "tft") return false;
    if (v != "eink" && v != "tft") return false;
    agent::store::setScreenModel(v);
    return true;
  }
  return false;
}

// ---- routes -----------------------------------------------------------------
void beginWeb(const WebConfig& wc) {
  s_wc = wc;

  // Bring up the orchestrator memory subsystem (loads persisted vectors +
  // scratchpad, binds the embedder, registers memory.* tools). Both modes: a
  // Notifier-mode device can still browse/edit its memory from the dashboard.
  agent::memory::begin();

  s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    // On the password-gated setup AP, land the browser authenticated even when it
    // navigates STRAIGHT to the AP IP (not via the captive-portal probe that
    // onNotFound handles) - otherwise a fresh device shows the identify gate with
    // no token to paste, and the onboarding wizard is unreachable. Mirror the
    // onNotFound rule exactly: AP-only (never leak the token to a STA/LAN peer),
    // and always the device's OWN token (never reflect a client-supplied ?t=).
    // Hand out the token on the AP ONLY while UNPROVISIONED (parity with the
    // /savewifi bootstrap exception): first-run setup stays one-tap, but once WiFi
    // is saved, an AP peer knowing the shipped password no longer gets full control
    // for free - it hits the identify gate like any LAN peer. (webAuthToken() is
    // stable, so this only changes WHEN the token is auto-supplied, not its value.)
    const bool onAp = r->client() && isApInterface(r->client()->localIP());
    const bool provisioned = solide::memory::getString(NIMBUS_KEY_STA_SSID, "").length() > 0;
    if (onAp && !provisioned && !(r->hasParam("t"))) {
      r->redirect(String("/?t=") + agent::store::webAuthToken().c_str());
      return;
    }
    sendConfigPage(r);
  });

  // The dotted-ring mark (generated PROGMEM SVG - tools/logo/gen_logo.py).
  // Deliberately UNGATED like "/": it is the page favicon and the identify
  // gate's logo, both needed before a token exists; a logo is not sensitive.
  // Cacheable - it only changes with a firmware flash.
  // --- Cloud tunnel (cumulo-nimbus) --------------------------------------------
  // GET returns the status object; POST performs an action (optin/optout/pair/unpair).
  // Token-gated like every other state-changing endpoint.
  s_server.on("/api/cloud", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    JsonDocument d;
    nimbus::relay::statusInto(d.to<JsonObject>());
    String out;
    serializeJson(d, out);
    r->send(200, "application/json", out);
  });
  s_server.on("/api/cloud", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String action = r->hasParam("action", true) ? r->getParam("action", true)->value() : "";
    if (action == "optin") {
      nimbus::relay::requestOptIn(true);
    } else if (action == "optout") {
      nimbus::relay::requestUnpair();
      nimbus::relay::requestOptIn(false);
    } else if (action == "pair") {
      nimbus::relay::requestOptIn(true);
      nimbus::relay::requestPair();
    } else if (action == "unpair") {
      nimbus::relay::requestUnpair();
    } else {
      r->send(400, "application/json", "{\"error\":\"bad_action\"}");
      return;
    }
    r->send(200, "application/json", "{\"ok\":true}");
  });

  s_server.on("/logo.svg", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res =
        r->beginResponse_P(200, "image/svg+xml", (const uint8_t*)UI_LOGO_SVG,
                           strlen_P(UI_LOGO_SVG));
    res->addHeader("Cache-Control", "max-age=86400");
    r->send(res);
  });

  s_server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;  // strict gate: NO data before identification (owner R2)
    String s; buildState(s);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", s);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  // Token-gated "how to connect to this device" bundle. Returns the setup-AP
  // PASSWORD and the auth TOKEN, which are deliberately kept OFF the open
  // /api/state (the token is the auth secret; the AP password lets you join the
  // setup network). Only a caller that already holds the token (scanned the QR)
  // can read them - an unauthenticated GET gets 401, and the page then shows a
  // "scan the QR" hint instead. The open state fields (name/ssid/mdns) stay on
  // /api/state; this endpoint completes the picture for an authenticated owner.
  s_server.on("/api/connect", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const bool sta = staConnected();
    const String tok  = agent::store::webAuthToken();
    const String host = sta && staIp().length() ? staIp() : apIp();
    JsonDocument d;
    d["name"]   = sys::deviceName();
    d["apSsid"] = apSsid();
    d["apPass"] = nimbus::net::apPass();
    d["mdns"]   = mdnsName();
    d["ip"]     = sta ? staIp() : "";
    d["apIp"]   = apIp();
    d["token"]  = tok;
    d["url"]    = String("http://") + host + "/?t=" + tok;
    // A SECOND token-bearing URL for the mDNS name. The browser stores the token per
    // ORIGIN (localStorage), and http://<ip> and http://<name>.local are different
    // origins - so a token minted at the IP does NOT sign you in at nimbus.local, which
    // read as "the IP works but the name asks me to identify again". Handing out both
    // lets either address sign in with one click. mDNS only runs once STA is up.
    if (sta) d["mdnsUrl"] = String("http://") + mdnsName() + "/?t=" + tok;
    // Bluetooth (Notifier link): the ring/e-ink are painted over a bonded BLE link in
    // Notifier mode. macOS hides the central's identity for a custom peripheral, so we
    // can show the bond COUNT + this device's BLE address (for targeting), not "who".
    d["bleOn"]    = net::ble::enabled();
    d["bleConn"]  = net::ble::connected();
    d["bleBonds"] = net::ble::numBonds();
    d["bleMac"]   = net::ble::macAddress();
    String s; serializeJson(d, s);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", s);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  // The onboarding page calls this only AFTER it has learned the STA IP and
  // constructed the token-bearing LAN URL. Main-loop code then gives the HTTP
  // response a few seconds to leave before dropping a TFT board's setup AP.
  // Without this handshake, GOT_IP could cut off /api/state mid-response and
  // strand the owner at a token gate on the new browser origin.
  s_server.on("/api/wifi/handoff", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    if (!staConnected() || !staIp().length()) {
      r->send(409, "application/json", "{\"error\":\"wifi not connected\"}");
      return;
    }
    s_wifiHandoffReady = true;
    JsonDocument d;
    d["ok"] = true;
    d["ip"] = staIp();
    d["dropInMs"] = 4000;
    String s; serializeJson(d, s);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", s);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  // Forget all BLE bonds (Connectivity "Forget paired devices"): every bonded central
  // must re-pair on its next connect. Token-gated; the BLE side is main-task-safe.
  s_server.on("/api/ble/forget", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    net::ble::forgetBonds();
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // --- Local Loops (token-gated like every route) --------------------------
  // GET: the loop table (status; no secrets). POST: STAGE a mutation ({action:
  // create|approve|pause|resume|delete}) drained on tg_poll - loop records are
  // single-writer there, so the AsyncTCP task only ever stages.
  s_server.on("/api/loops", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", agent::loops::loopsJson());
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  s_server.on("/api/loops", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    auto p = [&](const char* k) -> String {
      return r->hasParam(k, true) ? r->getParam(k, true)->value() : String();
    };
    const String action = p("action");
    if (action.length() == 0) { r->send(400, "application/json", "{\"error\":\"action required\"}"); return; }
    JsonDocument d;
    d["action"] = action.c_str();
    if (p("id").length()) d["id"] = p("id").c_str();
    if (action == "create") {
      d["name"]   = p("name").c_str();
      d["prompt"] = p("prompt").c_str();
      d["chatId"] = p("chatId").c_str();
      JsonDocument sd;   // schedule arrives as a JSON string param
      if (deserializeJson(sd, p("schedule")) == DeserializationError::Ok) d["schedule"] = sd;
    }
    String out; serializeJson(d, out);
    agent::loops::stageWebMutation(out);
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // Form-encoded apply. profile/mode AND p_<param>/clr_<param> overrides are all
  // staged here and applied in loopWeb() on the main task (the Config is never
  // written from this AsyncTCP task). Any mutation marks the config dirty so
  // loopWeb() fires onChanged() to persist after draining the staged edits.
  s_server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    bool touched = false;

    if (r->hasParam("profile", true)) {
      int p = r->getParam("profile", true)->value().toInt();
      if (p >= 0 && p < nimbus::kProfileCount) {
        s_pendProf = p; s_haveProf = true; touched = true;
      }
    }
    if (r->hasParam("mode", true)) {
      int m = r->getParam("mode", true)->value().toInt();
      if (m == 0 || m == 1) { s_pendMode = m; s_haveMode = true; touched = true; }
    }
    // Device identity (P2): sanitize + persist directly (NVS is internally
    // mutexed - same pattern as the /api/orch store writes). Reboot-to-apply:
    // the AP SSID / mDNS / BLE name re-derive on the next boot. An empty value
    // clears the name so the next boot re-runs first-boot auto-numbering.
    if (r->hasParam("devName", true)) {
      sys::saveDeviceName(r->getParam("devName", true)->value());
      touched = true;
    }
    // Timezone (POSIX string, e.g. "GMT0BST,M3.5.0/1,M10.5.0/2"; blank = UTC).
    // NVS write here (internally mutexed, devName pattern) - but the APPLY is
    // staged to tg_poll: loop records are single-writer there, and setenv/tzset
    // must not race that task's civil-time math. Write-then-stage ordering
    // guarantees the drain reads the new value.
    if (r->hasParam("devTz", true)) {
      String tz = r->getParam("devTz", true)->value();
      tz.trim();
      if (tz.length() > 48) tz = tz.substring(0, 48);
      // Only apply on a REAL change: a no-op re-save (the field is pre-filled,
      // so a reflexive Save is one click) would otherwise rebase every
      // wall-clock loop and erase a due-but-not-yet-fired one (prism).
      if (tz != agent::store::deviceTz()) {
        agent::store::setDeviceTz(tz);
        agent::loops::stageWebMutation("{\"action\":\"tzapply\"}");
        touched = true;
      }
    }
    // "Sync now" - re-kick SNTP (rate-limited in the drain). No immediate ack
    // is possible (sync is async); the UI watches clock.synced on the poll.
    if (r->hasParam("clockSync", true)) {
      agent::loops::stageWebMutation("{\"action\":\"sntp\"}");
      touched = true;
    }
    // Battery/LED protection knobs (owner feature 2026-07-17). NVS-mutexed direct
    // writes, live-applied by the main loop within one tick (cap) / one telemetry
    // tick (sleep threshold). The overrides carry REAL risk - the UI explains it -
    // and are deliberately settable by both the human here and the AI (config
    // action, whose schema requires informing the owner).
    if (r->hasParam("sleepMv", true)) {
      agent::store::setSleepMv(uint16_t(r->getParam("sleepMv", true)->value().toInt()));
      touched = true;
    }
    if (r->hasParam("wakeMv", true)) {
      agent::store::setWakeMv(uint16_t(r->getParam("wakeMv", true)->value().toInt()));
      touched = true;
    }
    // Battery HARDWARE (divider resistors + pack capacity). Applied live on the
    // main task via g_battHwReconfig. ⚠ a divider change re-scales every mV, so the
    // BATTCAL anchor goes stale - the UI tells the owner to re-Calibrate.
    bool battHw = false;
    if (r->hasParam("battRtop", true)) {
      agent::store::setBattRtop(uint32_t(strtoul(r->getParam("battRtop", true)->value().c_str(), nullptr, 10)));
      battHw = true;
    }
    if (r->hasParam("battRbot", true)) {
      agent::store::setBattRbot(uint32_t(strtoul(r->getParam("battRbot", true)->value().c_str(), nullptr, 10)));
      battHw = true;
    }
    if (r->hasParam("battCapMah", true)) {
      agent::store::setBattCapMah(uint16_t(r->getParam("battCapMah", true)->value().toInt()));
      battHw = true;
    }
    if (r->hasParam("battChem", true)) {
      agent::store::setBattChem(r->getParam("battChem", true)->value());   // "liion"|"lifepo4"
      battHw = true;
    }
    if (r->hasParam("battCells", true)) {
      agent::store::setBattCells(uint8_t(r->getParam("battCells", true)->value().toInt()));  // 1/2, 0=board
      battHw = true;
    }
    if (r->hasParam("battCurve", true)) {
      // Store only a valid custom curve (or "" to clear); a bad string never lands.
      String cv = r->getParam("battCurve", true)->value();
      nimbus::power::LiIonCurvePoint pts[nimbus::power::kMaxCurvePoints];
      if (cv.length() == 0 || nimbus::power::parseCurveCsv(cv.c_str(), pts, nimbus::power::kMaxCurvePoints) >= 2)
        agent::store::setBattCurve(cv);
      battHw = true;
    }
    if (battHw) { s_battHwPending = true; touched = true; }
    if (r->hasParam("sleepOvr", true)) {
      agent::store::setSleepOvr(r->getParam("sleepOvr", true)->value().toInt() != 0);
      touched = true;
    }
    if (r->hasParam("brightOvr", true)) {
      agent::store::setBrightOvr(r->getParam("brightOvr", true)->value().toInt() != 0);
      touched = true;
    }
    if (r->hasParam("scrFlip", true)) {
      // Display mounted 180 deg round (TFT only). setFlip re-arms MADCTL with no
      // reset, so it applies live; forceRepaint makes the next frame land even
      // when the composed pixels are unchanged. Persisted for the next boot too.
      const bool on = r->getParam("scrFlip", true)->value().toInt() != 0;
      agent::store::setTftFlip(on);
      if (agent::store::screenModel() == "tft") {
        solide::display_tft::setFlip(on);
        hw::tft::forceRepaint();
      }
      touched = true;
    }
    // Low-battery light (default OFF). s_ringRefresh is REQUIRED: compose() only
    // re-runs on some other trigger otherwise, so the toggle would appear to do
    // nothing until an unrelated event repainted the ring.
    if (r->hasParam("lbRing", true)) {
      agent::store::setLowBattRing(r->getParam("lbRing", true)->value().toInt() != 0);
      s_ringRefresh = true;
      touched = true;
    }
    // Low battery switches the battery mode (default ON - shipped behaviour).
    // No s_ringRefresh needed: the power tick re-syncs and re-resolves the profile
    // level-triggered within 2 s, which repaints on its own.
    if (r->hasParam("lbSaver", true)) {
      agent::store::setLowBattSaver(r->getParam("lbSaver", true)->value().toInt() != 0);
      touched = true;
    }
    // Battery monitoring on/off. Applied at boot (the ADC is brought up in setup),
    // so a change takes effect after restart - the UI says so.
    if (r->hasParam("battMon", true)) {
      agent::store::setBattMon(r->getParam("battMon", true)->value().toInt() != 0);
      touched = true;
    }
    // OTA auto-install knob (device-level, both modes). Handled HERE on /api/config
    // (not applyOrchField) because the web Firmware panel POSTs it here and OTA is
    // not orchestrator-gated. Default OFF; the idle-window gate still applies.
    if (r->hasParam("autoUpd", true)) {
      agent::store::setOtaAutoUpdate(r->getParam("autoUpd", true)->value().toInt() != 0);
      touched = true;
    }
    // Every remaining param is a p_<n>/clr_<n> override field.
    const size_t np = r->params();
    for (size_t i = 0; i < np; i++) {
      const AsyncWebParameter* prm = r->getParam(i);
      if (!prm->isPost()) continue;
      if (applyParam(prm->name(), prm->value())) touched = true;
    }

    if (touched) s_dirty = true;
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // ---- orchestrator control surface (ROUND 3 Part A) ----
  s_server.on("/api/orch", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;  // strict gate: NO data before identification (owner R2)
    String s; buildOrchState(s);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", s);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  // Daily per-provider usage buckets for the Usage-pane graphs (owner: spend over
  // time + estimated price). Raw counts only - the $ math happens client-side from
  // the editable rates, so the stored data stays honest. The payload can reach tens
  // of KB at the bucket cap, so it is built in PSRAM and STREAMED chunked (review
  // HIGH: an internal String + AsyncBasicResponse copy would spike internal heap by
  // 2x payload - the near-OOM class v2.5.1 eliminated). The shared_ptr rides the
  // filler closure, so the buffer frees when the response is destroyed on EVERY
  // path (complete or client-aborted), never leaking.
  // Last-turn introspection (owner ask 2026-07-16): the exact system prompt, the
  // exact per-turn input block, and the raw model output of the most recent turn,
  // as ONE plain-text page - open it in a browser tab / curl it to see the raw
  // conversation the model actually received. Contains memories + the directive,
  // so it is token-gated like every other data surface. PSRAM + chunked (the blob
  // can be tens of KB - same rule as the usage history below).
  s_server.on("/api/lastturn", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    size_t len = 0;
    char* raw = agent::orchestrator::lastTurnDebugPs(len);
    if (!raw) { r->send(404, "text/plain", "no turn has run yet"); return; }
    std::shared_ptr<char> body(raw, free);
    AsyncWebServerResponse* res = r->beginChunkedResponse(
        "text/plain; charset=utf-8",
        [body, len](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
          if (index >= len) return 0;
          size_t take = len - index;
          if (take > maxLen) take = maxLen;
          memcpy(buf, body.get() + index, take);
          return take;
        });
    res->addHeader("Cache-Control", "no-store");
    res->addHeader("X-Content-Type-Options", "nosniff");   // untrusted model/tool text, match /api/mem/blob
    r->send(res);
  });
  // Glass Box P3: one PAST turn's full anatomy (/api/lastturn is a single RAM
  // slot the next turn overwrites). The path is built server-side from a
  // validated turn id - never from client text. The 404 body names WHY, so the
  // chat can be honest instead of showing a blank panel.
  s_server.on("/api/trace", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    if (!r->hasParam("turn")) { r->send(400, "text/plain", "turn required"); return; }
    const String turn = r->getParam("turn")->value();
    size_t len = 0;
    char* raw = agent::memory::readTraceFilePs(turn, len);
    if (!raw) {
      if (!agent::store::orchTrace())
        r->send(404, "text/plain",
                "off: activity recording is off, so this turn wasn't captured");
      else if (!agent::memory::traceActive())
        r->send(404, "text/plain",
                "nosd: no SD card, so turn details can't be stored");
      else
        r->send(404, "text/plain",
                "evicted: this turn's details were removed to save space");
      return;
    }
    std::shared_ptr<char> body(raw, free);
    AsyncWebServerResponse* res = r->beginChunkedResponse(
        "text/plain; charset=utf-8",
        [body, len](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
          if (index >= len) return 0;
          size_t take = len - index;
          if (take > maxLen) take = maxLen;
          memcpy(buf, body.get() + index, take);
          return take;
        });
    res->addHeader("Cache-Control", "no-store");
    res->addHeader("X-Content-Type-Options", "nosniff");   // untrusted model/tool text, match /api/mem/blob
    r->send(res);
  });
  s_server.on("/api/usage/history", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    size_t len = 0;
    char* raw = agent::store::usageHistoryJsonPs(len);
    if (!raw) { r->send(500, "application/json", "{\"error\":\"oom\"}"); return; }
    std::shared_ptr<char> body(raw, free);
    AsyncWebServerResponse* res = r->beginChunkedResponse(
        "application/json",
        [body, len](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
          if (index >= len) return 0;   // complete
          size_t take = len - index;
          if (take > maxLen) take = maxLen;
          memcpy(buf, body.get() + index, take);
          return take;
        });
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  s_server.on("/api/orch", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    bool touched = false, cfgDirty = false;
    const size_t np = r->params();
    for (size_t i = 0; i < np; i++) {
      const AsyncWebParameter* prm = r->getParam(i);
      if (!prm->isPost()) continue;
      if (applyOrchField(prm->name(), prm->value(), cfgDirty)) touched = true;
    }
    // A directive edit is consumed by the orchestrator at its next turn-task
    // drain; a token/allowlist change needs a reboot (the poll task captured
    // them at begin) - the page surfaces that hint.
    if (cfgDirty) agent::orchestrator::noteConfigChanged();
    r->send(touched ? 200 : 400, "application/json",
            touched ? "{\"ok\":true}" : "{\"error\":\"no known field\"}");
  });

  // Stage a live ring preview: profile=<0..kProfileCount-1>. Applied + reverted
  // on the main task (main.cpp startPreview()); never touches Config, but it
  // CHANGES device behavior (drives the ring) - token-gated like every mutation.
  s_server.on("/api/preview", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    if (!r->hasParam("profile", true)) {
      r->send(400, "application/json", "{\"error\":\"profile required\"}");
      return;
    }
    int p = r->getParam("profile", true)->value().toInt();
    if (p < 0 || p >= nimbus::kProfileCount) {
      r->send(400, "application/json", "{\"error\":\"bad profile\"}");
      return;
    }
    // Optional status to demo (0..5 = solide::ring::Status); absent/-1 => the
    // default two-arc showcase. Lets the ring simulator's status picker drive the
    // on-device demo, not just the theme.
    int st = r->hasParam("status", true)
                 ? r->getParam("status", true)->value().toInt() : -1;
    s_pendPreview = p;
    s_pendPreviewStatus = st;
    s_havePreview = true;
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // ---- connectors (Phase C) - OWNER-ONLY registry of per-provider external
  // tools (MCP servers / OpenAI first-party connectors / Mistral built-ins).
  // GET returns a SANITIZED view (secrets never echoed - tok/oauth become
  // has-flags); POST replaces the whole blob (form param `blob`, token-gated).
  // The model cannot touch this: `connector` is a protected config key.
  s_server.on("/api/connectors", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;  // strict gate: NO data before identification (owner R2)
    JsonDocument in;
    JsonDocument out;
    // configured[]: the owner's blob, SANITIZED (secrets -> has-flags).
    JsonArray arr = out["configured"].to<JsonArray>();
    if (!deserializeJson(in, agent::store::connectorsJson())) {
      for (JsonObjectConst c : in.as<JsonArrayConst>()) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = c["name"]; o["prov"] = c["prov"]; o["kind"] = c["kind"];
        o["url"]  = c["url"];  o["cid"]  = c["cid"];  o["en"]   = c["en"];
        o["type"] = (const char*)(c["type"] | "");
        o["hasTok"]   = ((const char*)(c["tok"] | ""))[0] != 0;
        o["hasOauth"] = !c["oauth"].isNull();
      }
    }
    // known[]: the Tier-1 + built-ins catalog (descriptions/links) so the UI can
    // offer not-yet-configured connectors and join metadata onto configured ones.
    JsonDocument known;
    if (!deserializeJson(known, agent::connectors::knownCatalog())) out["known"] = known;
    // Provider state so the client can badge availability (own-turn vs sub-agent).
    JsonObject keyed = out["keyed"].to<JsonObject>();
    keyed["openai"]    = agent::store::hasOpenaiKey();
    keyed["anthropic"] = agent::store::hasAnthropicKey();
    keyed["mistral"]   = agent::store::hasMistralKey();
    String host = agent::store::orchHost();
    if (!host.length()) {  // first token of the priority list (same rule as the turn)
      String pri = agent::store::providerPriority();
      int e = 0;
      while (e < (int)pri.length() && isalpha((unsigned char)pri[e])) e++;
      host = pri.substring(0, e);
    }
    out["host"] = host;
    String s; serializeJson(out, s);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", s);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  s_server.on("/api/connectors", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    // Three modes, all token-gated:
    //   blob=<array>    whole-blob replace (the Advanced raw editor)
    //   del=<name>      remove one connector by name
    //   patch=<object>  upsert ONE connector by name - secrets (tok/oauth) are
    //                   PRESERVED from the stored entry when the patch omits them,
    //                   so a card edit never has to round-trip a secret.
    auto param = [&](const char* n) -> String {
      return r->hasParam(n, true) ? r->getParam(n, true)->value() : String();
    };
    String blob = param("blob");
    if (blob.length()) {
      JsonDocument d;
      if (blob.length() > 3500 || deserializeJson(d, blob) || !d.is<JsonArray>()) {
        r->send(400, "application/json", "{\"error\":\"blob must be a JSON array (<=3500B)\"}");
        return;
      }
      agent::store::setConnectorsJson(blob);
      agent::store::setOrchConvId("");  // connectors pin at conversation creation
      r->send(200, "application/json", "{\"ok\":true}");
      return;
    }

    // Load the current blob (default to []), then del/upsert into it.
    JsonDocument cur;
    if (deserializeJson(cur, agent::store::connectorsJson()) || !cur.is<JsonArray>())
      cur.to<JsonArray>();
    JsonArray arr = cur.as<JsonArray>();

    String del = param("del");
    if (del.length()) {
      for (size_t i = 0; i < arr.size(); i++)
        if (del == (const char*)(arr[i]["name"] | "")) { arr.remove(i); break; }
    } else {
      String patch = param("patch");
      JsonDocument pd;
      if (!patch.length() || deserializeJson(pd, patch) || !pd.is<JsonObject>()) {
        r->send(400, "application/json", "{\"error\":\"patch must be a JSON object\"}");
        return;
      }
      const char* nm = pd["name"] | "";
      if (!nm[0]) { r->send(400, "application/json", "{\"error\":\"patch needs a name\"}"); return; }
      JsonObject dst;                       // find existing (preserve its secrets) or append
      for (JsonObject o : arr)
        if (del.length() == 0 && strcmp(nm, (const char*)(o["name"] | "")) == 0) { dst = o; break; }
      if (dst.isNull()) dst = arr.add<JsonObject>();
      // Copy patch fields; tok/oauth only overwrite when explicitly provided.
      for (JsonPairConst kv : pd.as<JsonObjectConst>()) {
        if ((!strcmp(kv.key().c_str(), "tok") || !strcmp(kv.key().c_str(), "oauth")) &&
            (kv.value().isNull() || (kv.value().is<const char*>() && !kv.value().as<const char*>()[0])))
          continue;                          // blank secret -> keep the stored one
        dst[kv.key()] = kv.value();
      }
    }

    String outBlob; serializeJson(cur, outBlob);
    if (outBlob.length() > 3500) {
      r->send(400, "application/json", "{\"error\":\"connector set too large (<=3500B)\"}");
      return;
    }
    agent::store::setConnectorsJson(outBlob);
    // Mistral pins connectors at conversation creation, so a changed connector set
    // is otherwise ignored until the conversation resets. Drop the stored convId so
    // the next turn starts a fresh conversation with the new set.
    agent::store::setOrchConvId("");
    r->send(200, "application/json", "{\"ok\":true}");
  });

  s_server.on("/api/verify", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String p = r->hasParam("provider", true) ? r->getParam("provider", true)->value() : "";
    if (p != "openai" && p != "anthropic" && p != "mistral" && p != "telegram" && p != "tavily") {
      r->send(400, "application/json", "{\"error\":\"unknown provider\"}");
      return;
    }
    if (!agent::provider_verify::request(p)) {
      r->send(409, "application/json", "{\"error\":\"verify busy, retry\"}");
      return;
    }
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // Finish first-run onboarding. Token-gated. Refuses unless BOTH hard gates are
  // met - WiFi joined AND >=1 verified LLM provider - so the wizard can't be
  // dismissed before the device is actually usable. Everything else the wizard
  // walks through (mode, Telegram, voice, device name) is skippable and needs no
  // gate here. Sets the plain-NVS 'onboarded' flag; buildState() then reports
  // needsOnboarding:false and the overlay never shows again.
  s_server.on("/api/onboard/complete", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    if (!staConnected()) {
      r->send(409, "application/json", "{\"error\":\"wifi not connected\"}");
      return;
    }
    bool anyVerified = agent::store::verifyResult("openai") == 1 ||
                       agent::store::verifyResult("anthropic") == 1 ||
                       agent::store::verifyResult("mistral") == 1;
    if (!anyVerified) {
      r->send(409, "application/json", "{\"error\":\"no verified provider\"}");
      return;
    }
    agent::store::setOnboarded(true);
    // Tell the wizard whether a restart is still needed: the chosen panel type
    // (stored) vs the driver actually bound at boot. Deciding it here (not from a
    // possibly-stale client /api/state snapshot) is robust to the TFT Wi-Fi
    // handoff, which reloads the wizard and would otherwise lose the choice.
    int boot = agent::store::bootScreenIsTft();
    bool restart = boot >= 0 && (agent::store::screenIsTft() != (boot == 1));
    r->send(200, "application/json",
            String("{\"ok\":true,\"restart\":") + (restart ? "true" : "false") + "}");
  });

  // Restart to apply the wizard's display choice (scrModel binds its display +
  // input drivers ONCE at boot, so a changed panel type needs a restart - the
  // wizard offers it right after /api/onboard/complete). Token-gated and
  // non-destructive: nothing is erased, saved settings are untouched.
  s_server.on("/api/onboard/restart", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    r->send(202, "application/json", "{\"ok\":true,\"rebooting\":true}");
    s_onbRestartPending = true;
  });

  // Rotate the web/MCP auth token (owner-triggered from the UI). Authenticated with the
  // CURRENT token; returns the NEW one so the calling browser keeps its session. Every
  // OTHER browser is logged out and must re-scan the Config QR (Connectivity).
  s_server.on("/api/token/regen", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String t = agent::store::regenWebAuthToken();
    r->send(200, "application/json", String("{\"ok\":true,\"token\":\"") + t + "\"}");
  });

  // Web chat: POST a message -> runs one orchestrator turn on the poll task; GET polls
  // for the reply. Orchestrator-mode only (Notifier has no turn engine). Token-gated.
  // W18: the URL-download queue - list + owner approve/deny (the web counterpart
  // of Telegram /fetch). Token-gated; mutations are mutex-safe from this task.
  s_server.on("/api/fetchq", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    r->send(200, "application/json", String(agent::files::fetchQueueJson().c_str()));
  });
  s_server.on("/api/fetchq", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    uint32_t id = r->hasParam("id", true)
                      ? (uint32_t)r->getParam("id", true)->value().toInt() : 0;
    String op = r->hasParam("op", true) ? r->getParam("op", true)->value() : "";
    bool ok = false;
    if (op == "approve") ok = agent::files::fetchApprove(id);
    else if (op == "deny") ok = agent::files::fetchDeny(id);
    r->send(ok ? 200 : 400, "application/json",
            ok ? "{\"ok\":true}" : "{\"error\":\"no pending request with that id\"}");
  });

  s_server.on("/api/chat", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const int mode = s_wc.currentMode ? s_wc.currentMode() : (int)solide::memory::getInt("mode", 0);
    if (mode != 1) { r->send(409, "application/json", "{\"error\":\"switch to Orchestrator mode to chat\"}"); return; }
    String text = r->hasParam("text", true) ? r->getParam("text", true)->value() : "";
    text.trim();
    if (text.length() == 0) { r->send(400, "application/json", "{\"error\":\"empty message\"}"); return; }
    if (!s_wc.chatSend) { r->send(501, "application/json", "{\"error\":\"unsupported\"}"); return; }
    if (!s_wc.chatSend(text)) {
      r->send(503, "application/json",
              "{\"error\":\"busy - the device is finishing a reply; send that again in a moment\"}");
      return;
    }
    r->send(200, "application/json", "{\"ok\":true}");
  });
  s_server.on("/api/chat", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String reply = s_wc.chatPoll ? s_wc.chatPoll() : String();
    // `pending` = a turn is RUNNING, not merely "no reply queued" (the resting
    // state). Falls back to the old inference when the hook is absent.
    const bool pending = reply.length() == 0 &&
                         (s_wc.chatPending ? s_wc.chatPending() : true);
    JsonDocument d; d["reply"] = reply; d["pending"] = pending;
    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  // Factory reset: wipe ALL config (Wi-Fi, keys, allowlist, BLE bonds, memory config,
  // token) and reboot to first-boot. Token-gated AND requires an explicit confirm phrase
  // so a stray/replayed POST can't brick the device. The actual erase is deferred to the
  // main loop via the hook (NVS erase on the AsyncTCP task is unsafe).
  s_server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String confirm = r->hasParam("confirm", true) ? r->getParam("confirm", true)->value() : "";
    if (confirm != "FACTORY RESET") {
      r->send(400, "application/json", "{\"error\":\"confirm phrase required\"}");
      return;
    }
    if (!s_wc.factoryReset) { r->send(501, "application/json", "{\"error\":\"unsupported\"}"); return; }
    r->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    s_wc.factoryReset();   // sets the deferred flag; main loop erases NVS + reboots
  });

  // SD reset: erase the durable data store (/mem - vector memories, conversation
  // history, saved files, media blobs) but KEEP config (Wi-Fi, keys, token). Same
  // confirm-phrase + deferred-to-main-loop pattern as factory reset (the recursive
  // SD delete + reboot must not run on the AsyncTCP task).
  s_server.on("/api/sdreset", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String confirm = r->hasParam("confirm", true) ? r->getParam("confirm", true)->value() : "";
    if (confirm != "ERASE STORAGE") {
      r->send(400, "application/json", "{\"error\":\"confirm phrase required\"}");
      return;
    }
    if (!s_wc.sdReset) { r->send(501, "application/json", "{\"error\":\"unsupported\"}"); return; }
    r->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    s_wc.sdReset();
  });

  // TTS voice catalog for the picker: OpenAI static; Mistral live from its
  // /v1/audio/voices (fetched + cached on a background task, never blocks here).
  s_server.on("/api/voices", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;  // strict gate: NO data before identification (owner R2)
    String prov = r->hasParam("provider") ? r->getParam("provider")->value() : String("mistral");
    AsyncWebServerResponse* res =
      r->beginResponse(200, "application/json", agent::ttsvoices::voicesJson(prov));
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  // ---- Telegram access management (P8): chips + first-message approval + public
  // mode. Reads are open (no secrets); mutations are token-gated. Applied LIVE by
  // the poll task (reloadAllowlist) - no reboot for allowlist/public changes.
  // Effective owner check (web-task side; mirrors telegram::isOwner from NVS). An
  // owner is an allow-listed id that is in `owners` - or, if `owners` is empty, the
  // FIRST allow-list entry (so an un-configured device keeps a single working owner).
  static auto tgIsOwner = [](const String& id, const String& allow, const String& owners) -> bool {
    bool inAllow = false; String firstId; int idx = 0, s = 0;
    while (s < (int)allow.length()) {
      int e = allow.indexOf(',', s); if (e < 0) e = allow.length();
      String t = allow.substring(s, e); t.trim();
      if (t.length()) { if (idx == 0) firstId = t; if (t == id) inAllow = true; idx++; }
      s = e + 1;
    }
    if (!inAllow) return false;
    String ow = owners; ow.trim();
    if (ow.length() == 0) return firstId == id;
    s = 0;
    while (s < (int)ow.length()) {
      int e = ow.indexOf(',', s); if (e < 0) e = ow.length();
      String t = ow.substring(s, e); t.trim();
      if (t == id) return true;
      s = e + 1;
    }
    return false;
  };
  s_server.on("/api/telegram", HTTP_GET, [](AsyncWebServerRequest* r) {
    // Token-gated GET (unlike open status GETs): the pending list carries inbound
    // message PREVIEWS from third parties - owner-only management data (prism).
    if (authBlocked(r)) return;
    // Current allowlist as chips {id,name} + pending approvals + public flag.
    String al = agent::store::telegramAllowlist();
    String names = agent::store::telegramNames();   // "id:name,..."
    String owners = agent::store::telegramOwners();
    String out = "{\"public\":";
    out += agent::store::telegramPublic() ? "true" : "false";
    out += ",\"hasToken\":";
    out += agent::store::telegramToken().length() ? "true" : "false";
    out += ",\"allow\":[";
    int s = 0; bool first = true;
    while (s < (int)al.length()) {
      int e = al.indexOf(',', s); if (e < 0) e = al.length();
      String id = al.substring(s, e); id.trim();
      if (id.length()) {
        // Exact-id entry walk (the old indexOf(id+":") substring match hit the
        // FIRST occurrence - a stale pre-rename entry, and id "12" matched "312:").
        String nm;
        int ns = 0;
        while (ns < (int)names.length()) {
          int ne = names.indexOf(',', ns); if (ne < 0) ne = names.length();
          String entry = names.substring(ns, ne); entry.trim(); ns = ne + 1;
          int colon = entry.indexOf(':');
          if (colon > 0 && entry.substring(0, colon) == id) nm = entry.substring(colon + 1);
          // keep scanning: the LAST entry wins (upsert appends the newest mapping)
        }
        if (!first) out += ',';
        out += "{\"id\":\"" + id + "\",\"name\":\"" + nm + "\",\"owner\":" + (tgIsOwner(id, al, owners) ? "true" : "false") + "}";
        first = false;
      }
      s = e + 1;
    }
    out += "],\"pending\":" + agent::telegram::pendingJson() + "}";
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", out);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  s_server.on("/api/telegram/add", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String id = r->hasParam("id", true) ? r->getParam("id", true)->value() : "";
    String nm = r->hasParam("name", true) ? r->getParam("name", true)->value() : "";
    id.trim();
    if (!id.length()) { r->send(400, "application/json", "{\"error\":\"id required\"}"); return; }
    agent::telegram::approvePending(id, nm);   // approve == add (idempotent) + reload
    r->send(200, "application/json", "{\"ok\":true}");
  });
  s_server.on("/api/telegram/remove", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String id = r->hasParam("id", true) ? r->getParam("id", true)->value() : "";
    id.trim();
    // Rebuild the allowlist without `id`, persist, reload.
    String al = agent::store::telegramAllowlist(), out;
    int s = 0;
    while (s < (int)al.length()) {
      int e = al.indexOf(',', s); if (e < 0) e = al.length();
      String t = al.substring(s, e); t.trim();
      if (t.length() && t != id) out += (out.length() ? "," : "") + t;
      s = e + 1;
    }
    agent::store::setTelegramAllowlist(out);
    // Keep owners ⊆ allowlist: prune `id` from tgOwners too, so a removed-then-
    // re-added chat comes back as a plain member (not a silently-resurrected owner).
    // Only rewrite when an EXPLICIT owners list exists - an empty list is the
    // first-entry default and must stay empty.
    String ow = agent::store::telegramOwners(); ow.trim();
    if (ow.length()) {
      String ownOut; int os = 0;
      while (os < (int)ow.length()) {
        int e = ow.indexOf(',', os); if (e < 0) e = ow.length();
        String t = ow.substring(os, e); t.trim();
        if (t.length() && t != id) ownOut += (ownOut.length() ? "," : "") + t;
        os = e + 1;
      }
      if (ownOut != ow) agent::store::setTelegramOwners(ownOut);
    }
    agent::telegram::reloadAllowlist();
    // Drop the RBAC row too. Safe in THIS order only: they are already off the
    // allowlist, so roleOfChat's "allow-listed => User" fallback cannot re-grant
    // access to a chat with no row. Leaving the row would slowly fill a bounded
    // table with people who can no longer talk to the device.
    // If the row cannot go (they are the last admin), say so - silently leaving
    // an Admin row behind means re-adding that chat later silently restores
    // admin rights nobody granted. Falling back to an explicit `unknown` row
    // keeps them denied either way.
    std::string terr;
    if (!agent::orchestrator::tenantRemove(std::string(id.c_str()), terr)) {
      std::string rerr;
      agent::orchestrator::tenantSetRole(std::string(id.c_str()),
                                         nimbus::orch::Role::Unknown, rerr);
      JsonDocument e;
      e["ok"] = true;
      e["note"] = terr.empty() ? "access revoked" : terr;
      String o; serializeJson(e, o);
      r->send(200, "application/json", o);
      return;
    }
    r->send(200, "application/json", "{\"ok\":true}");
  });
  s_server.on("/api/telegram/rename", HTTP_POST, [](AsyncWebServerRequest* r) {
    // Rename an allowlisted chat's display name (owner ask: label who sent what in
    // the unified chat). Display-only sidecar - no allowlist/reload side effects.
    if (authBlocked(r)) return;
    String id = r->hasParam("id", true) ? r->getParam("id", true)->value() : "";
    String nm = r->hasParam("name", true) ? r->getParam("name", true)->value() : "";
    id.trim();
    if (!id.length()) { r->send(400, "application/json", "{\"error\":\"id required\"}"); return; }
    nm.replace(",", " "); nm.replace(":", " ");   // same sanitize as approvePending
    if (nm.length() > 32) nm = nm.substring(0, 32);
    agent::store::replaceTelegramName(id, nm);
    r->send(200, "application/json", "{\"ok\":true}");
  });
  // Set an allow-listed chat's ROLE: owner (may change settings / install OTA) vs
  // member (conversational turns only). Rebuilds the owners list from the current
  // effective set (materializing the "first entry" default so promoting a 2nd owner
  // never silently demotes the 1st). Refuses to leave zero owners.
  // v3.7.0 RBAC: roles + per-tenant quotas. Writes go to the SAME TenantStore the
  // conversational tenant.* tools use (staged onto tg_poll, the single writer),
  // so the web and the assistant can never disagree about who may do what.
  s_server.on("/api/tenant", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    JsonDocument d;
    JsonArray arr = d["tenants"].to<JsonArray>();
    size_t admins = 0;
    for (const auto& t : agent::orchestrator::tenantSnapshot(&admins)) {
      const auto q = nimbus::orch::effectiveQuota(t.role, t.quota);
      JsonObject o = arr.add<JsonObject>();
      o["id"]      = t.chatId;
      o["role"]    = nimbus::orch::roleName(t.role);
      o["vectors"] = q.maxVectors;
      o["bytes"]   = q.maxBytes;
      o["ttl"]     = q.maxTtlHours;
      o["pins"]    = q.maxPins;
    }
    d["admins"] = (unsigned)admins;
    String out; serializeJson(d, out);
    r->send(200, "application/json", out);
  });
  s_server.on("/api/tenant", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    auto pv = [&](const char* k) {
      return r->hasParam(k, true) ? r->getParam(k, true)->value() : String();
    };
    String id = pv("id"); id.trim();
    if (!id.length()) { r->send(400, "application/json", "{\"error\":\"id required\"}"); return; }
    const std::string sid(id.c_str());
    std::string err;
    // Remove the row entirely, freeing a slot (the table is bounded, so a device
    // that only ever revokes would fill with ghosts). What the person stored is
    // left alone - see tenantRemove.
    //
    // ⚠ Removing a row is NOT the same as revoking. roleOfChat falls back to
    // "allow-listed ⇒ User" for a chat with no row, so deleting the row of
    // someone still on the Telegram allowlist would silently RESTORE their
    // access. Revoke = an explicit `unknown` row; remove = only once they are
    // off the allowlist too.
    if (pv("remove") == "1") {
      if (onFreshAllowlist(id)) {
        r->send(409, "application/json",
                "{\"error\":\"still on the Telegram allowlist - remove them there first, "
                "or set their role to unknown to revoke access\"}");
        return;
      }
      if (!agent::orchestrator::tenantRemove(sid, err)) {
        JsonDocument e; e["error"] = err; String o; serializeJson(e, o);
        r->send(err == "no such tenant" ? 404 : 409, "application/json", o); return;
      }
      r->send(200, "application/json", "{\"ok\":true}");
      return;
    }
    String roleS = pv("role");
    if (roleS.length()) {
      nimbus::orch::Role role;
      if (!nimbus::orch::roleFromName(std::string(roleS.c_str()), role)) {
        r->send(400, "application/json", "{\"error\":\"bad role\"}"); return;
      }
      // No pre-seeding: setRole upserts, so without this an admin (or a stolen
      // token) could plant role=admin on an id that has never been approved, and
      // the normal approval flow would later hand it admin rights nobody granted.
      // Revoking (unknown) is always allowed - it only ever removes access.
      if (role != nimbus::orch::Role::Unknown && !onFreshAllowlist(id)) {
        r->send(409, "application/json",
                "{\"error\":\"that chat isn't allow-listed yet - approve them first\"}");
        return;
      }
      if (!agent::orchestrator::tenantSetRole(sid, role, err)) {
        JsonDocument e; e["error"] = err; String o; serializeJson(e, o);
        r->send(409, "application/json", o); return;   // e.g. the last admin
      }
    }
    if (pv("vectors").length() || pv("bytes").length() ||
        pv("ttl").length() || pv("pins").length()) {
      nimbus::orch::Quota q;
      agent::orchestrator::tenantQuotaOf(sid, q);   // start from what is set today
      // toInt() is SIGNED: "ttl=-1" would store 0xFFFFFFFF, and clampTtl reads
      // that back as -1 == "never expires" - the most restrictive input becoming
      // the most permissive setting. Refuse rather than wrap.
      auto num = [&](const char* k, long& out) -> bool {
        const String v = pv(k);
        if (!v.length()) return false;
        const long n = v.toInt();
        if (n < 0 || n > 2147483647L) { out = -1; return true; }
        out = n; return true;
      };
      long n = 0;
      bool bad = false;
      if (num("vectors", n)) { if (n < 0) bad = true; else q.maxVectors  = (uint32_t)n; }
      if (num("bytes", n))   { if (n < 0) bad = true; else q.maxBytes    = (uint32_t)n; }
      if (num("ttl", n))     { if (n < 0) bad = true; else q.maxTtlHours = (uint32_t)n; }
      if (num("pins", n))    { if (n < 0 || n > 65535) bad = true; else q.maxPins = (uint16_t)n; }
      if (bad) {
        r->send(400, "application/json",
                "{\"error\":\"limits must be 0 or more (0 restores the default for their role)\"}");
        return;
      }
      if (!agent::orchestrator::tenantSetQuota(sid, q, err)) {
        JsonDocument e; e["error"] = err; String o; serializeJson(e, o);
        r->send(404, "application/json", o); return;
      }
    }
    r->send(200, "application/json", "{\"ok\":true}");
  });

  s_server.on("/api/telegram/role", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String id = r->hasParam("id", true) ? r->getParam("id", true)->value() : "";
    bool owner = r->hasParam("owner", true) && r->getParam("owner", true)->value() == "1";
    id.trim();
    if (!id.length()) { r->send(400, "application/json", "{\"error\":\"id required\"}"); return; }
    String al = agent::store::telegramAllowlist();
    if (owner && !onFreshAllowlist(id)) { r->send(400, "application/json", "{\"error\":\"not allow-listed\"}"); return; }
    String owners = agent::store::telegramOwners(); owners.trim();
    // Materialize the current effective owner set into a comma list `cur`.
    String cur;
    if (owners.length() == 0) { int e = al.indexOf(','); String f = (e < 0) ? al : al.substring(0, e); f.trim(); cur = f; }
    else cur = owners;
    // Rebuild without id, then add if owner.
    String out; int s2 = 0;
    while (s2 < (int)cur.length()) {
      int e = cur.indexOf(',', s2); if (e < 0) e = cur.length();
      String t = cur.substring(s2, e); t.trim();
      if (t.length() && t != id) out += (out.length() ? "," : "") + t;
      s2 = e + 1;
    }
    if (owner) out += (out.length() ? "," : "") + id;
    if (out.length() == 0) { r->send(409, "application/json", "{\"error\":\"at least one owner required\"}"); return; }
    agent::store::setTelegramOwners(out);
    agent::telegram::reloadAllowlist();   // re-reads owners on the poll task
    r->send(200, "application/json", "{\"ok\":true}");
  });
  s_server.on("/api/telegram/approve", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String id = r->hasParam("id", true) ? r->getParam("id", true)->value() : "";
    String nm = r->hasParam("name", true) ? r->getParam("name", true)->value() : "";
    if (!id.length()) { r->send(400, "application/json", "{\"error\":\"id required\"}"); return; }
    agent::telegram::approvePending(id, nm);
    r->send(200, "application/json", "{\"ok\":true}");
  });
  s_server.on("/api/telegram/deny", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String id = r->hasParam("id", true) ? r->getParam("id", true)->value() : "";
    agent::telegram::denyPending(id);
    r->send(200, "application/json", "{\"ok\":true}");
  });
  s_server.on("/api/telegram/public", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    bool on = r->hasParam("on", true) && (r->getParam("on", true)->value().toInt() != 0);
    agent::store::setTelegramPublic(on);
    agent::telegram::reloadAllowlist();
    r->send(200, "application/json", String("{\"public\":") + (on ? "true" : "false") + "}");
  });

  // Device health (P5): the Home-tab panel. Passive snapshot; the caller (this
  // net layer) supplies the BLE/WiFi/battery bits the aggregator can't see.
  s_server.on("/api/health", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;  // strict gate: NO data before identification (owner R2)
    agent::health::Env env;
    env.wifiKnown = true;
    env.wifiUp = staConnected();
    env.wifiRssi = env.wifiUp ? rssi() : 0;
    const int mode = s_wc.currentMode ? s_wc.currentMode() : (int)solide::memory::getInt("mode", 0);
    if (mode == 0) {   // BLE only runs in Notifier mode
      env.bleAvail = true;
      env.bleOn = net::ble::enabled();
      env.bleConnected = net::ble::connected();
    }
    power::Sample b = s_wc.power ? s_wc.power->sample() : power::Sample{};
    env.battValid = b.valid; env.battPct = b.percent; env.battExt = b.onExternalPower;
    AsyncWebServerResponse* res =
      r->beginResponse(200, "application/json", agent::health::reportJson(env).c_str());
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  // LED theme palettes, served from the SINGLE source of truth (nimbus::theme,
  // P4) so the page never duplicates the RGB values. [{name,colors:[[r,g,b],...]}]
  s_server.on("/api/themes", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;  // strict gate: NO data before identification (owner R2)
    JsonDocument d;
    // {themes:[{name,colors}], roles:[{status,role,alert,anim,desc}]} - the roles
    // block comes straight from nimbus::statusStyle so the web legend can NEVER
    // drift from the device's status->style table (it used to be a hand-copied
    // JS array). role -1 = the theme's alert hue (error).
    JsonArray arr = d["themes"].to<JsonArray>();
    {
      struct Row { const char* label; solide::ring::Status st; const char* desc; };
      static const Row rows[] = {
        {"Running",     solide::ring::Status::Running,          "model / tool working"},
        {"Needs input", solide::ring::Status::WaitingInput,     "waiting on YOU"},
        {"Approval",    solide::ring::Status::AwaitingApproval, "decision / permission gate"},
        {"Done",        solide::ring::Status::Done,             "finished, settling"},
        {"Error",       solide::ring::Status::Error,            "errored (theme alert hue - red family is RESERVED for this)"},
      };
      auto animName = [](solide::ring::Anim a) -> const char* {
        switch (a) {
          case solide::ring::Anim::Comet:   return "comet";
          case solide::ring::Anim::Breathe: return "breathe";
          case solide::ring::Anim::Blink:   return "blink";
          case solide::ring::Anim::Fade:    return "fade";
          case solide::ring::Anim::Off:     return "off";
          default:                          return "solid";
        }
      };
      JsonArray roles = d["roles"].to<JsonArray>();
      for (const Row& row : rows) {
        const nimbus::StatusStyle ss = nimbus::statusStyle(row.st);
        JsonObject o = roles.add<JsonObject>();
        o["status"] = row.label;
        o["role"]   = ss.alert ? -1 : ss.roleIdx;
        o["anim"]   = animName(ss.anim);
        o["desc"]   = row.desc;
      }
    }
    std::string list = nimbus::themeList();
    size_t start = 0;
    while (start <= list.size()) {
      size_t comma = list.find(',', start);
      std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      if (!name.empty()) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = name;
        JsonArray cs = o["colors"].to<JsonArray>();
        nimbus::ThemeColor pal[nimbus::kThemeMaxColors];
        int n = nimbus::themePalette(name, pal, nimbus::kThemeMaxColors);
        for (int i = 0; i < n; i++) {
          JsonArray c = cs.add<JsonArray>();
          c.add(pal[i].r); c.add(pal[i].g); c.add(pal[i].b);
        }
      }
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    String s; serializeJson(d, s);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", s);
    res->addHeader("Cache-Control", "max-age=3600");   // palettes are build-constant
    r->send(res);
  });

  // ---- Audio diagnostics (Device tab: mic VU meter + speaker + acoustic loopback).
  // All POST (so the auth token rides automatically) + token-gated: they actuate the
  // mic/speaker, so a LAN peer shouldn't be able to sample the room or blare the amp.
  // Each records/plays SYNCHRONOUSLY on the AsyncTCP task - bounded to a few hundred
  // ms, which only delays other web requests (no WDT, no crash); fine for a diagnostic
  // the owner starts/stops by hand. Buffers are static: the async server runs handlers
  // one at a time, so there is no re-entrancy, and this keeps them off the small task
  // stack and out of the scarce internal heap.
  s_server.on("/api/audio/mic", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    if (nimbus::fault::active(nimbus::fault::MIC)) {            // resilience: simulated dead mic
      r->send(200, "application/json", "{\"ok\":false,\"faulted\":true,\"rms\":0,\"peak\":0,\"samples\":0}");
      return;
    }
    // PSRAM per use (a 3.8 KB static internal buffer forever, for a meter, is
    // exactly the class of allocation that starved TLS - see the beep note).
    int16_t* buf = (int16_t*)heap_caps_malloc(1920 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) { r->send(200, "application/json", "{\"ok\":false}"); return; }
    size_t n = solide::audio::recordToBuffer(buf, 1920, 120, nullptr);
    double sq = 0; int peak = 0;
    for (size_t i = 0; i < n; i++) { int v = buf[i]; sq += (double)v * v; int a = v < 0 ? -v : v; if (a > peak) peak = a; }
    int rms = n ? (int)sqrt(sq / (double)n) : 0;
    free(buf);
    agent::health::recordMicSample(n > 0, millis());   // feed the health verdict
    JsonDocument d;
    d["ok"] = n > 0; d["rms"] = rms; d["peak"] = peak; d["samples"] = (uint32_t)n;
    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  });
  s_server.on("/api/audio/beep", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    if (nimbus::fault::active(nimbus::fault::SPEAKER)) {        // resilience: simulated dead amp
      r->send(200, "application/json", "{\"ok\":false,\"faulted\":true}");
      return;
    }
    // PSRAM, allocated per use: a static internal buffer here cost 9.6 KB of
    // the ~25 KB resting internal heap FOREVER for a diagnostics beep (measured
    // live 2026-07-11 - it starved TLS handshakes). playPcm copies into DMA
    // buffers, so a PSRAM source is fine.
    int16_t* tone = (int16_t*)heap_caps_malloc(4800 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!tone) { r->send(200, "application/json", "{\"ok\":false}"); return; }
    for (int i = 0; i < 4800; i++)
      tone[i] = (int16_t)(18000.0 * sinf(2.0 * M_PI * 880.0 * i / 16000.0));
    bool ok = solide::audio::playPcm(tone, 4800, 16000);   // 300 ms @ 16 kHz, 880 Hz
    free(tone);
    agent::health::recordBeep(ok, millis());               // feed the health verdict
    JsonDocument d; d["ok"] = ok;
    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  // SFX test-play: queue a sound clip by event slug on the sfx task
  // (non-blocking, unlike the diagnostic beep). Bypasses the level gate - a
  // test affordance; the speaker fault + voice mute still apply.
  s_server.on("/api/audio/sfx", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String slug;
    if (const AsyncWebParameter* p = r->getParam("slug", true)) slug = p->value();
    const bool ok = ::sfx::play(slug.c_str());
    r->send(ok ? 200 : 400, "application/json",
            ok ? "{\"ok\":true}" : "{\"error\":\"unknown slug\"}");
  });
  // ---- self-test / health check ---------------------------------------------
  // GET = the silent set (safe, unauthenticated read like /api/state).
  // A SCREENSHOT of the colour panel: the exact bytes currently on the glass
  // (the dirty-gate snapshot, not a re-render), so it cannot disagree with what
  // the owner sees. Renders with `python3 tools/tftpreview.py render`.
  //
  // This exists because every other check of this UI is indirect - host goldens
  // prove the RASTERISER agrees with itself, and RENDER? only names the screen.
  // Neither can catch a panel showing the wrong thing.
  s_server.on("/api/screenshot", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!webAuthOk(r)) { r->send(401, "text/plain", "Unauthorized. Scan the Config QR to sign in."); return; }
    const uint8_t* fb = nimbus::hw::tft::lastFrame();
    if (!fb) {
      r->send(404, "text/plain",
              "No colour frame yet. This device is on the e-ink panel, or nothing has been drawn.");
      return;
    }
    const size_t len = nimbus::hw::tft::lastFrameBytes();
    AsyncWebServerResponse* res = r->beginChunkedResponse(
        "application/octet-stream",
        [fb, len](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
          if (index >= len) return 0;
          size_t take = len - index;
          if (take > maxLen) take = maxLen;
          memcpy(buf, fb + index, take);
          return take;
        });
    {
      char hdr[32];
      snprintf(hdr, sizeof hdr, "rgb565-be %dx%d",
               int(solide::display_tft::kW), int(solide::display_tft::kH));
      res->addHeader("X-Nimbus-Frame", hdr);
    }
    r->send(res);
  });

  s_server.on("/api/selftest", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;  // strict gate: NO data before identification (owner R2)
    auto items = nimbus::hw::runNow(false);
    AsyncWebServerResponse* res =
        r->beginResponse(200, "application/json", nimbus::hw::selfTestJson(items));
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  // POST full = also the AUDIBLE tests (token-gated + silent/owner gated).
  s_server.on("/api/ota/check", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const bool ok = otaupd::requestCheck();
    r->send(ok ? 202 : 409, "application/json",
            ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"busy or gated\"}");
  });
  s_server.on("/api/ota/apply", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const bool dry   = r->hasParam("dry", true)   && r->getParam("dry", true)->value() == "1";
    const bool force = r->hasParam("force", true) && r->getParam("force", true)->value() == "1";
    const char* why = "";
    const bool ok = otaupd::requestInstall(dry, force, &why);
    String body = ok ? String("{\"ok\":true}")
                     : String("{\"ok\":false,\"err\":\"") + why + "\"}";
    r->send(ok ? 202 : 409, "application/json", body);
  });
  s_server.on("/api/selftest/full", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const bool allowed = !::sfx::isSilent() && agent::store::allowHwTests();
    auto items = nimbus::hw::runNow(allowed);
    String body = nimbus::hw::selfTestJson(items);
    if (!allowed)   // strip the trailing "}" and note why audible was skipped
      body = body.substring(0, body.length() - 1) +
             ",\"audibleSkipped\":\"device silent or owner-disabled\"}";
    r->send(200, "application/json", body);
  });

  // Owner asserts the pack is full NOW -> anchor 100% to the current reading and
  // persist it (corrects the S3 ADC top-band under-read where a true 8.4 V pack
  // reads ~7.9 V -> ~76 %; see docs/hardware.md). Token-gated + STAGED: the battery
  // model is main-task-owned, so loopWeb() applies it on the next tick - the page
  // re-fetches /api/state to see batt.calibrated=true + percent=100. This is the
  // production-reachable equivalent of the TEST-only BATTCAL console command.
  s_server.on("/api/battcal", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    s_battCalPending = true;
    r->send(200, "application/json", "{\"ok\":true,\"queued\":true}");
  });

  // POST /api/battreset - discard LEARNED battery analytics (discharge-rate EWMA +
  // the as-new health baselines), keeping the BATTCAL anchor. The recovery path after
  // a drain campaign taught the model a synthetic ~1.5 A load: that state persists to
  // NVS, so re-flashing the exclusion fix does NOT by itself heal an already-poisoned
  // device (live: Board 2 projected ~4-6 h from a FULL pack after the curve run).
  // Staged + applied on the main task, which owns the battery model.
  s_server.on("/api/battreset", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    s_battResetPending = true;
    r->send(200, "application/json", "{\"ok\":true,\"queued\":true}");
  });

  // POST /api/storage - discharge to a storage SoC (pct=<40-95>, default 70) then hold;
  // pct=0 cancels. Production feature (prep a full pack for long-term shelf storage).
  // Staged + applied on the main task (owns the LEDs + battery model).
  s_server.on("/api/storage", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    const int pct = r->hasParam("pct", true) ? r->getParam("pct", true)->value().toInt() : 70;
    // ⚠ VALIDATE HERE, not only on the main task. This used to answer
    // {"ok":true,"queued":true} unconditionally while storageSet() later refused an
    // out-of-range pct on the main task and threw the reason away - so a host was told
    // "armed" by a device that had refused, and (battlab, live) instantly concluded the
    // storage target was already reached. Same range as storageSet(): 0 = cancel,
    // 40-95 = a real storage target (parking a li-ion below 40 % damages it).
    if (pct != 0 && (pct < 40 || pct > 95)) {
      r->send(400, "application/json",
              "{\"ok\":false,\"error\":\"pct must be 0 (cancel) or 40-95 - storage mode "
              "prepares a pack for shelf storage; to discharge lower use a drain run\"}");
      return;
    }
    s_storagePct = pct;
    s_storagePending = true;
    r->send(200, "application/json", "{\"ok\":true,\"queued\":true}");
  });

  s_server.on("/api/audio/loopback", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    if (nimbus::fault::active(nimbus::fault::MIC) || nimbus::fault::active(nimbus::fault::SPEAKER)) {
      r->send(200, "application/json", "{\"ok\":false,\"faulted\":true,\"tonePresent\":false}");
      return;
    }
    solide::audio::LbDiag diag{};
    bool tone = solide::audio::loopbackSelfTest(1000, nullptr, nullptr, &diag);
    agent::health::recordLoopback(tone, millis());   // P5: freshens the passive mic/speaker verdict
    JsonDocument d;
    d["ok"] = true; d["tonePresent"] = tone;
    d["toneMag"] = diag.toneMag; d["ctrlMag"] = diag.ctrlMag;
    d["rms"] = diag.rms; d["peak"] = diag.peak;
    d["dcMean"] = diag.dcMean; d["samples"] = diag.samples;
    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  });

#ifdef NIMBUS_TEST
  // Resilience fault injection (TEST builds ONLY - never compiled into production).
  // POST cap=<sd|memory|mic|speaker|led|screen|all> & on=<0|1> marks a capability
  // simulated-absent so its degraded path runs on demand, no hardware unplugged.
  // Token-gated (it changes device behavior). The HIL suite drives this over the LAN
  // (reliable), the FAULT console command drives the same registry over serial.
  // v3.6.0 fold + max-memory HIL seams over the LAN (same rationale as /api/fault:
  // serial CDC opens reset the board and wedge the host driver - HTTP is the
  // reliable transport). All TEST-only + token-gated.
  // Address a turn to ANY chat id (the multi-principal suites): /api/chat is
  // hardcoded to "web", and the privacy assertions need several distinct
  // principals. Rides the same inbound queue as every other injected message,
  // and reports a full queue rather than dropping (D3).
  s_server.on("/api/test/inject", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String chat = r->hasParam("chat", true) ? r->getParam("chat", true)->value() : "";
    String text = r->hasParam("text", true) ? r->getParam("text", true)->value() : "";
    chat.trim(); text.trim();
    if (!chat.length() || !text.length()) {
      r->send(400, "application/json", "{\"error\":\"need chat + text\"}"); return;
    }
    if (!agent::telegram::injectMessage(chat, text)) {
      r->send(503, "application/json", "{\"error\":\"inbound queue full\"}"); return;
    }
    r->send(202, "application/json", "{\"ok\":true}");
  });
  // Non-destructive restart: persistence + migration proofs need a reboot, and
  // the only other route is a serial open - which resets the board anyway AND
  // wedges the host CDC driver. Deferred one tick so the response is sent.
  // Grid-validation seam (owner ask 2026-08-07, after the server_label 400):
  // return the EXACT connector tool JSON the firmware's attach builders produce,
  // per provider, over the FULL known catalog - so a host-side grid can validate
  // every (provider x connector) shape against the real provider APIs without a
  // parallel Python re-implementation that could drift from this code.
  s_server.on("/api/test/connwire", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String prov = r->hasParam("prov") ? r->getParam("prov")->value() : "";
    if (prov != "openai" && prov != "mistral" && prov != "anthropic") {
      r->send(400, "application/json", "{\"error\":\"prov: openai|mistral|anthropic\"}");
      return;
    }
    // Synthesize an all-enabled ConnectorInfo list from the known catalog,
    // targeting `prov` where the catalog says it can attach. MCP URLs are not
    // part of the catalog (owner-configured), so known public defaults ride
    // here purely for shape validation.
    int n = 0;
    const nimbus::orch::KnownConnector* k = nimbus::orch::knownConnectors(n);
    std::vector<nimbus::orch::ConnectorInfo> cs;
    for (int i = 0; i < n; i++) {
      if (String(k[i].providers).indexOf(prov) < 0) continue;
      nimbus::orch::ConnectorInfo c;
      c.name = k[i].id;
      c.prov = prov.c_str();
      c.kind = k[i].kind;
      c.connectorId = k[i].connectorId;
      c.type = k[i].id;
      if (String(k[i].kind) == "mcp") {
        c.url = !strcmp(k[i].id, "github") ? "https://api.githubcopilot.com/mcp/"
              : !strcmp(k[i].id, "notion") ? "https://mcp.notion.com/mcp"
              : !strcmp(k[i].id, "linear") ? "https://mcp.linear.app/mcp"
                                           : "https://example.invalid/mcp";
      }
      c.enabled = true;
      cs.push_back(std::move(c));
    }
    JsonDocument d;
    if (prov == "openai")        nimbus::orch::attachOpenAIWire(d, cs, nullptr);
    else if (prov == "mistral")  nimbus::orch::attachMistralWire(d, cs);
    else                         nimbus::orch::attachAnthropicWire(d, cs, nullptr);
    String out; serializeJson(d, out);
    r->send(200, "application/json", out.length() ? out : "{}");
  });
  s_server.on("/api/test/reboot", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    r->send(202, "application/json", "{\"ok\":true,\"rebooting\":true}");
    agent::orchestrator::stageTestReboot();
  });
  s_server.on("/api/test/memfill", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String kind = r->hasParam("kind", true) ? r->getParam("kind", true)->value() : "";
    int n     = r->hasParam("n", true) ? r->getParam("n", true)->value().toInt() : 0;
    int bytes = r->hasParam("bytes", true) ? r->getParam("bytes", true)->value().toInt() : 64;
    String text = r->hasParam("text", true) ? r->getParam("text", true)->value() : "";
    int added = -1;
    if (kind == "epi")      added = agent::memory::testFillEpisodic(n, bytes,
                                        text.length() ? text.c_str() : nullptr);
    else if (kind == "vec") added = agent::memory::testFillVectors(n);
    if (added < 0) { r->send(400, "application/json", "{\"error\":\"kind: epi|vec\"}"); return; }
    r->send(200, "application/json", String("{\"added\":") + added + "}");
  });
  s_server.on("/api/test/compact", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String chat = r->hasParam("chat", true) ? r->getParam("chat", true)->value() : "";
    if (!chat.length()) { r->send(400, "application/json", "{\"error\":\"need chat\"}"); return; }
    agent::orchestrator::stageManualFold(chat.c_str());
    r->send(202, "application/json", "{\"ok\":true}");
  });
  // Act as a given chat against the real tool surface. The point of this seam is
  // that it does NOT re-implement the principal - it calls the same
  // principalForRole(chat, roleOfChat(chat)) the turn path builds, so a test that
  // proves chat B cannot read chat A's memory here proves the production rail,
  // not a parallel one. Without it the only way to exercise a second tenant is a
  // real LLM turn per assertion (slow, and it spends the owner's credits).
  s_server.on("/api/test/astool", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String chat = r->hasParam("chat", true) ? r->getParam("chat", true)->value() : "";
    String body = r->hasParam("body", true) ? r->getParam("body", true)->value() : "";
    if (!chat.length() || !body.length()) {
      r->send(400, "application/json", "{\"error\":\"need chat + body\"}"); return;
    }
    const std::string id(chat.c_str());
    nimbus::orch::Quota q;
    agent::orchestrator::tenantQuotaOf(id, q);
    const nimbus::orch::Principal who =
        nimbus::orch::principalForRole(id, agent::orchestrator::roleOfChat(chat), q);
    std::string resp = agent::memory::handleMcp(std::string(body.c_str()), who);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", resp.c_str());
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  // Seed one entry into the recent-results ring UNDER `chat`'s owning namespace,
  // so the spill→results.get→scoping round-trip is testable without waiting for a
  // real deep turn to overflow. TEST-only: it writes device state a production
  // build has no path to. Returns the assigned tag; the HIL suite then fetches it
  // back with results.get AS the same chat (present) and AS a foreign chat (the
  // prism CRITICAL namespace-scoping check - a miss).
  s_server.on("/api/test/resultput", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String chat = r->hasParam("chat", true) ? r->getParam("chat", true)->value() : "";
    String text = r->hasParam("text", true) ? r->getParam("text", true)->value() : "";
    if (!chat.length() || !text.length()) {
      r->send(400, "application/json", "{\"error\":\"need chat + text\"}"); return;
    }
    String kind = r->hasParam("kind", true) ? r->getParam("kind", true)->value() : "tool";
    String name = r->hasParam("name", true) ? r->getParam("name", true)->value() : "seed";
    const std::string id(chat.c_str());
    nimbus::orch::Quota q;
    agent::orchestrator::tenantQuotaOf(id, q);
    const nimbus::orch::Principal who =
        nimbus::orch::principalForRole(id, agent::orchestrator::roleOfChat(chat), q);
    std::string tag = agent::orchestrator::resultsPut(
        kind.c_str(), std::string(name.c_str()), std::string(text.c_str()),
        std::string(), who.ns);
    JsonDocument d;
    d["tag"] = tag.c_str();
    d["ns"] = who.ns.c_str();
    String out; serializeJson(d, out);
    r->send(200, "application/json", out);
  });
  // The bullets that WOULD be injected into `chat`'s prompt, via the same
  // memory::recall(text, k, who) the turn's ComposeInputs hook calls. This is
  // the last line of defence for the privacy suite: every tool-layer gate can
  // be correct and the device still leak if the assembled context pulls rows in
  // on a path that skips the tool layer. Asserting on the recall output tests
  // the bytes that actually reach the model.
  s_server.on("/api/test/recall", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String chat = r->hasParam("chat", true) ? r->getParam("chat", true)->value() : "";
    String q    = r->hasParam("q", true) ? r->getParam("q", true)->value() : "";
    if (!chat.length() || !q.length()) {
      r->send(400, "application/json", "{\"error\":\"need chat + q\"}"); return;
    }
    const std::string id(chat.c_str());
    nimbus::orch::Quota quota;
    agent::orchestrator::tenantQuotaOf(id, quota);
    const nimbus::orch::Principal who =
        nimbus::orch::principalForRole(id, agent::orchestrator::roleOfChat(chat), quota);
    JsonDocument d;
    JsonArray arr = d["bullets"].to<JsonArray>();
    for (const auto& b : agent::memory::recall(q, 0, who)) arr.add(b);
    String out; serializeJson(d, out);
    r->send(200, "application/json", out);
  });
  s_server.on("/api/test/ctx", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String chat = r->hasParam("chat") ? r->getParam("chat")->value() : "";
    r->send(200, "text/plain",
            agent::orchestrator::foldStatusText(chat.length() ? chat.c_str() : nullptr));
  });

  s_server.on("/api/fault", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    String cap = r->hasParam("cap", true) ? r->getParam("cap", true)->value() : "";
    const bool on = r->hasParam("on", true) ? (r->getParam("on", true)->value().toInt() != 0) : true;
    if (cap == "all" || cap == "clear") {
      nimbus::fault::clearAll();
      agent::memory::applyConfig();                               // restore full tier cap
    } else {
      nimbus::fault::Cap c;
      if (!nimbus::fault::parse(cap.c_str(), c)) {
        r->send(400, "application/json", "{\"error\":\"unknown capability\"}");
        return;
      }
      nimbus::fault::set(c, on);
      if (c == nimbus::fault::SD) agent::memory::applyConfig();   // re-apply degraded cap live
    }
    JsonDocument d;
    const uint16_t m = nimbus::fault::mask();
    JsonObject fj = d["faults"].to<JsonObject>();
    for (uint8_t i = 0; i < nimbus::fault::COUNT; i++)
      fj[nimbus::fault::name(nimbus::fault::Cap(i))] = (m & (1u << i)) != 0;
    String s; serializeJson(d, s);
    r->send(200, "application/json", s);
  });

  // POST /api/drain - battery drain campaign (on=<0|1>, deep=<0|1>). TEST ONLY: it pins
  // the ring to ~2 A and (deep) suppresses the clean shutdown to run the pack to cutoff,
  // so it must never exist in production. Staged + applied on the main task.
  s_server.on("/api/drain", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    s_drainOn   = r->hasParam("on",   true) && r->getParam("on",   true)->value().toInt() != 0;
    s_drainDeep = r->hasParam("deep", true) && r->getParam("deep", true)->value().toInt() != 0;
    // battlab: optional per-run LED load (clamped device-side to the thermal-safe
    // ceiling in drainSet); -1 = use the firmware default (kDrainBright).
    s_drainBright = r->hasParam("bright", true)
                        ? int(r->getParam("bright", true)->value().toInt()) : -1;
    // battlab HOST DEAD-MAN: seconds the device will hold the load without a
    // refresh. -1 = firmware default, 0 = disarmed. ⚠ a keepalive MUST re-send
    // on=1 (an omitted `on` parses as 0 = drain OFF).
    s_drainTtl = r->hasParam("ttl", true)
                     ? int(r->getParam("ttl", true)->value().toInt()) : -1;
    s_drainPending = true;
    r->send(200, "application/json", "{\"ok\":true,\"queued\":true}");
  });
#endif

  // --- Known Wi-Fi networks -------------------------------------------------
  // The device remembers up to nimbus::wifi::kMaxKnownNetworks networks, so moving
  // it between home / office / a hotspot no longer destroys the credentials that
  // got it online last time. This is the web surface over that list.
  //
  // A PASSWORD IS NEVER RETURNED - not by GET, not in an error, not masked. The
  // store keeps them; `connect` looks one up and hands it straight back to the
  // radio, which is the whole point of remembering a network.
  //
  // GET  /api/wifi[?scan=1]
  //   {"max":5,"count":2,"networks":[{ssid,open,auto,current}],
  //    "sta":true,"staIp":"…","apUp":true,"apSsid":"…","apIp":"…",
  //    "scan":{"scanning":true} | {"scanning":false,"networks":[{ssid,rssi,enc}]}}
  //   `scan` appears only with ?scan=1: scanJson() STARTS a scan when none is
  //   running and CONSUMES a finished one, so a plain list refresh must not call it.
  s_server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    JsonDocument d;
    JsonDocument kd;
    if (deserializeJson(kd, knownNetworksJson()) == DeserializationError::Ok) {
      d["max"]      = kd["max"];
      d["networks"] = kd["networks"];
    }
    d["count"] = knownCount();
    const bool sta = staConnected();
    d["sta"]   = sta;
    d["staIp"] = sta ? staIp() : String("");
    // The setup network, so the page can say where to find the device when the
    // station is down. apIp() reads 0.0.0.0 while the AP is not up.
    const String aip = apIp();
    d["apUp"]   = aip.length() > 0 && aip != "0.0.0.0";
    d["apSsid"] = apSsid();
    d["apIp"]   = aip;
    if (r->hasParam("scan")) {
      JsonDocument sd;
      if (deserializeJson(sd, scanJson()) == DeserializationError::Ok) d["scan"] = sd;
    }
    String s; serializeJson(d, s);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", s);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  // POST /api/wifi - one form-encoded `action` per request:
  //   add       ssid + pass  remember it, then join it
  //   connect   ssid         join a remembered network with its STORED password
  //   forget    ssid         drop it (and its password)
  //   publishap              stop joining and bring the setup network up
  //   resume                 resume joining
  //   scan                   start a radio scan (read the result with ?scan=1)
  // Answers {"ok":true,…} or a 4xx whose "error" is the sentence the page shows.
  s_server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;
    auto p = [&](const char* k) -> String {
      return r->hasParam(k, true) ? r->getParam(k, true)->value() : String();
    };
    auto fail = [&](int code, const String& msg) {
      JsonDocument e;
      e["error"] = msg;
      String o; serializeJson(e, o);
      r->send(code, "application/json", o);
    };
    auto ok = [&](JsonDocument& d) {
      d["ok"] = true;
      String o; serializeJson(d, o);
      r->send(200, "application/json", o);
    };
    String action = p("action"); action.trim();
    // NEITHER ssid NOR pass is trimmed. The store compares SSIDs exactly (802.11
    // names are octet strings), so trimming here would forget or fail to rejoin a
    // network whose name really does carry a space - and a passphrase may contain
    // any character at all.
    const String ssid = p("ssid");
    const String pass = p("pass");

    if (action == "add") {
      if (!ssid.length()) { fail(400, "Enter a network name."); return; }
      // Honest at capacity. The store would evict the least recently joined entry
      // to make room; losing a network the owner still needs, silently, is worse
      // than refusing. Replacing a network already on the list is always allowed.
      if (wifistore::indexOf(ssid) < 0 && knownCount() >= nimbus::wifi::kMaxKnownNetworks) {
        fail(409, String("This device already remembers ") + nimbus::wifi::kMaxKnownNetworks +
                      " networks. Forget one, then add this network.");
        return;
      }
      String evicted;
      if (!addNetwork(ssid, pass, &evicted)) {
        fail(400, "That network name can't be saved. Use 1 to 32 characters.");
        return;
      }
      // ⚠ SAVE, don't switch - when the device is already online.
      //
      // This used to call saveAndConnect() unconditionally, which does
      // `WiFi.disconnect(); WiFi.begin(new)`. So "remember this network" actually
      // meant "leave the network you are on and join this one instead" - and the
      // whole point of saving a second network is that you are usually NOT near it
      // yet. Adding a hotspot as a backup from your home Wi-Fi therefore dropped
      // the device onto a network that was not there, and nothing brought it back:
      // setAutoReconnect does not undo an explicit disconnect, and the policy
      // engine that would try the next candidate (step 8) is not landed. Caught on
      // hardware - the board came back `sta=0 ip=0.0.0.0 reason=201 NO_AP_FOUND`
      // and needed a cable.
      //
      // When the device is OFFLINE, joining immediately is still right: that is
      // first-time provisioning through the setup AP, and there is no connection
      // to protect. `connect` remains the explicit "switch to this one now".
      // Join immediately ONLY for genuine first-time provisioning: the request
      // arrived over the setup AP, or nothing is stored yet.
      //
      // Gating on staConnected() is too fragile - a link drops for a second all
      // the time, and a request landing in that window took the "offline" branch
      // and joined the brand-new network, which is the exact stranding this
      // guard exists to prevent. Board 3 fell through it. Which interface the
      // request arrived on is a fact about the caller, not a race.
      const bool onAp = r->client() && isApInterface(r->client()->localIP());
      const bool joinNow = onAp || !provisioned();
      if (joinNow) saveAndConnect(ssid, pass);
      s_ledConfirm = true;   // creds accepted -> LED confirm blip (P3)
      JsonDocument d;
      if (joinNow) d["joining"] = ssid; else d["saved"] = ssid;
      d["count"]   = knownCount();
      if (evicted.length()) d["evicted"] = evicted;
      ok(d);
      return;
    }

    if (action == "connect") {
      if (!ssid.length()) { fail(400, "Select a network first."); return; }
      const int at = wifistore::indexOf(ssid);
      nimbus::wifi::KnownNet n;
      if (at < 0 || !wifistore::getAt(at, n)) {
        fail(404, "That network isn't saved. Add it with its password first.");
        return;
      }
      saveAndConnect(ssid, String(n.pass.c_str()));   // stored password -> radio, never out
      s_ledConfirm = true;
      JsonDocument d;
      d["joining"] = ssid;
      ok(d);
      return;
    }

    if (action == "forget") {
      if (!ssid.length()) { fail(400, "Select a network first."); return; }
      if (!forgetNetwork(ssid)) {
        fail(404, "That network isn't saved. Refresh the list.");
        return;
      }
      JsonDocument d;
      d["count"] = knownCount();
      ok(d);
      return;
    }

    if (action == "publishap" || action == "resume") {
      s_wifiLinkAction = (action == "publishap") ? 1 : 2;   // applied on the main task
      JsonDocument d;
      d["queued"] = true;
      d["apSsid"] = apSsid();
      ok(d);
      return;
    }

    if (action == "scan") {
      JsonDocument d;
      JsonDocument sd;
      if (deserializeJson(sd, scanJson()) == DeserializationError::Ok) d["scan"] = sd;
      ok(d);
      return;
    }

    fail(400, "That action isn't supported.");
  });

  s_server.on("/scan", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (authBlocked(r)) return;  // strict gate: NO data before identification (owner R2)
    String j = scanJson();
    // A completed scan ({"scanning":false,...}) happens exactly once per scan
    // cycle (scanDelete follows) -> one subtle LED confirm blip (P3).
    if (j.indexOf("\"scanning\":false") >= 0) s_ledConfirm = true;
    r->send(200, "application/json", j);
  });

  s_server.on("/savewifi", HTTP_POST, [](AsyncWebServerRequest* r) {
    // Bootstrap exception: first-time provisioning (no STA saved yet) is allowed WITHOUT
    // the token - it is protected by the setup-AP password and the owner has no token
    // path before joining. Once provisioned, repointing WiFi needs the token so a LAN
    // attacker can't move the device onto a hostile network.
    bool provisioned = solide::memory::getString(NIMBUS_KEY_STA_SSID, "").length() > 0;
    if (provisioned && authBlocked(r)) return;
    String ssid = r->hasParam("ssid", true) ? r->getParam("ssid", true)->value() : "";
    String pass = r->hasParam("pass", true) ? r->getParam("pass", true)->value() : "";
    if (!ssid.length()) { r->send(400, "application/json", "{\"error\":\"ssid required\"}"); return; }
    // Arm BEFORE WiFi.begin(): a fast association must still leave enough time
    // for the browser to receive the exact LAN IP + carry its auth token across
    // origins. E-ink ignores the flag because its setup AP remains available.
    s_wifiJoinStarted = true;
    saveAndConnect(ssid, pass);
    s_ledConfirm = true;   // creds accepted -> LED confirm blip (P3)
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // Orchestrator memory dashboard + LAN MCP endpoint (Part B Ph3/Ph4). Kept in
  // its own module (src/net/web_memory) so this shell stays thin.
  registerMemoryRoutes(s_server);

  // E1 artifact store: /api/files (list) /upload /dl /rm - token-gated, SD-backed.
  registerFileRoutes(s_server);

  // Dynamic skills (roadmap P2): /api/skills/list /get /save /delete - token-
  // gated owner CRUD over the SD capsules (the model can read, never write).
  registerSkillRoutes(s_server);

  // Captive-portal catch-all: unknown paths land on the config page.
  s_server.onNotFound([](AsyncWebServerRequest* r) {
    // Captive-portal catch-all. Redirect to the config page WITH the token so the OS
    // mini-browser (which hits probe URLs like /generate_204, not /?t=) lands
    // AUTHENTICATED - else its first POST 401s and the owner has to paste the token by
    // hand. But ONLY on the password-gated SoftAP interface: the server listens on
    // 0.0.0.0, so this catch-all also answers on the joined STA/LAN, where echoing the
    // device token to any curl'ing peer would defeat the whole per-device token gate
    // (prism HIGH). On STA/LAN we redirect to bare "/" with no token. And we NEVER
    // reflect the client's own ?t= param - always the device's own token - so a
    // CRLF-laden query value can't split the Location header (prism response-splitting).
    // AP token handout is UNPROVISIONED-only (see the "/" handler): a provisioned
    // device with the shipped AP password no longer leaks its full-control token to
    // anyone in RF range - provisioned AP peers land on the token-less identify gate.
    const bool onAp = r->client() && isApInterface(r->client()->localIP());
    const bool provisioned = solide::memory::getString(NIMBUS_KEY_STA_SSID, "").length() > 0;
    if (onAp && !provisioned)
      r->redirect(String("/?t=") + agent::store::webAuthToken().c_str());
    else
      r->redirect("/");
  });

  s_server.begin();
}

bool consumeRingRefresh() {
  if (!s_ringRefresh) return false;
  s_ringRefresh = false;
  return true;
}

bool consumeLedConfirm() {
  if (!s_ledConfirm) return false;
  s_ledConfirm = false;
  return true;
}

bool consumeWifiJoinStarted() {
  if (!s_wifiJoinStarted) return false;
  s_wifiJoinStarted = false;
  return true;
}

bool consumeWifiHandoffReady() {
  if (!s_wifiHandoffReady) return false;
  s_wifiHandoffReady = false;
  return true;
}

bool consumeAuthQrRequest() {
  // Main-task only (like every consume*). 3 fails inside the rolling 60 s
  // window trip the QR; a 5-min re-show guard stops refresh churn if a
  // token-less script keeps hammering the API.
  static uint32_t s_qrShownMs = 0;   // main-task local - no volatile needed
  if (s_authFails < 3) return false;
  const uint32_t now = millis();
  s_authFails = 0;
  if (s_qrShownMs != 0 && now - s_qrShownMs < 300000UL) return false;
  s_qrShownMs = now;
  return true;
}

void loopWeb() {
  // Apply staged profile/mode switches on the main task (they may re-init ring
  // hardware), then fire the persistence callbacks once.
  if (s_haveProf) {
    s_haveProf = false;
    if (s_wc.selector) s_wc.selector->setUser((ProfileId)s_pendProf);
  }
  if (s_haveMode) {
    s_haveMode = false;
    if (s_wc.onModeChanged) s_wc.onModeChanged(s_pendMode);
  }
  if (s_havePreview) {
    s_havePreview = false;
    if (s_wc.onPreview) s_wc.onPreview(s_pendPreview, s_pendPreviewStatus);
  }
  if (s_battCalPending) {
    s_battCalPending = false;
    if (s_wc.calibrateBatteryFull) s_wc.calibrateBatteryFull();
  }
  if (s_battResetPending) {
    s_battResetPending = false;
    if (s_wc.resetBatteryLearning) s_wc.resetBatteryLearning();
  }
  if (s_battHwPending) {
    s_battHwPending = false;
    if (s_wc.reconfigureBattery) s_wc.reconfigureBattery();
  }
  if (s_storagePending) {
    s_storagePending = false;
    if (s_wc.setStorage) s_wc.setStorage(s_storagePct);
  }
  // POST /api/wifi publishap|resume - they re-point the radio and can restart the
  // captive DNS server that process() pumps from this same task, so they run here.
  if (s_wifiLinkAction) {
    const int8_t act = s_wifiLinkAction;
    s_wifiLinkAction = 0;
    if (act == 1) publishSetupNetwork();
    else          cancelSetupHold();
  }
#ifdef NIMBUS_TEST
  if (s_drainPending) {
    s_drainPending = false;
    if (s_wc.setDrain) s_wc.setDrain(s_drainOn, s_drainDeep, s_drainBright, s_drainTtl);
  }
#endif

  // Drain staged single-param overrides onto the Config on THIS (main) task,
  // under the lock so buildState() on the AsyncTCP task can't observe a torn
  // has_/val_ pair. This must run before onChanged() so applyConfig() (which
  // re-reads the Config) sees the new values in the same loopWeb() pass - the
  // stale-read window the old inline path had is gone.
  if (s_wc.config) {
    for (int i = 0; i < kParamMetaCount; i++) {
      const PendOv act = s_pendOv[i];
      if (act == PendOv::None) continue;
      s_pendOv[i] = PendOv::None;
      lockConfig();
      if (act == PendOv::Set)
        s_wc.config->setOverride(kParams[i].param, s_pendOvVal[i]);
      else  // PendOv::Clear
        s_wc.config->clearOverride(kParams[i].param);
      unlockConfig();
    }
  }

  if (s_dirty) {
    s_dirty = false;
    if (s_wc.onChanged) s_wc.onChanged();
  }

  // Deferred wizard restart (POST /api/onboard/restart): a short grace lets the
  // 202 response reach the browser before the device goes down.
  if (s_onbRestartPending) {
    static uint32_t s_restartAtMs = 0;   // main-task local
    if (s_restartAtMs == 0) s_restartAtMs = millis() + 750;
    else if ((int32_t)(millis() - s_restartAtMs) >= 0) ESP.restart();
  }
}

}  // namespace nimbus::net
