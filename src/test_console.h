#pragma once
// test_console - serial command console + boot beacon + render-state emit +
// WiFi-reason surfacing for the HIL harness (the HIL test spec). Compiled ONLY under
// -DNIMBUS_TEST; in every production build (esp32s3/notifierdbg) this header is
// a set of no-op inline stubs and test_console.cpp is an empty translation unit,
// so the module is byte-for-byte invisible to production behavior.
//
// Design (the HIL test spec, build-spec §1):
//   - Callers in main.cpp invoke tc::* unconditionally; the calls vaporize when
//     NIMBUS_TEST is undefined, so main.cpp needs no #ifdef fence around each
//     hook site (keeps it a clean layer, not tangled into the NIMBUS_NOTIFIER_DEBUG
//     prints, which stay exactly as-is and independent).
//   - The module never reaches into main.cpp globals: main.cpp hands it a Hooks
//     bundle at begin(), so the render/router state stays owned by main.cpp.
//   - Serial carries console commands ONLY, in every mode (BLE is the sole nsn
//     frame transport - see AGENTS.md): pumpOrch() is the one console pump,
//     used regardless of g_orchMode.
//
// This is orthogonal to NIMBUS_NOTIFIER_DEBUG: [env:test] enables BOTH (live
// orch-hang tracing still works), but the two never share a code path.

#include <Arduino.h>
#include <functional>

namespace nimbus::tc {

// State accessors the console needs from main.cpp, supplied once at begin() so
// the module stays decoupled from the render/router globals. Providers that read
// job/config state should take net::ConfigLockGuard themselves.
struct Hooks {
  // --- STATUS / RENDER? sources ---
  std::function<int()>     mode;       // 0=notifier, 1=orchestrator (g_orchMode)
  std::function<bool()>    wifiStaUp;  // net::staConnected()
  std::function<String()>  wifiIp;     // net::staIp()
  std::function<int()>     rssi;       // net::rssi()
  std::function<uint8_t()> curScreen;  // last screen id pushed to the panel
  std::function<uint8_t()> posture;    // g_cfg.posture()
  // Which display driver ACTUALLY bound at boot (g_screenIsTft), not the stored
  // preference. The two differ whenever the fail-soft path trips, and that is
  // exactly the case a test must be able to see - a board that silently fell
  // back to e-ink looks identical to a working one if STATUS echoes the setting.
  std::function<bool()>    screenIsTft;
  // Feed ONE byte of a synthetic nsn frame through the same decoder/mapper/
  // router path a real BLE frame takes. Backs NSNFEED: it lets the notifier UI
  // (session cards, status colours) be driven with no broker and no BLE - which
  // matters because macOS BLE permissions block host-side bleak entirely, so
  // the card design could otherwise only ever be seen in its EMPTY state.
  std::function<void(uint8_t byte)> nsnFeedByte;
  // TFTFLIP - apply the landscape 180-degree flip live (MADCTL only). Set on a
  // TFT board; absent on e-ink, where the command is a no-op beyond the NVS write.
  std::function<void(bool)> tftFlip;
  // PROFILE <0|1|2> - set the battery mode from the console so HIL can assert
  // mode-dependent behaviour (backlight, ring) without the network. Routed
  // through the SAME selector path the menu and web use; never a second writer.
  std::function<bool(int)> setProfile;
  // PANELPROBE <0|1> - the panel watchdog's register probe, togglable live.
  // (The RING/RADIO/TOUCHPOLL bisect toggles are gone - white-screen solved.)
  std::function<bool(bool)> panelProbe;
  // Jobs currently in the attention router. The ONLY unambiguous read of
  // whether a fed frame landed: bright=/seg= in RENDER? are posture-scaled, so
  // on a pack-less board (forced passive posture) they stay flat whether the
  // frame was rejected or merely suppressed.
  std::function<int()> jobCount;
  // Fill the four ring-summary out-params from the last composed ring::Plan
  // (read under net::ConfigLockGuard by the provider).
  std::function<void(int& segCount, bool& single, bool& dark, uint8_t& bright)>
      ringSummary;

  // --- actions ---
  std::function<void(const String& text)>                 turn;    // TURN
  std::function<bool(const String& ssid, const String& pass)> wifi;  // WIFI
  std::function<void()>                                   reboot;  // REBOOT
  std::function<void()>                                   factoryReset;  // FACTRESET (HIL)
  std::function<void()>                                   hang;    // HANG (spin)
  // MODE <0|1>: persist the operating mode and restart so it takes effect (mode
  // is resolved once at boot). Lets HIL tests drive Notifier-path assertions on
  // a device that booted in Orchestrator mode, and vice versa.
  std::function<void(int mode)>                           setMode;
  // BLE radio state for the `BLE?` diagnostic: enabled = advertising-enabled
  // (net::ble::enabled()), connected = a central is linked. Lets the harness
  // assert the Connectivity > Bluetooth toggle actually starts/stops the radio.
  std::function<void(int& enabled, int& connected)>       bleState;
  // Secure-pairing diagnostics (bt-secure-pairing). `BLEMAC?` prints the device
  // BLE address - needed because both boards advertise "Nimbus", so a BLE client
  // targets the spare by ADDRESS. `BONDS?` prints the bonded-central count +
  // whether a pairing is mid-flight; `FORGETBONDS` wipes all bonds. Let the
  // harness verify bonding persists across reboot + the Forget path re-triggers
  // pairing, without a screen.
  std::function<String()>                                 bleMac;
  std::function<void(int& bonds, int& pairing)>           bleBonds;
  std::function<void()>                                   bleForget;
  // solide::leds::currentState().rawFrame for the `RAWFRAME?` diagnostic: true
  // while the Active-posture Animator (src/hw/ring_out.cpp) owns the physical
  // ring via showFrame(). Proves the raw-frame path actually engages/releases
  // on posture transitions - the mechanically-verifiable half of P-E; the
  // Animator's frame CONTENT (does the motion look right) needs eyes on the
  // device, this only proves the wiring runs.
  std::function<bool()>                                   rawFrameActive;

  // BATTCAL - owner asserts the pack is full; anchor 100% to the current reading
  // (S3 ADC under-reads a full 2S pack) and persist. Returns a short status line.
  std::function<String()>                                 battCal;
  // BATTRESET: discard learned battery analytics (rate EWMA + health baselines),
  // KEEPING the BATTCAL anchor. Recovers a model poisoned by a drain campaign -
  // the learned state persists to NVS, so a reflash alone does NOT heal it.
  std::function<String()>                                 battReset;
  // SLEEP: enter the low-battery deep sleep NOW (test-only) - verifies the e-ink
  // sleep screen + knob-rotation/timer wake mechanics without draining a pack.
  // ⚠ USB serial dies with the chip; the 5-min charger-sniff timer wakes it.
  std::function<void()>                                   sleepNow;
  // Battery drain/storage (battery-measurement). DRAIN is a TEST characterization tool;
  // STORAGE is the production discharge-to-storage-SoC feature (also reachable here).
  std::function<String(bool on, bool deep)>               drain;    // DRAIN on|off [deep]
  std::function<String(int pct)>                          storage;  // STORAGE <pct>|off
  std::function<void(bool& active, bool& deep, uint16_t& restMv)> drainState;  // STATUS
  // MENU?: dump the live settings-menu view (title/selection/rows/help) on one
  // line so HIL can assert menu WORDING and structure over serial - RENDER?
  // only proves WHICH screen shows, goldens only run on the host.
  std::function<String()>                                menuView;
  // DREAM: force the reserved dream loop to fire NOW (test-only) - runs the
  // real two-phase path (memory maintenance + reflection turn) so HIL can
  // verify dreaming without waiting for 03:30. Returns a short summary line.
  std::function<String()>                                dreamNow;
  // v3.6.0 fold seams (plan §1f): CTX? one-line fold state (arg = chat, default
  // last-active); COMPACT <chat> stages a manual fold (async - poll CTX?);
  // MEMFILL <epi|vec> <n> [bytes] bulk-fills a store, chunked ≤200 rows/call.
  std::function<String(const String& chat)>              ctxInfo;
  std::function<String(const String& chat)>              compactNow;
  std::function<String(const String& kind, int n, int bytes)> memFill;
  // EPIQ [before] <text> - one COLD episodic query (deep history), timed. Runs
  // here rather than on the web task so the measurement is of the card, not of
  // AsyncTCP, and so a HIL check of the cold path has a non-web route.
  std::function<String(const String& before, const String& text)> epiQuery;
};

#if defined(NIMBUS_TEST)

// Capture the hooks. Call once from setup().
void begin(const Hooks& h);

// Emit the one-shot boot beacon "READY mode=<n> ip=<..>" + flush. Call as the
// last line of setup(). This is the fixed boot_ok / orch_boot_ok target (F11/F14).
void ready();

// Console pump (Serial otherwise idle): read Serial and dispatch complete
// command lines. Call once per loop, in either mode - BLE is the sole nsn
// frame transport now, so Serial never carries frame data to tee out.
void pumpOrch();

// Emit one machine-readable render summary on each render (screen/posture/ring
// summary). Same schema as the RENDER? reply so L3 tests can poll OR consume the
// push stream deterministically (F2/F4). Always on under NIMBUS_TEST.
void onRender(uint8_t screenId, uint8_t posture, int segCount, bool single,
              bool dark, uint8_t bright);

// Drain one screensaver console command. `SAVER` queues -1 (force the saver
// screen NOW - HIL/demo); `SAVER <min>` queues the new threshold (persisted +
// applied by the main loop; 0 disables). Returns false when nothing is queued.
bool popSaverCmd(int& minsOut);

// Surface a WiFi disconnect reason: prints "WIFI_DISCONNECTED reason=<n>" and
// arms the e-ink error badge (F9). Called from a WiFi.onEvent handler.
void onWifiReason(int reason);

// Print "WIFI_GOT_IP <ip>" on a successful STA association (F8). Called from the
// same WiFi.onEvent handler.
void onWifiGotIp(const String& ip);

// True while an e-ink error badge is armed (main.cpp paints ScreenId::StatusIdle with
// the reason). F9.
bool errorBadgePending(int& reasonOut);
void clearErrorBadge();

#else  // !NIMBUS_TEST - production: every symbol is a no-op inline, so callers
       // stay unguarded and the calls optimize away to nothing.

inline void begin(const Hooks&) {}
inline void ready() {}
inline void pumpOrch() {}
inline void onRender(uint8_t, uint8_t, int, bool, bool, uint8_t) {}
inline bool popSaverCmd(int&) { return false; }
inline void onWifiReason(int) {}
inline void onWifiGotIp(const String&) {}
inline bool errorBadgePending(int&) { return false; }
inline void clearErrorBadge() {}

#endif  // NIMBUS_TEST

}  // namespace nimbus::tc
