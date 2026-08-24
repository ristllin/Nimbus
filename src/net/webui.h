#pragma once
#include <Arduino.h>      // String - used by the chatSend/chatPoll hooks below
#include <functional>

#include "nimbus/power/power_monitor.h"
#include "nimbus/power/battery_model.h"   // BatteryEstimate (analytics seam)
#include "nimbus/profile.h"

// webui - the Nimbus config web surface. Adapted from Nuage-Solide
// src/webui.{h,cpp} but gutted to a config-only page: dropped auth/login,
// WebSocket, chat/agent/provider/Telegram, and every debug/selftest endpoint.
// What remains is one config page plus the WiFi scan/save routes.
//
// The web layer is a thin device-side adapter over the already-host-tested
// portable core (nimbus::Config / nimbus::Selector). It does NOT own
// persistence: the caller supplies a `changed` callback that is invoked
// (on the main task, from webui::loop()) after any mutation so it can
// serializeConfig(...) -> NVS and re-resolve the selector.
//
// Threading: AsyncWebServer callbacks run in the AsyncTCP task (core 0), which
// is a DIFFERENT core from the Arduino main loop (core 1). The shared Config's
// override arrays are not atomic, so ALL mutations are STAGED from the handler
// and applied from webui::loop() on the main task - no /api/config write ever
// touches the Config on the AsyncTCP task. So the caller MUST pump webui::loop()
// from its main loop and treat `changed` as running there.
//
// The one remaining cross-task access is buildState() (GET /api/state) READING
// the Config on the AsyncTCP task while the settings menu WRITES it on the main
// task. That read/write pair is serialized by the config spinlock below, which
// the main-task menu glue must also take around its own Config writes.

class AsyncWebServerRequest;

namespace nimbus::net {

// Per-device web auth (prism security fix). A state-changing request must carry the
// device token (X-Nimbus-Token header OR ?t= param) matching store::webAuthToken();
// constant-time compared. The owner gets the token via the Config QR. Shared by the
// /api/* POST handlers (webui) and the /mcp endpoint (web_memory). GETs stay open.
bool webAuthOk(::AsyncWebServerRequest* r);

// Everything the config page needs to read/write. Pointers are borrowed and
// must outlive the server; the callbacks are optional.
struct WebConfig {
  Config*         config   = nullptr;   // required: the sparse-override config
  Selector*       selector = nullptr;   // required: profile precedence
  power::Monitor* power    = nullptr;   // optional: battery header (null => hidden)
  // Battery analytics estimate (time-to-empty / health / rate), computed on the
  // main task; null => the extra battery fields are omitted from /api/state.
  std::function<power::BatteryEstimate()> batteryEstimate;
  // Owner-asserted "the pack is full right now" (POST /api/battcal): anchors 100%
  // to the current reading + persists it, correcting the S3 ADC's top-band under-
  // read. STAGED from the handler (AsyncTCP task) and invoked here on the MAIN task
  // in loopWeb() - the battery model + NVS save are main-task-owned, so a
  // synchronous web-task call would race the loop's per-tick model update. Null =>
  // the endpoint is a no-op.
  std::function<void()> calibrateBatteryFull;
  // Discard LEARNED battery analytics (rate EWMA + health baselines), keeping the
  // BATTCAL anchor - recovery from a drain campaign, whose learned state is in NVS.
  std::function<void()> resetBatteryLearning;
  // Re-arm the ADC + model after a battery HARDWARE config change (divider/capacity).
  std::function<void()> reconfigureBattery;
  // Battery drain/storage (battery-measurement). setDrain = campaign (TEST); setStorage =
  // discharge-to-storage-SoC (production); drainState fills the /api/state batt fields.
  // bright: -1 = firmware default. ttlS: -1 = default host dead-man, 0 = DISARMED
  // (console/human - no refresher), >0 = host promises to refresh within ttlS.
  std::function<void(bool on, bool deep, int bright, int ttlS)> setDrain;
  std::function<void(int pct)>            setStorage;
  std::function<void(bool& drainA, bool& drainDeep, bool& storageA, uint16_t& restMv,
                     uint32_t& restAgeS)> drainState;
  // Thermal guard state (fried-panel fix): die temp + trip/abort surfacing so the
  // drain harness and the owner can SEE how hot it ran (nothing recorded it before).
  std::function<void(float& dieC, bool& tripped, uint8_t& trips, bool& aborted)> thermalState;
  std::function<uint8_t()> drainBright;   // battlab: the active per-run LED load
  // battlab host dead-man: seconds left before the device self-stops the load.
  // 0 = disarmed (no dead-man armed / not draining). Lets the host VERIFY the
  // safety net is actually armed instead of assuming it.
  std::function<uint32_t()> drainTtlLeftS;

  // Live stats the web layer can't derive itself. Provide real accessors when
  // the P4 mode manager lands; until then leave null (jobs=0, mode from NVS).
  std::function<int()>  activeJobs;     // Notifier segments / Orchestrator jobs
  std::function<int()>  currentMode;    // 0=notifier, 1=orchestrator
  // HAL subsystem health from solide::begin(), packed: bit0 display, bit1 leds,
  // bit2 storage, bit3 memory, bit4 input. Null => reported unknown (all-up).
  std::function<uint8_t()> halMask;

  // Applied from webui::loop() after any /api/config mutation (main task):
  //   onChanged(cfg, sel) -> serializeConfig + persist blob + re-resolve.
  std::function<void()>       onChanged;
  // mode select persistence (e.g. solide::memory::setInt("mode", m)).
  std::function<void(int)>    onModeChanged;
  // Live ring preview (POST /api/preview): fired from loopWeb() on the main task
  // with the requested profile id and an optional status to demo (0..5 =
  // solide::ring::Status, -1 = default showcase); the device drives + auto-reverts
  // the ring without touching Config/Selector (a preview never persists).
  std::function<void(int,int)> onPreview;
  // Factory reset (POST /api/factory-reset, confirm-gated): sets a deferred flag so
  // the MAIN loop erases NVS + reboots (never on the AsyncTCP task). The device
  // IDENTITY (name) is preserved across the wipe; eraseSd=true additionally erases
  // the durable /mem store in the same flow. Null => no-op.
  std::function<void(bool /*eraseSd*/)> factoryReset;
  // SD reset (POST /api/sdreset, confirm-gated): erase the durable /mem store
  // (memories/history/files/blobs), keep config, reboot. Deferred flag like above.
  std::function<void()>       sdReset;
  // Full-card format (POST /api/sdformat, its OWN typed confirm): reformat the whole
  // card, not just /mem. Deferred flag; null or unsupported => the route reports it.
  std::function<void()>       sdFormat;
  // Web chat (POST /api/chat): inject a message as an orchestrator turn (runs on the
  // poll task). chatPoll() returns + clears the last web-turn reply ("" if none yet).
  // FALSE when the device could not accept the message (inbound queue full
  // while a turn runs) - the route answers 503 instead of pretending success.
  std::function<bool(const String&)> chatSend;
  std::function<String()>            chatPoll;
  // TRUE while a web turn is actually running. GET /api/chat reports this as
  // `pending`; without it the route inferred pending from an empty reply, which
  // is also the normal resting state - so an observer saw "pending" forever
  // after a run finished. Null => the old inference (compat).
  std::function<bool()>              chatPending;
};

// Register routes and start the server on port 80. `wc` is copied; its borrowed
// pointers/callbacks must outlive the server.
void beginWeb(const WebConfig& wc);

// Pump deferred work: apply any staged profile/mode/override change on the main
// task and fire onChanged/onModeChanged. Call every main loop.
void loopWeb();

// True (once) if a web change wants the LED ring re-composed immediately - e.g. the
// colour theme was switched. The main loop consumes this to refreshRing() so a web
// theme change shows on the device instantly instead of at the next ring event.
bool consumeRingRefresh();

// True (once) when a web action succeeded (WiFi creds saved / scan completed) and
// deserves a subtle LED confirmation blip (P3). The main loop consumes it and
// fires a short Pattern::Flash window - POSTURE-GATED there (never in Dark /
// lights-off / a model-painted ring).
bool consumeLedConfirm();

// First-run Wi-Fi handoff (TFT AP teardown). /savewifi arms a short grace
// period before the setup AP may be dropped; once the browser has received its
// token-bearing LAN URL, POST /api/wifi/handoff shortens that grace. Both are
// one-shot flags written by AsyncTCP and consumed by the main task.
bool consumeWifiJoinStarted();
bool consumeWifiHandoffReady();

// True (once) when repeated web-auth failures (3 x 401 inside 60 s) suggest the
// owner is on a token-less page - the main loop consumes it and shows the
// Config QR screen on the panel. Re-arms after 5 min (no refresh churn); any
// successfully authed request resets the failure counter.
bool consumeAuthQrRequest();

// Config spinlock. buildState() reads the shared Config on the AsyncTCP task
// concurrently with the settings menu writing it on the main task; both sides
// must hold this lock while touching the Config's override arrays. lockConfig()
// takes it; unlockConfig() releases. RAII helper below for scoped use.
void lockConfig();
void unlockConfig();

struct ConfigLockGuard {
  ConfigLockGuard() { lockConfig(); }
  ~ConfigLockGuard() { unlockConfig(); }
  ConfigLockGuard(const ConfigLockGuard&) = delete;
  ConfigLockGuard& operator=(const ConfigLockGuard&) = delete;
};

}  // namespace nimbus::net
