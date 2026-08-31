// Nimbus firmware - Notifier mode device.
//
// BLE nsn frames -> decoder -> Mapper -> attention Router -> ring + panel.
// USB serial is power + flashing only, never a status transport (decided
// 2026-07 - see AGENTS.md). The ring is instant (per-job segments / cursor);
// the panel follows on the dwell/coalesce schedule. Encoder rotates a cursor
// over the job list and the detail pane renders after the cursor settles.
//
// P4 wiring lives here now:
//   - boot loads the persisted Config + Mode from NVS/SD (config_nvs), so the
//     profile/overrides survive reboots; a fresh device falls back to defaults.
//   - a LONG-PRESS on the encoder opens the portable SettingsMenu; while it is
//     open, touch drives the menu (rotate/click/long-press gestures) and the panel
//     shows ScreenId::Menu rendered from view(). Any menu mutation applies live
//     (scheduler dwell/coalesce + ring brightness) and persists.
//   - a WiFi AP + captive config portal (src/net) starts in setup(); its save
//     callback persists the same Config and re-syncs the active profile.
// The Notifier BLE path is untouched when the menu is closed: nsn frames keep
// flowing to the ring + panel exactly as before.
//
// Build:  pio run -e esp32s3 -t upload           (silent - clean nsn stream)
//         pio run -e notifierdbg -t upload        (adds a device->host status
//                                                  echo for E2E validation)
#include <Arduino.h>
#include <esp_heap_caps.h>  // heap_caps_calloc (PSRAM-backed mbedTLS allocator)
#include <new>              // placement new (PSRAM-backed panel framebuffers)
#include <esp_bt.h>         // esp_bt_controller_mem_release (free BT RAM in Orch mode)
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>   // nvs_flash_erase() - web factory reset
#include <nvs.h>         // raw NVS read/write to preserve identity across the wipe
#include "agent/agent_config.h"            // AKEY_* frozen NVS key macros
#include "nimbus/config/identity_keys.h"  // keys that must survive a factory reset
#include "nimbus/orch/danger_zone.h"      // CUM-15: device-name keep-list preserved across a reset
#include <mbedtls/platform.h>  // mbedtls_platform_set_calloc_free (route TLS -> PSRAM)
#include <solide/solide.h>

#include "nimbus/power/bright_cap.h"   // 60% LED safety cap (owner feature)

#include "agent/adapters/adapter_factory.h"  // fabricInit
#include "agent/adapters/audio_stt.h"        // Telegram voice-note + voice-input STT
#include "agent/adapters/tts_voices.h"        // live TTS voice catalog (/api/voices)
#include "sys/agent_log.h"                  // alogf -> /api/log ring (voice diag)
#include "agent/memory_subsystem.h"           // setDataFs(SD) - route orch data to the card
#include "agent/files_subsystem.h"            // E1: /mem/files artifact store
#include "agent/orchestrator.h"
#include "agent/loops_subsystem.h"   // Local Loops scheduler (hooks wired below)
#include "agent/dream_subsystem.h"   // DREAMING: reserved nightly maintenance loop
#include "agent/store.h"                      // store::theme()
#include "nimbus/theme.h"                     // themeAccent - LED colour personality
#include "nimbus/status_style.h"              // status -> {theme role, anim} single source
#include <LittleFS.h>
#include <SD.h>                 // SD card FS (mounted via solide::storage) for orch data
#include <solide/storage.h>     // microSD mount helper
#include <solide/audio.h>                     // long-press-to-talk mic capture
#include <vector>
#include "agent/provider_verify.h"
#include "agent/store.h"
#include "agent/telegram.h"
#include "agent/adapters/audio_tts.h"  // orchSpeakSink - tts device action -> Telegram audio
#include "sys/tls_arbiter.h"
#include "hw/hal_status.h"
#include "hw/tft_out.h"      // colour touch panel (screenModel=tft)
#include "hw/touch_input.h"  // taps -> the same gestures the encoder makes
#include "solide/touch.h"     // hold-to-talk release detection on a touch board
#include "nimbus/touch_cal.h"  // persisted resistive-touch calibration
#include "nimbus/tft_render/menu_tap.h"  // portable tap -> menu-FSM mapping
#include "hw/ring_out.h"
#include "hw/selftest.h"   // device health-check engine + device.* tools + SELFTEST
#include "sys/ota_update.h"   // OTA firmware update (boot guard + check/install)
#include "modes/notifier_mode.h"
#include "nimbus/attention.h"
#include "nimbus/wifi/copy.h"                  // portable, tested Wi-Fi copy + deviceUrl
#include "nimbus/wifi/setup_ap.h"              // portable, tested setup-AP recovery policy (CUM-190)
#include "nimbus/render_context.h"
#include "nimbus/render_sched.h"
#include "hw/power_fuelgauge.h"
#include "hw/power_battery_adc.h"
#include "nimbus/power/power_manager.h"
#include "nimbus/power/battery_model.h"   // battery analytics (time-left, health)
#include "nimbus/power/thermal_guard.h"   // LED-load thermal breaker (fried-panel fix)
#include "nimbus/profile.h"
#include "nimbus/ring_plan.h"
#include "nimbus/saver.h"                 // screensaver idle clock (logo after 1 h idle)
#include "nimbus/settings_menu.h"
#include "nimbus_board_power.h"           // explicit per-board battMon default (CUM-202)
#include "nimbus_config.h"
#include "version.h"
#include "net/ble_notifier.h"
#include "net/relay_client.h"  // cloud tunnel (cumulo-nimbus)
#include "nimbus/cloud/relay_timing.h"  // kTaskWdtTimeoutMs - single source for the WDT budget
#include "net/webui.h"
#include "net/wifi_portal.h"
#include "net/wifi_store.h"   // the Wi-Fi menu reads saved networks by name
#include "sfx/sound_fx.h"
#include "sfx/sfx_sync.h"            // sfxsync::statusStr -> device.status sfxSync (W11)
#include "sfx/music.h"              // CUM-40 music player (media.* tools + /play)
#include "nimbus/orch/caps.h"        // spawn capacity constants -> device.status (W11)
#include "nimbus/orch/fetch_policy.h"  // fetchPolicyName -> device.status (W18)
#include "sys/config_nvs.h"
#include "test_console.h"  // no-op inline stubs unless -DNIMBUS_TEST (env:test)
#include <WiFi.h>  // WiFi.onEvent - SFX link sounds (all builds) + F9 reason (test)

using namespace nimbus;

static attn::Router g_router;
static Config g_cfg;
static Selector g_selector;
static ring::Cursor g_cursor;
// Orchestrator voice UX (Phase 4): a cached snapshot of the running sub-sessions
// the encoder cursor moves over. Refreshed once per orch render cycle (not per
// detent - sessionInfos() iterates the journal + heap-allocs). g_cursor.index()
// indexes into this list.
static std::vector<nimbus::orch::SessionInfo> g_sessionList;
// A full-screen Ask-screen override (mode-switch confirmation, voice transcript,
// voice reply). When set, the Ask screen renders it.
static String g_askOverride;
// STICKY reply (P2.3): a reply/voice message on the Ask screen HOLDS until the
// next click (the scheduler is gated while set, like the menu), and the cursor
// pages through it - it used to be overwritten by the next scheduled render
// within seconds ("can't read more than a line or two").
static bool g_askSticky = false;
static int  g_askPage = 0;
// A transient Ask override that precedes a reboot (mode switch): the device
// restarts on its own, so the Ask screen draws NO Close button (it would be dead -
// the reset lands before a tap could reach it). Set true right before rendering
// such a confirmation; a reboot follows immediately, so it never leaks to a later
// dismissible reply (and a fresh boot clears it).
static bool g_askTransient = false;
// Voice hold-to-talk state. g_voiceActive guards re-entry (LongPress auto-repeats
// while held); g_voiceStop is the recordToFile stop flag driven by the release
// watcher; g_voiceDone lets the watcher exit. g_voiceReply* carries the async turn
// reply back from the poll task to the main loop for panel rendering.
static volatile bool g_voiceActive = false;
static volatile bool g_voiceStop = false;
static volatile bool g_voiceDone = false;
static volatile bool g_voiceReplyPending = false;
static String        g_voiceReply;
// Web chat (POST /api/chat): the turn runs on tg_poll; its reply is captured here
// (chatId "web" in orchSendSink) for GET /api/chat to poll. Single-slot, one owner.
static volatile bool g_webReplyPending = false;
static String        g_webReply;
// Deadline of the web turn currently in flight (0 = none), so GET /api/chat can
// report an HONEST `pending`. Set when a message is accepted, cleared when its
// reply is staged; the deadline is only a backstop for a turn that dies without
// replying (a fan-out head turn may legitimately run for minutes).
static volatile uint32_t g_webTurnDeadlineMs = 0;
static constexpr uint32_t kWebTurnMaxMs = 12UL * 60UL * 1000UL;
// millis() of the last sub-agent start/finish, for the Calm-level ring activity blink
// (set when the session set changes; 0 = no recent activity).
static uint32_t      g_lastActivityMs = 0;
// Screensaver (owner 2026-07-16): after saverMin minutes (NVS, default 60,
// 0 = off) with no activity - knob/button, real job EDGES (broker heartbeat
// re-sends do NOT count), or any non-ambient screen render - the panel swaps
// StatusIdle for ScreenId::Screensaver (the dotted-ring logo + device name).
// Any activity restores live status via saverKick().
static nimbus::SaverTimer g_saver;
static void saverKick();   // defined after renderScreen/g_menu exist
// "Wake the ring" reveal: a single click promotes the ring to the full-brightness
// segment treatment until this deadline (0 = not revealing), so live status is
// glanceable from any posture. compose(reveal=...) reads it via refreshRing().
static uint32_t      g_revealUntilMs = 0;
// Auth-fail Config QR is TRANSIENT: when repeated 401s surface the QR unprompted,
// hold it ~45 s then fall back to StatusIdle. Without this a client that keeps
// polling with a bad token (a stale browser tab) parked the panel on the QR
// forever, so the idle screensaver - which only arms from StatusIdle - never
// showed (owner: "a full night, no logo"). 0 = not currently showing the auth QR.
static uint32_t      g_authQrUntilMs = 0;
static constexpr uint32_t kAuthQrHoldMs = 45000;
static uint32_t      g_revealDurMs   = 4000;   // actual length of the live reveal window
                                               // (the boot handoff uses a shorter one)
static constexpr uint32_t kRevealMs = 4000;  // how long a single click lights the ring
// Wake reveal is EASED in/out (not snapped): the loop ramps ring brightness over
// these windows while keeping the composed per-segment status colours, so the
// wake breathes on and off gracefully. (leds::setBrightness scales the raw frame.)
static constexpr uint32_t kRevealFadeInMs  = 400;
static constexpr uint32_t kRevealFadeOutMs = 600;
// Boot flourish: a calm breathing-white "I'm coming up" for the first few seconds
// after boot (Pattern::Pulse self-animates), unless a real attention event needs
// the ring first. 0 = not booting. Set at the end of setup().
static uint32_t      g_bootBreatheUntilMs = 0;
static constexpr uint32_t kBootBreatheMs = 2500;
// Theme flourish: picking a theme lights the ring in the new palette for a moment
// so the change is VISIBLE on the main screen. Without it, an idle ring (dark) made
// the pick look like it turned the lights off (owner-reported on the touch board,
// 2026-08-01: "after changing theme the lights seem to be gone"). 0 = not active.
static uint32_t      g_themePreviewUntilMs = 0;
static bool          g_themeFlourishPending = false;  // theme picked -> arm flourish on menu close
static constexpr uint32_t kThemePreviewMs = 2500;
// After a critical-battery deep sleep, wake this often to re-check for a charger
// (the ADC board has no charge-detect pin to wake on - see the shutdownT2 path).
static constexpr uint32_t kLowBattWakeMinutes = 5;

// smoothstep eased 0..256 for x in [0,span] (t^2*(3-2t)); >=span -> 256. Fixed-point.
static uint16_t smoothstep256(uint32_t x, uint32_t span) {
  if (span == 0 || x >= span) return 256;
  const uint32_t t = (x * 256) / span;               // 0..256
  const uint32_t s = (t * t * (768 - 2 * t)) >> 16;  // 256 * smoothstep(t)
  return uint16_t(s > 256 ? 256 : s);
}
// The active ring brightness (RingBrightness param, byte-clamped) - the peak the
// boot breathe + wake-reveal envelope scale within.
static uint8_t ringBrightByte() {
  const int b = g_cfg.effective(Param::RingBrightness);
  return uint8_t(b < 0 ? 0 : (b > 255 ? 255 : b));
}
// Web ring preview (POST /api/preview -> webui's onPreview, fired from loopWeb()
// on the main task). Never touches g_cfg/g_selector: refreshRing() composes from
// a throwaway Config carrying only the previewed profile's presets, so nothing is
// persisted or visible to /api/state. loop() reverts once g_previewUntil passes.
static bool      g_previewActive = false;
static uint32_t  g_previewUntil = 0;
static ProfileId g_previewProfile = ProfileId::Balanced;
// The status the web ring simulator asked to demo (0..5 = solide::ring::Status),
// or -1 = the default two-arc showcase. Lets "Demo on Device" mirror the exact
// status the on-page simulator is showing, not just the theme.
static int       g_previewStatus = -1;
// Posture of the last PLAN actually applied to the ring - diverges from
// g_cfg.posture() while a preview is live; RENDER? must report what the ring shows.
static uint8_t   g_lastPosture = 0;
// P3: end of the web-action LED confirmation Flash window (0 = none active).
// Set when a save/scan confirm blips the ring; the loop restores the composed
// ring (refreshRing) when it expires.
static uint32_t      g_ledConfirmUntilMs = 0;

// Orchestrator device-action overrides (DeviceSink; staged on the poll task under
// the config lock, applied by the main loop). "lights off" darkens the ring until
// lights:full, a user click (reveal), or a NEW attention event re-lights it - an
// owner-silenced ring must not swallow a call-to-action. A `led` action paints an
// explicit pattern that owns the ring until a lights action or click clears it.
static volatile bool g_lightsOff = false;
static volatile bool g_ledOverrideActive = false;
static uint8_t  g_ledOvMode = 0, g_ledOvR = 0, g_ledOvG = 120, g_ledOvB = 255, g_ledOvBright = 128;
static bool     g_ledOvHasBright = false;
static volatile bool g_devActDirty = false;  // loop: apply the staged override state
// Expiry for a MODEL-painted `led` pattern (millis deadline; 0 = no expiry). Set
// by the device sink from the per-mode AttnHoldMs; the battery-drain pin leaves it
// 0 on purpose - a measurement load must hold for hours, decoration must not.
static volatile uint32_t g_ledOvUntilMs = 0;

// Hard ceiling on the Calm "thinking" breathe (orchWorking = turnInFlight). The
// stuck-turn reaper already frees a hung turn (~loop deadline + 2 min), but as an
// absolute belt-and-suspenders - so the ring can NEVER breathe "working" for hours
// if that reaper is ever wedged - the watchdog latches this past kWorkingHardMaxMs
// of continuous turnInFlight, and compose() then drops the breathe. Cleared the
// instant turnInFlight goes false (the reaper or a clean turn-end).
static volatile bool g_workingCeilingHit = false;

// Battery drain / storage (battery-measurement). A high, constant LED load (solid white)
// is pinned via the SAME g_ledOverrideActive latch the `led` action uses, so refreshRing()
// + tickAnimation() release the ring and Pattern::Solid holds with no maintenance. A
// separate g_highLoadActive marks it as a BATTERY op so attention/refresh can't reclaim
// the ring mid-run, and the settle state machine periodically drops the ring for a few
// seconds to read a NEAR-RESTING voltage - under ~2 A the pack sags ~0.4 V/cell, so raw
// under-load voltage can't calibrate the resting liIonPercent table. DRAIN is TEST-only
// (campaign, runs to cutoff); STORAGE is a production user feature (drain to ~70% + hold).
static volatile bool     g_highLoadActive  = false;  // drain OR storage pinning the ring
static volatile bool     g_drainDeep       = false;  // DRAIN deep: suppress T2 -> run to cutoff
static volatile uint16_t g_storageTargetMv = 0;      // STORAGE stop (pack mV); 0 = campaign drain
static volatile bool     g_hlSettling      = false;  // in a load-off settle window
static uint32_t          g_hlLastSettleMs  = 0;
static uint32_t          g_hlSettleStartMs = 0;
static volatile uint16_t g_hlRestingMv     = 0;      // last load-off resting pack mV (0 = none yet)
static uint32_t          g_hlRestingAtMs   = 0;
// HOST DEAD-MAN (battlab). A host-driven campaign drain is armed by a host process
// that MUST keep refreshing it. If that host dies/restarts/sleeps or the network
// drops, NOTHING else stops the ~1.5 A load and the pack runs FLAT - that happened
// live (2026-07-16: a lab restart orphaned a 45 % drained-mode run; it drained ~26
// min unsupervised and killed the pack). The device must fail safe on its own.
// Rollover-correct by construction: store the last-refresh stamp and compare an
// UNSIGNED difference (same idiom as the settle machine), never `now > deadline`.
// 0 ttl = disarmed (a human at the console, and STORAGE which auto-stops itself).
static volatile uint32_t g_drainRefreshedMs = 0;     // millis() of last arm/refresh
static volatile uint32_t g_drainTtlMs       = 0;     // 0 = dead-man DISARMED
static constexpr uint32_t kDrainTtlDefaultS = 120;   // host must refresh within this
static constexpr uint32_t kDrainTtlMaxS     = 900;   // 15 min hard ceiling
static constexpr uint8_t  kDrainBright        = 150;              // DEFAULT + RECOMMENDED max
                                                                  // (~1.5 A on the 45-LED ring). WAS
                                                                  // 190 (~2 A) - that fried an e-ink
                                                                  // panel in ~15 min (2026-07-15).
// The web `bright=` param may now request the FULL scale (up to 255) so the lab
// can characterize at higher loads, but that is above the recommended ceiling and
// the caller is warned. The runtime protection is the THERMAL GUARD below (trips
// the load at 65 °C / +18 °C rise) + the host max-temp stop - NOT a fixed clamp.
static constexpr uint8_t  kDrainBrightHardMax = 255;
static constexpr uint32_t kHlSettleIntervalMs = 15u * 60u * 1000u;
static constexpr uint32_t kHlSettleMs         = 9000;            // load-off relax window

// THERMAL GUARD (fried-panel incident 2026-07-15: ~10 W of white LEDs beside the
// SSD1680 killed a screen in ~15 min - and nothing was even reading the die
// sensor, so there is no record of how hot it got). The S3's internal DIE sensor
// is the only thermometer on this hardware; nimbus::power::ThermalGuard (pure,
// host-tested) trips on an absolute ceiling OR a rise-over-baseline delta:
//   Trip   -> LED load OFF, cool;   Resume (hysteresis) -> reduced brightness;
//   Abort (>3 trips) -> the drain/storage op is cancelled outright.
// A separate GLOBAL backstop kills ANY ring output at kGlobalLedKillC, so a
// model-driven `led` pattern can't cook the panel either.
static nimbus::power::ThermalGuard g_thermal;
static volatile float g_dieTempC = 0.0f;        // latest die reading (/api/state)
static uint32_t       g_thermTickMs = 0;        // 2 s poll gate
static volatile bool  g_thermalAbortLatch = false;  // op cancelled by heat (surfaced)
static constexpr uint8_t kDrainBrightReduced = 110;  // post-trip resume level (~1.1 A)
static constexpr float   kGlobalLedKillC     = 70.0f;
// battlab: the ACTIVE per-run LED load (web `bright=` param, clamped in drainSet;
// defaults to kDrainBright). Surfaced as drainBright in /api/state.
static uint8_t g_drainBright = kDrainBright;
// Guard-appropriate high-load brightness: off while tripped; reduced after any trip;
// never above the per-run load.
static uint8_t hlBright() {
  if (g_thermal.tripped()) return 0;
  const uint8_t base = g_drainBright;
  return g_thermal.trips() > 0 ? (kDrainBrightReduced < base ? kDrainBrightReduced : base)
                               : base;
}

// Talk-to-configure staging (DeviceSink Config knobs; poll task -> main loop).
// -1 = not staged. Theme is a short slug (bounded copy under the lock).
static int16_t  g_cfgPendPosture = -1;
static int16_t  g_cfgPendProfile = -1;
static int32_t  g_cfgPendAttnHold = -1;
static char     g_cfgPendTheme[16] = {0};
static volatile bool g_cfgPendDirty = false;

// Deferred reboot: web-layer mode changes (AsyncTCP task) and the orchestrator's
// `reboot` device action (poll task) can't call ESP.restart() inline; they set
// this and loop() (main task) reboots at a clean point.
static volatile bool g_rebootPending = false;
static volatile int  g_modeSwitchTo = -1;   // >=0: the pending reboot is a web mode switch
                                            // (target mode) -> show the same confirm UX as the menu
static volatile bool g_factoryResetPending = false;  // web factory-reset: erase NVS + reboot on main task
static volatile bool g_factoryEraseSd = false;       // CUM-15: also erase /mem in the factory-reset flow
static volatile bool g_sdResetPending = false;       // web SD reset: erase /mem durable store + reboot
static volatile bool g_sdFormatPending = false;      // CUM-15: web full-card format (reformat whole SD) + reboot
static volatile bool g_powerOffPending = false;      // CUM-224: web power-off -> clean shutdown + deep sleep on main task
// Voice turn "waiting for the agent" state: after the transcript is sent, a theme
// spinner runs until the reply lands (or a timeout) so the ring shows work in flight
// instead of going dark. Cleared in the reply drain / timeout in loop().
static volatile bool g_voiceWaiting = false;
static uint32_t      g_voiceWaitStart = 0;
static render::Scheduler g_sched;
static NotifierMode g_notifier(g_router);
static SettingsMenu g_menu(g_cfg);

// The panel currently on the panel - always compiled: the loop's header-
// glyph change-detector gates its repaint on "is StatusIdle live?" (below), so
// production needs this too. Set by renderScreen()/renderMenu().
static volatile uint8_t g_lastScreen = uint8_t(attn::ScreenId::StatusIdle);

// An AMBIENT (non-attention) screen intent must NOT repaint over the long-idle
// screensaver logo. When the logo is up, the broker's ~30 s snapshot heartbeats
// still emit a StatusIdle screen intent; repainting it under the logo makes the
// screensaver entry re-fire, and each StatusIdle<->Screensaver swap is a ~2.2 s
// FastBW invert flash (owner: "the standby logo keeps flashing"). A CTA
// (attention) still renders - it wakes the panel because a job needs the owner -
// and a genuine job edge calls saverKick() -> StatusIdle first, so real activity
// is never dropped. This gates ONLY the ambient heartbeats while the logo shows.
static inline bool screenAmbientAllowedOverSaver(bool attention) {
  return attention || g_lastScreen != uint8_t(attn::ScreenId::Screensaver);
}
#ifdef NIMBUS_TEST
// Render-state snapshot for the test console (RENDER? / onRender emit, F2/F4).
// Written by renderScreen()/renderMenu()/refreshRing() below and read by the
// tc:: hooks. Guarded so production (esp32s3) carries only the one screen byte.
static volatile int     g_lastSeg = 0;
static volatile bool    g_lastSingle = false;
static volatile bool    g_lastDark = true;
static volatile uint8_t g_lastBright = 0;
#endif

// ---- Battery / power (plan P6) ---------------------------------------------
// No battery hardware exists on the board yet (solide-drivers v0.1.0 ships
// none), so the default monitor is NullMonitor: every sample is invalid, the
// policy never fires, and the device runs as desk-powered with the battery UI
// hidden. Define NIMBUS_HAS_FUEL_GAUGE (and verify the pins in nimbus_config.h)
// to compile in the MAX17048 impl and drive the real thresholds.
#if defined(NIMBUS_HAS_FUEL_GAUGE)
static hw::FuelGaugeMonitor g_fuelgauge;
static power::Monitor* g_monitor = &g_fuelgauge;
#elif defined(NIMBUS_HAS_BATTERY_ADC)
static hw::AdcBatteryMonitor g_battAdc;   // resistor-divider ADC sense (no fuel-gauge chip)
static power::Monitor* g_monitor = &g_battAdc;
#else
static power::NullMonitor g_nullPower;
static power::Monitor* g_monitor = &g_nullPower;
#endif
static power::Manager g_power(
    g_monitor, &g_selector,
    power::PolicyConfig{NIMBUS_BATT_T1_PCT, NIMBUS_BATT_T2_PCT,
                        NIMBUS_BATT_HYST_PCT, NIMBUS_VBUS_DEBOUNCE_MS});
static uint32_t g_lastPowerTick = 0;

// Battery analytics: turns the raw Sample stream into time-to-empty + health +
// a self-improving discharge rate (nimbus::power::BatteryModel, host-tested).
// Sampled on each telemetryDue edge; learned state persists to NVS so accuracy
// carries across reboots. g_battEstimate is a POD snapshot the web layer reads.
// Cells comes from the board map (the Freenove is 1S, the Solide 2S), not a
// compile constant - a wrong cell count makes the per-cell SoC read a 1S pack as
// a half-charged 2S one (pinned at 0%). board() is constexpr-initialized, so it is
// safe to read here at static-init time.
static nimbus::power::BatteryModel   g_battModel(solide::board().batt.cells);
static nimbus::power::BatteryEstimate g_battEstimate;

// Battery hardware from the board map. cells never 0 (both boards set it); the
// divider is owner-tuned on hand-built boards (resistors vary) but fixed on an
// all-in-one (no separate panel option -> board().epd.sck < 0).
static uint8_t  battCells()  { if (uint8_t o = agent::store::battCellsOvr()) return o; const uint8_t c = solide::board().batt.cells; return c ? c : uint8_t(NIMBUS_BATT_CELLS); }
static int      battAdcPin() { return solide::board().batt.sense >= 0 ? int(solide::board().batt.sense) : int(NIMBUS_BATT_SENSE_PIN); }
static uint16_t battDivX100() { return solide::board().epd.sck < 0 ? solide::board().batt.dividerX100 : agent::store::battDividerX100(); }
// Battery monitoring on/off. A hand-built board (Solide S3) ships WITH a pack, so
// monitoring defaults ON; an all-in-one desk board (Freenove CYD) treats a battery as
// an add-on, so it defaults OFF (opt-in) - otherwise the floating ADC reads "empty"
// and the device sleeps with no pack fitted. The owner can opt in (web Settings). The
// default is an EXPLICIT per-board field (nimbus_board_power.h), not the old
// epd.sck>=0 e-ink proxy that would silently flip if the deprecated epd pins were
// cleaned out of the board table (CUM-202).
static bool battMonOn() { return agent::store::battMon(nimbus::battMonDefaultForThisBoard()); }
static uint16_t g_battSavedSegments = 0xFFFF;   // last-persisted segment count (persist on change)
// Low-battery ping gate (field 2026-08-11: the T1 edge re-fires on every wake-
// sniff boot; the ping needs its own persisted memory). Loaded in setup().
static nimbus::power::AlertGate g_lowBattGate;
static uint32_t g_lowBattSavedPingEp = 0;
static uint16_t g_battSavedAnchor   = 0xFFFF;   // last-persisted full anchor mV (persist on change)

// Apply the owner's battery chemistry + cell count + optional custom SoC curve to
// the model. Defaults (chemistry "liion", board cells, no custom curve) reproduce
// the shipped behaviour exactly. Called at boot and after a live config change.
static void applyBattChemConfig() {
  g_battModel.setChemistry(nimbus::power::chemistryFromSlug(agent::store::battChem().c_str()));
  g_battModel.setCells(battCells());
  String cv = agent::store::battCurve();
  nimbus::power::LiIonCurvePoint pts[nimbus::power::kMaxCurvePoints];
  int n = cv.length() ? nimbus::power::parseCurveCsv(cv.c_str(), pts, nimbus::power::kMaxCurvePoints) : 0;
  if (n >= 2) g_battModel.setCustomCurve(pts, n);
  else        g_battModel.clearCustomCurve();
}

// Serialize/parse BatteryModelState as a compact CSV in one NVS key.
static void loadBattModel() {
  String csv = agent::store::batteryModelState();
  if (!csv.length()) return;
  nimbus::power::BatteryModelState st;
  // "learnedRate,segments,baselineMs,health[,fullAnchorCellMv[,baselineRuntimeSec]]"
  // - the trailing fields are optional for forward-compat with older blobs (anchor
  // landed with top-end calibration; runtime baseline with battery-measurement).
  int a = csv.indexOf(','), b = csv.indexOf(',', a + 1), c = csv.indexOf(',', b + 1);
  if (a < 0 || b < 0 || c < 0) return;
  int d = csv.indexOf(',', c + 1);
  int e = d < 0 ? -1 : csv.indexOf(',', d + 1);
  st.learnedRatePctPerHr = csv.substring(0, a).toFloat();
  st.segments = (uint16_t)csv.substring(a + 1, b).toInt();
  st.baselineBandMs = (uint32_t)csv.substring(b + 1, c).toInt();
  st.healthPct = (uint8_t)csv.substring(c + 1, d < 0 ? csv.length() : d).toInt();
  if (d >= 0)
    st.fullAnchorCellMv = (uint16_t)csv.substring(d + 1, e < 0 ? csv.length() : e).toInt();
  if (e >= 0) st.baselineRuntimeSec = (uint32_t)csv.substring(e + 1).toInt();
  g_battModel.load(st);
  g_battSavedSegments = st.segments;
  g_battSavedAnchor   = st.fullAnchorCellMv;
}
static void saveBattModel() {
  nimbus::power::BatteryModelState st = g_battModel.save();
  char buf[88];
  snprintf(buf, sizeof buf, "%.3f,%u,%lu,%u,%u,%lu", st.learnedRatePctPerHr,
           (unsigned)st.segments, (unsigned long)st.baselineBandMs, (unsigned)st.healthPct,
           (unsigned)st.fullAnchorCellMv, (unsigned long)st.baselineRuntimeSec);
  agent::store::setBatteryModelState(String(buf));
  g_battSavedSegments = st.segments;
  g_battSavedAnchor   = st.fullAnchorCellMv;
}

// ---- Orchestrator mode state (plan §3.6) -----------------------------------
// The orchestrator turn loop + Telegram poll run on their own FreeRTOS task
// (tg_poll, spawned by telegram::begin). Sub-session states flow OUT via the
// EventSink onto the SAME attn::Router the Notifier feeds, so the ring + panel
// render sub-agent jobs exactly like nsn jobs. The sink runs on the poll task
// while the main loop reads g_router on the main task, so the two are serialized
// with the same net::ConfigLockGuard the menu/web layer already uses; a flag
// tells the main loop to recompose the ring + queue an screen intent.
static agent::HeavyFabric g_fabric;
static bool          g_orchMode = false;       // resolved once at boot
// The color touch panel is the only supported display (e-ink removed in v4.4).
// True once it is up; flips false only in the fail-soft path when panel bring-up
// fails, which gates rendering off so a dead panel cannot stall the device.
static bool          g_screenIsTft = false;
// True when the stored scrModel is not "tft" (a stale "eink" unit; frozen NVS key).
// We run the color panel regardless and hold a clear "unsupported display" notice on
// it at the end of setup (see the boot-notice block).
static bool          g_unsupportedScreenModel = false;
// White-screen mitigation (TFT): drop the SoftAP once STA is up so its continuous
// beacon TX stops disturbing the panel; bring it BACK when STA goes down so the
// setup/recovery web page is always reachable when Wi-Fi is unavailable. Set in
// the WiFi events, drained in loop() (the mode change must run on the main task).
static volatile bool g_dropApPending = false;
static volatile bool g_restoreApPending = false;
// A fresh TFT needs a bounded AP+STA overlap so its browser can carry the auth
// token to the new LAN origin. Normal boot/reconnect still drops immediately;
// only /savewifi arms this window, and /api/wifi/handoff shortens it once the
// browser has safely received the destination.
static volatile bool g_apHandoffArmed = false;
static volatile uint32_t g_dropApAfterMs = 0;
static uint32_t      g_lastApReconcileMs = 0;  // periodic AP<->STA reconcile (self-heal)
static solide::BeginResult g_hal{};            // per-subsystem HAL health from solide::begin()
static bool          g_bleEnabled = true;       // Connectivity > Bluetooth (NVS, runtime)
static volatile bool g_orchRingDirty = false;  // set by the sink, drained by loop()
static volatile bool g_orchScreenRender = false;  // an attention event wants the panel
static volatile bool g_saverRestoreReq = false; // saverKick wants the screensaver
                                                // painted over with live status; the
                                                // RENDER is deferred to loop() because
                                                // saverKick fires from the event tap on
                                                // the tg_poll turn task (crash, below)
static volatile uint8_t g_orchScreenId = uint8_t(attn::ScreenId::StatusIdle);
static volatile bool g_orchScreenAttn = false;

// EventSink: route a sub-session event into the attention Router (poll task).
// Held under the net config lock so /api/state and the main loop never read a
// torn job table. Stashes the fan-out for the main loop to act on.
// SFX event tap: installed on the shared router, so EVERY attention event -
// notifier BLE feed, orchestrator sink, battery policy - reaches the sound
// engine through one seam. JobState goes through per-key edge detection (a
// broker re-sends frames; repeats must not re-voice); the rest map 1:1. All
// gating (per-mode level rank, rate limit, speaker fault, voice mute) happens
// inside sfx::fire - this stays a cheap dispatch.
static void sfxEventTap(const attn::Event& e) {
  using nimbus::sfx::Ev;
  switch (e.type) {
    case attn::Event::Type::JobState:
      // onJobState's per-key edge table separates real edges from the broker's
      // snapshot heartbeats; only a real edge counts as screensaver activity.
      if (::sfx::onJobState(e.key, e.status)) saverKick();
      break;
    case attn::Event::Type::IncomingAsk: ::sfx::fire(Ev::NeedsYou); saverKick(); break;
    case attn::Event::Type::AskCleared:  ::sfx::fire(Ev::AskCleared); saverKick(); break;
    case attn::Event::Type::LowBattery:  ::sfx::fire(Ev::LowBattery); break;
    case attn::Event::Type::BatteryOk:   ::sfx::fire(Ev::BatteryOk); break;
    case attn::Event::Type::NetworkDegraded: ::sfx::fire(Ev::NetDegraded); break;
    case attn::Event::Type::NetworkOk:   ::sfx::fire(Ev::NetOk); break;
    default: break;   // JobProgress / Voice: never voiced from the tap
  }
}

static void orchEventSink(const attn::Event& e) {
  net::ConfigLockGuard lk;
  // A NEW call-to-action overrides a silenced OR model-painted ring: neither
  // "lights off" nor a decorative led pattern may swallow a needs-you/error cue
  // (safety over silence). Ambient events respect both.
  if ((g_lightsOff || g_ledOverrideActive) && !g_highLoadActive &&
      e.type == attn::Event::Type::JobState &&
      e.status <= uint8_t(solide::ring::Status::Offline) &&
      attn::isAttentionStatus(solide::ring::Status(e.status))) {
    g_lightsOff = false;
    g_ledOverrideActive = false;
    g_devActDirty = true;
  }
  const attn::Decision d = g_router.route(e, millis());
  if (d.ringDirty) g_orchRingDirty = true;
  if (d.screen.render) {
    g_orchScreenRender = true;
    g_orchScreenId = uint8_t(d.screen.id);
    g_orchScreenAttn = d.screen.attention;
  }
}

// DeviceSink: execute a VALIDATED led/lights/reboot action from the orchestrator
// (poll task). Only STAGES state under the lock; the main loop owns the LEDs and
// applies it (g_devActDirty), so the poll task never races tickAnimation.
static void orchDeviceSink(const nimbus::orch::ValidatedAction& a) {
  using nimbus::orch::ActionKind;
  if (a.kind == ActionKind::Reboot) { g_rebootPending = true; return; }
  net::ConfigLockGuard lk;
  if (a.kind == ActionKind::Lights) {
    agent::alogf("devsink: lights -> %s", a.lightsOff ? "off" : "full");
    g_lightsOff = a.lightsOff;
    g_ledOverrideActive = false;         // a lights action clears any led pattern
    g_devActDirty = true;
  } else if (a.kind == ActionKind::Led) {
    g_ledOverrideActive = true;
    g_lightsOff = false;
    g_ledOvMode = uint8_t(a.mode);
    g_ledOvR = a.r; g_ledOvG = a.g; g_ledOvB = a.b;
    g_ledOvHasBright = a.hasBrightness;
    g_ledOvBright = a.brightness;
    // A model-painted pattern is decoration, not a call-to-action - bound it to
    // the SAME per-mode hold a real CTA gets (Full 5 min / Balanced 2 / Dark 1,
    // owner-customizable). Without this a single turn's `led` action owned the
    // ring FOREVER (no TTL; the only clear was a new attention event) - the
    // field report was a rainbow breathing at full brightness for hours.
    g_ledOvUntilMs = millis() +
                     uint32_t(g_cfg.effective(nimbus::Param::AttnHoldMs));
    g_devActDirty = true;
  } else if (a.kind == ActionKind::Config) {
    // Talk-to-configure: stage the validated knobs; the main loop applies them
    // through the SAME pipeline as the menu/web UI (g_cfg + applyConfig +
    // persistConfig), so a spoken "go full" behaves exactly like a tap.
    if (a.hasPosture)    g_cfgPendPosture = a.posture;
    if (a.hasProfile)    g_cfgPendProfile = a.profile;
    if (a.hasAttnHoldMs) g_cfgPendAttnHold = a.attnHoldMs;
    if (a.hasTheme) {
      strncpy(g_cfgPendTheme, a.theme.c_str(), sizeof(g_cfgPendTheme) - 1);
      g_cfgPendTheme[sizeof(g_cfgPendTheme) - 1] = 0;
    }
    g_cfgPendDirty = true;
  }
}

// SendSink: deliver an orchestrator reply to the owner over Telegram. Thread-safe
// queue on the device; returns false when Telegram isn't provisioned OR the reply
// queue is full, so the orchestrator can log a dropped message instead of losing it
// silently.
static bool orchSendSink(const String& chatId, const String& text) {
#ifdef NIMBUS_NOTIFIER_DEBUG
  // Mirror every orchestrator reply to serial so a `TURN` test (no Telegram) can
  // see the model's answer. chatId "serial" is the local test channel.
  Serial.printf("ORCH REPLY [%s]: %s\n", chatId.c_str(), text.c_str());
#endif
  // Console TURN channel: the reply's home IS serial - never hand it to
  // telegram::send ("serial" isn't a chat id; it burned 5 failed TLS retries
  // and lost the reply in [env:test], where the echo above is compiled out).
  if (chatId == "serial") {
#ifndef NIMBUS_NOTIFIER_DEBUG
    Serial.printf("ORCH REPLY [serial]: %s\n", text.c_str());
#endif
    return true;
  }
  // On-device voice turns (chatId "voice") route the reply to the panel, not
  // Telegram. Runs on the poll task, so hand it to the main loop via a flag (the
  // panel is main-loop-owned) under the config lock.
  if (chatId == "voice") {
    net::ConfigLockGuard lk;
    g_voiceReply = text;
    g_voiceReplyPending = true;
    return true;
  }
  // Web chat turn (POST /api/chat): capture the reply for GET /api/chat to poll.
  if (chatId == "web") {
    net::ConfigLockGuard lk;
    g_webReply = text;
    g_webReplyPending = true;
    g_webTurnDeadlineMs = 0;   // the turn answered - no longer pending
    return true;
  }
  return agent::telegram::send(chatId, text);
}

// SpeakSink: voice the `tts` device action's text. Prefer the on-device speaker
// (that is what the action promises and what the world prompt now tells the model),
// and fall back to a Telegram audio message only when on-device speech is not
// possible (no speaker / synth failed) - so the action is no longer a silent no-op
// on a device with no Telegram allowlist. Runs on the poll task between long-polls
// (single-TLS: the Telegram socket is closed), so the TTS synth + upload are safe.
static void orchSpeakSink(const String& text) {
  if (text.length() == 0) return;
  // capture=false: the tts device action records the spoken text to history itself
  // (apply.cpp captureAssistant), so speakOnDevice must not also capture it.
  if (agent::orchestrator::speakOnDevice(text, /*capture=*/false)) return;   // played on the speaker
  String allow = agent::store::telegramAllowlist();
  int c = allow.indexOf(','); String chat = (c > 0) ? allow.substring(0, c) : allow; chat.trim();
  if (chat.length() == 0) return;
  size_t n = agent::tts::synthesizeToFile(text, "/tts.mp3", "mp3");
  if (n) agent::telegram::sendMedia(chat, "audio", "/tts.mp3", "Nimbus voice");
}

// Incoming Telegram message -> one orchestrator turn (runs on the poll task, with
// the Telegram TLS socket already closed by the client).
static void orchOnMessage(const String& from, const String& chatId,
                          const String& text) {
  agent::orchestrator::handleMessage(text, from, chatId);
}

// TickCallback: advance background jobs once per poll cycle; returns active count.
static int orchTick() { return agent::orchestrator::pollJobs(); }

// Menu repaint request: the menu paints directly (bypassing the scheduler), but
// the panel takes ~2.2 s per refresh, so a burst of knob events must coalesce.
// Events set this flag; the loop flushes one paint once the menu's OWN refresh
// window (g_menuDoneAt) has elapsed - deliberately independent of the status
// render path so an in-flight/stuck status render can never stall the menu (the
// "long-press shows nothing" failure).
static bool     g_menuNeedsPaint = false;
static uint32_t g_menuDoneAt = 0;   // menu-refresh busy deadline (its own)
static uint32_t g_updCheckKickMs = 0;  // last menu "Check for updates" kick - the loop-body
                                       // reseed holds "Checking..." through the async task
                                       // spin-up instead of overwriting it with stale state
static uint8_t  g_updateAnim = 0;      // Software update indeterminate-bar phase (CUM-193)

// The owner-facing update view (CUM-193): one place maps the live OTA state to
// the status line + affordance flags, so the reseed (status line + HIL MENU?
// seam) and renderMenu (the on-screen status band) never diverge.
static nimbus::ota::UpdateView otaViewNow() {
  return nimbus::ota::updateView(
      nimbus::ota::stateFromStr(otaupd::statusStr()), otaupd::progressPct(),
      otaupd::latestSeen().c_str(), otaupd::lastError(), NIMBUS_FW_VERSION);
}
// (g_rebootPending - the deferred-reboot flag serviced at the top of loop() - is
// declared with the device-action override state above, since the orchestrator
// DeviceSink also sets it for the model's `reboot` action.)

// g_router's job table (solide::ring::Allocator) is mutated by orchEventSink() on
// the tg_poll task (core 0) under net::ConfigLockGuard. Its writes are multi-field
// and non-atomic (upsert sets used=true then fills key/status; remove clears the
// whole slot), so EVERY reader - count()/snapshot() - must take the SAME lock or it
// can observe a half-written slot or a torn count. The guard is a non-recursive
// portMUX spinlock, so these helpers must never be called while already holding it;
// callers here only take it around flag-drain/menu writes (which read no jobs).
static int jobCount() {
  net::ConfigLockGuard lk;
  return g_router.jobs().count();
}

// A live job or a top-attention call-to-action is on the ring - cosmetic overlays
// (the theme flourish) must yield to it. Reads g_router under the config lock.
static bool ringHasAttention() {
  net::ConfigLockGuard lk;
  return g_router.topAttention().active || g_router.jobs().count() > 0;
}


// Point-to-focus universe (Orchestrator mode). The Orchestrator itself is ALWAYS
// focus index 0 - the head agent you're always talking to - so the encoder never
// lands on "nothing" and SessionDetail never shows "no active session". The heavy-
// fabric sub-sessions (g_sessionList) are focus indices 1..N. (This is the on-device
// UX view only; the LLM prompt's "RUNNING SESSIONS" digest stays sub-agents-only, so
// "none running" there remains authoritative - a different axis.)
static size_t focusCount() { return 1 + g_sessionList.size(); }

// The active orchestrator host provider, for the root's label: the explicit orchHost
// override, else the first KEYED provider in the priority list (so the label matches
// the provider a turn will actually run on), else "auto".
static std::string activeHostProvider() {
  const String h = agent::store::resolvedOrchHost();
  return h.length() ? std::string(h.c_str()) : std::string("auto");
}

// Populate ScreenCtx session fields for focus index `idx`: 0 = the Orchestrator root
// (live - provider/state computed now, not snapshotted), >=1 = a sub-session snapshot.
static void fillFocus(render::ScreenCtx& c, size_t idx) {
  if (idx == 0) {
    c.sessionIsRoot   = true;
    c.sessionTitle    = "Nimbus";
    c.sessionProvider = activeHostProvider();
    c.sessionState    = g_voiceActive ? "listening" : "ready";
    return;
  }
  const auto& s = g_sessionList[idx - 1];
  c.sessionTitle    = s.title;
  c.sessionProvider = s.provider;
  c.sessionState    = s.state;
}

static std::string configUrl();      // defined below (net-derived, token-carrying)
static std::string setupUrl();       // defined below (ALWAYS the SoftAP address - P1.2)
static std::string netStatusLine();  // defined below (one-line connectivity readout)

// Fill the ScreenCtx fields the shared panel HEADER reads (mode / profile /
// posture + WiFi/BT glyphs + battery % + net-degraded "!"). Called from BOTH the
// status render (buildCtx) and the menu render (renderMenu) so the header is
// identical on EVERY screen - the menu path used to leave these defaulted, which
// hid the battery % and froze the glyphs at wi-/bt- (off) on all sub-menus.
static void fillHeaderCtx(render::ScreenCtx& c) {
  c.deviceName = std::string(sys::deviceName().c_str());   // top-left identity (which unit is this)
  c.modeName = (g_menu.mode() == Mode::Orchestrator) ? "orchestrator" : "notifier";
  c.posture = g_cfg.posture();
  c.profileName = profileLabel(g_cfg.profile());   // header shows the battery-mode
                                                   // vocabulary (Dark/Balanced/Full)
  // Header radio glyphs: WiFi up(2)/connecting(1)/off(0); BT linked(2)/
  // advertising(1)/off(0). BLE only runs in Notifier mode, so it's off in Orch.
  c.wifiState = net::staConnected() ? 2 : (net::staConfigured() ? 1 : 0);
  c.btState   = g_orchMode ? 0
                           : (net::ble::connected() ? 2
                              : (net::ble::enabled() ? 1 : 0));
  c.networkDegraded = g_router.networkDegraded();  // header "!" marker (was never set anywhere)
  // Header L2 sound state: the ACTIVE mode's SFX level + master volume (R4).
  c.sfxLevel  = g_orchMode ? agent::store::sfxLevelOrch() : agent::store::sfxLevelNotif();
  c.sfxVolume = agent::store::sfxVolume();
  // Header L1 SD token (owner ask): in/out at a glance on every screen.
  c.sdShort = agent::memory::sdLost() ? "lost"
            : (solide::storage::available() ? "ok" : "none");
  // Battery telemetry for the header (invalid -> glyph hidden). Show the
  // BatteryModel's corrected SoC + charge state, not the raw driver percent (the
  // S3 ADC under-reads a full pack; the model calibrates + infers charging/full).
  c.battery = g_power.last();
  if (c.battery.valid) {
    c.battery.percent = g_battEstimate.percent;
    c.battery.onExternalPower = g_battEstimate.onExternalPower;
    c.battery.charging = g_battEstimate.chargeState == nimbus::power::ChargeState::Charging;
    // Hysteresis on the DISPLAYED percent: the S3 ADC (÷3.2 divider, no coulomb
    // counter) dithers the reading +/-1-2%, which would otherwise change the header
    // frame every telemetry tick and defeat the panel dirty-gate - a redundant
    // ~2.2 s refresh once a minute forever. Only move the shown value on a
    // sustained >=2-point change (0/100 always shown exactly). Purely cosmetic
    // for the header; the model's raw SoC is unchanged for analytics/logging.
    static int s_shownPct = -1;
    const int p = c.battery.percent;
    if (s_shownPct < 0 || p == 0 || p == 100 ||
        p >= s_shownPct + 2 || p <= s_shownPct - 2)
      s_shownPct = p;
    c.battery.percent = uint8_t(s_shownPct);
    // F27: the trend GLYPH (^/=/none, from charging+onExternalPower) had NO
    // hysteresis while the % beside it did - a dithering charge inference flipped
    // it every ~2 s telemetry tick, changing the header frame and defeating the
    // panel dirty-gate (a redundant ~2.2 s refresh a minute, WiFi-independent).
    // Debounce the trend pair the same way: only commit a NEW trend that has held
    // for ~3 consecutive ticks (~6 s); a real sustained transition still shows.
    static int8_t s_shownTrend = -1;   // -1 unset; 0 draining, 1 external, 2 charging
    const int8_t trend = c.battery.charging ? 2 : (c.battery.onExternalPower ? 1 : 0);
    static int8_t s_pendTrend = -1, s_pendCount = 0;
    if (s_shownTrend < 0) {
      s_shownTrend = trend;
    } else if (trend == s_shownTrend) {
      s_pendCount = 0;                 // settled back - cancel any pending flip
    } else if (trend != s_pendTrend) {
      s_pendTrend = trend; s_pendCount = 1;   // new candidate
    } else if (++s_pendCount >= 3) {
      s_shownTrend = trend; s_pendCount = 0;  // held -> commit
    }
    c.battery.charging       = (s_shownTrend == 2);
    c.battery.onExternalPower = (s_shownTrend == 1);
  }
}

static render::ScreenCtx buildCtx(int cursorJob) {
  render::ScreenCtx c;
  fillHeaderCtx(c);   // mode/profile/posture + WiFi/BT/battery/net-degraded header
  // Setup/QR context - filled on EVERY build (P2/P3): SetupInfo shows the AP
  // SSID + a scan-to-configure QR on unprovisioned boot, and a scheduler-driven
  // ConfigQr (the repeated-401 reaction) needs the URL without going through
  // renderMenu. Cheap string builds; renders are ~2 s events.
  c.apName = std::string(net::apSsid().c_str());
  c.apPass = std::string(net::apPass().c_str());   // per-device stored passphrase
  c.apUp = ((uint32_t)WiFi.softAPIP() != 0u);
  c.staConnected = net::staConnected();   // locked-out ConfigQr shows AP creds (CUM-200)
  c.configUrl = configUrl();
  c.setupUrl = setupUrl();   // SetupInfo QR: always the AP address (P1.2)
  c.fwVersion = NIMBUS_FW_VERSION;   // shown small on the Setup screen
  c.webToken = std::string(agent::store::webAuthToken().c_str());
  c.netStatus = netStatusLine();
  c.cursorJob = cursorJob;
  // SessionDetail (Orchestrator): the focused agent under the encoder cursor. Index 0
  // is always the Orchestrator root; 1..N are sub-sessions (g_sessionList, main-task
  // owned - same task as this). Clamp the cursor against the full focus count.
  if (g_orchMode) {
    size_t idx = g_cursor.index();
    if (idx >= focusCount()) idx = 0;
    fillFocus(c, idx);
  }
  if (g_askOverride.length()) {
    c.askText = std::string(g_askOverride.c_str());  // mode-switch / voice screen
    c.askClosable = !g_askTransient;                 // no Close on a pre-reboot notice
    c.detailPage = g_askPage;                        // rotate pages a held reply (P2.3)
  }
  // Copy the slots under the lock into a local buffer (tiny, no heap), then build
  // the vector OUTSIDE the critical section - push_back allocates and must never run
  // under a spinlock.
  solide::ring::Slot snap[RING_MAX_SEGMENTS];
  int n;
  bool lowBatt;
  attn::Router::Attention top;
  {
    net::ConfigLockGuard lk;
    n = g_router.jobs().snapshot(snap, RING_MAX_SEGMENTS);
    lowBatt = g_router.lowBattery();
    top = g_router.topAttention();
  }
  for (int i = 0; i < n; ++i) {
    // Skip the synthetic "head" turn job on the PANEL (it stays on the ring): its
    // Running/Offline edges re-rendered the job list at the start and end of every
    // turn - two full 2.2 s panel flashes per turn, four with the synthesis turn
    // (owner: "heavy flickering while processing", 2026-07-16).
    if (g_orchMode && snap[i].key == agent::orchestrator::headJobKey()) continue;
    // nsn v2: name the session (harness + title) from the notifier Mapper, keyed by
    // segment index == job key. Empty in Orchestrator mode / on a v1 broker.
    const char* title = g_orchMode ? "" : g_notifier.mapper().titleOf(snap[i].key);
    const uint8_t harness = g_orchMode ? 0 : g_notifier.mapper().harnessOf(snap[i].key);
    c.jobs.push_back({snap[i].key, uint8_t(snap[i].status), snap[i].progress,
                      snap[i].hasAccent ? snap[i].accentHue : uint8_t(255),
                      std::string(title), harness});
  }

  // (header battery/WiFi/BT is filled by fillHeaderCtx above.)

  // BLE secure-pairing passkey for the Pairing screen. DORMANT under the active
  // Just Works config (pairingActive() never becomes true - macOS bonds with no
  // passkey); populates only if MITM is re-enabled, when the panel becomes
  // the code's channel.
  if (net::ble::pairingActive()) {
    char pk[8];
    std::snprintf(pk, sizeof pk, "%06u", unsigned(net::ble::pairingPasskey()));
    c.pairingCode = pk;
  }
  // Cloud (cumulo-nimbus) pairing: the claim code + a scannable claim URL. claimUrl
  // non-empty switches the Pairing screen to the cloud variant (see the renderer).
  if (nimbus::relay::pairingActive()) {
    c.pairingCode = nimbus::relay::claimCode().c_str();
    c.claimUrl = nimbus::relay::claimUrl().c_str();
  }

  // Badge text: low battery wins, else the top attention job's state.
  c.badgeActive = lowBatt || top.active;
  if (lowBatt) {
    c.badgeText = "LOW BATT";
  } else if (top.active) {
    switch (top.status) {
      case solide::ring::Status::AwaitingApproval: c.badgeText = "APPROVE?"; break;
      case solide::ring::Status::WaitingInput:     c.badgeText = "INPUT?";   break;
      case solide::ring::Status::Error:            c.badgeText = "ERROR";    break;
      default:                                     c.badgeText = "!";        break;
    }
  }

  // On-screen ring: a board with no physical LED ring mirrors the composited ring
  // frame onto the panel (the notifier draws it as a dot-ring). Only meaningful on
  // the colour panel; the renderer ignores ringLeds.
  if (g_screenIsTft && !solide::board().hasRing) {
    const solide::ring::RGB* rf = hw::currentRingFrame();
    const int n = hw::currentRingCount();
    c.ringLeds.resize(size_t(n));
    for (int i = 0; i < n; ++i)
      c.ringLeds[size_t(i)] = { rf[i].r, rf[i].g, rf[i].b };
    c.micHeld = g_voiceActive;   // instant pressed feedback on the hold-to-talk button
  }
  return c;
}

static void renderScreen(attn::ScreenId screen, int cursorJob, bool fullClear) {
  (void)fullClear;   // legacy panel refresh hint; the color panel ignores it
  const uint32_t tnow = millis();
  // Honour a dropped push. renderAndPush returns false when the previous
  // ~31 ms blit is still in flight - and it only swaps the tap map on a
  // SUCCESSFUL push, so pretending it painted leaves the panel AND the hit
  // regions a screen behind while the FSM has already moved on. Leave
  // g_lastScreen alone so the caller's entry gates still re-fire.
  const auto push = hw::tft::renderAndPush(screen, buildCtx(cursorJob));
  if (push == hw::tft::Push::Dropped) {
    g_sched.onRenderDone(tnow);   // never leave the scheduler latched
    return;                       // panel is a frame behind; caller retries
  }
  // Pushed OR Unchanged: the panel genuinely shows this screen, so the entry
  // gates that key off g_lastScreen must see it (an Unchanged frame is a
  // SUCCESS - treating it as a drop is what latched the repaint loop).
  g_lastScreen = uint8_t(screen);
  // ⚠ Release the scheduler's busy latch. g_sched.tick() sets busy_ when it
  // issues a render and clears it ONLY in onRenderDone(). A frame is ~31 ms with
  // no dwell to wait out, so it is done the moment the push returns - but if this
  // call is missed the scheduler stays busy forever and every ambient StatusIdle
  // refresh, CTA badge and detail intent silently stops after the FIRST frame.
  g_sched.onRenderDone(tnow);
#ifdef NIMBUS_TEST
  // RENDER? must follow every push, or it reports a stale frame forever and every
  // test asserting on it passes against a frozen value.
  tc::onRender(g_lastScreen, uint8_t(g_cfg.posture()), g_lastSeg, g_lastSingle,
               g_lastDark, g_lastBright);
#endif
}

// Long-press-to-talk (Orchestrator, Phase 4): record the mic, transcribe via the
// configured STT provider (Mistral Voxtral by default), and inject the transcript
// as an orchestrator turn hinting at the cursor-focused session ("[re: <id>]").
// Blocks ~4-5 s (record + STT) so the main-loop watchdog is suspended across it
// (like the console MICREC). The reply echoes on the injected chatId's sink.
// Sub-session tell/poll is unsupported, so this always talks to the ORCHESTRATOR,
// never the sub-agent directly - the '[re:]' hint is a human-style reference.
static void refreshRing();                                // fwd decls (defined below)
static void renderScreen(attn::ScreenId screen, int cursorJob, bool fullClear = false);

// Screensaver activity: reset the idle clock and, if the logo saver is on the
// panel, restore live status first - so whatever caused the activity paints
// over a live screen, and a knob event that renders nothing (e.g. a rotate
// with no jobs) can't leave the logo stuck with the owner present.
static void saverKick() {
  g_saver.noteActivity(millis());
  // On a TFT the BACKLIGHT is the idle draw, so resting the screen means
  // blanking it - a drawn screensaver at full brightness would cost more power
  // than showing live status. Restore it on any activity.
  // Restore to the BATTERY MODE's level, not a hardcoded 100 - waking a resting
  // screen straight to full brightness ignored the mode the owner chose.
  if (g_screenIsTft && hw::tft::backlight() <= nimbus::kBacklightRestPct)
    hw::tft::setBacklight(nimbus::backlightPctFor(g_cfg.posture()));
  // ⚠ Do NOT renderScreen() here. saverKick fires from the attention event tap
  // (sfxEventTap), which the Router runs SYNCHRONOUSLY on whatever task routed the
  // event - during a turn that is tg_poll (handleMessage -> clearAsk -> route ->
  // tap -> saverKick). Rendering there pushes buildCtx()'s ScreenCtx + a devName
  // NVS read onto the already-deep turn stack AND races the main render task's NVS
  // handle -> nvs::Lock abort() (backtrace decoded live 2026-08-04, fan-out turn).
  // Defer to loop() (the only task that renders), mirroring g_orchScreenRender.
  if (g_lastScreen == uint8_t(attn::ScreenId::Screensaver) && !g_menu.isOpen())
    g_saverRestoreReq = true;
}

// Watches the encoder button while recording and signals recordToFile to stop the
// instant it is RELEASED - this is what makes it true hold-to-talk. Exits once the
// main task marks g_voiceDone (so it never double-frees or leaks if the record hit
// its max while still held).
static void voiceReleaseWatcher(void*) {
  // Two guards make the release robust: (1) a MIN-CAPTURE floor so even a quick
  // release still records ~kMinMs - the PDM mic's first ~100 ms is warm-up ramp
  // (near-silence), so a too-short clip transcribes to nothing; (2) a DEBOUNCE so
  // a mechanical bounce mid-hold can't look like a release and truncate speech.
  const uint32_t start = millis();
  const uint32_t kMinMs = 900;        // floor: warm-up + a short word
  const int kReleaseStable = 4;       // ~60 ms of sustained "not pressed" = real release
  int releasedFor = 0;
  // ⚠ Which control is being held depends on the board. On a TFT the encoder is
  // NOT fitted - solide::begin() never starts its task, so pressed() is false
  // FOREVER and every touch dictation stopped at the kMinMs floor (~0.96 s) no
  // matter how long the owner held the mic. Read the touch driver directly:
  // hw::touch::poll() cannot be used here because the main task is blocked
  // inside recordToFile for the whole capture, and polling would also eat
  // gestures the drain owes.
  const auto stillHeld = []() -> bool { return solide::touch::read().down; };
  while (!g_voiceDone) {
    if (stillHeld()) {
      releasedFor = 0;                // still held (or a bounce recovered): keep going
    } else if (millis() - start >= kMinMs) {
      if (++releasedFor >= kReleaseStable) break;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
  g_voiceStop = true;                 // button up (debounced, past the floor) -> stop recordToFile
  while (!g_voiceDone) vTaskDelay(pdMS_TO_TICKS(20));
  vTaskDelete(nullptr);
}

// Voice hold-to-talk sub-steps, split out of captureVoiceTurn to keep it under the
// complexity gate. All run on the main task and touch the same file-scope voice +
// screen state captureVoiceTurn does.

// Pre-flight: returns true (having shown the reason on-screen) when a voice turn
// can't start now - firmware updating, or no speech-to-text key configured.
static bool voiceStartBlocked() {
  if (otaupd::installing()) {  // flash write + TLS own the device right now
    g_askOverride = "Updating firmware - try again after the restart.";
    g_askSticky = true; g_askPage = 0;
    renderScreen(attn::ScreenId::Ask, -1);
    return true;
  }
  if (!agent::stt::available()) {
    // Give the owner ACTUAL feedback instead of a silent no-op on a silent-serial
    // device: a panel line telling them voice needs a key (audit device-flows).
    Serial.println("VOICE: no STT provider key");
    g_askOverride = "Voice needs a speech-to-text key (set one in the web UI).";
    g_askSticky = true; g_askPage = 0;   // hold until click (P2.3)
    renderScreen(attn::ScreenId::Ask, -1);
    return true;
  }
  return false;
}

// Instant press feedback: repaint so the hold-to-talk button shows pressed the
// moment it is touched (the region-only ring repaint would miss it - the button
// sits outside the ring rectangle). On a panel-ring board also paint a static
// LISTENING frame (the whole ring in the theme hue): the physical Pulse drives
// only the absent LED, and recordToFile blocks the loop so the animator can't
// breathe it live - a steady lit ring is the honest single-task listening cue.
static void voicePressFeedback() {
  if (!g_screenIsTft || g_menu.isOpen()) return;
  if (!solide::board().hasRing) {
    nimbus::ThemeColor lt =
        nimbus::themeAccent(std::string(agent::store::theme().c_str()));
    hw::paintRingSolid(lt.r, lt.g, lt.b);
  }
  renderScreen(attn::ScreenId::StatusIdle, -1);
}

// Release feedback: recompose + un-press the mic button if still on the ring home
// (a reply/ask screen otherwise replaces it, un-pressing it by leaving Idle).
static void voiceReleaseFeedback() {
  if (!g_screenIsTft || g_menu.isOpen() ||
      g_lastScreen != uint8_t(attn::ScreenId::StatusIdle))
    return;
  refreshRing();   // replace the static LISTENING frame with the real idle ring
  renderScreen(attn::ScreenId::StatusIdle, -1);
}

// Nothing usable was heard: LEDs off, ring back to idle, and a panel-aware retry
// hint held until the owner clicks.
static void voiceEmptyTranscript() {
  solide::leds::off();
  refreshRing();
  // Hold-to-talk is the mic button on the touch panel.
  g_askOverride = "Didn't catch that - hold the mic button and speak.";
  g_askSticky = true; g_askPage = 0;   // hold until click (P2.3)
  renderScreen(attn::ScreenId::Ask, -1);
}

static void captureVoiceTurn() {
  if (g_voiceActive) return;   // re-entry guard: LongPress auto-repeats while held
  if (voiceStartBlocked()) return;
  g_voiceActive = true;
  voicePressFeedback();
  String reId;  // snapshot the focused session id BEFORE recording (it can complete mid-record)
  // Focus 0 = the Orchestrator itself -> talk to it directly, no "[re:]" hint. Focus
  // >=1 = a sub-session -> hint the turn at that job's id (still routed to the head).
  {
    size_t idx = g_cursor.index();
    if (idx >= 1 && (idx - 1) < g_sessionList.size())
      reId = g_sessionList[idx - 1].id.c_str();
  }
  // IMMEDIATE LED feedback: the ring turns a self-animating red breathe the instant
  // you hold (the panel is far too slow to be the primary cue). It keeps pulsing
  // through the blocking record because solide::leds Patterns self-animate.
  nimbus::ThemeColor th = nimbus::themeAccent(std::string(agent::store::theme().c_str()));
  solide::leds::clearFrame();
  solide::leds::show(solide::leds::Pattern::Pulse, th.r, th.g, th.b);  // theme breathe = LISTENING
  Serial.println("VOICE: hold-to-talk recording (release to stop)...");
  // HOLD-TO-RECORD: record until the button is released (watcher sets g_voiceStop),
  // capped at 60 s so a stuck button can't record forever (was 15 s - raised once
  // the WAV double-copy died; see transcribePcm). The cap is also CLAMPED by free
  // LittleFS space (32 KB/s at 16 kHz mono 16-bit, 256 KB reserved for the other
  // flash writers) so a full partition truncates the CAP, not the capture.
  // SFX is MUTED for the whole capture (a clip played mid-record would pollute
  // the STT audio; the red breathe ring is the "listening" cue instead - no
  // VoiceListen sound by design).
  uint32_t capMs = 60000;
  {
    const size_t freeB = LittleFS.totalBytes() - LittleFS.usedBytes();
    const size_t avail = freeB > 262144 ? freeB - 262144 : 0;
    const uint32_t fitMs = (uint32_t)(avail / 32);   // 32 bytes per ms
    if (fitMs < capMs) capMs = fitMs;
  }
  ::sfx::setMuted(true);
  g_voiceStop = false; g_voiceDone = false;
  xTaskCreatePinnedToCore(voiceReleaseWatcher, "vrel", 2560, nullptr, 4, nullptr, 1);
  esp_task_wdt_delete(nullptr);
  const uint32_t recStart = millis();
  size_t bytes = solide::audio::recordToFile(LittleFS, "/voice.pcm", capMs, &g_voiceStop);
  const uint32_t recMs = millis() - recStart;
  ::sfx::setMuted(false);
  ::sfx::fire(nimbus::sfx::Ev::VoiceStop);   // "I gotcha." - capture confirmed (heavy tier)
  // 16 kHz mono 16-bit = 32000 B/s; ~<0.4 s of audio can't hold a word.
  Serial.printf("VOICE: recorded bytes=%u durMs=%lu (~%.2fs audio)%s\n",
                (unsigned)bytes, (unsigned long)recMs, bytes / 32000.0f,
                bytes < 12000 ? "  <-- SHORT, likely warm-up only" : "");
  agent::alogf("[voice] recorded bytes=%u durMs=%lu audio=%.2fs%s",
               (unsigned)bytes, (unsigned long)recMs, bytes / 32000.0f,
               bytes < 12000 ? " SHORT/warmup" : "");
  solide::leds::show(solide::leds::Pattern::Spinner, th.r, th.g, th.b);  // theme sweep = TRANSCRIBING
  // WAV header streams INLINE in the upload (transcribePcm) - no /voice.wav copy,
  // so the partition pays once per capture and 60 s fits where 15 s used to.
  String transcript = agent::stt::transcribePcm("/voice.pcm", 16000);
  esp_task_wdt_add(nullptr);
  g_voiceDone = true;   // release the watcher task
  Serial.printf("VOICE: bytes=%u transcript=\"%s\"\n", (unsigned)bytes, transcript.c_str());
  agent::alogf("[voice] result transcriptLen=%u text=\"%.80s\"",
               (unsigned)transcript.length(), transcript.c_str());
  g_voiceActive = false;
  voiceReleaseFeedback();
  if (transcript.length() == 0) { voiceEmptyTranscript(); return; }
  g_askOverride = String("You: ") + transcript;   // show what was heard on the panel
  g_askSticky = true; g_askPage = 0;   // hold the transcript while the turn runs;
                                       // the reply overwrites it directly (P2.3)
  agent::orchestrator::clearAsk();   // the owner just answered by voice -> clear any pending ask
  renderScreen(attn::ScreenId::Ask, -1);
  // Durable audit copy of the spoken turn -> /mem/blobs on the SD (SD-gated no-op).
  // Raw headerless PCM now (the WAV copy no longer exists) - the transcript is the
  // primary record; the blob is a 16 kHz/16-bit/mono forensic artifact.
  agent::memory::captureMediaFile("voice", "user", nimbus::orch::MsgKind::Audio,
                                  transcript, "/voice.pcm", "pcm");
  String msg = reId.length() ? (String("[re: ") + reId + "] " + transcript) : transcript;
  if (msg.length() > 4000) msg = msg.substring(0, 4000);   // inbound text buffer is 4097
                                                           // (was a stale 1000-char guard
                                                           // from the old 1024 buffer)
  // WAIT FEEDBACK: the turn runs async on the poll task, so keep a theme spinner
  // running from send until the reply lands (cleared in the reply drain / a timeout in
  // loop()) - the ring shows the agent is working instead of going dark.
  solide::leds::show(solide::leds::Pattern::Spinner, th.r, th.g, th.b);
  g_voiceWaiting = true; g_voiceWaitStart = millis();
  if (!agent::telegram::injectMessage("voice", msg))    // reply routes back to the panel (see orchSendSink)
    agent::alog("voice: inbound queue full - transcript dropped");
}

// Mode-switch visual: a short LED sweep in the destination mode's colour (teal for
// Orchestrator, amber for Notifier) so the switch is unmistakable before the reboot
// drops the CDC. LEDs are instant; the panel can't render in the reboot window.
static void playModeSwitchFeedback(bool toOrch) {
  // Breathe through the current THEME's PALETTE (rainbow cycles ROYGBIV, ember red->
  // amber, etc.) over ~2.4 s - long enough for the ~2.2 s panel transition screen.
  nimbus::ThemeColor pal[nimbus::kThemeMaxColors];
  int n = nimbus::themePalette(std::string(agent::store::theme().c_str()), pal, nimbus::kThemeMaxColors);
  if (n < 1) n = 1;
  solide::leds::clearFrame();
  Serial.printf("MODE switch visual -> %s\n", toOrch ? "orchestrator" : "notifier");
  Serial.flush();
  const int step = 2400 / n;
  for (int i = 0; i < n; ++i) {
    solide::leds::show(solide::leds::Pattern::Pulse, pal[i].r, pal[i].g, pal[i].b);
    delay(step);
  }
}

static void refreshRing() {
  // During an OTA install the ring is the PRIMARY "do not power off" indicator -
  // otaLoopUx owns it as a live progress bar. Nothing else may re-compose over it
  // (a Telegram-turn reply / Notifier job edge / status render would otherwise
  // stomp the progress bar mid-flash-write). Released when the install ends.
  if (otaupd::installing()) return;
  // Orchestrator device-action overrides own the ring: an explicit `led` pattern
  // must not be clobbered by a re-compose, and an owner-silenced (lights:off) ring
  // stays dark (a NEW attention event clears the silence in orchEventSink first,
  // so calls-to-action still get through).
  if (g_ledOverrideActive) return;
  if (g_lightsOff) {
    solide::leds::clearFrame();   // release any raw-frame (Full animator) hold
    solide::leds::off();
    // On a panel-ring board the ring is mirrored from g_animBuf, which the off()
    // above does NOT clear - without this the 30 fps panel repaint keeps showing the
    // last frame, so a lights:off (or any lights-off state) would FREEZE the ring
    // instead of darkening it. Zero the buffer so the mirror goes dark.
    if (g_screenIsTft && !solide::board().hasRing) hw::paintRingSolid(0, 0, 0);
#ifdef NIMBUS_TEST
    g_lastSeg = 0; g_lastSingle = false; g_lastDark = true; g_lastBright = 0;
    tc::onRender(g_lastScreen, uint8_t(g_cfg.posture()), 0, false, true, 0);
#endif
    return;
  }
  // Boot flourish: a calm breathing-WHITE ring for the first ~kBootBreatheMs after
  // boot, instead of snapping to the discrete status colours. A real attention
  // event (a job / CTA) ends it early so nothing urgent is hidden; otherwise loop()
  // recomposes when the window expires. Pattern::Pulse self-animates the breathe.
  if (g_bootBreatheUntilMs != 0 && int32_t(millis() - g_bootBreatheUntilMs) < 0) {
    bool attention;
    { net::ConfigLockGuard lk; attention = g_router.topAttention().active ||
                                           g_router.jobs().count() > 0; }
    if (!attention) {
      solide::leds::clearFrame();
      solide::leds::setBrightness(
          nimbus::power::clampBright(ringBrightByte(), agent::store::brightOvr()));
      solide::leds::show(solide::leds::Pattern::Pulse, 255, 255, 255);  // breathing white
      return;
    }
    g_bootBreatheUntilMs = 0;   // an early event ends the boot flourish
  }
  // compose() reads g_router's job table (snapshot + topAttention), which the
  // orchestrator sink mutates on the poll task - take the config lock across it so
  // it can't read a torn slot/count. compose() does no heap work, so it is safe
  // inside the spinlock's critical section; applyRingPlan() runs after, unlocked.
  // The LED THEME as a ring hue: tints the Calm "working" breathe + the idle cursor
  // glow so the user's chosen colour actually shows on the ring (semantic per-agent
  // segment hues stay as-is). Read once, outside the lock.
  const std::string themeName = std::string(agent::store::theme().c_str());
  const uint8_t th = nimbus::themeHue(themeName);
  // Also read OUTSIDE the lock: this is an NVS read, and the config lock is a
  // non-recursive portMUX spinlock where blocking work is forbidden.
  const bool lowBattCueOn = agent::store::lowBattRing();
  uint8_t attnTh = 255;
  ring::Plan p;
  {
    net::ConfigLockGuard lk;
    // Theme-resolve the TOP ATTENTION hue for the Dark/Calm single LED (owner R3:
    // the last wire-hue passthrough). INSIDE the lock: the orchestrator sink
    // mutates g_router on the tg_poll task, and compose() below re-reads
    // topAttention() - an unlocked read here was both torn-read-prone and a
    // TOCTOU against compose's own locked read (review finding 2026-07-13).
    {
      const attn::Router::Attention att = g_router.topAttention();
      if (att.active) {
        const nimbus::StatusStyle ss = nimbus::statusStyle(att.status);
        attnTh = ss.alert ? nimbus::themeAlertHue(themeName)
                          : nimbus::themeRoleHue(themeName, ss.roleIdx);
      }
    }
    // orchWorking (a turn is running) + g_lastActivityMs (last sub-agent start/finish)
    // drive the Calm-level activity cue; ignored in Dark/Full and Notifier mode.
    // ComposeOpts (not positional args) because a comment reflow TWICE swallowed
    // `working, g_lastActivityMs,` here and the call still compiled - args shifted
    // left, 1500 landed in the orchWorking bool, and Balanced breathed 24/7 (the
    // owner's "still breathing white", d3a800d + the worktree-harness build). One
    // full statement per field: a future reflow now breaks the build or falls back
    // to the struct's FAIL-DARK defaults - it can never latch a light on.
    ring::ComposeOpts co;
    co.cursorDecayMs = 8000;   // panel needs ~2 s to even show the detail - the
                               // cursor marker must outlive the refresh + reading time
    co.orchWorking = g_orchMode && agent::orchestrator::turnInFlight() && !g_workingCeilingHit;
    // Orchestrator only: live sub-sessions split the Balanced ring into per-
    // session arcs (the notifier-style fan-out view). The Notifier must never
    // set this - its Calm single-LED grammar is a deliberate contract.
    co.fanoutSegments = g_orchMode;
    co.lastActivityMs = g_lastActivityMs;
    co.activityWindowMs = 1500;
    co.themeHue = th;
    co.reveal = g_revealUntilMs != 0 && int32_t(millis() - g_revealUntilMs) < 0;
    co.attnThemedHue = attnTh;
    co.lowBattCue = lowBattCueOn;   // owner opt-in; default OFF = no low-batt light
    // While a web preview is active, compose from a throwaway Config seeded with
    // the previewed profile's presets instead of g_cfg - router/cursor state stays
    // live, only the profile-derived posture/brightness knobs change, and g_cfg is
    // never touched (loop() reverts by letting g_previewActive expire).
    if (g_previewActive) {
      Config previewCfg;
      previewCfg.setProfile(g_previewProfile);
      p = ring::compose(g_router, previewCfg, g_cursor, /*panelBusy=*/false, millis(), co);
    } else if (g_screenIsTft && !solide::board().hasRing) {
      // On a board with NO physical LED ring, the ring is drawn on the panel, so
      // the battery-mode postures (Dark/Calm) that collapse the LED ring to a
      // single dim LED to save POWER make no sense - there is no LED power to save,
      // and the collapse froze the on-screen ring (g_animActive went false, so
      // tickAnimation stopped advancing g_animBuf and the panel mirrored a stale
      // frame = the "stuck full green" bug). Compose the FULL per-session ring at
      // full brightness always; the battery mode dims the BACKLIGHT instead.
      Config ringCfg;
      ringCfg.setProfile(ProfileId::Desk);   // Desk => Full posture + full brightness
      p = ring::compose(g_router, ringCfg, g_cursor, /*panelBusy=*/false, millis(), co);
    } else {
      p = ring::compose(g_router, g_cfg, g_cursor, /*panelBusy=*/false, millis(), co);
    }
  }
  // Preview DEMO (audit P1.4): with no live jobs an idle ring composes DARK (the
  // idle-dark change), so "Preview selected look" looked like a dead button - it
  // previewed... darkness. Synthesize a representative look instead: Full posture
  // shows a Running arc + a needs-you arc (themed below like real segments, with
  // the grow-in/collapse lifecycle from the Animator's key deltas); Dark/Calm show
  // their single-LED attention breathe. Synthetic keys; reverts when the preview
  // window expires and the keys vanish (collapse animation included).
  if (g_previewActive && p.segCount == 0 && p.voice == attn::VoiceStage::None &&
      !p.single.lit) {
    if (p.posture == Posture::Full) {
      auto demoSeg = [&](int i, uint32_t key, solide::ring::Status st) {
        p.segs[i].used = true;
        p.segs[i].key = key;
        p.segs[i].status = st;
        p.segs[i].enteredAt = millis();
        p.segs[i].progress = 0;
        p.segs[i].hasAccent = false;   // themed by the role-hue overwrite below
        p.segs[i].accentHue = 0;
      };
      if (g_previewStatus >= 0) {
        // Mirror the simulator's picked status across a few arcs so its color +
        // motion read clearly (matches the page's "Full lights the whole ring").
        const solide::ring::Status st = solide::ring::Status(g_previewStatus);
        demoSeg(0, 0xD3300001u, st);
        demoSeg(1, 0xD3300002u, st);
        demoSeg(2, 0xD3300003u, st);
        p.segCount = 3;
      } else {
        demoSeg(0, 0xD3300001u, solide::ring::Status::Running);
        demoSeg(1, 0xD3300002u, solide::ring::Status::WaitingInput);
        p.segCount = 2;
      }
    } else {
      // Dark/Calm preview: the single attention LED, animating the picked status in
      // its theme hue (default: the needs-you breathe) at the previewed brightness.
      const solide::ring::Status st = g_previewStatus >= 0
                                          ? solide::ring::Status(g_previewStatus)
                                          : solide::ring::Status::WaitingInput;
      const nimbus::StatusStyle ss = nimbus::statusStyle(st);
      p.single.lit = true;
      p.single.hue = ss.alert ? nimbus::themeAlertHue(themeName)
                              : nimbus::themeRoleHue(themeName, ss.roleIdx);
      p.single.anim = uint8_t(ss.anim);
      p.single.periodMs = 2600;
    }
  }
  // Owner: the THEME drives the ring, keyed by STATUS. Overwrite each live segment's
  // hue with the theme-family role hue for its status (Error -> the theme's alert
  // hue) so a theme change actually recolors the ring and statuses stay family-
  // distinct (the "all green / theme ignored" fix). The per-status ANIMATION pattern
  // is applied downstream in applyRingPlan() from the same statusStyle() source.
  for (int i = 0; i < p.segCount; ++i) {
    if (p.segs[i].status == solide::ring::Status::Idle) {
      // Idle is deliberately THEME-LESS (owner: a warm theme's dim primary read
      // as "red idle"): neutral white at low style brightness - dull, static, boring.
      p.segs[i].hasAccent = true;
      p.segs[i].accentHue = 255;   // 255 = white per the accent convention
      continue;
    }
    const nimbus::StatusStyle ss = nimbus::statusStyle(p.segs[i].status);
    p.segs[i].hasAccent = true;
    p.segs[i].accentHue = ss.alert ? nimbus::themeAlertHue(themeName)
                                   : nimbus::themeRoleHue(themeName, ss.roleIdx);
  }
  // Rainbow theme (owner, "purely for looks"): flag the plan so the Animator
  // renders arcs as a rotating full-wheel hue cycle. alertHue rides along so
  // Error keeps its fixed reserved hue even on this theme.
  p.rainbow = (themeName == "rainbow");
  p.alertHue = nimbus::themeAlertHue(themeName);
  // compose() is portable + session-agnostic: it glows the cursor.index()-th ring
  // SEGMENT. But in Orchestrator focus-space the cursor also counts the Orchestrator
  // root at index 0 (the head has no ring segment), so segment-space = focus-space - 1.
  // Re-key the glow here, where the root concept lives: focus 0 (head) -> no segment
  // highlight (ambient only); focus k>=1 -> the focused sub-session's segment (k-1).
  if (g_orchMode) {
    const size_t fi = g_cursor.index();
    if (fi == 0) {
      p.cursor.active = false;                       // head focused: no per-segment glow
    } else {
      const size_t seg = fi - 1;
      if (p.segCount > 0 && seg < size_t(p.segCount)) {
        // Point the glow at the CENTRE of the focused segment's arc - segments split
        // the ring roughly evenly. Using LED #seg made the cursor crawl LED-by-LED
        // (0,1,2) instead of jumping between session arcs. ring_out resolves this
        // ORDINAL to the live arc centre via g_anim.arcCenter() (the Animator's own
        // spans - the single layout source, no duplicate gap rule here).
        p.cursor.index = seg;   // ORDINAL - ring_out resolves the live arc centre
        p.cursor.hue = p.segs[seg].accentHue;
      } else {
        // No segment arc to point at (Calm/Dark) -> theme-coloured positional glow.
        p.cursor.index = int(seg);
        p.cursor.hue = th;
      }
    }
  } else {
    // Notifier: the cursor is a job ordinal (0..jobCount-1) with no root offset.
    // Point the glow at the focused segment's ARC CENTRE (was LED #ordinal = 0,1,2,
    // which never lined up with the session's arc - owner bug #1).
    const size_t seg = g_cursor.index();
    if (p.cursor.active && p.segCount > 0 && seg < size_t(p.segCount)) {
      p.cursor.index = seg;   // ORDINAL - ring_out resolves the live arc centre
      p.cursor.hue = p.segs[seg].accentHue;
    }
  }
  hw::applyRingPlan(p);
  // Posture snapshotted from the PLAN actually applied (not g_cfg) so a live
  // preview's posture is what RENDER? / the test hooks report.
  g_lastPosture = uint8_t(p.posture);
#ifdef NIMBUS_TEST
  // Snapshot the composed plan for RENDER? and emit the push summary (F2/F4).
  // dark = no segments lit AND (in Dark/Calm) the single attention LED not lit.
  g_lastSeg = p.segCount;
  g_lastSingle = p.single.lit;
  g_lastDark = (p.segCount == 0 && !p.single.lit);
  g_lastBright = p.brightness;
  tc::onRender(g_lastScreen, g_lastPosture, g_lastSeg, g_lastSingle,
               g_lastDark, g_lastBright);
#endif
}

// Stage + apply a POST /api/preview request (fired from webui's onPreview,
// itself called from loopWeb() - so this always runs on the main task, same as
// every other g_cfg-adjacent mutation). Drives the ring to `profileId`'s look
// immediately; loop() reverts once NIMBUS_PREVIEW_MS elapses. Deliberately does
// not touch g_cfg/g_selector/persistConfig() - a preview is a look, not a choice.
static void startPreview(int profileId, int status = -1) {
  if (profileId < 0 || profileId >= nimbus::kProfileCount) return;
  // status: 0..5 = a specific solide::ring::Status to demo; anything else = the
  // default two-arc showcase (-1).
  g_previewStatus = (status >= 0 && status <= int(solide::ring::Status::Error))
                        ? status : -1;
  // A preview is meant to be SEEN, so wake the panel if the screensaver has it
  // blanked (restores the backlight + repaints StatusIdle). On a ringless board
  // this is the difference between the demo showing on the panel and nothing at
  // all; refreshRing() alone only recomposes the frame, it never un-blanks.
  saverKick();
  g_previewActive = true;
  g_previewProfile = ProfileId(profileId);
  g_previewUntil = millis() + NIMBUS_PREVIEW_MS;
  // A preview fired mid-reveal must not render at the CURRENT profile's brightness:
  // end the reveal + release the brightness hold so applyRingPlan applies the
  // PREVIEWED profile's brightness (review finding on P2.4's hold).
  g_revealUntilMs = 0;
  hw::setBrightnessHold(false);
  if (!g_menu.isOpen()) refreshRing();
}

// The on-device config-page URL encoded into the Config QR: the STA address
// when joined to a LAN (reachable from the user's phone), else the SoftAP
// captive address. Plain http on port 80 (net::beginWeb serves it).
// Snapshot the live radio state for the portable copy layer. Until the link policy
// is wired in (it owns the real state machine), state is derived from the one fact
// available today: whether the station is associated.
static nimbus::wifi::LinkView liveLinkView() {
  nimbus::wifi::LinkView v;
  const bool up = net::staConnected();
  v.state    = up ? nimbus::wifi::LinkState::Online : nimbus::wifi::LinkState::Unreachable;
  v.staIp    = net::staIp().c_str();
  v.apIp     = net::apIp().c_str();
  v.apSsid   = net::apSsid().c_str();     // the LIVE name, never the compile-time macro
  v.mdnsName = net::mdnsName().c_str();
  v.rssi     = net::rssi();
  // softAP()'s result is not observable yet (step 6), so infer from the address it
  // reports: a failed AP reports 0.0.0.0. Conservative - never claims an AP is up.
  v.apUp       = !v.apIp.empty() && v.apIp != "0.0.0.0";
  v.knownCount = net::provisioned() ? 1 : 0;
  return v;
}

// The single-use sign-in code carried in the Sign-in QR / setup QR as `?c=` (CUM-45,
// wired for CUM-209). A bearer token in a URL is logged, cached, and shared by
// accident, so the web side never accepts `?t=<token>`; the browser exchanges a short
// single-use code for a session over POST /api/signin/exchange. The device-screen QR
// now carries a real code so a scan signs the browser in with no typing (before this
// the code was empty, so the landing page still asked for it by hand). The code comes
// from the web layer's own table (net::panelSigninCode() mints + caches it, spinlock-
// guarded for this main-task caller), so the exact code encoded here is the one the
// exchange endpoint will redeem. It is never the durable token, so a code in a URL is
// inert once used or expired. When there is no reachable address (Notifier mode has no
// web surface / Wi-Fi), deviceUrl() drops the query entirely and the QR is a bare URL.
static std::string signinCode() {
  return std::string(net::panelSigninCode().c_str());
}

static std::string configUrl() {
  const String ip = net::staConnected() ? net::staIp() : net::apIp();
  // Scanning the QR opens the config page; a single-use `?c=` code (when present) signs
  // the browser in. deviceUrl() returns "" for 0.0.0.0 - an interface that never came up
  // reports that address, and it encodes into a valid QR that resolves to nothing.
  return nimbus::wifi::deviceUrl(std::string(ip.c_str()), signinCode());
}

// The ONBOARDING URL for the SetupInfo screen - ALWAYS the SoftAP address. The
// phone that scans the setup QR has just joined the device's setup AP, so only
// 192.168.4.x is routable from it; configUrl()'s STA preference sent early users
// to the LAN IP ("join 192.0.2.10") which is unreachable from the AP subnet
// (field bug, audit P1.2). Token-carrying like configUrl().
static std::string setupUrl() {
  // Same 0.0.0.0 guard: a failed softAP() reports it, and this screen is exactly
  // where a confident QR to nowhere does the most damage. Sign-in code as `?c=`
  // (CUM-45), same interim-empty behavior as configUrl() until integration.
  return nimbus::wifi::deviceUrl(std::string(net::apIp().c_str()), signinCode());
}

// Live Bluetooth state for the Connectivity > Bluetooth row + Config QR line.
// Reflects the ACTUAL runtime (net::ble::enabled/connected), not just the menu
// intent, so a just-toggled state shows truthfully. BLE only runs in Notifier
// mode; in Orchestrator mode it is inert regardless of the persisted toggle.
static std::string bleStatusLive() {
  if (g_orchMode) return "off (orch mode)";
  if (!net::ble::enabled()) return "off";
  return net::ble::connected() ? "linked" : "advertising";
}

// Live WiFi state for the Connectivity > WiFi status row. Shows the STA IP when
// joined (so the row doubles as "how do I reach this device's web UI"), else the
// setup-AP fallback address.
static std::string wifiStatusLive() {
  return nimbus::wifi::wifiRowLabel(liveLinkView());
}

// One-line live connectivity readout under the Config QR (WiFi + BLE). Uses only
// net:: accessors (SSID lives behind the test-only WiFi.h include). BLE state is
// shown only in Notifier mode, where the GATT server actually runs.
// Three bugs lived in the old body, all fixed by delegating to the portable, tested
// version: it hardcoded the compile-time NIMBUS_AP_SSID (so a board named Nimbus-3
// told you to join a network that does not exist), it spelled "WiFi", and the BLE
// suffix blew the 48-char panel budget the moment the line said anything useful -
// the 2-line header already carries a `bt:` word, so it was duplicated anyway.
static std::string netStatusLine() {
  return nimbus::wifi::netStatusLine(liveLinkView());
}

// Push the settings-menu view onto the panel via the ScreenId::Menu renderer.
// Bypasses the scheduler: the menu is a foreground, interactive surface that
// must repaint on every knob event, not on the dwell/coalesce cadence. When the
// menu is on the "Config QR" row, the SAME foreground path renders the
// full-screen ConfigQr screen instead (device owns the net-derived URL).
static void renderMenu() {
  render::ScreenCtx c;
  fillHeaderCtx(c);   // same header (mode/profile/posture + WiFi/BT/battery) as the status screen
  const bool qr = g_menu.showingConfigQr();
  const bool tokenDetail = g_menu.showingTokenDetail();
  const bool stScreen = g_menu.showingSelfTest();
  const bool batScreen = g_menu.showingBattery();
  attn::ScreenId screen = attn::ScreenId::Menu;
  if (qr) screen = attn::ScreenId::ConfigQr;
  else if (tokenDetail) screen = attn::ScreenId::TokenDetail;
  else if (stScreen) screen = attn::ScreenId::SelfTest;
  else if (batScreen) screen = attn::ScreenId::Battery;
  if (qr) {
    // ConfigQr uses the best live address: LAN when joined, setup AP otherwise.
    // setupUrl remains populated so the renderer can tell those states apart and
    // show AP credentials only when they are actually needed.
    c.setupUrl = setupUrl();
    c.configUrl = configUrl();
    c.fwVersion = NIMBUS_FW_VERSION;
    c.apName = std::string(net::apSsid().c_str());
    c.apPass = std::string(net::apPass().c_str());   // per-device stored passphrase
    c.apUp = ((uint32_t)WiFi.softAPIP() != 0u);
    c.staConnected = net::staConnected();   // locked-out ConfigQr shows AP creds (CUM-200)
    c.webToken = std::string(agent::store::webAuthToken().c_str());
    c.netStatus = netStatusLine();
    c.showCodeAffordance = true;   // menu state: the ShowCode tap routes to TokenDetail
  } else if (tokenDetail) {
    c.webToken = std::string(agent::store::webAuthToken().c_str());
  } else if (stScreen) {
    // Menu-triggered self-test: SILENT set only - the panel must never blare a tone.
    auto items = nimbus::hw::runNow(false);
    c.selfTestSummary = std::string(nimbus::hw::selfTestSummary(items).c_str());
    for (auto& it : items)
      c.selfTest.push_back({std::string(it.name), uint8_t(it.status)});
  } else if (batScreen) {
    // c.battery (header + gauge) is filled by fillHeaderCtx; add the full-screen
    // Battery BODY extras (time-left / health / charge-state text).
    c.battMinutesToEmpty = g_battEstimate.valid ? g_battEstimate.minutesToEmpty : -1;
    c.battHealthPct = g_battEstimate.healthPct;
    c.battChargeState = nimbus::power::chargeStateStr(g_battEstimate.chargeState);
  } else {
    solide::menu::MenuView v = g_menu.view();
    c.menuItems = v.items;
    c.menuSelected = v.selected;
    c.menuTitle = v.title;             // breadcrumb path band
    c.menuHelp = g_menu.helpText();    // param help pane ("" = hidden)
    if (g_menu.showingUpdateMenu()) {  // Software update status band (CUM-193;
                                       // not the install-confirm sub-screen)
      const nimbus::ota::UpdateView uv = otaViewNow();
      c.updateLine = uv.line;
      c.updateBusy = uv.busy;
      c.updatePct = uv.pct;
      c.updateAnim = g_updateAnim;
    }
    c.menuAdjusting = g_menu.adjustingValue();   // invert the row while editing (P2.2)
    // Overlay the Connectivity > Bluetooth row (index 0) with LIVE status the
    // portable FSM can't know (advertising / linked / off), so the menu shows
    // the real radio state, not just the persisted on/off intent.
    if (g_menu.showingConnectivity() && !c.menuItems.empty()) {
      // NO "Wi-Fi: " prefix here - wifiRowLabel() already returns a COMPLETE
      // row, its own label and its own chevron. Prefixing produced
      // "Wi-Fi: Wi-Fi: 192.0.2.10 >" on the Connectivity screen of every
      // shipped device, and no golden covered the composed row, so CI was green
      // while the panel was wrong.
      c.menuItems[0] = wifiStatusLive();
      // Overlay the Bluetooth row (index 1) with live radio status.
      if (c.menuItems.size() > 1)
        c.menuItems[1] = "Bluetooth: " + bleStatusLive();
      // Overlay the Forget row (index 2) with the live bond count the FSM can't know.
      if (c.menuItems.size() > 2)
        c.menuItems[2] = "Forget paired devices (" + std::to_string(net::ble::numBonds()) + ")";
      // Overlay the Re-probe SD row (index 3) with the live card state (mounted /
      // absent / lost) so the owner sees why memory demoted before re-probing.
      if (c.menuItems.size() > 3) {
        const char* sd = agent::memory::sdLost() ? "lost"
                       : (agent::memory::haveSd() ? "mounted" : "absent");
        c.menuItems[3] = std::string("Rescan SD card (") + sd + ")";
      }
      // Token row (index 5 = ConnToken): keep the credential OFF the compact
      // two-column row. It clipped there and its chevron led nowhere. The row is
      // now a real destination whose full-screen view receives c.webToken above.
      if (c.menuItems.size() > 5)
        c.menuItems[5] = "Device sign-in code >";   // canonical (CUM-45)
    }
#ifdef NIMBUS_NOTIFIER_DEBUG
    Serial.printf("renderMenu: visible=%d items=%d sel=%d title=%s\n",
                  int(v.visible), int(v.items.size()), v.selected,
                  v.title.c_str());
#endif
  }
  // One renderer, one buffer pair: the menu is just another frame.
  // Honour a dropped push, exactly as renderScreen does: renderAndPush swaps
  // the tap map ONLY on success, so clearing g_menuNeedsPaint on a drop leaves
  // the panel AND the hit regions one screen behind the FSM with nothing to
  // reschedule the repaint - and the next tap resolves against the stale map.
  // ⚠ ONLY a genuine drop may retain g_menuNeedsPaint. An Unchanged frame
  // means the panel is already correct - retaining the flag there left the
  // gate permanently satisfied (g_menuDoneAt only advances on a real push),
  // re-composing a full frame plus a 150 KB memcmp every ~3 ms forever.
  // Reachable in steady state: the ~1 Hz update-status reseed, or a '+' tap
  // at Volume 100 (onRotate marks dirty even when the value clamps).
  if (hw::tft::renderAndPush(screen, c) == hw::tft::Push::Dropped) return;
  g_menuNeedsPaint = false;
  g_lastScreen = uint8_t(screen);
  // ⚠ Keep g_menuDoneAt moving. The repaint gate is int32_t(now - g_menuDoneAt)
  // >= 0; left at 0 that becomes int32_t(now), which goes NEGATIVE once uptime
  // passes ~24.8 days and the menu then never repaints again. A frame has no
  // dwell to wait out, so "done now" is the honest value.
  g_menuDoneAt = millis();
#ifdef NIMBUS_TEST
  // Without this RENDER? never follows the menu and the touch-nav tests assert
  // against a frozen value.
  tc::onRender(g_lastScreen, uint8_t(g_cfg.posture()), g_lastSeg, g_lastSingle,
               g_lastDark, g_lastBright);
#endif
}

// Re-derive device timings from the (possibly overridden) config and re-apply
// them live. Called after boot-load and after any menu/web mutation so a changed
// DwellMs/CoalesceMs/brightness takes effect without a reboot.
static void applyConfig() {
  // fullEveryN = 0: the color panel has no ghosting, so the render-count refresh
  // upgrade stays off. Dwell and the coalesce window still pace the scheduler.
  g_sched.configure({uint32_t(g_cfg.effective(Param::DwellMs)),
                     uint32_t(g_cfg.effective(Param::CoalesceMs)),
                     0});
  // On a colour panel the BACKLIGHT is the largest continuous draw - larger than
  // the ring - so the battery mode has to reach it, or "Dark" saves almost
  // nothing on a TFT board. Skipped while the screensaver has it blanked, so a
  // config change cannot light a resting screen back up.
  if (g_screenIsTft && hw::tft::backlight() > nimbus::kBacklightRestPct)
    hw::tft::setBacklight(nimbus::backlightPctFor(g_cfg.posture()));
  g_power.setTelemetryPeriodMs(
      // A battery drain/storage op forces dense 30 s telemetry (dense on-device %/log)
      // regardless of the profile, so a mid-run profile re-resolve can't stomp it.
      g_highLoadActive ? 30000u
                       : uint32_t(g_cfg.effective(Param::TelemetryPeriodS)) * 1000u);
  // The failed-sub-agent red arc holds for the same tunable attention window as
  // the Notifier's calls-to-action (then reaps - never a stuck-forever red ring).
  agent::orchestrator::setAttnHoldMs(uint32_t(g_cfg.effective(Param::AttnHoldMs)));
  hw::setRingFps(g_cfg.effective(Param::RingFps));   // wire the RingFps knob to the anim cadence
  refreshRing();  // picks up RingBrightness via compose()
}

// ---- Battery drain / storage (battery-measurement) -------------------------
// ── Low-battery deep sleep (owner feature 2026-07-17) ─────────────────────────
// The pack's own BMS undervoltage cut is POOR - measured, it let the pack fall to
// 5574 mV live. The firmware now protects at agent::store::sleepMv() (default 6000 mV =
// ~10% REAL SoC from the discharge study), debounced in the power policy and
// overridable by the owner/AI (agent::store::sleepOvr - deep-discharge risk accepted).
// Wake source: the periodic charger-sniff timer (plus the fuel-gauge VBUS pin
// where fitted). The panel has no wake gesture, so the copy must not promise one.
static void persistConfig();                // defined below (config section)
static bool s_wokeFromLowBatt = false;      // this boot is a low-batt wake
static uint32_t s_lowBattGraceUntil = 0;    // awake window before re-sleeping

[[noreturn]] static void enterLowBattSleep() {
  persistConfig();
  // Leave the instructions on the panel. They are only readable until the rails
  // drop, so the copy promises only the wake gesture this board actually has.
  // The idle path may have blanked the backlight (that IS the power saving), and
  // neither the T2 path nor this function calls saverKick - so without this the
  // message would be blitted onto an unlit panel.
  hw::tft::setBacklight(nimbus::backlightPctFor(g_cfg.posture()));
  g_askOverride = "Battery empty - going to sleep.\nPlease charge me.\n"
                  "It will wake when you plug in a charger.";
  renderScreen(attn::ScreenId::Ask, -1);
  solide::leds::clearFrame();
  solide::leds::show(solide::leds::Pattern::Solid, 0, 0, 0);
  // 4800, not 2400: the render queue is ASYNC - a StatusIdle repaint scheduled by
  // the same tick that fired T2 can be mid-refresh (2214 ms), and our Ask frame
  // queues BEHIND it. 4800 covers one in-flight refresh + ours. (A long-idle
  // de-ghost (~18 s OTP waveform) can still be in flight - accepted: waiting 18 s
  // on a dying pack costs more than a stale screen; the wake path redraws.)
  delay(4800);
#if defined(NIMBUS_HAS_FUEL_GAUGE)
  esp_sleep_enable_ext0_wakeup(gpio_num_t(NIMBUS_VBUS_SENSE_PIN), 1);
#endif
  // The panel has no wake gesture (GPIO 1/2 are the panel's MISO/backlight, and
  // T_IRQ is not wired), so the charger-sniff timer below is the only recovery
  // path - exactly what the on-screen copy above promises.
  // Periodic charger sniff (no VBUS pin): wake, read the pack, stay up only if
  // it recovered - so plugging a charger in revives the device on its own.
  esp_sleep_enable_timer_wakeup(uint64_t(kLowBattWakeMinutes) * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
  __builtin_unreachable();
}

// ── Software power-off (CUM-224) ─────────────────────────────────────────────
// The "Power off" menu row and the web power-off button land here: a clean
// shutdown (persist config, flush the memory journal), a readable notice on the
// panel, ring + backlight off, then ESP32-S3 deep sleep. Wake is per board: a
// board that wires the touch controller's INT line to an RTC GPIO (Freenove
// CYD's FT6336U INT) wakes on a tap via ext0; a board that leaves it
// unconnected (Solide S3, XPT2046 T_IRQ not routed) has no wake gesture and
// returns only on a power-cycle, so the copy never promises a tap.

// The touch interrupt line, if the board wires it to a GPIO: FT6336U INT
// (capacitive) or XPT2046 T_IRQ (resistive). -1 when left unconnected.
static int touchWakePin() {
  const auto& b = solide::board();
  if (b.touchI2c.intr >= 0) return b.touchI2c.intr;   // FT6336U INT (Freenove CYD)
  if (b.tft.tirq >= 0)      return b.tft.tirq;          // XPT2046 T_IRQ (resistive, if a board wires it)
  return -1;
}
// ext0 deep-sleep wake needs an RTC-capable GPIO (0-21 on the ESP32-S3).
static bool boardCanWakeOnTouch() {
  const int p = touchWakePin();
  return p >= 0 && p <= 21;
}

// Distinguishes a power-off wake from a low-batt wake at the next boot: both use
// ext0, so the wakeup cause alone cannot tell them apart. Survives deep sleep in
// RTC memory; the boot path clears it and skips the low-batt grace window.
RTC_DATA_ATTR static bool s_rtcPowerOff = false;

[[noreturn]] static void enterPowerOffSleep() {
  persistConfig();
  agent::memory::flushPendingEvents();   // commit any queued memory/journal writes

  const bool tapWakes = boardCanWakeOnTouch();
  hw::tft::setBacklight(nimbus::backlightPctFor(g_cfg.posture()));
  g_askOverride = tapWakes
      ? "Powered off.\nTap the screen to wake it."
      : "Powered off.\nReconnect power to turn it back on.";
  renderScreen(attn::ScreenId::Ask, -1);
  solide::leds::clearFrame();
  solide::leds::show(solide::leds::Pattern::Solid, 0, 0, 0);
  delay(4800);                // let the notice + any in-flight refresh land first
  hw::tft::setBacklight(0);   // panel dark: the point of powering off is to stop drawing

  s_rtcPowerOff = true;       // tell the next boot this was a deliberate power-off
  if (tapWakes) {
    // The FT6336U INT idles high and pulses LOW on a touch, so wake on level 0.
    esp_sleep_enable_ext0_wakeup(gpio_num_t(touchWakePin()), 0);
  }
  // No timer wake: "Power off" stays off until the owner acts (a tap where the
  // panel can wake it, a power-cycle otherwise) - never a periodic self-wake.
  esp_deep_sleep_start();
  __builtin_unreachable();
}

// Pin the ring to solid white at `bright` (0 during a settle read). Same apply path the
// `led` override uses, so RENDER?/`/api/state` stay honest. Main task only (owns the LEDs).
static void applyHighLoadRing(uint8_t bright) {
  solide::leds::clearFrame();
  solide::leds::setBrightness(bright);
  solide::leds::show(solide::leds::Pattern::Solid, 255, 255, 255);
#ifdef NIMBUS_TEST
  g_lastSeg = 0; g_lastSingle = true; g_lastDark = (bright == 0); g_lastBright = bright;
  tc::onRender(g_lastScreen, uint8_t(g_cfg.posture()), 0, true, (bright == 0), bright);
#endif
}

// DRAIN (campaign): pin ~2 A, dense telemetry, optionally suppress T2 (deep -> run to the
// pack cutoff). TEST-only (console + POST /api/drain); the ring/settle machinery is shared
// with STORAGE. Guarded so production (no drain trigger) has no unused-function path.
#ifdef NIMBUS_TEST
static String drainSet(bool on, bool deep, int bright = -1, int ttlS = -1) {
  // ttlS: -1 = default dead-man, 0 = DISARMED (a human at the console has no
  // refresher), >0 = host promises to refresh within this many seconds.
  const uint32_t ttl = (ttlS < 0) ? kDrainTtlDefaultS
                     : uint32_t(ttlS > int(kDrainTtlMaxS) ? kDrainTtlMaxS : ttlS);
  // ── KEEPALIVE FAST-PATH ────────────────────────────────────────────────────
  // A host refresh (every ~5 s) must ONLY extend the deadline. It must NOT fall
  // through to the arm path below, because that would:
  //   • reset g_hlLastSettleMs -> the 15-min settle would NEVER fire, so the run
  //     would record ZERO resting-mV samples (the calibration-grade signal the
  //     whole campaign exists to collect), and
  //   • re-arm g_thermal to the now-hot die -> the rise-over-baseline trip could
  //     never fire, and the trip history hlBright() uses to hold the reduced
  //     level would be cleared, silently letting the load climb back to full.
  // Only a campaign drain (g_storageTargetMv == 0) is refreshable.
  if (on && g_highLoadActive && g_storageTargetMv == 0) {
    g_drainRefreshedMs = millis();
    g_drainTtlMs       = ttl * 1000u;
    return String("drain refresh ttl=") + ttl + "s rest=" + g_hlRestingMv + "mV";
  }
  g_storageTargetMv = 0;                 // campaign drain never auto-stops
  g_highLoadActive  = on;
  g_drainDeep       = on && deep;
  if (on) {
    // battlab: per-run LED load. -1/0 = firmware default (kDrainBright); anything
    // else clamps to [10, kDrainBrightHardMax(255)] - the FULL scale is allowed so
    // the lab can characterize higher loads, with the thermal guard as the live
    // protection (loads above kDrainBright are above the recommended ceiling and
    // the web UI warns before requesting them).
    g_drainBright = (bright <= 0) ? kDrainBright
                    : uint8_t(bright < 10 ? 10
                              : (bright > kDrainBrightHardMax ? kDrainBrightHardMax : bright));
    // The 60% safety cap applies to the drain too - a harness must not exceed what
    // the owner/AI has accepted (the fried-panel run WAS a drain). Override lifts it.
    g_drainBright = nimbus::power::clampBright(g_drainBright, agent::store::brightOvr());
    g_ledOverrideActive = true; g_lightsOff = false;
    g_ledOvUntilMs = 0;   // measurement pin: holds for hours, never expires
    g_hlLastSettleMs = millis(); g_hlSettling = false;
    g_thermal.arm(temperatureRead());    // thermal breaker: baseline = die temp NOW
    g_thermalAbortLatch = false;
    g_drainRefreshedMs = millis();       // arm the host dead-man
    g_drainTtlMs       = ttl * 1000u;    // 0 => disarmed (console/human)
    applyHighLoadRing(g_drainBright);
    g_power.setTelemetryPeriodMs(30000);
  } else {
    g_ledOverrideActive = false;
    g_drainTtlMs = 0;                    // disarm the dead-man on every stop path
    g_thermal.disarm();
    applyConfig();                       // restores telemetry period + normal ring
  }
  return String("drain ") + (on ? (deep ? "on deep" : "on") : "off") +
         (on && ttl ? String(" ttl=") + ttl + "s" : String(" ttl=off")) +
         " rest=" + g_hlRestingMv + "mV";
}
#endif  // NIMBUS_TEST (drainSet)

// STORAGE (production user feature): discharge DOWN to a storage SoC (~70% ≈ 3.80 V/cell)
// then drop the load and hold. The device can't charge, so if already below target it can
// only advise charging. pct<=0 turns it off. Never over-discharges (target ≫ empty).
static String storageSet(int pct) {
  if (pct <= 0) {
    g_highLoadActive = false; g_storageTargetMv = 0; g_ledOverrideActive = false;
    g_thermal.disarm();
    applyConfig();
    return String("storage off");
  }
  if (pct < 40 || pct > 95) return String("bad pct (40-95)");
  const uint16_t targetPack =
      uint16_t(nimbus::power::liIonCellMvForPct(uint8_t(pct))) * battCells();
  power::Sample s = g_monitor->sample();
  if (s.valid && s.millivolts <= targetPack) {
    g_highLoadActive = false; g_storageTargetMv = 0;
    return String("already at/below storage (") + (s.millivolts / battCells()) +
           "mV/cell) - charge to " + pct + "%";
  }
  g_storageTargetMv = targetPack;
  g_highLoadActive = true;
  g_ledOverrideActive = true; g_lightsOff = false;
  g_ledOvUntilMs = 0;   // storage load: deliberate long hold
  g_hlLastSettleMs = millis(); g_hlSettling = false;
  g_drainBright = kDrainBright;         // storage always runs the default load
  // STORAGE is a PRODUCTION op with no host driving it and its own voltage
  // auto-stop - it must never be killed by the host dead-man. Explicitly disarm.
  g_drainTtlMs = 0;
  g_thermal.arm(temperatureRead());     // thermal breaker: baseline = die temp NOW
  g_thermalAbortLatch = false;
  applyHighLoadRing(kDrainBright);
  g_power.setTelemetryPeriodMs(30000);
  return String("storage -> ") + pct + "% (target " + (targetPack / battCells()) +
         "mV/cell), draining";
}

// Optional low-battery Telegram ping (Orchestrator mode). Sends to the first
// allowlisted chat; no-op without an owner / not in Orchestrator mode. Runs on
// the main render loop, so it uses the NON-BLOCKING send (block=false): dropping
// a redundant low-battery ping on a full reply queue is fine, stalling the loop
// up to 100 ms is not.
// ⚠ Reports the CALIBRATED estimate, never the raw curve percent - the raw
// scale reads 0% with ~a third of the pack left (measured), and "battery low:
// 0%" from a board that then keeps running for hours reads as a broken device.
// Falls back to the measured pack voltage when the model has no estimate yet
// (first tick after a wake boot).
static void lowBatteryPing() {
  if (!g_orchMode) return;
  if (!g_cfg.effective(Param::TgLowBattPing)) return;   // owner opt-in (web/menu Tune param)
  String al = agent::store::telegramAllowlist();
  const int comma = al.indexOf(',');
  String owner = comma < 0 ? al : al.substring(0, comma);
  owner.trim();
  if (!owner.length()) return;
  String msg;
  if (g_battEstimate.millivoltsTrue > 0) {
    char buf[64];
    snprintf(buf, sizeof buf, "Battery low: ~%u%% (%u.%02u V). Plug it in soon.",
             (unsigned)g_battEstimate.percent,
             (unsigned)(g_battEstimate.millivoltsTrue / 1000),
             (unsigned)((g_battEstimate.millivoltsTrue % 1000) / 10));
    msg = buf;
  } else {
    char buf[56];
    snprintf(buf, sizeof buf, "Battery low (%u mV). Plug it in soon.",
             (unsigned)g_power.last().millivolts);
    msg = buf;
  }
  agent::telegram::send(owner, msg, /*block=*/false);
}

// Persist the current Config + Mode to NVS/SD. Best-effort: returns void, the
// glue never blocks the UI on a missing SD card / closed NVS namespace.
//
// The persisted profile byte must always be the USER's pick (g_selector.user()),
// never the transient forced/VBUS profile that a battery T1 event or an auto-Desk
// switch drops into the ACTIVE g_cfg.profile() (see the power tick + web/menu
// onChanged). Serializing g_cfg.profile() directly would let a T1/VBUS window
// overwrite the stored pick, so we persist a copy whose profile is the user's
// choice and leave the live g_cfg (which drives effective()/presets) untouched.
static void persistConfig() {
  Config toSave = g_cfg;              // copy the sparse overrides
  toSave.setProfile(g_selector.user());  // ...but store the user's pick
  sys::saveConfig(toSave);
  sys::saveMode(g_menu.mode() == Mode::Orchestrator ? sys::Mode::Orchestrator
                                                    : sys::Mode::Notifier);
  sys::saveBleEnabled(g_menu.bleEnabled());  // Connectivity > Bluetooth toggle
}

// Bring up the Orchestrator subsystem (plan §3.6). Every network/adapter call is
// gated so a device with no STA creds / no provider keys / no Telegram token
// still boots and runs: the TLS arbiter + heavy fabric init are heap-only (safe
// offline), the orchestrator loads its persisted journal/memory, and telegram::
// begin() is a NO-OP when the bot token is empty (no task spawned, no blocking).
// Adapters register unconditionally but never open a socket until a turn/dispatch
// fires - which only happens on an incoming Telegram message, so an unprovisioned
// device simply idles. Called once from setup() when Mode==Orchestrator.
#ifdef NIMBUS_NOTIFIER_DEBUG
#define ORCH_MARK(s) do { Serial.println(s); Serial.flush(); } while (0)
#else
#define ORCH_MARK(s) do {} while (0)
#endif

// Ring boot-stage indicator (test builds): each setup stage paints the ring a
// distinct SOLID colour. The LED render task runs on its own core, so if setup
// HANGS the ring FREEZES on the last stage reached - a tool-free "where did boot
// die" probe, essential on boards whose USB-CDC drops serial (V0.1). A completed
// boot moves past all stages and the app takes over the ring. Legend:
//   white=HAL  blue=config  cyan=SD-mount  magenta=web+memory  green=verify
//   yellow=orchestrator. Frozen colour => the NEXT stage hung.
// clearFrame() first: firstRender()/applyConfig() put the ring in the high-priority
// RAW-FRAME layer (often Passive/dark), which would swallow a plain show(). Exiting
// raw-frame lets the SOLID pattern display; the pattern layer has no staleness
// watchdog, so the colour PERSISTS through a hang (unlike showFrame, which the
// staleness timer would clear).
#if defined(NIMBUS_TEST)
#define BOOT_STAGE(r, g, b) do { solide::leds::clearFrame(); \
        solide::leds::show(solide::leds::Pattern::Solid, (r), (g), (b)); } while (0)
#else
#define BOOT_STAGE(r, g, b) do {} while (0)
#endif

// Contiguous-SRAM instrumentation (CUM-185). Samples the scarce axis - internal
// free + LARGEST contiguous internal block (what a TLS handshake / AsyncTCP bind
// needs) - and reports the delta from the previous snapshot, so the boot -> mode
// switch -> Orchestrator steady-state curve is visible on Serial and in
// GET /api/log even before (or when) the web server has not bound. The per-stage
// dIntFree attributes internal-SRAM consumption to the boot stage that ran
// between two snaps, which is how the top holders are identified. Cheap (a few
// heap_caps reads + one log line); matches the existing [psram]/[sd] boot-line
// style so it is useful field diagnostics too, not just a bench probe.
static void sramSnap(const char* tag) {
  static uint32_t prevIntFree = 0;
  static bool have = false;
  uint32_t intFree    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t intLargest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  uint32_t heapMin    = (uint32_t)ESP.getMinFreeHeap();
  uint32_t psramFree  = (uint32_t)ESP.getFreePsram();
  long dFree = have ? (long)intFree - (long)prevIntFree : 0;
  agent::alogf("[sram] %-14s intFree=%u intLargest=%u heapMin=%u psramFree=%u dIntFree=%+ld",
               tag, (unsigned)intFree, (unsigned)intLargest, (unsigned)heapMin,
               (unsigned)psramFree, dFree);
  prevIntFree = intFree;
  have = true;
}

static void orchestratorBegin() {
  ORCH_MARK("orch: arbiter");
  agent::arbiter::begin();      // work-slot arbitration (heap-only)
  ORCH_MARK("orch: fabric");
  agent::fabricInit(g_fabric);  // register adapters + category bindings from NVS

  agent::orchestrator::Sinks sinks;
  sinks.event  = orchEventSink;   // -> attn::Router (ring/panel feedback)
  sinks.send   = orchSendSink;    // -> Telegram
  sinks.speak  = orchSpeakSink;   // tts action -> Telegram audio (device-speaker tool: P6)
  sinks.device = orchDeviceSink;  // -> led/lights/reboot executor (main loop applies)
  ORCH_MARK("orch: begin");
  agent::orchestrator::begin(&g_fabric, sinks);

  // Register the device's own secrets so the agent log ring (GET /api/log) masks
  // them if a provider error body ever echoes one (CUM-73). This is the reliable
  // redaction layer for keys provisioned by now; keys added later are still
  // caught by the Bearer/api_key heuristics in core::LogRing::redact.
  agent::logring::clearSecrets();
  agent::logring::g_secrets.reserve(8);   // no realloc during the appends below, so a
                                          // concurrent redact() reader never sees a moved buffer
  agent::logring::addSecret(agent::store::openaiKey().c_str());
  agent::logring::addSecret(agent::store::anthropicKey().c_str());
  agent::logring::addSecret(agent::store::mistralKey().c_str());
  agent::logring::addSecret(agent::store::tavilyKey().c_str());
  agent::logring::addSecret(agent::store::customKey().c_str());
  agent::logring::addSecret(agent::store::telegramToken().c_str());

  // Local Loops: wire the scheduler's hooks (kept out of the subsystem so it
  // carries no orchestrator/telegram deps). fire = run a scheduled turn on
  // tg_poll; chatAllowed = fire-time allowlist re-check; alert = owner ping.
  agent::loops::begin(
      [](const nimbus::orch::LoopFireRequest& r) -> nimbus::orch::FireOutcome {
        // DREAMING: the reserved dream loop fires through its own two-phase
        // path (maintenance + reflection turn); everything else is a plain
        // scheduled turn. r.id rides through as the tag "loop:<id>".
        if (agent::dream::isReserved(r.id)) return agent::dream::fire(r);
        return agent::orchestrator::injectScheduledTurn(
            String(r.chatId.c_str()), String(r.prompt.c_str()), String(r.name.c_str()),
            String(r.id.c_str()), /*quietOk=*/false, r.once, r.ownerReminder);
      },
      [](const std::string& cid) -> bool {
        return agent::orchestrator::isChatAllowed(String(cid.c_str()));
      },
      [](agent::loops::AlertLevel lvl, const std::string&, const std::string& msg) {
        if (lvl == agent::loops::AlertLevel::Info) return;   // Info: ring only (silent)
        String owner = agent::orchestrator::firstAllowedChat();
        if (owner.length()) agent::telegram::send(owner, String(msg.c_str()), /*block=*/false);
      });

  // DREAMING - the reserved nightly maintenance + reflection loop (owner-visible
  // "dream", daily 03:30 local). Ensured every boot (insert-if-missing; a
  // persisted owner pause survives), reserved (pause-only, never deletable from
  // any surface), and idle-gated at fire time (busy device => defer +15 min
  // WITHOUT consuming a fire). Fires under all the ordinary LoopCaps +
  // scheduled-turn rails.
  {
    agent::Hooks dh;
    dh.onDreamStart = [](const agent::DreamStartEv& e) {
      agent::alogf("dream: start (epoch=%llu)", (unsigned long long)e.epoch);
    };
    dh.onDreamEnd = [](const agent::DreamEndEv& e) {
      agent::alogf("dream: end %s pruned=%d deduped=%d tokens=%u",
                   e.turnOk ? "ok" : "FAIL", e.prunedVectors, e.dedupedVectors,
                   (unsigned)e.tokens);
      // Timeline row - dream outcomes used to vanish into the 1280 B RAM log.
      agent::memory::captureEvent("dream",
          String("Nightly dream ") + (e.turnOk ? "ok" : "FAILED") +
          " pruned=" + e.prunedVectors + " deduped=" + e.dedupedVectors +
          " tokens=" + (unsigned)e.tokens);
    };
    agent::dream::begin(dh);
  }
  agent::loops::ensureLoop(agent::dream::reservedLoopRecord());
  agent::loops::setFireGate(
      [](const nimbus::orch::LoopRecord& l) { return agent::dream::gateDefer(l); });

  // Telegram long-poll task. begin() returns immediately (spawns tg_poll) only if
  // a token is configured; otherwise it's a no-op and the device idles in
  // Orchestrator mode with just the local ring/panel + web UI live.
  ORCH_MARK("orch: telegram");
  agent::telegram::begin(agent::store::telegramToken(),
                         agent::store::telegramAllowlist(), orchOnMessage);
  agent::telegram::setTick(orchTick);
  agent::telegram::setSttSink(agent::stt::transcribe);  // Telegram voice notes -> STT -> turn
  // Cloud relay (cumulo-nimbus): its own resident TLS, gated on cloudOptIn + a heap
  // floor. Orchestrator-only by construction (this function only runs in that mode);
  // the task no-ops until the owner opts in, so it is free to always spawn here.
  nimbus::relay::begin();
  ORCH_MARK("orch: done");
}

// Live device state for the self-test engine (hw::selftest reaches nothing on its
// own). Installed as the provider; called on whatever task runs a check.
static nimbus::hw::SelfTestInputs buildSelfTestInputs() {
  nimbus::hw::SelfTestInputs in;
  in.halDisplay = g_hal.display; in.halLeds = g_hal.leds; in.halStorage = g_hal.storage;
  in.halMemory  = g_hal.memory;
  in.halTouch   = g_hal.touch;
  in.battery = g_power.last();
  in.batteryEst = g_battEstimate;
  in.wifiConnected = net::staConnected();
  in.wifiRssi = net::staConnected() ? net::rssi() : 0;
  in.bleAdvertising = !g_orchMode && net::ble::enabled();
  in.bleBonds = net::ble::numBonds();
  in.orchMode = g_orchMode;
  return in;
}

// Register the orchestrator's device-introspection tools on the shared registry
// (auto-advertised to the head tool-loop + reachable over /mcp). device.status
// is a silent snapshot; device.selftest runs the health check, gating the
// AUDIBLE items on sfx::isSilent() + store::allowHwTests() so a silent device
// never blares. Neither mutates durable state.
static void registerDeviceTools() {
  auto& reg = agent::memory::registry();
  reg.add("device.status",
          "Read the COMPLETE live device state - call this before answering any "
          "question about current settings, capacity, versions or the time. One "
          "JSON object: identity + firmware + update state, clock + timezone "
          "(time 'unsynced' means NEVER state a date from memory), heap/PSRAM, "
          "storage (SD, live sdLost, flash, file-store usage), battery "
          "(battTrend is the VOLTAGE TREND only - no charge-detect hardware "
          "exists, so never claim plugged-in or charging), network, faults, "
          "memory counts vs live caps, sub-agent capacity, billed token usage "
          "INCLUDING usage.budget[] - per-provider spend this period (tokens "
          "in/out, calls, estimated $ from the owner's rates) vs the owner's $ "
          "and token ceilings + the reset day - use it to plan or answer any "
          "cost/budget question, "
          "and cfg{} = the current value of every knob the config action sets. "
          "Silent - never makes a sound.",
          [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
            // W11: the hw snapshot PLUS the agent-layer live state (composed here,
            // the top layer - src/hw must not include src/agent subsystems). One
            // comprehensive self-state answer instead of scattered partial ones.
            String s = nimbus::hw::deviceStatusJson();
            if (s.endsWith("}")) s.remove(s.length() - 1);
            // Ring/display config the model can set but couldn't read.
            {
              net::ConfigLockGuard lk;
              static const char* kPostures[] = {"dark", "calm", "full"};
              int p = (int)g_cfg.effective(nimbus::Param::Posture);
              s += ",\"posture\":\"";
              s += kPostures[p >= 0 && p <= 2 ? p : 1];
              s += "\",\"profile\":\"";
              s += nimbus::profileName(g_selector.user());
              s += "\",\"effectiveProfile\":\"";
              s += nimbus::profileName(g_selector.resolve());
              s += "\",\"attnHoldMs\":";
              s += (uint32_t)g_cfg.effective(nimbus::Param::AttnHoldMs);
            }
            s += ",\"devName\":\""; s += nimbus::sys::deviceName(); s += "\"";
            s += ",\"subPriority\":\""; s += agent::store::subPriority(); s += "\"";
            // Clock/timezone (the tz knob is web-settable; the model had no read).
            s += ",\"tz\":\""; s += agent::store::deviceTz(); s += "\"";
            s += ",\"clockSynced\":"; s += agent::memory::clockSynced() ? "true" : "false";
            // Storage tiers: LIVE sd state (the hw block's `sd` is the mount) +
            // internal flash + the artifact store's real usage vs caps.
            s += ",\"sdLost\":"; s += agent::memory::sdLost() ? "true" : "false";
            s += ",\"fsTotal\":"; s += (uint32_t)LittleFS.totalBytes();
            s += ",\"fsUsed\":";  s += (uint32_t)LittleFS.usedBytes();
            {
              bool fp; uint16_t fc; uint64_t fb; uint32_t ffree;
              agent::files::stats(fp, fc, fb, ffree);
              s += ",\"files\":{\"present\":"; s += fp ? "true" : "false";
              s += ",\"count\":"; s += fc;
              s += ",\"bytes\":"; s += (uint32_t)fb;
              s += ",\"freeBytes\":"; s += ffree; s += "}";
            }
            // Memory tiers as NUMBERS (count vs effective cap - the cap depends
            // on the storage tier, so the configured knob alone misleads).
            {
              agent::memory::Stats ms = agent::memory::stats();
              s += ",\"memory\":{\"vectors\":"; s += ms.vectorCount;
              s += ",\"maxVectors\":"; s += ms.maxVectors;
              s += ",\"episodicMsgs\":"; s += ms.episodicMsgs;
              s += ",\"scratchItems\":"; s += ms.scratchItems;
              s += ",\"flashFull\":"; s += ms.flashFull ? "true" : "false"; s += "}";
            }
            // Sub-agent capacity: the live picture behind [SPAWN CAPACITY].
            s += ",\"spawn\":{\"running\":"; s += agent::orchestrator::activeJobCount();
            s += ",\"queued\":"; s += agent::orchestrator::pendingSpawnCount();
            s += ",\"maxInflight\":"; s += nimbus::orch::kMaxActiveInflight;
            s += ",\"maxQueue\":"; s += nimbus::orch::kMaxPendingSpawns; s += "}";
            // Real billed token usage (last turn + since boot) + W16: the
            // per-provider PERIOD ledger - in/out split, calls, the $ estimate
            // and ceilings - so the model can plan work against the owner's
            // budget instead of discovering a refusal at dispatch. The $ math is
            // the ledger's own estCents(): the same number the web Usage page
            // and the budget gate use.
            {
              nimbus::orch::TokenUsage lu = agent::orchestrator::lastTurnUsage();
              nimbus::orch::TokenUsage su = agent::orchestrator::sessionUsage();
              s += ",\"usage\":{\"lastIn\":"; s += lu.promptTokens;
              s += ",\"lastOut\":"; s += lu.completionTokens;
              s += ",\"sessIn\":"; s += su.promptTokens;
              s += ",\"sessOut\":"; s += su.completionTokens;
              s += ",\"budget\":"; s += agent::store::providerBudgetJson(); s += "}";
            }
            s += ",\"sfxSync\":\""; s += sfxsync::statusStr(); s += "\"";
            s += ",\"otaSlot\":\""; s += otaupd::runningSlot(); s += "\"";
            s += ",\"autoUpdate\":"; s += agent::store::otaAutoUpdate() ? "true" : "false";
            // W18: the URL-download trust policy + pending count, so the model
            // can explain WHY a download waits ("pending owner approval").
            s += ",\"fetchPol\":\"";
            s += nimbus::orch::fetchPolicyName(
                nimbus::orch::fetchPolicyFromInt(agent::store::fetchPolicy()));
            s += "\",\"fetchPending\":"; s += agent::files::fetchPendingCount();
            s += "}";
            return nimbus::orch::ToolResult::ok(std::string(s.c_str()));
          },
          R"({"type":"object","properties":{},"required":[]})");
  reg.add("ota.status",
          "Read the firmware-update engine: state (idle/checking/available/"
          "downloading/up-to-date/error), the latest version seen + its release "
          "notes, and the last install outcome. Use when asked about updates or "
          "'what changed'. You cannot INSTALL updates - only the owner can "
          "(/update or the web UI); say so instead of promising to update.",
          [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
            // ArduinoJson, not concatenation (prism): release notes are free text
            // with quotes/newlines - hand-built JSON garbled exactly the "what
            // changed?" answer this tool exists for. Also: while a check is IN
            // FLIGHT the version/notes globals are being written by the check
            // task - report "checking" and skip them for that window instead of
            // racing the write.
            JsonDocument d;
            const char* st = otaupd::statusStr();
            d["state"] = st;
            d["running"] = NIMBUS_FW_VERSION " (" NIMBUS_FW_BUILD ")";
            if (strcmp(st, "checking") != 0) {
              String lat = otaupd::latestSeen();
              if (lat.length()) d["latest"] = lat;
              String notes = otaupd::latestNotes();
              if (notes.length()) d["notes"] = notes;
            }
            String last = agent::store::otaLastResult();
            if (last.length()) d["lastResult"] = last;
            const char* err = otaupd::lastError();
            if (err && err[0]) d["lastError"] = err;
            std::string out;
            serializeJson(d, out);
            return nimbus::orch::ToolResult::ok(out);
          },
          R"({"type":"object","properties":{},"required":[]})");
  reg.add("device.selftest",
          "Run a hardware self-test and return per-item PASS/FAIL/SKIP results. "
          "Set audible=true to also run the speaker+mic acoustic loopback and mic level; "
          "audible tests are refused when the device is on silent or the owner disabled "
          "hardware tests (the silent items always run).",
          [](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal&) -> nimbus::orch::ToolResult {
            bool wantAudible = a["audible"].is<bool>() && a["audible"].as<bool>();
            const bool allowed = wantAudible && !::sfx::isSilent() && agent::store::allowHwTests();
            auto items = nimbus::hw::runNow(allowed);
            std::string out = std::string(nimbus::hw::selfTestJson(items).c_str());
            if (wantAudible && !allowed)
              out += "  (audible tests skipped: device silent or owner-disabled)";
            return nimbus::orch::ToolResult::ok(out);
          },
          R"({"type":"object","properties":{"audible":{"type":"boolean"}},"required":[]})");
}

// ---- OTA wiring: owner notify + install UX + auto-install idle snapshot -----
// The OTA module (src/sys/ota_update) is hook-injected like loops_subsystem so
// it carries no telegram/orchestrator/LED link deps of its own.

static void otaEventHook(int ev, const char* a, const char* b) {
  switch (ev) {
    case otaupd::EvAvailable: {   // once per version (otaNotif dedup upstream)
      agent::memory::captureEvent("ota", String("Firmware ") + a + " available" +
                                         ((b && b[0]) ? (String(": ") + b) : String()));
      if (!g_orchMode) break;
      String owner = agent::orchestrator::firstAllowedChat();
      if (owner.length()) {
        String msg = String("Firmware ") + a + " is available";
        if (b && b[0]) { msg += ": "; msg += b; }
        msg += ".\nReply /update to install it now, or open Settings \xE2\x86\x92 Software update on the device's web page.";
        agent::telegram::send(owner, msg, /*block=*/false);
      }
      break;
    }
    case otaupd::EvInstallStart:
      // Intentionally DON'T touch the Telegram poll task here. The earlier
      // "stop the poller to free heap for the download" hook crash-rebooted the
      // device mid-install: telegram::stop() deletes g_replyQ out from under the
      // still-running tg_poll task, whose next xQueueReceive(g_replyQ,...) hit a
      // NULL queue (configASSERT -> abort -> RTC_SW_CPU_RST). It was also never
      // necessary - the dry-run path leaves the poller up and the full 3 MB
      // download + verify completes with internal heap steady at ~52 KB. So the
      // install now runs alongside the live poller (TLS is PSRAM-backed); the
      // device just reboots on success.
      agent::memory::captureEvent("ota", String("Update install started (") +
                                         (a ? a : "?") + ")");
      break;
    case otaupd::EvInstallFail:
      agent::memory::captureEvent("ota", String("Update install FAILED: ") +
                                         (a ? a : "?") +
                                         ((b && b[0]) ? (String(" (") + b + ")") : String()));
      break;
    case otaupd::EvRebooting:
      // The row lands in the PSRAM ring and is persisted by the append (SD) -
      // the restart follows in ~3.5 s, so this is the last pre-update record.
      agent::memory::captureEvent("ota", String("Update committed - restarting into ") +
                                         (a ? a : "?"));
      break;
    case otaupd::EvValidated: {   // post-update boot proved healthy
      // Release notes were persisted ACROSS the install reboot ("ver|notes") -
      // consume them once so the timeline row + the model's awareness turn carry
      // WHAT changed, not just the version number.
      String notes = agent::store::otaPendingNotes();
      String noteTxt;
      int bar = notes.indexOf('|');
      if (bar > 0 && notes.substring(0, bar) == a) noteTxt = notes.substring(bar + 1);
      if (notes.length()) agent::store::setOtaPendingNotes("");
      agent::memory::captureEvent("ota", String("Updated to ") + a + " - verified healthy" +
                                         (noteTxt.length() ? (String(". Notes: ") + noteTxt)
                                                           : String()));
      if (!g_orchMode) break;
      String owner = agent::orchestrator::firstAllowedChat();
      if (owner.length())
        agent::telegram::send(owner, String("Updated to ") + a + " \xE2\x9C\x93", /*block=*/false);
      // Unattended awareness turn (Glass Box A2): the model learns it was
      // updated + what changed, and decides what (if anything) to tell the
      // owner. Staged - drained on tg_poll (a turn here in the tick's task
      // context would trip the watchdog); quietOk so silence is a valid outcome.
      String prompt = String("[FIRMWARE UPDATED] This device just updated to ") + a +
                      " and the new firmware passed its health check.";
      if (noteTxt.length()) prompt += String(" Release notes: ") + noteTxt;
      prompt += " Note anything durable to memory; only message the owner if the "
                "changes matter to them.";
      agent::orchestrator::stageSystemTurn(prompt, "firmware-update");
      break;
    }
    default: break;
  }
}

static bool otaIdleSnapshot(nimbus::ota::IdleSnapshot& s) {
  s.turnInFlight = g_orchMode && agent::orchestrator::turnInFlight();
  s.voiceActive = g_voiceActive;
  s.audioPlaying = false;   // SFX clips are seconds-long; the turn/voice gates carry the policy
  s.onExternalPower = g_battEstimate.onExternalPower;
  s.battMonEnabled = battMonOn();
  s.battPct = g_battEstimate.percent;
  s.healthPct = g_battEstimate.healthPct;
  s.internalFreeB = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return true;
}

// Install-time UX, driven from loop(): panel painted once per state change (the
// panel is ~2.2 s/refresh), the ring is the live channel - a dim theme-colour
// fill bar tracking download progress (same mental model as the menu ring).
static void otaLoopUx() {
  static bool wasInstalling = false;
  static const char* lastState = "";
  const bool ins = otaupd::installing();
  const char* st = otaupd::statusStr();   // static strings - pointer compare is safe

  static uint32_t lastPanelMs = 0;
  if (ins) {
    if (!wasInstalling) {
      // Install just started: take the ring cleanly. The in-flight turn's Running
      // arc has latched the animator (g_animActive) and refreshRing() is locked
      // out for the whole install, so without this the 30 Hz animator keeps
      // racing the progress bar - the owner's "lights flicker heavily on the
      // download bar" (prism trace 2026-07-24, ~13% duty strobe). Dropping the
      // raw frame + the animation latch hands the ring to ONE writer: this bar.
      hw::stopAnimation();
      solide::leds::clearFrame();
    }
    const bool downloading = strcmp(st, "downloading") == 0;
    // Re-assert the "do not power off" screen on the state edge AND on a slow
    // throttle: statusStr() stays "downloading" for the whole multi-minute
    // stream, so a competing render (a Telegram-turn reply, a Notifier status
    // screen) could otherwise overwrite the warning and never repaint it, leaving
    // the user looking at a normal screen while a flash write is in flight.
    const bool edge = (st != lastState);
    if (downloading && (edge || millis() - lastPanelMs >= 15000)) {
      g_askOverride = String("Installing ") + otaupd::latestSeen() +
                      ". Keep the device powered on.";
      g_askSticky = false; g_askPage = 0;
      renderScreen(attn::ScreenId::Ask, -1);
      lastPanelMs = millis();
    } else if (edge && strcmp(st, "rebooting") == 0) {
      g_askOverride = "Update verified. Restarting...";
      g_askSticky = false; g_askPage = 0;
      renderScreen(attn::ScreenId::Ask, -1);
    }
    // Ring fill bar (raw frame takes the ring; released below when we finish).
    // Re-pushed EVERY pass - the menu-dark discipline - so the driver's 500 ms
    // raw-staleness watchdog can never release the frame and let a Pattern bleed
    // back (it did: the inline 2.2 s panel renders above starve a throttled
    // push). Cheap: a 45-LED memcpy per ~3 ms loop pass.
    if (!g_menu.isOpen()) {   // menu-dark wins while the owner reads the menu -
                              // two per-pass raw writers would strobe (prism)
      const int L = NIMBUS_RING_LEDS;
      solide::ring::RGB buf[NIMBUS_RING_LEDS] = {};
      nimbus::ThemeColor th =
          nimbus::themeAccent(std::string(agent::store::theme().c_str()));
      int pct = otaupd::progressPct();
      if (pct < 0) pct = strcmp(st, "downloading") == 0 ? 0 : 100;
      const int lit = (pct * L + 50) / 100;   // whole LEDs (45 ⇒ ~2.2%/LED)
      const solide::ring::RGB dim{uint8_t(th.r / 4), uint8_t(th.g / 4), uint8_t(th.b / 4)};
      for (int i = 0; i < lit && i < L; i++) buf[i] = dim;
      solide::leds::showFrame(buf, L);
    }
  } else if (wasInstalling) {
    // Install task ended without a reboot: dry-run success or a failure.
    solide::leds::clearFrame();
    refreshRing();
    if (strcmp(st, "error") == 0) {
      g_askOverride = String("The update didn't complete (") + otaupd::lastError() +
                      "). Still on " NIMBUS_FW_VERSION ".";
    } else {
      g_askOverride = String("Update dry-run passed - ") + otaupd::latestSeen() +
                      " verified.";
    }
    g_askSticky = true; g_askPage = 0;
    renderScreen(attn::ScreenId::Ask, -1);
  }
  wasInstalling = ins;
  lastState = st;
}


void setup() {
  // OTA rollback guard FIRST - before Serial and every driver, so a freshly
  // flipped image that crash-loops anywhere in bring-up still burns a boot
  // attempt and self-reverts (raw-NVS; ms-scale; no-op when nothing pending).
  otaupd::bootGuard();
  otaupd::setEventHook(otaEventHook);
  otaupd::setIdleProvider(otaIdleSnapshot);
  // Owner-only Telegram `/update` approves + installs a pending OTA. Runs on the
  // tg_poll task (handleMessage); returns the reply text the owner sees.
  // ⚠ The OTA state is IN-RAM and resets on every boot, so "not available" here
  // proves nothing - the owner may be replying to an update notice sent before a
  // reboot (live bug, Board 1 2026-07-22: notify -> reboot -> /update -> "up to
  // date on v3.0.0" while v3.1.1 was published). So /update runs a FRESH check
  // when no update is staged, waits for it (bounded ~25 s; tg_poll blocks far
  // longer on ordinary turns), and installs what the check finds.
  agent::orchestrator::setOtaInstallHook([]() -> String {
    if (otaupd::installing()) return "An update is already installing. I'll confirm when it's done.";
    String st = otaupd::statusStr();
    if (st == "unsupported") return "Updates aren't available on this build.";
    if (st != "available" && st != "checking") {
      if (!otaupd::requestCheck())
        return "Couldn't check for updates right now (network or memory busy). Try again in a minute.";
      st = "checking";
    }
    if (st == "checking") {   // ours or the scheduled one - wait for the verdict
      const uint32_t until = millis() + 25000;
      while ((int32_t)(until - millis()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(250));
        st = otaupd::statusStr();
        if (st != "checking") break;
      }
    }
    if (st == "available") {
      const char* why = "";
      if (otaupd::requestInstall(/*dry=*/false, /*force=*/false, &why)) {
        return String("Installing ") + otaupd::latestSeen() +
               ". The device will download, verify, and restart, about two "
               "minutes. The ring shows progress. Keep the device powered on.";
      }
      String w = why && why[0] ? why : "busy";
      if (w == "low-heap" || w == "unsupported")
        return "This device can't update itself right now. It needs Orchestrator mode and more free memory. Update it over USB instead.";
      if (w == "no-wifi") return "Couldn't reach the network. Check Wi-Fi and try again.";
      if (w == "need-power")
        return "Battery is low. Connect power, then send /update again. It will also install on its own once you plug in.";
      if (w == "need-recalibrate")
        return "Battery health reads low, so the level may be off. Charge to full and recalibrate to 100% in the app, then send /update again.";
      return String("Couldn't start the update (") + w + "). Try again in a moment.";
    }
    if (st == "error") {
      const char* err = otaupd::lastError();
      return String("Couldn't check for updates (") + (err && err[0] ? err : "network error") +
             "). Try again in a minute.";
    }
    if (st == "checking") return "Still checking for updates, send /update again in a minute.";
    return String("Nimbus is up to date (") + NIMBUS_FW_VERSION + ").";
  });
  Serial.begin(115200);
  // HWCDC (USB-serial-JTAG) TX must never block indefinitely: with no host
  // reading, buffered writes stall the writer - with our debug/heartbeat prints
  // that can wedge the loop and, observed live, the whole USB peripheral (a
  // suspected contributor to the replug-only bricks). But timeout 0 DROPS bytes
  // under burst (observed live: torn/truncated RENDER lines when the display
  // task's prints contend), so use a small bound: survives bursts, still can't
  // hang the writer when the host is gone. Diagnostics only, never product data.
  // setTxTimeoutMs is a USB-CDC method; the [env:testuart] build routes Serial to
  // UART0 (a HardwareSerial without it), so guard on the console type.
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(20);
#endif
  // The color touch panel is the only supported display: panel was removed in
  // v4.4. The stored scrModel is still read (frozen NVS key), but a unit that
  // still reads "eink" is a stale/unsupported configuration - bring the panel up
  // anyway and flag it (a clear notice, below), never a blank device. Typed OTA
  // guarantees a real e-ink unit never receives this image: it derives to the
  // untyped device type, which matches no variant, so its update check reports
  // no update and it stays on its last e-ink firmware (see docs/ota.md).
  //
  // ⚠ NVS must be OPEN first. agent::store reads go straight to solide::memory,
  // which silently returns the DEFAULT until memory::begin() has run - and
  // memory::begin() normally runs inside solide::begin(), i.e. after this point.
  // memory::begin() is idempotent, so solide::begin()'s own call below is safe.
  solide::memory::begin();
  g_unsupportedScreenModel = !agent::store::screenIsTft();  // stale "eink" flag
  if (g_unsupportedScreenModel)
    Serial.println("[disp] scrModel=eink is unsupported (e-ink removed) - running the color panel");
  g_screenIsTft = true;   // the only supported display
  g_hal = solide::begin({/*tft=*/true});  // per-subsystem health for STATUS/api-state
  // Publish it solide-free so the web + agent layers can read peripheral health
  // without depending on <solide/solide.h> (P5: /api/health, system.health tool,
  // the live capability manifest).
  // (published AFTER the panel bring-up below - a failed panel must not be
  //  reported healthy, and g_screenIsTft can still flip in the fail-soft path.)
  {
    // solide::begin() already brought the panel + touch up; this wires the
    // renderer to them.
    if (hw::tft::begin()) {
      // Which end of the landscape panel is up. Applied BEFORE the first frame so
      // the boot screen is already the right way round; MADCTL-only, so it costs
      // nothing and cannot blank the panel.
      solide::display_tft::setFlip(agent::store::tftFlip());
      // Apply the stored touch calibration. Resistive panels vary per unit, so
      // the driver's defaults are only a starting point - without this a
      // measured calibration could not survive a reboot, and every tap landing
      // in the wrong place looks exactly like broken hardware.
      const String cal = agent::store::touchCal();
      if (cal.length()) {
        nimbus::touch::Cal c;
        if (nimbus::touch::parseCal(std::string(cal.c_str()), c)) {
          solide::touch::Calibration sc;
          sc.minX = c.minX; sc.maxX = c.maxX;
          sc.minY = c.minY; sc.maxY = c.maxY;
          sc.swapXY = c.swapXY; sc.invertX = c.invertX; sc.invertY = c.invertY;
          solide::touch::setCalibration(sc);
          Serial.printf("[tft] touch calibration %s\n", cal.c_str());
        } else {
          Serial.printf("[tft] stored touch calibration is malformed (%s) - using defaults\n",
                        cal.c_str());
        }
      } else {
        // No stored calibration (a freshly flashed board): apply the per-board-model
        // DEFAULT (CUM-189), the SAME default the web "clear" path restores, from the
        // one boardDefaultCal() source so the two can never drift. On a capacitive
        // panel (FT6336U) only the orientation flags matter (it reports pixels); on a
        // resistive panel the driver keeps its measured min/max and this pins the
        // flags it already defaults to. A saved calibration overrides this.
        const bool cap = solide::board().touchKind == solide::TouchKind::CapacitiveI2c;
        const nimbus::touch::Cal d = nimbus::touch::boardDefaultCal(
            cap ? nimbus::touch::TouchKind::Capacitive : nimbus::touch::TouchKind::Resistive);
        solide::touch::Calibration sc;   // driver-measured min/max; board-model flags
        sc.swapXY = d.swapXY;
        sc.invertX = d.invertX;
        sc.invertY = d.invertY;
        solide::touch::setCalibration(sc);
        Serial.printf("[tft] touch: board-model default orientation (swap=%d invX=%d invY=%d)\n",
                      int(d.swapXY), int(d.invertX), int(d.invertY));
      }
      Serial.printf("[tft] colour touch panel up (%dx%d, touch=%d)\n",
                    int(solide::display_tft::kW), int(solide::display_tft::kH),
                    int(g_hal.touch));
    } else {
      // Fail SOFT: a wrong screenModel or a dead panel must not brick the device.
      // Everything else (ring, Wi-Fi, web, Telegram) still runs, and the web UI
      // stays reachable so the value can be set back.
      Serial.println("[tft] panel bring-up FAILED - continuing headless; check screenModel");
      // The render layer never came up, so renderAndPush will refuse forever and
      // a touch board with no framebuffer has no usable input either. Say so -
      // reporting display=true here made /api/selftest and /api/health call a
      // device healthy while it rendered nothing.
      g_screenIsTft = false;
      g_hal.display = false;
      g_hal.touch   = false;
    }
  }
  // Publish HAL health now that the panel outcome is final, so the self-test and
  // /api/health read the SAME state.
  hw::setHalHealth({g_hal.display, g_hal.leds, g_hal.storage, g_hal.memory,
                    g_hal.touch});
  // Record whether the panel actually bound (a fail-soft bring-up leaves it false
  // and differs from the stored preference until the next restart) so the
  // onboarding wizard can tell whether a display change needs a reboot.
  agent::store::setBootScreenIsTft(g_screenIsTft);
  BOOT_STAGE(50, 50, 50);   // WHITE: HAL (SD/mem/display/leds/input) up
  // Start the breathing-white boot flourish now that the LED ring is up - the
  // first refreshRing() (applyConfig, below) and the loop honor this window,
  // showing a calm breathe until the device settles into its status ring.
  g_bootBreatheUntilMs = millis() + kBootBreatheMs;

  // Route mbedTLS's allocations to PSRAM. The S3 has 8 MB PSRAM but it is NOT in
  // the default malloc pool (getFreeHeap is internal-only: ~218 KB at boot, but
  // ~63 KB by turn time in Orchestrator mode once WiFi + the web server + the
  // World-memory subsystem + the always-on turn task are up). A single TLS
  // handshake wants ~40 KB contiguous and was failing with mbedTLS -0x7F00
  // (ALLOC_FAILED) at ~50 KB. TLS record/session buffers are CPU-processed, never
  // DMA, so external RAM is fine - this lifts the whole handshake off the scarce
  // internal heap. Every driver DMA buffer keeps using the default (internal)
  // allocator, untouched. No-op if PSRAM isn't present.
  if (ESP.getPsramSize() > 0) {
    mbedtls_platform_set_calloc_free(
        [](size_t n, size_t sz) -> void* { return heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM); },
        free);
    // ...and route the GENERAL allocation churn to PSRAM too. mbedTLS's own big
    // buffers are covered above, but the machinery AROUND a network call - Arduino
    // String growth, ArduinoJson nodes, the WiFiClientSecure wrapper, the STT
    // multipart assembly, HTTP header buffers - is plain malloc/new, and the SDK
    // keeps everything <= CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4 KB) on the scarce
    // internal heap. With the Telegram long-poll + SFX + orchestrator all resident,
    // internal free sits at ~15 KB, so a voice hold-to-talk STT can't find the
    // headroom, fails (HTTP 0), and the near-OOM starves the ring/menu render into
    // a freeze. Lowering the spill threshold puts that churn on the 8 MB PSRAM
    // (getFreeHeap is internal-only, so it stops looking near-empty). Explicit
    // MALLOC_CAP_DMA/INTERNAL allocations (WiFi/lwIP/driver DMA) are unaffected.
    // Tunable - allocations >= this size go to the 8 MB PSRAM; smaller stay on the
    // scarce internal heap. 128 B pushes the String/JSON/HTTP churn that was
    // starving internal (turns deferring at <34 KB internal while PSRAM sat 98%
    // EMPTY) onto PSRAM. Only allocations under 128 B stay internal - enough to
    // keep tiny hot-path objects fast without meaningful internal pressure; DMA/ISR
    // allocations use MALLOC_CAP_DMA/INTERNAL explicitly and bypass this entirely
    // (WiFi/lwIP/driver buffers unaffected). getFreeHeap() is internal-only, so it
    // stops reading near-empty. See docs/memory-model.md.
    heap_caps_malloc_extmem_enable(128);
  }
  sramSnap("boot-hal");   // CUM-185: internal-SRAM baseline once PSRAM routing is armed

  // Task watchdog on the MAIN LOOP only (F12): a hung loop panics + reboots in
  // ~8 s instead of wedging the device (and its USB) until a physical reset.
  // tg_poll is deliberately NOT subscribed - its Telegram long-poll legitimately
  // blocks for ~25 s per cycle. Arduino may have already initialized the TWDT
  // (idle-task watch), so reconfigure on INVALID_STATE instead of failing.
  {
    // Single source (nimbus/cloud/relay_timing.h): the relay's TLS slot-hold budget
    // is derived from this same constant, so the two can never drift (CUM-160).
    esp_task_wdt_config_t wdt{/*timeout_ms=*/nimbus::cloud::kTaskWdtTimeoutMs,
                              /*idle_core_mask=*/0, /*trigger_panic=*/true};
    esp_err_t err = esp_task_wdt_init(&wdt);
    if (err == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&wdt);
    // Subscribe the loop task at the END of setup (see below), NOT here. setup()
    // can legitimately take >8 s on a slow/flaky SD or a slow WiFi/memory load, and
    // watchdog-killing setup would create an UNRECOVERABLE boot->WDT->reboot loop
    // (observed live on the V0.1 board with a marginal SD). The watchdog's job is
    // to catch a hung LOOP, not a slow boot.
  }

#if defined(NIMBUS_HAS_FUEL_GAUGE)
  g_fuelgauge.begin(NIMBUS_FUELGAUGE_SDA, NIMBUS_FUELGAUGE_SCL,
                    NIMBUS_VBUS_SENSE_PIN);
#elif defined(NIMBUS_HAS_BATTERY_ADC)
  // Battery hardware is described by the board map, not compile constants, so a
  // variant reads its OWN pack:
  //  - sense pin: the Freenove's GPIO9 vs the Solide's GPIO4 (else analogRead runs
  //    on an unconnected pin - the boot-time "not configured as analog channel").
  //  - cells: 1S (Freenove) vs 2S (Solide) - a wrong count reads the pack at 0%.
  //  - divider: a hand-built board's resistors are OWNER-tuned (220/100 vs 270/120,
  //    web Settings), but an all-in-one has a FIXED divider, so it takes the board's
  //    value (÷2) rather than the tuned default (÷3.2).
  // Only bring the ADC up when monitoring is enabled; otherwise the monitor stays
  // un-begun and reports invalid (desk-powered), so the glyph hides and the low-
  // battery sleep never fires on a board with no pack fitted.
  if (battMonOn())
    g_battAdc.begin(battAdcPin(), battDivX100(), battCells(), NIMBUS_BATT_VBUS_PIN);
  else
    Serial.println("power: battery monitoring off (opt-in) - no pack assumed");
  g_battModel.setCapacityMah(agent::store::battCapMah());
  applyBattChemConfig();   // chemistry + cells + optional custom SoC curve (owner settings)
#endif
  loadBattModel();   // restore the analytics learning (health baseline + rate) from NVS
  g_lowBattSavedPingEp = agent::store::lowBattPingEpoch();
  g_lowBattGate = nimbus::power::AlertGate(g_lowBattSavedPingEp);

  // Boot-load persisted state before configuring anything. A fresh device (no
  // blob / no SD) leaves g_cfg at its defaults; loadMode falls back to Notifier.
  sys::loadConfig(g_cfg);
  const sys::Mode m = sys::loadMode(sys::Mode::Notifier);
  g_menu.setMode(m == sys::Mode::Orchestrator ? Mode::Orchestrator : Mode::Notifier);
  g_menu.setFwVersion(NIMBUS_FW_VERSION);   // Main title: "Settings  v2.0.0"
  g_bleEnabled = sys::loadBleEnabled(true);   // Connectivity > Bluetooth (default on)
  g_menu.setBleEnabled(g_bleEnabled);
  // Resolve the operating mode ONCE at boot. A live Notifier<->Orchestrator swap
  // would mean starting/stopping the poll task + fabric mid-run; the menu/web
  // toggle persists the new Mode and the change takes effect on the next reboot
  // (the panel shows the pending mode name immediately).
  g_orchMode = (m == sys::Mode::Orchestrator);

  // MEMORY (docs/memory-model.md): in Orchestrator mode BLE is NEVER initialized
  // (the ble::begin() below is gated on !g_orchMode), yet the BT controller keeps
  // ~20-30 KB of scarce INTERNAL SRAM reserved for a radio that never turns on -
  // in the exact mode that was starving. Release it. Mode is boot-resolved, so BLE
  // cannot start later this boot; a mode switch reboots (fresh boot re-reserves it).
  if (g_orchMode) {
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
  }

  // Keep the selector's user pick and the active Config profile in agreement.
  // (Selector precedence - forced/VBUS - is a P5 power-manager concern; here the
  // user pick is authoritative and the web portal resolves through the same
  // selector below.)
  g_selector.setUser(g_cfg.profile());

  // Without battery hardware the monitor always reports "external power", which
  // would auto-force the Desk profile forever and silently override the user's
  // pick (seen live as brightness jumping up on menu-exit). Disable VBUS auto-Desk
  // when there is no fuel gauge; a real battery build keeps it.
#ifndef NIMBUS_HAS_FUEL_GAUGE
  g_power.setVbusAutoProfile(false);
#endif

  ORCH_MARK("setup: applyConfig");
  applyConfig();
  BOOT_STAGE(0, 0, 120);   // BLUE: config applied
  // WiFi AP + captive config portal. The AP is always up so the page is reachable
  // even with no saved STA creds. The save callback re-syncs the active profile
  // from the selector, re-applies timings live, and persists. Runs BEFORE the
  // first render so the device identity (AP SSID, first-boot auto-numbering) is
  // resolved and the SetupInfo screen can show the real SSID + config QR.
  ORCH_MARK("setup: net::begin");
  // ⚠ Wi-Fi is ORCHESTRATOR-ONLY. In Notifier mode the transport is Bluetooth,
  // and the BLE controller needs a large CONTIGUOUS block of internal SRAM that
  // Wi-Fi + BLE coexistence cannot spare on this board - with Wi-Fi up,
  // esp_nimble_hci_init() OOMs (ESP_ERR_NO_MEM) and the device boot-loops. So
  // Notifier never brings the radio up: BLE gets the memory, AND with the radio
  // off the colour TFT panel is never disturbed (the white-screen fix falls out
  // for free here too). Config in Notifier is via the on-device menu or a switch
  // to Orchestrator; the web UI is an Orchestrator surface by design.
  if (g_orchMode) {
    net::begin();  // AP "<name>-setup" + STA + captive DNS (identity resolved inside)
  } else {
    // Notifier skips net::begin(), so load the saved-network store here (it reads
    // NVS, no radio) - otherwise provisioned() and the Settings > Wi-Fi picker would
    // wrongly report zero saved networks in Notifier mode.
    nimbus::net::wifistore::begin();
    WiFi.mode(WIFI_OFF);   // Notifier: radio stays off (frees SRAM for BLE, quiet panel)
    agent::alog("[net] Notifier - Wi-Fi off (BLE transport; the controller gets the SRAM)");
  }
  sramSnap("post-net");   // CUM-185: after Wi-Fi AP+STA (Orch) / radio-off (Notifier)

  // Onboarding migration: a device already provisioned before the wizard existed
  // must NOT be dropped into first-run setup on upgrade. Treat any device with
  // stored STA creds as already-onboarded. A factory reset wipes NVS (creds AND
  // this flag), so a truly-fresh or reset device keeps onboarded=false and gets
  // the wizard.
  if (!agent::store::onboarded() && net::provisioned())
    agent::store::setOnboarded(true);

  // First paint: a factory-fresh device (no stored STA creds) opens on the
  // SetupInfo screen - AP SSID + password + scan-to-configure QR - instead of
  // an empty status list, so the out-of-box flow starts on-panel (plan P2).
  ORCH_MARK("setup: firstRender");
  // Orchestrator opens on SetupInfo only until Wi-Fi is provisioned. Notifier has
  // no Wi-Fi, so it opens on the (Bluetooth-worded) SetupInfo "waiting for a
  // connection" screen until the broker connects, then the first frame switches it
  // to live status - never a Wi-Fi-join instruction.
  renderScreen((g_orchMode && net::provisioned()) ? attn::ScreenId::StatusIdle
                                                   : attn::ScreenId::SetupInfo, -1);
  net::WebConfig wc;
  wc.config      = &g_cfg;
  wc.selector    = &g_selector;
  wc.power       = g_monitor;  // NullMonitor (invalid) hides the battery header
  wc.batteryEstimate = [] { return g_battEstimate; };   // analytics snapshot for /api/state
  // Owner-asserted full calibration (POST /api/battcal), applied on THIS main task
  // by loopWeb() - same path as the TEST-only BATTCAL console cmd. Anchors 100% to
  // the current reading, refreshes the /api/state snapshot, and persists to NVS so
  // the anchor survives reboot. No-op until a plausible full reading exists.
  wc.calibrateBatteryFull = [] {
    if (!g_battModel.calibrateFullNow()) return;
    g_battEstimate = g_battModel.estimate();
    saveBattModel();
  };
  // Discard learned analytics (drain-campaign recovery); keeps the BATTCAL anchor.
  wc.resetBatteryLearning = [] {
    g_battModel.resetLearned();
    g_battEstimate = g_battModel.estimate();
    saveBattModel();
  };
  // Battery HARDWARE reconfigure (divider resistors / pack capacity changed on the
  // web). Re-arms the ADC with the new divider and pushes capacity into the model -
  // both idempotent; pin/cells/divider come from the board map. Runs on the main
  // task (owns ADC + model), same staging as calibrateBatteryFull. ⚠ a divider
  // change re-scales every mV, so the BATTCAL anchor is now stale; the UI tells the
  // owner to re-Calibrate. Skips the ADC when monitoring is off (opt-in boards).
  wc.reconfigureBattery = [] {
    if (battMonOn())
      g_battAdc.begin(battAdcPin(), battDivX100(), battCells(), NIMBUS_BATT_VBUS_PIN);
    g_battModel.setCapacityMah(agent::store::battCapMah());
    applyBattChemConfig();   // chemistry + cells + custom SoC curve apply live
    g_battEstimate = g_battModel.estimate();
    agent::alogf("batt: hardware reconfig - divider=/%.2f capacity=%umAh",
                 double(agent::store::battDividerX100()) / 100.0,
                 unsigned(agent::store::battCapMah()));
  };
  // Battery drain/storage (battery-measurement). setStorage is production; setDrain is
  // TEST-only (the endpoint is compiled out of production, so the callback is never set).
  wc.setStorage  = [](int pct) { storageSet(pct); };
  wc.drainState  = [](bool& da, bool& dd, bool& sa, uint16_t& rmv, uint32_t& rage) {
    da = g_highLoadActive; dd = g_drainDeep; sa = g_highLoadActive && g_storageTargetMv != 0;
    rmv = g_hlRestingMv;
    rage = g_hlRestingAtMs ? (millis() - g_hlRestingAtMs) / 1000u : 0u;
  };
  wc.thermalState = [](float& dieC, bool& tripped, uint8_t& trips, bool& aborted) {
    dieC = g_dieTempC; tripped = g_thermal.tripped();
    trips = g_thermal.trips(); aborted = g_thermalAbortLatch;
  };
  wc.drainBright = []() -> uint8_t { return g_drainBright; };   // battlab: per-run load
  wc.drainTtlLeftS = []() -> uint32_t {           // battlab: host dead-man remaining
    if (!g_highLoadActive || !g_drainTtlMs) return 0;   // not armed
    const uint32_t el = millis() - g_drainRefreshedMs;  // rollover-correct
    return el >= g_drainTtlMs ? 0 : (g_drainTtlMs - el) / 1000u;
  };
#ifdef NIMBUS_TEST
  wc.setDrain    = [](bool on, bool deep, int bright, int ttl) { drainSet(on, deep, bright, ttl); };
#endif
  wc.activeJobs  = [] { return jobCount(); };
  wc.currentMode = [] { return int(g_menu.mode()); };
  // HAL health bitmask for /api/state + STATUS (bit order: display,leds,storage,memory,input).
  wc.halMask     = [] {
    return uint8_t((g_hal.display ? 1 : 0) | (g_hal.leds ? 2 : 0) | (g_hal.storage ? 4 : 0) |
                   (g_hal.memory ? 8 : 0) | (g_hal.input ? 16 : 0));
  };
  wc.onChanged   = [] {
    // Web mutations route the user's profile pick through the selector
    // (loopWeb() -> setUser). Mirror the RESOLVED (active) profile into the
    // Config the ring/screens/presets read - a live battery T1 / VBUS window may
    // make that differ from the user's pick. persistConfig() stores the pick
    // (g_selector.user()), never this transient resolved value.
    g_cfg.setProfile(g_selector.resolve());
    applyConfig();
    persistConfig();  // persists g_selector.user()
    if (!g_menu.isOpen()) renderScreen(attn::ScreenId::StatusIdle, -1);
  };
  wc.onModeChanged = [](int mode) {
    g_menu.setMode(mode == 1 ? Mode::Orchestrator : Mode::Notifier);
    persistConfig();  // saveMode(g_menu.mode())
    // Mode is boot-resolved; apply it by rebooting. Defer to loop() - this runs
    // on the AsyncTCP task, where calling ESP.restart() inline is unsafe.
    if ((mode == 1) != g_orchMode) { g_modeSwitchTo = mode; g_rebootPending = true; }
  };
  // Live ring preview (POST /api/preview): staged on the AsyncTCP task, fired
  // here from loopWeb() on the main task like every other webui mutation.
  wc.onPreview = [](int profileId, int status) { startPreview(profileId, status); };
  wc.factoryReset = [](bool eraseSd) { g_factoryEraseSd = eraseSd; g_factoryResetPending = true; };  // main loop erases NVS (+optional /mem) + reboots, keeps identity
  wc.sdReset = [] { g_sdResetPending = true; };            // main loop erases /mem + reboots
  wc.powerOff = [] { g_powerOffPending = true; };          // CUM-224: main loop runs clean shutdown + deep sleep
  wc.canWakeOnTouch = [] { return boardCanWakeOnTouch(); };  // honest web interstitial copy per board
  // Full-card format (CUM-15 / CUM-132): the board-support driver now exposes a
  // low-level format primitive (solide::storage::format(): FATFS f_mkfs over the
  // mounted card, SPI + SDMMC). Wiring this lights up webui's canFormat capability
  // (webui.cpp: canFormat = (bool)s_wc.sdFormat). Deferred to the main loop like
  // the other destructive SD actions - reformatting blocks and must not run on the
  // AsyncTCP task. The on-card destructive acceptance stays deferred pending a
  // scratch card (CUM-131); the driver's host tests cover the guards/state machine.
  wc.sdFormat = [] { g_sdFormatPending = true; };          // main loop reformats the whole card + reboots
  wc.chatSend = [](const String& t) {   // -> tg_poll turn
    if (!agent::telegram::injectMessage("web", t)) return false;
    g_webTurnDeadlineMs = millis() + kWebTurnMaxMs;
    return true;
  };
  wc.chatPending = []() -> bool {
    const uint32_t dl = g_webTurnDeadlineMs;
    return dl != 0 && (int32_t)(millis() - dl) < 0;
  };
  wc.chatPoll = []() -> String {
    net::ConfigLockGuard lk;
    if (!g_webReplyPending) return String();
    g_webReplyPending = false;
    return g_webReply;
  };
  // Route the memory WORKING SET (VDB int8 vector buffers + the entries array - the
  // dominant per-entry cost) to PSRAM, so hundreds/thousands of vectors don't eat the
  // scarce ~300 KB internal SRAM (docs/orchestrator-storage.md §2). Must precede
  // memory::begin() (which loads the blob -> first allocations). The small SSO strings
  // stay internal; heap_caps_free handles both regions. No-op without PSRAM (the
  // portable engine then uses malloc + the tier-aware degraded cap keeps it bounded).
  if (ESP.getPsramSize() > 0) {
    nimbus::orch::setWorkingAllocators(
        [](size_t n) -> void* {
          void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
          return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);  // fall back to internal
        },
        [](void* p) { heap_caps_free(p); });
    agent::alogf("[psram] VDB working set -> PSRAM (%u KB free, %u KB internal heap)",
                 (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getFreeHeap() / 1024));
  } else {
    agent::alogf("[psram] none -> VDB on internal heap (cap enforced)");
  }

  // Mount the microSD card (FAT32) and route the orchestrator's GROWING data -
  // vector memory + episodic history - to it (a 16 GB card vs the few-MB internal
  // LittleFS partition). Must precede beginWeb() (which calls memory::begin()).
  // Falls back to LittleFS with no card. (Voice clips stay on LittleFS: they're
  // temporary, deleted each turn, and don't need the space.)
  BOOT_STAGE(0, 120, 120);   // CYAN: WiFi/portal up; about to mount the SD
  if (solide::storage::begin()) {
    // activeFs() - NOT the SPI `SD` global directly. On an SDMMC board (Freenove)
    // the card is mounted via SD_MMC and `SD` is never begun (its pins are
    // repurposed for the TFT bus there); hardcoding SD silently wired orchestrator
    // memory to an unmounted filesystem, so every real I/O failed and the health
    // tracker demoted the tier to "no SD" despite the card being present and
    // readable (round-4 bench report).
    agent::memory::setDataFs(solide::storage::activeFs());
    agent::alogf("[sd] mounted: %llu MB total, %llu MB free",
                 (unsigned long long)solide::storage::cardSizeMB(),
                 (unsigned long long)solide::storage::freeMB());
  } else {
    // cardType 0 (CARD_NONE) means the card never answered on the bus at all ->
    // NOT a format issue (an exFAT card would still report its type; only the FAT
    // mount would fail). 0 => no card / unseated / slot wiring fault. Non-0 + mount
    // fail => reformat FAT32. Data stays on internal flash either way.
    // solide::storage::cardType() (not the SPI `SD` global) - same active-backend
    // reasoning as above, or this diagnostic always reads 0 on an SDMMC board.
    int ct = (int)solide::storage::cardType();
    agent::alogf("[sd] mount FAILED (cardType=%d: 0=none 1=MMC 2=SDSC 3=SDHC). %s", ct,
                 ct == 0 ? "Card not seen on the bus -> reseat it / check the slot solder."
                         : "Card seen but FAT mount failed -> reformat it FAT32.");
  }
  BOOT_STAGE(120, 0, 120);   // MAGENTA: SD stage done; entering web + memory::begin
  sramSnap("pre-web");    // CUM-185: after SD mount, before memory::begin + web bind
  ORCH_MARK("setup: beginWeb");
  if (g_orchMode) net::beginWeb(wc);   // web server is Orchestrator-only (no Wi-Fi in Notifier)
  sramSnap("post-web");   // CUM-185: after memory::begin (episodic/vector load) + s_server.begin()
  // Device-event timeline (Glass Box A3): the BOOT row - the answer to the
  // owner's live "did you manage to reboot?" that the model couldn't give
  // ("there's no reboot record available"). Reset reason distinguishes a
  // requested restart (SW) from a crash (PANIC/WDT) or power event.
  {
    const char* rr = "unknown";
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON:  rr = "power-on"; break;
      case ESP_RST_SW:       rr = "software-restart"; break;
      case ESP_RST_PANIC:    rr = "crash(panic)"; break;
      case ESP_RST_INT_WDT:
      case ESP_RST_TASK_WDT:
      case ESP_RST_WDT:      rr = "crash(watchdog)"; break;
      case ESP_RST_BROWNOUT: rr = "brownout"; break;
      case ESP_RST_DEEPSLEEP:rr = "deep-sleep wake"; break;
      default: break;
    }
    String ota = agent::store::otaLastResult();
    agent::memory::captureEvent("boot",
        String("Booted ") + NIMBUS_FW_VERSION " (" NIMBUS_FW_BUILD ") reason=" + rr +
        " mode=" + (g_orchMode ? "orchestrator" : "notifier") +
        (ota.length() ? (String(" ota=") + ota) : String()));
  }
  // E1 artifact store: AFTER beginWeb (memory::begin ran inside it and resolved
  // the SD tier); loads/rebuilds the /mem/files index + registers the file tools.
  agent::files::begin();
  ORCH_MARK("setup: webdone");
  BOOT_STAGE(0, 120, 0);     // GREEN: web server + memory subsystem up

  // Self-test seam: install the live-state provider + register the device.*
  // orchestrator tools (memory::begin() ran inside beginWeb, so the registry
  // exists). Order-independent of the tool loop; also exposed over /mcp.
  nimbus::hw::setInputsProvider(buildSelfTestInputs);
  registerDeviceTools();

  // Sound cue engine: after memory::begin (SD tier known) + NVS config.
  // The router tap is the single seam through which BOTH modes' attention
  // events reach the sound engine.
  ::sfx::begin(g_orchMode);
  // Music player (CUM-40): the media.* tools + /play are Orchestrator features, and
  // its dedicated task should not compete for Notifier mode's tight internal SRAM.
  if (g_orchMode) { music::begin(); music::registerTools(); }
  g_router.setEventTap(&sfxEventTap);
  // Screensaver idle clock: threshold from NVS (default 60 min, 0 = off); boot
  // counts as activity so a freshly powered device never saver-jumps early.
  g_saver.setThresholdMin(agent::store::saverMin());
  g_saver.noteActivity(millis());
  // WiFi link sounds, edge-triggered: Arduino re-emits DISCONNECTED every few
  // seconds while retrying, so only the had-an-IP -> lost transition (and its
  // inverse) is voiced - reconnect churn stays silent.
  WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t) {
    static bool s_hadIp = false;
    if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      if (!s_hadIp) ::sfx::fire(nimbus::sfx::Ev::WifiUp);
      agent::loops::onNetworkUp();   // kick SNTP for wall-clock loop schedules
      s_hadIp = true;
      if (g_screenIsTft) {
        const uint32_t now = millis();
        // First provisioning gets a bounded fallback window. The web page will
        // normally shorten it through /api/wifi/handoff; if the browser dies,
        // the RF-sensitive TFT is still protected after 20 s.
        g_dropApAfterMs = g_apHandoffArmed ? now + 20000UL : now;
        g_apHandoffArmed = false;
        g_dropApPending = true;
      }
    } else if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      if (s_hadIp) {
        ::sfx::fire(nimbus::sfx::Ev::WifiDown);
        if (g_screenIsTft) g_restoreApPending = true;   // Wi-Fi lost: bring the setup AP back
      }
      s_hadIp = false;
    }
  });

  // Provider key verification runs in BOTH modes (the web page can configure the
  // orchestrator from Notifier mode). Its dedicated task idles until /api/verify
  // enqueues a check; the TLS arbiter serializes it against orchestrator turns.
  // arbiter::begin() is idempotent, so calling it here and in orchestratorBegin()
  // is safe in either order.
  agent::arbiter::begin();
  agent::provider_verify::begin();
  agent::ttsvoices::begin();   // lazy-fetch the live Mistral voice catalog on demand

  // BLE nsn transport (plan §3.6): a GATT server accepting whole nsn frames
  // over Write Without Response, drained in the Notifier loop branch below.
  // Notifier mode ONLY in v1 - the Orchestrator loop never drains the notifier,
  // so advertising there would accept frames nobody applies. Coexists with the
  // AP/STA WiFi above via the IDF RF scheduler. Gated on the persisted
  // Connectivity > Bluetooth toggle (g_bleEnabled); it can be flipped live from
  // the menu without a reboot (net::ble::setEnabled).
  if (!g_orchMode && g_bleEnabled) net::ble::begin();

  // Orchestrator mode: bring up the turn loop + Telegram poll AFTER the network
  // stack is up (STA connects async; adapters/Telegram tolerate a not-yet-ready
  // link and simply retry). Notifier mode leaves all of this dormant - the nsn
  // serial path below is unchanged.
  if (g_orchMode) orchestratorBegin();
  sramSnap("post-orch");  // CUM-185: after tg_poll/fabric/orchestrator task stacks are up
  BOOT_STAGE(120, 120, 0);   // YELLOW: orchestrator up (final stage before loop)

#ifdef NIMBUS_TEST
  // Test-affordances island (the HIL test spec): wire the console's state/action hooks
  // to the real globals, surface WiFi disconnect reasons (F9), and emit the boot
  // beacon (F11/F14). This whole block is absent from production (esp32s3).
  {
    tc::Hooks h;
    h.mode      = [] { return g_orchMode ? 1 : 0; };
    h.wifiStaUp = [] { return net::staConnected(); };
    h.wifiIp    = [] { return net::staIp(); };
    h.rssi      = [] { return net::rssi(); };
    h.curScreen = [] { return uint8_t(g_lastScreen); };
    // The driver that actually bound, so STATUS can report reality rather than
    // the stored preference (they differ whenever the fail-soft path trips).
    h.screenIsTft = [] { return g_screenIsTft; };
    // NSNFEED: drive the notifier UI without a broker or BLE (blocked by macOS
    // BLE permissions on the bench). Same decoder/mapper/router path as a real
    // frame, so what renders is what a broker would produce.
    h.jobCount = [] { return jobCount(); };
    h.tftFlip = [](bool on) {
      if (g_screenIsTft) solide::display_tft::setFlip(on);
    };
    // The RING/RADIO/TOUCHPOLL bisect toggles that used to live here are GONE -
    // they were instruments for the white-screen hunt (root cause found: the
    // SoftAP beacon disturbing the jumper-wired panel; fixed by the AP drop).
    // Kept, a persisted OFF toggle made a healthy board read as dead hardware
    // ("lights at boot, no live control"), the exact failure they were built to
    // diagnose. Stale dbg* NVS keys are simply never read again.
    h.panelProbe = [](bool on) { hw::tft::setProbeEnabled(on); return true; };
    h.setProfile = [](int p) {
      if (p < 0 || p > 2) return false;
      g_selector.setUser(ProfileId(p));
      g_cfg.setProfile(g_selector.resolve());   // forced > VBUS > user
      applyConfig();
      persistConfig();
      return true;
    };
    h.nsnFeedByte = [](uint8_t b) {
      if (g_orchMode) return;
      if (g_notifier.feedBle(b, millis())) {
        refreshRing();
        renderScreen(attn::ScreenId::StatusIdle, -1);
      }
    };
    // Posture of the last PLAN applied to the ring - diverges from g_cfg while a
    // web preview (POST /api/preview) is live; RENDER? reports what the ring shows.
    h.posture   = [] { return uint8_t(g_lastPosture); };
    h.ringSummary = [](int& seg, bool& single, bool& dark, uint8_t& bright) {
      // g_last* are updated by refreshRing() under the config lock; read them
      // under the same lock so a torn compose can't be observed.
      net::ConfigLockGuard lk;
      seg = g_lastSeg; single = g_lastSingle; dark = g_lastDark;
      bright = g_lastBright;
    };
    h.turn   = [](const String& t) {
      // Route to the orchestrator's turn task (tg_poll), NOT inline: a real turn
      // blocks 5-30 s on TLS and this hook runs on the watchdog-subscribed main
      // loop, so an inline handleMessage() reboots the board. injectMessage()
      // enqueues to g_inboundQ, drained on tg_poll (not watchdog-subscribed).
      if (!agent::telegram::injectMessage("serial", t))
        Serial.println("TURN busy - inbound queue full, resend");
    };
    h.wifi   = [](const String& s, const String& p) {
      return net::saveAndConnect(s, p);
    };
    h.reboot = [] { ESP.restart(); };
    h.factoryReset = [] { g_factoryResetPending = true; };  // FACTRESET: same seam as web
    h.hang   = [] { for (;;) {} };  // test-only spin - the F12 WDT reboots us in ~8 s
    h.setMode = [](int m) {
      sys::saveMode(m == 1 ? sys::Mode::Orchestrator : sys::Mode::Notifier);
      ::sfx::fire(nimbus::sfx::Ev::ModeSwitch);
      delay(1500);  // reply flush + let the mode-switch clip finish before restart
      ESP.restart();
    };
    h.bleState = [](int& en, int& conn) {               // BLE? diagnostic
      en = (!g_orchMode && net::ble::enabled()) ? 1 : 0;
      conn = net::ble::connected() ? 1 : 0;
    };
    h.bleMac = [] { return net::ble::macAddress(); };   // BLEMAC?
    h.menuView = [] {                                   // MENU? (wording HIL seam)
      solide::menu::MenuView v = g_menu.view();
      String out = String("MENU open=") + (v.visible ? 1 : 0) +
                   " sel=" + v.selected + " n=" + int(v.items.size()) +
                   " title=" + v.title.c_str() + " items=";
      for (size_t i = 0; i < v.items.size(); ++i) {
        if (i) out += "|";
        out += v.items[i].c_str();
      }
      const char* help = g_menu.helpText();
      if (help && help[0]) { out += " help="; out += help; }
      return out;
    };
    h.bleBonds = [](int& bonds, int& pairing) {         // BONDS?
      bonds = net::ble::numBonds();
      pairing = net::ble::pairingActive() ? 1 : 0;
    };
    h.bleForget = [] { net::ble::forgetBonds(); };      // FORGETBONDS
    h.rawFrameActive = [] { return solide::leds::currentState().rawFrame; };  // RAWFRAME?
    h.battCal = [] () -> String {                                             // BATTCAL
      if (!g_battModel.calibrateFullNow())
        return String("no-sample (need a valid full reading first)");
      g_battEstimate = g_battModel.estimate();
      saveBattModel();
      return String("anchored full=") + g_battModel.fullAnchorMv() + "mV/cell pct=" +
             g_battEstimate.percent + "%";
    };
    h.sleepNow = [] { enterLowBattSleep(); };                                // SLEEP
    h.powerOffInfo = [] () -> String {                                       // POWEROFF?
      return String("tapWakes=") + (boardCanWakeOnTouch() ? "1" : "0") +
             " pin=" + String(touchWakePin());
    };
    h.powerOffNow = [] { enterPowerOffSleep(); };                            // POWEROFF
    h.dreamNow = [] () -> String {                                           // DREAM
      if (!g_orchMode) return String("ERR dream: Orchestrator mode only");
      nimbus::orch::LoopRecord rec = agent::dream::reservedLoopRecord();
      nimbus::orch::LoopFireRequest r;
      r.id     = rec.id;
      r.name   = rec.name;
      r.prompt = rec.prompt;
      r.chatId = std::string(agent::orchestrator::firstAllowedChat().c_str());
      nimbus::orch::FireOutcome o = agent::dream::fire(r, /*force=*/true);
      return String("DREAM fired ok=") + (o.ok ? "1" : "0") +
             " detail=" + o.detail.c_str();
    };
    h.ctxInfo = [](const String& chat) -> String {                           // CTX?
      if (!g_orchMode) return String("ERR ctx: Orchestrator mode only");
      return agent::orchestrator::foldStatusText(chat.length() ? chat.c_str() : nullptr);
    };
    h.compactNow = [](const String& chat) -> String {                        // COMPACT
      if (!g_orchMode) return String("ERR compact: Orchestrator mode only");
      agent::orchestrator::stageManualFold(chat.c_str());
      return String("COMPACT staged chat=") + chat + " (poll CTX? " + chat + ")";
    };
    h.memFill = [](const String& kind, int n, int bytes) -> String {         // MEMFILL
      if (!g_orchMode) return String("ERR memfill: Orchestrator mode only");
      int added = 0;
      if (kind == "epi")      added = agent::memory::testFillEpisodic(n, bytes);
      else if (kind == "vec") added = agent::memory::testFillVectors(n);
      else return String("ERR memfill kind: epi|vec");
      return String("MEMFILL ") + kind + " added=" + added;
    };
    h.epiQuery = [](const String& before, const String& text) -> String {    // EPIQ
      if (!g_orchMode) return String("ERR epiq: Orchestrator mode only");
      nimbus::orch::MsgQuery q;
      q.textContains = std::string(text.c_str());
      q.before = std::string(before.c_str());
      q.coldScan = true;
      q.limit = 3;
      nimbus::orch::EpiQueryInfo qi;
      const uint32_t t0 = millis();
      std::vector<nimbus::orch::EpisodicMessage> rows;
      {
        agent::memory::Lock lk;
        rows = agent::memory::episodic().query(q, &qi);
      }
      String out = String("EPIQ rows=") + rows.size() + " ms=" + (millis() - t0) +
                   " files=" + qi.coldFiles + " bytes=" + (unsigned long)qi.coldBytes +
                   " older=" + (qi.olderExists ? 1 : 0) +
                   " next=" + (qi.nextBefore.empty() ? "-" : qi.nextBefore.c_str());
      for (const auto& m : rows)
        out += String("\n  ") + m.id.c_str() + " " +
               m.text.substr(0, 48).c_str();
      return out;
    };
    h.battReset = [] () -> String {                                          // BATTRESET
      const float wasRate = g_battEstimate.ratePctPerHr;
      g_battModel.resetLearned();
      g_battEstimate = g_battModel.estimate();
      saveBattModel();
      return String("cleared learned analytics (was ") + String(wasRate, 1) +
             "%/hr) - kept battcal anchor=" + g_battModel.fullAnchorMv() + "mV/cell";
    };
    // DRAIN on|off [deep] - ttl=0: a human at the console has no refresher, so the
    // host dead-man must stay DISARMED here or it would yank the load mid-test.
    h.drain   = [](bool on, bool deep) { return drainSet(on, deep, -1, 0); };
    h.storage = [](int pct) { return storageSet(pct); };               // STORAGE <pct>|off
    h.drainState = [](bool& active, bool& deep, uint16_t& restMv) {     // STATUS surfacing
      active = g_highLoadActive; deep = g_drainDeep; restMv = g_hlRestingMv; };
    tc::begin(h);
  }
  // Surface WiFi disconnect reason on serial + arm the attention badge (F9),
  // and echo GOT_IP on success (F8). Same event pattern as provision.cpp:85.
  // wifi_portal.cpp has no such handler today; the console adds it under test.
  WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
    if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      tc::onWifiReason(info.wifi_sta_disconnected.reason);
    else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP)
      tc::onWifiGotIp(WiFi.localIP().toString());
  });
#endif

#ifdef NIMBUS_NOTIFIER_DEBUG
  Serial.println("NSN ready");
#endif

  // setup() finished - NOW arm the F12 watchdog on the loop task (configured above).
  // loop() feeds it every iteration; a hung loop panics + reboots in ~8 s.
  esp_task_wdt_add(nullptr);
  // Risk overrides DO NOT survive a reboot (review: a forgotten override outlives
  // the conversation that justified it - the heat cap and the deep-discharge guard
  // must fail back to SAFE). Cleared with a log line so the change is visible.
  if (agent::store::brightOvr()) {
    agent::store::setBrightOvr(false);
    agent::alogf("bright: override cleared at boot (does not persist)");
  }
  if (agent::store::sleepOvr()) {
    agent::store::setSleepOvr(false);
    agent::alogf("power: sleep override cleared at boot (does not persist)");
  }

  // Low-battery protection: seed the policy from NVS, and if THIS boot is a wake
  // from the low-batt sleep (the charger-sniff timer or the VBUS pin), give a
  // grace window before re-sleeping so a human can react / a charger can be seen.
  g_power.policyRef().setT2PackMv(agent::store::sleepMv());
  g_power.policyRef().setT2Override(agent::store::sleepOvr());
  {
    const esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
    if (s_rtcPowerOff) {
      // A wake from a deliberate "Power off" (touch ext0 on a wake-capable board):
      // boot normally, never into the low-batt grace path. Clear the RTC flag so a
      // later low-batt sleep is classified correctly.
      s_rtcPowerOff = false;
      agent::alogf("power: woke from software power-off (cause=%d) - normal boot", int(wc));
    } else if (wc == ESP_SLEEP_WAKEUP_TIMER || wc == ESP_SLEEP_WAKEUP_EXT0) {
      s_wokeFromLowBatt = true;
      s_lowBattGraceUntil = millis() + 90u * 1000u;   // 90 s to react
      agent::alogf("power: woke from low-batt sleep (cause=%d) - timer/charger sniff", int(wc));
    }
  }

  // A stale scrModel of "eink" is unsupported (e-ink was removed). The color panel
  // is up regardless; hold a clear notice on it so the owner is never left guessing,
  // and the web page can switch the setting to the touch screen. Sticky, so it holds
  // until a tap dismisses it; the scheduler gate (loop, !g_askSticky) keeps status
  // frames from painting over it. A normal unit (scrModel=tft) never sees this.
  if (g_unsupportedScreenModel && g_screenIsTft) {
    g_askOverride = "Unsupported display setting.\n"
                    "This unit is set to e-ink, which is no longer supported.\n"
                    "Open the device web page to switch it to the touch screen.";
    g_askSticky = true; g_askPage = 0;
    renderScreen(attn::ScreenId::Ask, -1);
  }

  tc::ready();  // "READY mode=<n> ip=<..>" boot beacon (no-op in production)
  ::sfx::fire(nimbus::sfx::Ev::Boot);   // "CommLink online." - boot complete chime
}

// Seed the settings menu from persisted state + open it. Shared by every open
// gesture (Notifier long-press / double-click, Orchestrator double-click) so the
// seed logic lives in ONE place. Seeds the user's authoritative profile pick (not
// the transient active profile a battery T1 / VBUS event forced into g_cfg).
// Live SD status for the Main menu row: mounted -> free/total, else why not.
static String sdStatusLine() {
  if (agent::memory::sdLost()) return "dropped - click to retry";
  if (!solide::storage::available()) return "none - click to retry";
  const uint64_t total = solide::storage::cardSizeMB(), free = solide::storage::freeMB();
  char b[28];
  if (total >= 4096)  // big cards read better in GB (one decimal)
    snprintf(b, sizeof b, "%.1f/%.1f GB free", free / 1024.0, total / 1024.0);
  else
    snprintf(b, sizeof b, "%llu/%llu MB free", (unsigned long long)free,
             (unsigned long long)total);
  return String(b);
}

// MENU RING (two owner asks, reconciled): while the menu is open the ring's
// BASELINE is black - the click-wake reveal and the Full-posture animator
// resuming over the top were washing out the 2.9" panel (the v3.0.0 menu-dark
// fix). But the dim fill-bar POSITION ECHO is restored (it was removed with the
// flood by mistake - owner: "the % of the ring colored is almost instant...
// very helpful for menu navigation"): each detent repaints the bar (list mode:
// fill = cursor position, option 8/12 lights two thirds; value-adjust: fill =
// the value) and holds ~2 s so you can count your clicks, then the ring yields
// back to BLACK - never to the bright status ring - until the next detent.
// loop() re-pushes the current frame every pass (showFrame is the highest-
// precedence layer but is auto-released by the driver's ~500 ms staleness
// watchdog, so a paint-once frame would let a stale Pattern bleed back). The
// menu-close path (clearFrame + refreshRing) restores live status.
static solide::ring::RGB g_menuRingBuf[NIMBUS_RING_LEDS];
static uint32_t g_menuRingUntilMs = 0;          // fill-bar hold; 0 or expired = black
static constexpr uint32_t kMenuRingHoldMs = 2000;

// Repaint the menu ring frame. bump=true (open / a detent) restarts the fill-bar
// hold; bump=false (the per-pass re-push) keeps the current phase, transitioning
// fill -> black when the hold expires.
static void menuRingRepaint(bool bump) {
  const int L = NIMBUS_RING_LEDS;
  const uint32_t now = millis();
  if (bump) g_menuRingUntilMs = now + kMenuRingHoldMs;
  for (int i = 0; i < L; i++) g_menuRingBuf[i] = {0, 0, 0};
  if (g_menuRingUntilMs && int32_t(g_menuRingUntilMs - now) > 0) {
    int pct;
    if (g_menu.valueAdjusting()) {
      pct = g_menu.adjustValuePct();
    } else {
      const solide::menu::MenuView v = g_menu.view();
      const int n = v.items.empty() ? 1 : (int)v.items.size();
      pct = ((v.selected + 1) * 100) / n;
    }
    const nimbus::ThemeColor th =
        nimbus::themeAccent(std::string(agent::store::theme().c_str()));
    // MINIMAL brightness (owner: full accent was blinding next to the panel -
    // an indicator, not a lamp). ~1/7th of the accent reads clearly in a lit room.
    const solide::ring::RGB dim{uint8_t(th.r / 7), uint8_t(th.g / 7), uint8_t(th.b / 7)};
    const int lit = (pct * L + 50) / 100;
    for (int i = 0; i < lit && i < L; i++) g_menuRingBuf[i] = dim;
  }
  solide::leds::showFrame(g_menuRingBuf, L);
}

static void openSettingsMenu() {
  g_cfg.setProfile(g_selector.user());
  g_menu.setSdStatus(std::string(sdStatusLine().c_str()));
  g_menu.setTheme(nimbus::themeIndexOf(std::string(agent::store::theme().c_str())));
  g_menu.setSfxLevel(g_orchMode ? agent::store::sfxLevelOrch()
                                : agent::store::sfxLevelNotif());
  g_menu.setSfxVoice(agent::store::sfxTheme() == "protoss" ? 1
                     : agent::store::sfxTheme() == "zerg" ? 2 : 0);
  g_menu.setSfxVolume(agent::store::sfxVolume());
  // Panel type drives which screensaver choices the row offers - minutes on a
  // backlit panel (where resting IS the power saving), hours on panel (where it
  // is about ghosting).
  g_menu.setScreenFlip(agent::store::tftFlip());   // Settings > Display > Display flip (TFT only)
  g_menu.setHasRing(solide::board().hasRing);      // hide ring-only Customize params on a ringless board (CUM-187)
  g_menu.setTouchWake(boardCanWakeOnTouch());      // Power off copy: "tap to wake" only where the touch INT is wired (CUM-224)
  g_menu.setSaverMinutes(agent::store::saverMin());
  g_menu.setAutoUpdate(agent::store::otaAutoUpdate());
  g_menu.setSttProvider(agent::store::sttProvider() == "openai" ? 1 : 0);
  g_menu.setTtsProvider(agent::store::ttsProvider() == "openai" ? 1 : 0);
  // OTA is Orchestrator-mode-only (Notifier's BLE owns the update RAM): the
  // Software update submenu renders Check as unavailable in Notifier mode.
  g_menu.setOtaAllowed(g_orchMode);
  g_menu.setUpdateStatus("");
  g_menu.setUpdateAvailable(std::string(otaupd::statusStr()) == "available"
                                ? std::string(otaupd::latestSeen().c_str())
                                : std::string());
  g_menu.open();
  g_menuNeedsPaint = true;
  // Cancel any in-flight click-wake reveal + its brightness hold (the flood
  // fix), then show the fill-bar echo so the list layout reads the instant the
  // menu opens; it fades to the black baseline after its hold.
  g_revealUntilMs = 0;
  hw::setBrightnessHold(false);
  menuRingRepaint(true);
}

// On-device tap-the-crosses touch calibration (CUM-189). Entered ONLY from
// Settings > Display > Calibrate touch (an opt-in menu request), so it can never run
// at boot or disturb normal operation. The pure CalWizard owns the targets + solve;
// this wrapper draws each target, reads RAW touch (not the calibration under test),
// and on completion applies + persists the result. Blocking and bounded (max four
// 30 s waits), so it suspends the main-loop watchdog for its duration (the same
// pattern the record/STT path uses) and aborts cleanly if a target is never tapped.
static void runTouchCalibration() {
  if (!g_screenIsTft || !solide::touch::present()) return;
  const int16_t w = int16_t(solide::display_tft::kW);
  const int16_t h = int16_t(solide::display_tft::kH);
  nimbus::touch::CalWizard wiz;
  wiz.begin(w, h, 24);

  esp_task_wdt_delete(nullptr);   // blocking flow: suspend the loop WDT (MICREC pattern)
  const uint32_t kPerTargetMs = 30000;
  bool aborted = false;
  while (!wiz.done()) {
    render::ScreenCtx c;
    c.calTotal   = wiz.count();
    c.calStep    = wiz.step();
    c.calTargetX = wiz.targetX();
    c.calTargetY = wiz.targetY();
    c.calMessage = "Tap each corner target";
    hw::tft::renderAndPush(attn::ScreenId::TouchCal, c);

    // Capture ONE clean press-then-release for this target, bounded to kPerTargetMs.
    // The panel must first be seen UP (so a held or bouncing finger from the previous
    // corner cannot fill this one), then a press, then a release records the corner.
    // A target that is never cleanly recorded - absent touch (never down) OR a
    // stuck-down line (never released within the window) - aborts the whole flow, so
    // the watchdog-suspended loop can never wedge on a shorted or held panel.
    const uint32_t start = millis();
    bool armed = false, sawDown = false, recorded = false;
    uint16_t rx = 0, ry = 0, rz = 0, lastX = 0, lastY = 0;
    while (int32_t(millis() - start) < int32_t(kPerTargetMs)) {
      const bool down = solide::touch::readRaw(rx, ry, rz);
      if (!armed) {                    // wait for the panel to be released first
        if (!down) armed = true;
      } else if (down) {
        lastX = rx; lastY = ry; sawDown = true;
      } else if (sawDown) {
        wiz.recordRaw(lastX, lastY);   // release edge: this corner is captured
        recorded = true;
        break;
      }
      delay(15);
    }
    if (!recorded) { aborted = true; break; }   // no clean press+release in time: cancel
  }
  esp_task_wdt_add(nullptr);   // re-arm the loop WDT

  if (!aborted && wiz.done()) {
    nimbus::touch::Cal cal;
    if (wiz.solve(cal)) {
      agent::store::setTouchCal(String(nimbus::touch::formatCal(cal).c_str()));
      solide::touch::Calibration sc;
      sc.minX = cal.minX; sc.maxX = cal.maxX; sc.minY = cal.minY; sc.maxY = cal.maxY;
      sc.swapXY = cal.swapXY; sc.invertX = cal.invertX; sc.invertY = cal.invertY;
      solide::touch::setCalibration(sc);
      agent::alog("[cal] touch calibrated on device (applied + persisted)");
    } else {
      agent::alog("[cal] calibration presses were degenerate - calibration left unchanged");
    }
  }
  hw::tft::forceRepaint();
  renderScreen(attn::ScreenId::StatusIdle, -1, true);   // hand the screen back
}

// Everything that must happen AFTER the settings-menu FSM mutates: the ring
// echo, the dirty()->persist+applyConfig block, and every request-flag drain
// (bonds, SD probe, Wi-Fi, OTA install).
//
// ⚠ This used to live INSIDE the encoder pump's for(;;) loop. On a TFT board
// solide::begin() never starts the encoder task, so pop() returns nothing and
// the loop breaks on its first iteration - which meant NOTHING here ever ran
// from touch. The menu moved and repainted while every setting silently failed
// to persist or apply. Both input paths now call this explicitly.
//
// Callers must NOT hold the net config lock: the block re-acquires it via
// applyConfig()/renderScreen() and does real work (NVS writes, renders).
static void settleMenuAfterMutation(uint32_t now) {
  // Menu ring echo: every detent repaints the dim fill-bar INSTANTLY (the
  // panel is the slow truth, the LEDs are the echo) and restarts its ~2 s
  // hold, after which the ring yields back to the black baseline. If this
  // event just CLOSED the menu, release the raw frame and restore status.
  if (g_menu.isOpen()) {
    // ⚠ The ring echo is a KNOB affordance. On the encoder it mirrors the scroll
    // position, which is genuinely useful when your hand is on the dial and your
    // eyes are on the panel. On a touch board there is no dial, your finger is
    // already ON the row you picked, and the ring lighting up for plain
    // navigation is just noise (owner, 2026-07-30).
    //
    // It still earns its place while a VALUE is being adjusted - a +/- stepper is
    // exactly where seeing the change reflected is worth having - so echo then
    // and stay quiet otherwise.
    if (!g_screenIsTft || g_menu.adjustingValue()) menuRingRepaint(true);
    else                                          solide::leds::off();
  } else {
    solide::leds::clearFrame();
    // A theme was picked this menu session -> arm the flourish NOW, when the ring
    // becomes visible again, rather than at pick time. Otherwise browsing other rows
    // for >kThemePreviewMs after the pick would consume the window before the main
    // screen returns and the flourish would never show (prism).
    if (g_themeFlourishPending) {
      g_themePreviewUntilMs = now + kThemePreviewMs;
      g_themeFlourishPending = false;
    }
    refreshRing();
    // Menu CLOSED: force the panel back to LIVE STATUS. Without this the
    // render scheduler resumes on its last ambient intent - which can be a
    // STALE Screensaver latched before the menu session - flashing the logo
    // for one refresh before the next status tick (field bug, 2026-07-18:
    // close-menu -> ~3 s of screensaver -> StatusIdle). The saver clock was
    // already kicked by the per-event saverKick() above.
    g_sched.onIntent(uint8_t(attn::ScreenId::StatusIdle), true, millis());
  }
  // Any menu mutation applies live (timings/brightness) + persists. While
  // the menu is open g_cfg.profile() is the user's pick (seeded on open,
  // power-tick re-resolve suppressed), so syncing it into the selector's
  // user pick is safe - no transient forced/VBUS value can leak in here.
  if (g_menu.dirty()) {
    g_selector.setUser(g_cfg.profile());
    // LED theme picked in the Theme submenu -> persist + let applyConfig()'s
    // refreshRing() pick up the new colour (idempotent when unchanged). On a REAL
    // change, flourish the new palette on the ring so the pick is visible when the
    // menu closes (the idle ring is otherwise dark - see g_themePreviewUntilMs).
    const String newTheme = String(g_menu.themeSlug().c_str());
    if (newTheme != agent::store::theme()) g_themeFlourishPending = true;   // armed on menu close
    agent::store::setTheme(newTheme);
    // SFX rows: persist the ACTIVE mode's level + the voice theme, then let
    // the engine re-read (and recount SD variants on a theme change).
    if (g_orchMode) agent::store::setSfxLevelOrch((uint8_t)g_menu.sfxLevel());
    else            agent::store::setSfxLevelNotif((uint8_t)g_menu.sfxLevel());
    agent::store::setSfxTheme("pulse");
    agent::store::setSfxVolume((uint8_t)g_menu.sfxVolume());
    ::sfx::refreshConfig();
    // Screensaver delay: persist + apply live only on a real change (the
    // menu snaps to buckets; an untouched console value must not be rewritten).
    if (g_menu.saverMinutes() != agent::store::saverMin()) {
      agent::store::setSaverMin(g_menu.saverMinutes());
      g_saver.setThresholdMin(g_menu.saverMinutes());
    }
    // Display flip (TFT only): persist + apply live. setFlip re-arms MADCTL with
    // no reset; the menu's own label change repaints, so the new orientation is
    // visible immediately. forceRepaint covers any caller whose frame is identical.
    if (g_menu.screenFlip() != agent::store::tftFlip()) {
      agent::store::setTftFlip(g_menu.screenFlip());
      if (g_screenIsTft) {
        solide::display_tft::setFlip(g_menu.screenFlip());
        hw::tft::forceRepaint();
      }
    }
    if (g_menu.autoUpdate() != agent::store::otaAutoUpdate())
      agent::store::setOtaAutoUpdate(g_menu.autoUpdate());
    {
      const String stt = g_menu.sttProvider() ? "openai" : "mistral";
      const String tts = g_menu.ttsProvider() ? "openai" : "mistral";
      if (stt != agent::store::sttProvider()) agent::store::setSttProvider(stt);
      if (tts != agent::store::ttsProvider()) {
        // Mirrors the web UI: a provider change resets the picked voice to
        // that provider's default (the old voice id is meaningless there).
        agent::store::setTtsProvider(tts);
        agent::store::setTtsVoice("");
      }
    }
    applyConfig();
    persistConfig();  // persists g_selector.user() + saveMode + saveBleEnabled
    g_menu.clearDirty();
    // Bluetooth toggle (Connectivity): apply LIVE - no reboot. Enabling begins
    // the stack if it never ran; disabling stops advertising. Only meaningful
    // in Notifier mode (BLE is inert in Orchestrator mode); the persisted flag
    // still updates so it takes effect if the device later boots Notifier.
    if (g_menu.bleEnabled() != g_bleEnabled) {
      g_bleEnabled = g_menu.bleEnabled();
      if (!g_orchMode) net::ble::setEnabled(g_bleEnabled);
    }
    // Operating mode is resolved ONCE at boot (g_orchMode gates which
    // subsystems run: BLE/nsn for Notifier, telegram/fabric for Orchestrator).
    // Toggling the Mode row persists the new mode but only takes effect after
    // a restart - otherwise the header label flips while nothing actually
    // switches (a "changed mode, still dead" surprise). Mirror the console
    // MODE command: persist (done above) then reboot into the chosen mode.
    const bool wantOrch = (g_menu.mode() == Mode::Orchestrator);
    if (wantOrch != g_orchMode) {
      Serial.printf("MODE menu -> %d (reboot)\n", int(wantOrch));
      g_askOverride = wantOrch ? "Switching to Orchestrator mode..."
                               : "Switching to Notifier mode...";
      g_askTransient = true;                          // reboot follows: no Close button
      ::sfx::fire(nimbus::sfx::Ev::ModeSwitch);       // "Set the course." (plays under the sweep)
      renderScreen(attn::ScreenId::Ask, -1);        // panel confirmation screen
      playModeSwitchFeedback(wantOrch);             // LED sweep (+ time for the panel)
      ESP.restart();
    }
  }
  // Request-flag drains live OUTSIDE the dirty() gate: these clicks raise a
  // flag WITHOUT dirtying the config (asserted by the FSM tests), so gating
  // them on dirty() starved every request until an unrelated setting changed
  // (latent since the Connectivity rows landed; the new Software-update
  // requests would have inherited the same starvation).
  if (g_menu.forgetBondsRequested()) {
    // Forget paired devices (Connectivity): wipe all BLE bonds so every Mac
    // must re-pair (enter a fresh passkey). Applied live; no reboot.
    net::ble::forgetBonds();
    g_menu.clearForgetRequest();
    g_menuNeedsPaint = true;
  }
  if (g_menu.cloudPairRequested()) {
    // Cloud link code (Connectivity): initiate cumulo-nimbus pairing FROM the
    // device, mirroring the web "pair" action - opt in, then request a fresh
    // claim code. The FSM only raises this in Orchestrator mode (the relay does
    // not run in Notifier), so no mode guard is needed here. The rising-edge
    // Pairing-screen driver in loop() surfaces the claim code + QR once the
    // relay task produces it. (CUM-48 #4)
    nimbus::relay::requestOptIn(true);
    nimbus::relay::requestPair();
    g_menu.clearCloudPairRequest();
    g_menuNeedsPaint = true;
  }
  if (g_menu.sdProbeRequested()) {
    // Rescan SD card: force a real SD bus re-init (end()+begin()) so a card
    // re-seated after boot re-mounts + the memory tier promotes back to SD -
    // no reboot. promoteSd() write-probes before clearing the lost latch.
    bool sdUp = agent::memory::promoteSd();
    Serial.printf("SD re-probe (menu) -> %s\n", sdUp ? "mounted" : "still absent");
    if (sdUp) ::sfx::refreshConfig();   // re-count SD clip variants now the tier is back
    g_menu.clearSdProbeRequest();
    g_menu.setSdStatus(std::string(sdStatusLine().c_str()));  // refresh the Main row
    g_menuNeedsPaint = true;
  }
  if (g_menu.calibrateRequested()) {
    // Settings > Display > Calibrate touch: hand the screen to the on-device
    // tap-the-crosses flow, then restore the normal UI. Blocking + opt-in (CUM-189).
    g_menu.clearCalibrateRequest();
    g_menu.close();
    runTouchCalibration();
    g_menuNeedsPaint = false;   // the routine already restored the live screen
  }
  if (g_menu.powerOffRequested()) {
    // Settings > Power off: clean shutdown + deep sleep. The FSM already closed
    // the menu; this never returns (the chip sleeps). Opt-in and confirmed, so it
    // can never fire from a stray tap. (CUM-224)
    g_menu.clearPowerOffRequest();
    enterPowerOffSleep();
  }
  // ---- Connectivity > Wi-Fi: the on-device escape hatch --------------
  // These four flags are the ONLY thing that makes that submenu real. The
  // menu core raises them and waits; without this drain the rows latch a
  // bool nobody reads, the pickers stay empty, and the help text asserts
  // "No networks in range" on a device surrounded by them. That is the
  // inert-rails shape this codebase has now been bitten by twice - a UI
  // whose tests pass because the tests clear the flags themselves.
  //
  // Same contract as the siblings above: do the work, clear the flag, ask
  // for a repaint. Nothing here blocks - scanJson() is async (it starts a
  // scan and returns immediately), and the join is a begin(), not a wait.
  if (g_menu.publishApRequested()) {
    net::publishSetupNetwork();
    g_menu.clearPublishApRequest();
    g_menuNeedsPaint = true;
  }
  if (g_menu.wifiScanRequested()) {
    // Kick the scan and seed whatever is already available. The rows refresh
    // on the next pass as results land - scanJson() is non-blocking, so the
    // main loop never sits here.
    const String js = net::scanJson();
    std::vector<std::string> rows;
    JsonDocument d;
    if (!deserializeJson(d, js)) {
      // Build the picker rows via the pure, host-tested formatter (CUM-48): saved
      // networks first, then by signal; hidden APs collapse to a marker. Hidden APs
      // scan with an empty SSID, so they are carried through (not dropped here) for
      // buildScanRows to count and mark.
      std::vector<nimbus::wifi::ScanHit> scan;
      for (JsonVariantConst n : d["networks"].as<JsonArrayConst>()) {
        nimbus::wifi::ScanHit h;
        h.ssid   = std::string(n["ssid"] | "");
        h.rssi   = (int8_t)(int)(n["rssi"] | -127);
        h.locked = n["enc"] | true;
        scan.push_back(h);
      }
      std::vector<nimbus::wifi::KnownNet> known;
      nimbus::net::wifistore::all(known);
      rows = nimbus::wifi::buildScanRows(scan, known);
    }
    if (!rows.empty()) g_menu.setWifiScan(rows);
    if (!d["scanning"].as<bool>()) g_menu.clearWifiScanRequest();
    g_menuNeedsPaint = true;
  }
  if (g_menu.wifiJoinRequested()) {
    // Join a SAVED network by name - its password comes from the store and
    // is never shown, typed, or logged.
    const std::string picked = g_menu.wifiPickedSsid();
    nimbus::wifi::KnownNet kn;
    bool joined = false;
    for (int i = 0; i < nimbus::net::wifistore::count(); i++) {
      if (nimbus::net::wifistore::getAt(i, kn) && kn.ssid == picked) {
        net::saveAndConnect(String(kn.ssid.c_str()), String(kn.pass.c_str()));
        joined = true;
        break;
      }
    }
    if (!joined) Serial.printf("menu: no saved network '%s'\n", picked.c_str());
    g_menu.clearWifiJoinRequest();
    g_menuNeedsPaint = true;
  }
  if (g_menu.wifiForgetRequested()) {
    net::forgetNetwork(String(g_menu.wifiPickedSsid().c_str()));
    g_menu.clearWifiForgetRequest();
    g_menuNeedsPaint = true;
  }
  // Keep the saved-network picker in step with the store whenever the menu
  // is open, so "Forget network" never lists something already gone.
  if (g_menu.showingConnectivity()) {
    std::vector<std::string> known;
    nimbus::wifi::KnownNet kn;
    for (int i = 0; i < nimbus::net::wifistore::count(); i++)
      if (nimbus::net::wifistore::getAt(i, kn)) known.push_back(kn.ssid);
    g_menu.setWifiKnown(known);
  }

  if (g_menu.updateCheckRequested()) {
    // Software update > Check: kick the real OTA check; the ~1 Hz loop-body
    // reseed (see loop(), next to the menu-repaint flush) tracks it live.
    // requestCheck() REFUSES without ever entering Checking (busy / no-wifi /
    // low-heap / unsupported) - branch on it so the panel can't show a
    // perpetual, false "Checking..." (prism v3.1.0 finding).
    g_menu.clearUpdateCheckRequest();
    const char* why = "";
    if (otaupd::requestCheck(&why)) {
      g_menu.setUpdateStatus("Checking for updates...");
      g_updCheckKickMs = now;   // grace: don't let a stale state overwrite this
    } else {
      // Name the real cause (no-wifi/low-heap/unsupported/busy), not "the
      // network" - a refusal is always local (CUM-197).
      g_menu.setUpdateStatus(nimbus::ota::checkRefusalCopy(why));
    }
    g_menuNeedsPaint = true;
  }
  bool installRefusedAsk = false;  // Ask override set THIS pass (guard below)
  if (g_menu.updateInstallRequested()) {
    g_menu.clearUpdateInstallRequest();
    const char* why = nullptr;
    if (!otaupd::requestInstall(false, false, &why)) {
      // Refused (low heap / no Wi-Fi / already installing): reopen the
      // submenu path is overkill - surface the reason on the panel Ask,
      // STICKY (holds until a click) so the menu-close cleanup below can't
      // wipe it in this same event pass (prism v3.1.0 finding).
      g_askOverride = String("Couldn't start the update: ") +
                      (why ? why : "unknown") + "\nStill on " NIMBUS_FW_VERSION ".";
      g_askSticky = true; g_askPage = 0;
      installRefusedAsk = true;
      renderScreen(attn::ScreenId::Ask, -1);
    }
    // On success the menu is already closed and otaLoopUx() owns the screen.
  }
  if (g_menu.isOpen()) {
    g_menuNeedsPaint = true;  // flushed when the panel is free
  } else {
    // Menu just closed: re-resolve the ACTIVE profile (a battery T1 / VBUS
    // event may have changed forced/VBUS while the tick was suppressed), then
    // return to the normal status surface. Any held reply is DISMISSED here
    // (review: this direct StatusIdle render used to paint over a sticky reply
    // while leaving g_askSticky true - the scheduler gate then stayed shut
    // forever with plain status on the panel) - UNLESS this very pass just
    // posted the install-refusal Ask, which must survive until the owner
    // clicks it away.
    if (!installRefusedAsk) {
      g_askSticky = false; g_askPage = 0; g_askOverride = "";
    }
    g_cfg.setProfile(g_selector.resolve());
    applyConfig();
    g_menuNeedsPaint = false;
    refreshRing();
    if (!installRefusedAsk) renderScreen(attn::ScreenId::StatusIdle, -1);
  }
}

// Page a held Ask reply by `dir` (+1/-1) and repaint. Shared by the cursor
// (encoder rotate, panel) and touch (swipe, TFT) paths - each renderer has its
// Page count from the renderer's actual pixel geometry/font metrics, so the tail
// page is never stranded unreachable.
static void pageAskReply(int dir) {
  const std::string text(g_askOverride.c_str());
  const int pages = nimbus::tft::askPageCount(text);
  g_askPage = std::max(0, std::min(g_askPage + dir, pages > 0 ? pages - 1 : 0));
  renderScreen(attn::ScreenId::Ask, -1);
}

// ---- touch input (TFT boards) ------------------------------------------------
// Translate a tap into the SAME calls the encoder makes. Nothing downstream is
// touch-aware: the menu FSM keeps its onRotate/onClick/onLongPress contract, and
// the dirty-persist + request-flag drains in loop() run exactly as they do for
// the same gestures. That is the whole reason a second input device needed no FSM change.
static void drainTouch(uint32_t now) {
  const auto g = hw::touch::poll();
  if (g.kind == hw::touch::Gesture::Kind::None) return;

  // A finger on the panel is the owner being present - same signal a step
  // gives, so the screensaver clock resets before the event is handled.
  saverKick();

  const auto* t = hw::tft::hitTest(g.x, g.y);

  // Hold anywhere on the mic control is push-to-talk; releasing ends it. The
  // encoder expresses this as long-press, and both land on captureVoiceTurn().
  if (g.kind == hw::touch::Gesture::Kind::HoldStart) {
    if (t && t->action == nimbus::tft::TapRegion::Action::Mic && g_orchMode)
      captureVoiceTurn();
    return;
  }
  if (g.kind == hw::touch::Gesture::Kind::HoldEnd) return;   // recording self-terminates

  // Swipe = scroll the open menu, OR page a held Ask reply. With no knob, a
  // list longer than one page (or a reply longer than one screen) had no way
  // to move except the header pager, which is easy to miss (owner: "no easy
  // way to go up or down to see more options" - the same gap that left a long
  // orchestrator reply silently cut off with nothing to swipe with).
  //
  // Menu scroll moves a CHUNK rather than one row: at six rows a page,
  // single-stepping needs a swipe per row, which is worse than the problem
  // being solved. The FSM clamps at the ends, so over-scrolling is harmless.
  if (g.kind == hw::touch::Gesture::Kind::SwipeUp ||
      g.kind == hw::touch::Gesture::Kind::SwipeDown) {
    const int dir = (g.kind == hw::touch::Gesture::Kind::SwipeDown) ? +1 : -1;
    if (g_menu.isOpen()) {
      constexpr int kSwipeRows = 5;
      {
        net::ConfigLockGuard lk;
        for (int i = 0; i < kSwipeRows; i++) g_menu.onRotate(dir);
      }
      settleMenuAfterMutation(now);
    } else if (g_askSticky) {
      pageAskReply(dir);
    }
    return;
  }
  if (g.kind != hw::touch::Gesture::Kind::Tap) return;
  if (!t) return;                                            // tapped dead space

  using Action = nimbus::tft::TapRegion::Action;
  if (g_menu.isOpen()) {
    // Menu open: every gesture is a menu gesture, under the same config lock the
    // encoder path takes. The mapping itself is PORTABLE (nimbus::tft::
    // applyMenuTap) so it is host-tested against the real FSM - with no knob,
    // an off-by-one here activates a setting the owner did not touch, and that
    // is not something hardware-only tests should be the first to notice.
    // The lock covers the FSM mutation ONLY - settleMenuAfterMutation does NVS
    // writes and renders and re-acquires the lock itself, exactly as the
    // encoder path does (it releases the guard before settling).
    bool acted = false;
    {
      net::ConfigLockGuard lk;
      acted = nimbus::tft::applyMenuTap(g_menu, *t);
    }
    // ⚠ Must call the SAME settle the gesture path calls. Setting g_menuNeedsPaint alone
    // repaints the screen while the change never persists or applies - the menu
    // would look like it worked and silently revert on reboot.
    if (acted) settleMenuAfterMutation(now);
    return;
  }

  // Menu closed.
  switch (t->action) {
    case Action::OpenMenu:
      openSettingsMenu();
      break;
    case Action::SessionCard:
      // Focus that session - the touch equivalent of rotating the cursor onto
      // it, so the ring highlights the same one.
      //
      // ⚠ Notifier ONLY. t->index indexes ctx.jobs, which is the attention-
      // router snapshot; in Orchestrator mode the cursor indexes a DIFFERENT
      // collection (0 = the Orchestrator root, 1..N = g_sessionList) and
      // buildCtx additionally drops the head job from ctx.jobs, so the two are
      // neither the same set nor the same origin. Mapping one onto the other
      // focuses the wrong session, which is worse than focusing none - so in
      // Orchestrator mode a card tap just opens the detail screen for whatever
      // the cursor already points at.
      // TODO: give ctx.jobs a stable focus id so a card can name its session
      // directly in both modes.
      if (!g_orchMode) {
        g_cursor.setIndex(t->index, jobCount() > 0 ? jobCount() : 1, now);
        g_sched.onDetent(uint8_t(attn::ScreenId::JobDetail), now);
      } else {
        g_sched.onDetent(uint8_t(attn::ScreenId::SessionDetail), now);
      }
      refreshRing();
      break;
    case Action::Back:
    case Action::Home:
      // Same as a single click: clear any override and show live status.
      g_lightsOff = false;
      g_ledOverrideActive = false;
      g_askOverride = "";
      refreshRing();
      renderScreen(attn::ScreenId::StatusIdle, -1);
      break;
    case Action::Mic:
      break;   // a bare tap on the mic is not a hold; nothing to do
    default:
      break;
  }
}

// Pin the portable identity-key list to the frozen AKEY_* machine keys, so a
// rename on one side fails the build instead of silently un-preserving a key.
namespace {
constexpr bool cstreq(const char* a, const char* b) {
  return *a == *b && (*a == '\0' ? true : cstreq(a + 1, b + 1));
}
static_assert(cstreq(nimbus::config::kIdentityStrKeys[0], AKEY_SCREEN_MODEL), "identity drift: scrModel");
static_assert(cstreq(nimbus::config::kIdentityStrKeys[1], AKEY_TOUCH_CAL), "identity drift: tchCal");
static_assert(cstreq(nimbus::config::kIdentityStrKeys[2], AKEY_OTA_TYPE), "identity drift: otaType");
static_assert(cstreq(nimbus::config::kIdentityIntKeyTftFlip, AKEY_TFT_FLIP), "identity drift: tftFlip");
}  // namespace

// Factory reset that KEEPS ONLY the board's PHYSICAL identity. A bare nvs_flash_erase()
// wipes scrModel/tftFlip/tchCal/otaType too, so a TFT board reboots into the panel
// default driver and shows a white screen it cannot correct from its own controls
// (CUM-50). Everything the owner set - Wi-Fi, keys, token, bonds, config, AND the
// user-visible device name - is scrapped (CUM-230): a reset unit comes back as a
// clean, re-onboardable device with a fresh auto-generated name + mDNS, never carrying
// its pre-reset identity. Capture the hardware-identity keys via raw NVS, erase every
// namespace, re-init, then write them back plus a seeded operating mode so the next
// boot is clean onboarding on the RIGHT panel, in the Wi-Fi setup posture.
static void factoryResetPreserveIdentity() {
  static const char* kNs = "solide";
  constexpr int kN = nimbus::config::kIdentityStrKeyCount;
  // The mode key is boot-authoritative (sys::loadMode reads "nimbus_mode"). Seeding it
  // to Orchestrator brings the Wi-Fi setup AP up so the onboarding wizard is reachable;
  // a merely-cleared key defaults to Notifier (radio off, no AP) - the CUM-230 bug.
  static const char* kModeKey = "nimbus_mode";
  static_assert(nimbus::orch::kFactoryResetSeedMode == (int)sys::Mode::Orchestrator,
                "factory-reset seed mode must be Orchestrator (the setup-AP posture)");
  char strv[kN][64];
  bool strPresent[kN] = {false};
  int32_t flip = 0;
  bool flipPresent = false;
  nvs_handle_t h;
  if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
    for (int i = 0; i < kN; i++) {
      size_t len = sizeof strv[i];
      strPresent[i] = nvs_get_str(h, nimbus::config::kIdentityStrKeys[i], strv[i], &len) == ESP_OK;
    }
    flipPresent = nvs_get_i32(h, nimbus::config::kIdentityIntKeyTftFlip, &flip) == ESP_OK;
    nvs_close(h);
  }
  // Deinit before erase (CUM-15 hardware finding): solide::memory left NVS initialized
  // at boot, and its Preferences handle is stale after the wipe while its begin() is
  // idempotent (will not reopen). Without the deinit, nvs_flash_init() below is a no-op
  // over erased pages and the write-back can be lost. deinit -> erase -> init, then
  // restore through fresh raw nvs_open handles (read-compatible with Preferences on
  // the next boot). Best-effort throughout: a failure just means the board auto-names.
  nvs_flash_deinit();
  nvs_flash_erase();   // wipe every namespace
  nvs_flash_init();    // re-mount the now-empty partition
  if (nvs_open(kNs, NVS_READWRITE, &h) == ESP_OK) {
    for (int i = 0; i < kN; i++)
      if (strPresent[i]) nvs_set_str(h, nimbus::config::kIdentityStrKeys[i], strv[i]);
    if (flipPresent) nvs_set_i32(h, nimbus::config::kIdentityIntKeyTftFlip, flip);
    nvs_set_i32(h, kModeKey, nimbus::orch::kFactoryResetSeedMode);   // CUM-230: land in setup
    nvs_commit(h);
    nvs_close(h);
  }
}

void loop() {
  esp_task_wdt_reset();
#ifdef NIMBUS_NOTIFIER_DEBUG
  // CUM-185: steady-state contiguous-SRAM sampler (test/debug builds only, so the
  // production nsn serial stream stays clean). Emits the same line as the boot
  // snaps every ~20 s, so the Orchestrator-mode steady-state intLargest floor is
  // captured on Serial even while the web server is unreachable. /api/state gives
  // the same numbers over HTTP once it binds; this is the pre-bind / soak backup.
  {
    static uint32_t s_nextSramMs = 8000;   // first sample ~8 s after boot
    if ((int32_t)(millis() - s_nextSramMs) >= 0) {
      sramSnap(g_orchMode ? "steady-orch" : "steady-notif");
      s_nextSramMs = millis() + 20000;
    }
  }
#endif
#ifdef NIMBUS_TEST
  if (g_orchMode && agent::orchestrator::testRebootRequested()) {
    agent::alog("test: staged restart (HIL persistence proof)");
    Serial.flush();
    delay(80);
    ESP.restart();
  }
#endif  // feed the F12 watchdog every iteration
  otaupd::tick();        // OTA: mark-valid once healthy + check/auto-install cadence
  otaLoopUx();           // OTA install panel/ring UX (no-op unless installing)
  if (g_powerOffPending) {
    // Web "Power off" (CUM-224): the AsyncTCP task can't sleep the chip inline
    // (mode rule) so it set this flag; enter the shared clean-shutdown + deep-sleep
    // path here on the main task. Never returns.
    g_powerOffPending = false;
    enterPowerOffSleep();
  }
  if (g_factoryResetPending) {  // web factory reset: scrap everything, keep only the panel identity
    // CUM-50 + CUM-230: factoryResetPreserveIdentity() keeps ONLY the hardware identity
    // (scrModel/tftFlip/tchCal/otaType, so a TFT board comes back on the right driver,
    // never a white screen) and seeds the setup mode so the reset device boots into the
    // Wi-Fi onboarding wizard. Everything the owner set - keys/token/bonds/config AND the
    // device name - is scrapped; the unit re-onboards fresh with a new name + mDNS.
    // g_factoryEraseSd additionally wipes the durable /mem store in the same flow.
    Serial.println(g_factoryEraseSd ? "FACTORY RESET (+SD) -> keep identity, erase config + /mem, restart"
                                    : "FACTORY RESET -> keep identity, erase config, restart");
    // Progress screen with an honest duration; also buys the panel ~2.2 s to paint
    // before the erase blocks. The screen setup is preserved, so this is safe on TFT.
    g_askOverride = g_factoryEraseSd ? "Resetting to factory settings and erasing storage. Up to a minute."
                                     : "Resetting to factory settings. This takes a few seconds.";
    renderScreen(attn::ScreenId::Ask, -1);
    Serial.flush();
    if (g_factoryEraseSd) agent::memory::eraseDurableStore();   // CUM-15: optional combined /mem erase
    factoryResetPreserveIdentity();   // wipes NVS, restores hardware identity + device name
    delay(50);
    ESP.restart();
  }
  if (g_sdResetPending) {  // web SD reset: erase the durable /mem store, keep config, reboot
    g_sdResetPending = false;             // consume the request either way (no re-fire)
    Serial.println("SD RESET -> erase durable store + restart");
    g_askOverride = "Erasing storage...";
    renderScreen(attn::ScreenId::Ask, -1);
    Serial.flush();
    if (agent::memory::eraseDurableStore()) {  // /mem (or /data): vectors, episodic, files, blobs
      delay(50);
      ESP.restart();                      // engines reload empty; NVS config untouched
    } else {
      // Refused: the card that held the store is gone. Say so honestly and DON'T
      // reboot into a device that reads "erased" while the data survives on the card.
      Serial.println("SD RESET refused -> storage not available");
      g_askOverride = "Couldn't erase - storage not available";
      renderScreen(attn::ScreenId::Ask, -1);
    }
  }
  if (g_sdFormatPending) {  // web full-card format: reformat the whole SD, then reboot
    g_sdFormatPending = false;            // consume the request either way (no re-fire)
    Serial.println("SD FORMAT -> reformat whole card + restart");
    g_askOverride = "Formatting storage. Up to a minute.";
    renderScreen(attn::ScreenId::Ask, -1);
    Serial.flush();
    // Reformat the ENTIRE card (not just /mem). The driver refuses cleanly with no
    // side effects when no card is mounted, so a missing card yields an honest
    // message and NO reboot - never a device that reads "formatted" over surviving
    // data. On success everything on the card is gone; reboot so the memory engines
    // reload empty (NVS config is untouched).
    using FR = solide::storage::FormatResult;
    const FR fr = solide::storage::format();
    // Ok AND RemountFailed both mean the card is already erased (mkfs succeeded),
    // so reboot either way - boot re-probes the card and the memory engines reload
    // empty; only the pre-reboot message differs. NoCard and MkfsFailed left the
    // card's data as it was, so report honestly and do NOT reboot (never claim an
    // erase that did not happen).
    if (fr == FR::Ok || fr == FR::RemountFailed) {
      g_askOverride = fr == FR::Ok ? "Storage formatted. Restarting."
                                   : "Formatted; remount failed. Restarting.";
      Serial.printf("SD FORMAT -> %s; restarting\n", solide::storage::formatResultStr(fr));
      renderScreen(attn::ScreenId::Ask, -1);
      Serial.flush();
      delay(50);
      ESP.restart();
    } else {
      const char* msg =
        fr == FR::NoCard ? "Couldn't format - storage not available" : "Couldn't format - card error";
      Serial.printf("SD FORMAT refused/failed -> %s\n", solide::storage::formatResultStr(fr));
      g_askOverride = msg;
      renderScreen(attn::ScreenId::Ask, -1);
    }
  }
  if (g_rebootPending) {  // web mode change / orchestrator reboot action
    Serial.println("REBOOT pending -> restart");
    if (g_modeSwitchTo >= 0) {
      // A WEB mode switch: give the same confirmation UX as the on-device menu path
      // (was an abrupt ~50 ms reboot with no screen/LED/sound). panel "Switching
      // to X...", the ModeSwitch sound, and the teal/amber LED sweep (which also
      // buys the ~2.2 s the panel needs to actually paint before the reset).
      const bool toOrch = (g_modeSwitchTo == 1);
      agent::memory::captureEvent("mode", String("Switching to ") +
          (toOrch ? "Orchestrator" : "Notifier") + " mode (restart follows)");
      g_askOverride = toOrch ? "Switching to Orchestrator mode..."
                             : "Switching to Notifier mode...";
      g_askTransient = true;                          // reboot follows: no Close button
      ::sfx::fire(nimbus::sfx::Ev::ModeSwitch);
      renderScreen(attn::ScreenId::Ask, -1);
      playModeSwitchFeedback(toOrch);
    }
    Serial.flush();
    delay(50);
    ESP.restart();
  }
  const uint32_t now = millis();

  // LED safety cap: 60% unless the owner/AI accepted the heat risk. Refreshed
  // every iteration (one volatile write) so a config change can't be missed.
  {
    const bool ovr = agent::store::brightOvr();
    hw::setBrightnessCap(ovr ? 255 : nimbus::power::kBrightCap);
    // Revoking the override must LOWER a live load, not just future ones (review:
    // an armed 255-bright drain otherwise burns at 255 for hours after the owner
    // turned the override off). Falling edge: re-clamp the drain in place and
    // re-apply a live model led-override through its (now-clamping) apply path.
    static bool s_prevOvr = false;
    if (s_prevOvr && !ovr) {
      if (g_highLoadActive) {
        const uint8_t want = nimbus::power::clampBright(g_drainBright, false);
        if (want != g_drainBright) {
          g_drainBright = want;
          if (!g_hlSettling && !g_thermal.tripped()) applyHighLoadRing(hlBright());
          agent::alogf("bright: override revoked mid-drain - re-clamped to %u", want);
        }
      }
      if (g_ledOverrideActive) g_devActDirty = true;   // re-apply clamped
    }
    s_prevOvr = ovr;
  }

  // --- Ring attention watchdog (fallback) ----------------------------------
  // No attention arc (red Error / CTA) may ever strand forever. The happy-path
  // reap runs on the tg_poll / broker task (orchestrator scheduleReap, notifier
  // Mapper::timeout); if THAT task is OOM-killed mid-TLS or a clearing Offline
  // is dropped, a red arc would otherwise stick for hours (observed). This runs
  // on the watchdog-fed MAIN loop - which cannot stall - and ages out any
  // attention source past AttnHoldMs + a grace, guaranteeing recovery regardless
  // of the reap path's health. Cheap (a snapshot); throttled to ~5 s.
  static uint32_t s_attnSweepAt = 0;
  if ((int32_t)(now - s_attnSweepAt) >= 0) {
    s_attnSweepAt = now + 5000;
    const uint32_t cap = uint32_t(g_cfg.effective(Param::AttnHoldMs)) + 60000;
    bool cleared, doneCleared;
    {
      net::ConfigLockGuard lk;
      cleared = g_router.forceExpireAttention(now, cap);
      // CUM-221 (the recurring stuck-ring class): also collapse a terminal Done
      // ember whose tg_poll reap (JobEngine::reapDone) never fired. Done is not an
      // attention status, so forceExpireAttention skips it - it was the one arc
      // with no always-alive backstop, which is exactly what strands lit after a
      // Telegram answer when the poll task stalls. Same generous cap.
      doneCleared = g_router.forceExpireDoneArcs(now, cap);
    }
    if (cleared) {
      agent::alogf("ring: attention watchdog force-expired a stuck arc (>%us) - reap path stalled",
                   (unsigned)(cap / 1000));
      if (g_orchMode) agent::orchestrator::noteRingBackstopFired();   // CUM-11 metric
    }
    if (doneCleared) {
      agent::alogf("ring: watchdog collapsed a stranded Done arc (>%us) - tg_poll reap stalled",
                   (unsigned)(cap / 1000));
      if (g_orchMode) agent::orchestrator::noteRingBackstopFired();   // CUM-11/CUM-221 metric
    }
    if ((cleared || doneCleared) && !g_menu.isOpen() && !g_voiceWaiting) refreshRing();
    // Head-turn reaper: the blue "processing" Running arc is NOT an attention status,
    // so forceExpireAttention above skips it. A Telegram turn whose task died leaves it
    // pulsing forever (owner bug). Free it here on the main loop.
    if (g_orchMode && agent::orchestrator::reapStuckTurn(now)) {
      agent::alog("ring: stuck-turn reaper freed a dead head-turn arc (blue processing ring)");
      g_orchRingDirty = true;
      if (!g_menu.isOpen() && !g_voiceWaiting) refreshRing();
    }
    // W6: keep the orchestrator's own head arc lit while its sub-agents run (a
    // fan-out returns the instant it enqueues children, so the TurnGuard would
    // otherwise collapse the arc before the child arcs appear). This lights /
    // periodically refreshes / clears the arc from the always-alive main loop.
    if (g_orchMode && agent::orchestrator::reconcileHeadArc(now)) {
      g_orchRingDirty = true;
      if (!g_menu.isOpen() && !g_voiceWaiting) refreshRing();
    }
    // Absolute working-breathe ceiling (belt-and-suspenders over reapStuckTurn):
    // if turnInFlight has been continuously true past kWorkingHardMaxMs, latch the
    // ceiling so compose() drops the "thinking" breathe - the ring can't breathe
    // "working" for hours even if the reaper is wedged. Reset the instant the flag
    // clears (a clean turn-end or the reaper), so a normal turn is never affected.
    if (g_orchMode) {
      constexpr uint32_t kWorkingHardMaxMs = 30u * 60 * 1000;   // 30 min, well above the reaper
      static uint32_t s_workingSinceMs = 0;
      const bool tif = agent::orchestrator::turnInFlight();
      if (!tif) {
        if (s_workingSinceMs != 0 || g_workingCeilingHit) { s_workingSinceMs = 0; g_workingCeilingHit = false; }
      } else {
        if (s_workingSinceMs == 0) s_workingSinceMs = now;
        if (!g_workingCeilingHit && (int32_t)(now - s_workingSinceMs) >= (int32_t)kWorkingHardMaxMs) {
          g_workingCeilingHit = true;
          agent::orchestrator::noteRingBackstopFired();   // CUM-11 metric
          agent::alog("ring: working-breathe hit its 30 min absolute ceiling - dropped (stuck-turn reaper stalled?)");
          if (!g_menu.isOpen() && !g_voiceWaiting) refreshRing();
        }
      }
    }
  }

  // W3b active capability validation: when the owner opts into "active", refresh
  // one provider's key-verification on a slow cadence so the catalog's VERIFIED
  // marking stays honest (catches a revoked key / changed access). This reuses
  // provider_verify::request() - the SAME watchdog-safe, self-deleting, TLS-
  // arbited task the web "Verify" button uses (no new concurrency), enqueues
  // (non-blocking) one provider per wake, round-robin, spread so each keyed
  // provider refreshes about every capProbeHours. Orchestrator-only (verify needs
  // the TLS heap); a reboot resets the RAM timer (re-probes soon, harmless).
  if (g_orchMode && agent::store::capProbe() == 2 &&
      !agent::provider_verify::pending() &&
      // Never open the probe's TLS beside a live turn: with tlsSlots=2 the
      // second slot exists for VOICE beside a turn, not for an unattended probe
      // (the 78dab2c concurrent-TLS heap-collapse class). With slots=1 the
      // arbiter serializes anyway; this keeps the probe out of the queue too.
      !agent::orchestrator::turnInFlight() &&
      WiFi.status() == WL_CONNECTED) {   // a probe with no LAN only burns a -1
    // now-relative init (not a constant): a bare `= 30000` went dead for ~25
    // days once uptime crossed the int32 wrap horizon.
    static uint32_t s_nextCapProbeAt = 0;
    static bool s_capProbeInit = false;
    if (!s_capProbeInit) { s_capProbeInit = true; s_nextCapProbeAt = now + 30000; }
    if ((int32_t)(now - s_nextCapProbeAt) >= 0) {
      static int rr = 0;
      const char* provs[3] = {"openai", "anthropic", "mistral"};
      bool keyed[3] = {agent::store::hasOpenaiKey(), agent::store::hasAnthropicKey(),
                       agent::store::hasMistralKey()};
      int keyedCount = (keyed[0] ? 1 : 0) + (keyed[1] ? 1 : 0) + (keyed[2] ? 1 : 0);
      for (int i = 0; i < 3; i++) {
        int idx = (rr + i) % 3;
        if (keyed[idx]) { agent::provider_verify::request(provs[idx]); rr = idx + 1; break; }
      }
      // Spread the interval so each keyed provider is refreshed ~every capProbeHours.
      uint32_t stepMs = (uint32_t)agent::store::capProbeHours() * 3600000u /
                        (uint32_t)(keyedCount > 0 ? keyedCount : 1);
      if (stepMs < 60000) stepMs = 60000;       // never hammer faster than 1/min
      s_nextCapProbeAt = now + stepMs;
    }
  }

  // Auto-revert a POST /api/preview once its window elapses: nothing else
  // necessarily calls refreshRing() again on its own, so this is the one place
  // that proactively drops back to the live composed state.
  if (g_previewActive && int32_t(now - g_previewUntil) >= 0) {
    g_previewActive = false;
    if (!g_menu.isOpen()) refreshRing();
  }

  // Pump the captive DNS / mDNS and apply any staged web config on the main task.
  // Orchestrator-only: in Notifier the radio (and the web server) never came up.
  if (g_orchMode) {
    net::process();
    net::loopWeb();
  }
#ifdef NIMBUS_TEST
  // ── HOST DEAD-MAN CHECK (battlab) ─────────────────────────────────────────
  // A host-driven campaign drain must be refreshed by its host. If the host dies,
  // restarts, sleeps, or the network drops, NOTHING else stops the ~1.5 A load and
  // the pack runs FLAT (killed a pack live, 2026-07-16). This is the ONLY thing
  // that fails the load safe on its own.
  // Placement is deliberate and load-bearing:
  //   • 2-space indent = mode-agnostic. The thermal poll AND the settle machine are
  //     trapped inside `if (g_orchMode)` below, so in Notifier mode this is the ONLY
  //     watcher of the load. It must never join them in there.
  //   • immediately after loopWeb() - that is what drains a just-arrived keepalive
  //     into drainSet(), so a refresh landing this pass is honoured this pass.
  //   • keys ONLY on g_highLoadActive + the ttl. NOT on ring brightness: the settle
  //     machine legitimately darkens the ring for 9 s every 15 min, and hlBright()
  //     returns 0 while thermally tripped - dark != off.
  //   • g_storageTargetMv == 0 gates it to a campaign drain; STORAGE is a production
  //     op with no host and its own voltage auto-stop.
  //   • unsigned-difference compare = millis()-rollover correct.
  if (g_highLoadActive && g_drainTtlMs && g_storageTargetMv == 0 &&
      uint32_t(millis() - g_drainRefreshedMs) >= g_drainTtlMs) {
    const unsigned aliveS = unsigned(g_drainTtlMs / 1000u);
    drainSet(false, false);              // full stop: ring released, guard disarmed
    agent::alogf("drain: HOST DEAD-MAN expired (no refresh for %us) - load OFF, "
                 "pack protected", aliveS);
  }
#endif
  // A web theme switch asks for an immediate ring re-compose so the new colour
  // shows on the device instantly (not only at the next ring event).
  if (net::consumeRingRefresh() && !g_menu.isOpen()) refreshRing();

  // P3: subtle LED confirmation for web actions (WiFi saved / scan done) - a
  // short theme-accent soft-pulse window, then restore the composed ring. (Was
  // Pattern::Flash - 3 hard full-ring ON/OFF snaps; ambient grammar, owner
  // 2026-07-16: even one-shot confirms swell, they don't strobe.) Respects
  // every LED silence: never in Dark posture, lights-off, or over a model-
  // painted ring, and never while the menu owns the interaction.
  if (net::consumeLedConfirm() && !g_menu.isOpen() && !g_lightsOff &&
      !g_ledOverrideActive && g_cfg.posture() != Posture::Dark) {
    nimbus::ThemeColor th =
        nimbus::themeAccent(std::string(agent::store::theme().c_str()));
    solide::leds::clearFrame();   // exit any raw frame so the pattern shows
    solide::leds::show(solide::leds::Pattern::Pulse, th.r, th.g, th.b);
    g_ledConfirmUntilMs = now + 1200;
  }
  if (g_ledConfirmUntilMs != 0 && int32_t(now - g_ledConfirmUntilMs) >= 0) {
    g_ledConfirmUntilMs = 0;
    if (!g_menu.isOpen()) refreshRing();   // menu/override may have taken the ring mid-window
  }

  // P3: repeated web-auth failures (3 x 401 / 60 s) - someone is on a token-less
  // config page, so surface the Config QR on the panel unprompted (buildCtx
  // carries configUrl on every render since P2). Foreground menu wins. Held
  // TRANSIENTLY (kAuthQrHoldMs) so a bad-token poller can't park the panel here.
  if (net::consumeAuthQrRequest() && !g_menu.isOpen() &&
      g_lastScreen != uint8_t(attn::ScreenId::Screensaver)) {
    // (The request is consumed either way; we just don't clobber a deep-idle
    // logo with a QR meant for a user who is plausibly present at live status.)
    renderScreen(attn::ScreenId::ConfigQr, -1);
    g_authQrUntilMs = now + kAuthQrHoldMs;
  }
  // Auth-QR hold elapsed: fall back to ambient status (unless the menu is now
  // deliberately showing the QR, or a later screen already replaced it) so the
  // panel returns to idle and the screensaver can arm.
  if (g_authQrUntilMs && int32_t(now - g_authQrUntilMs) >= 0) {
    g_authQrUntilMs = 0;
    if (!g_menu.isOpen() && g_lastScreen == uint8_t(attn::ScreenId::ConfigQr))
      renderScreen(attn::ScreenId::StatusIdle, -1);
  }

  // Battery: sample + two-threshold policy at a low cadence. Inert with
  // NullMonitor (no hardware). T1 raises a low-battery badge through the SAME
  // attention Router as job attention and forces the Battery Saver profile;
  // recovery/VBUS re-resolves the profile; T2 persists + clean deep-sleeps
  // (waking on VBUS when the sense pin exists).
  if (uint32_t(now - g_lastPowerTick) >= 2000) {
    g_lastPowerTick = now;
    // Owner control over the T1 battery-mode switch, re-synced every tick (NOT
    // inside the telemetry gate below, which fires only every 60-300 s - a toggle
    // must not appear inert for minutes).
    g_power.setAutoSaverOnLow(agent::store::lowBattSaver());
    power::ManagerActions pa = g_power.tick(now);
    // Re-resolve the ACTIVE profile (forced > VBUS > user) into g_cfg so the
    // presets take effect live. The user's authoritative pick lives in the
    // selector (g_selector.user()) and is what persistConfig() stores - this
    // only ever mutates the transient active profile, never the stored pick.
    // Suppressed while the menu is open: there g_cfg.profile() holds the pick the
    // user is editing (seeded from g_selector.user() on open), and clobbering it
    // with resolve() would corrupt the in-progress edit. The active profile is
    // re-resolved when the menu closes.
    // ⚠ LEVEL-triggered, not edge-triggered. pa.profileChanged is the OR of four
    // EDGES (enter/exit T1, VBUS connect/disconnect), so gating on it meant that
    // toggling "save power when low" while a T1 warning was ALREADY latched did
    // nothing until the next edge - possibly hours, and indistinguishable from a
    // broken setting. Comparing against resolve() is idempotent and subsumes the
    // edge form: outside the menu, g_cfg.profile() == g_selector.resolve() always
    // holds (boot seeds it and every mutation path re-resolves).
    if (!g_menu.isOpen() && g_cfg.profile() != g_selector.resolve()) {
      g_cfg.setProfile(g_selector.resolve());  // forced > VBUS > user
      applyConfig();                            // live; transient, not persisted
    }
    if (pa.warnT1 || pa.clearedT1) {
      attn::Event be;
      be.type = pa.warnT1 ? attn::Event::Type::LowBattery
                          : attn::Event::Type::BatteryOk;
      be.value = g_power.last().percent;
      attn::Decision d;
      { net::ConfigLockGuard lk; d = g_router.route(be, now); }
      if (!g_menu.isOpen()) {
        refreshRing();
        if (d.screen.render) g_sched.onIntent(uint8_t(d.screen.id), d.screen.attention, now);
      }
      // The ping itself fires below, AFTER the telemetry block has refreshed
      // g_battEstimate - the message reports the calibrated scale, and on the
      // first tick both warnT1 and telemetryDue are due in the same pass.
    }
    if (pa.telemetryDue) {
      // Feed the analytics model + snapshot the estimate for the web layer. Persist
      // the learned state only when a discharge segment completed (NVS wear) - the
      // learning that must survive a reboot.
      // ⚠ Pass the drain/storage harness state: a synthetic LED load (up to ~1.5 A vs
      // the device's normal idle draw) must NEVER teach the rate EWMA or the health
      // baselines - those describe NORMAL use and persist to NVS. Without this a curve
      // run leaves the device predicting hours-from-full forever (live-caught on Board 2
      // after the 5.75 h campaign: ~17 %/hr learned => ~4-6 h projected from a full pack).
      const bool artificialLoad = g_highLoadActive || g_storageTargetMv != 0;
      g_battModel.update(now, g_power.last(), artificialLoad);
      g_battEstimate = g_battModel.estimate();
      // Persist when a discharge segment completed OR the full-charge anchor moved
      // (an owner BATTCAL/api-battcal or an auto-learned plateau) - else an auto-
      // learned anchor never closing a segment is lost on reboot and two identical
      // boards drift apart in displayed % (one calibrated, one not).
      if (g_battEstimate.segments != g_battSavedSegments ||
          g_battModel.fullAnchorMv() != g_battSavedAnchor)
        saveBattModel();
      // Durable SD discharge log (only while actually draining - bounds growth +
      // is the data the model learns from). No-op with no card / while demoted.
      if (g_battEstimate.chargeState == nimbus::power::ChargeState::Discharging &&
          g_power.last().valid)
        agent::memory::appendBatteryHistory(agent::memory::nowHours(),
                                            g_power.last().millivolts, g_battEstimate.percent,
                                            nimbus::power::chargeStateStr(g_battEstimate.chargeState));
      // Only repaint StatusIdle if it's the LIVE screen - else the telemetry tick
      // yanks the user off a JobDetail / SessionDetail they're reading (same guard
      // the WiFi/BT header-refresh path below already uses; this one was missing it).
      if (!g_menu.isOpen() && g_lastScreen == uint8_t(attn::ScreenId::StatusIdle))
        g_sched.onIntent(uint8_t(attn::ScreenId::StatusIdle), false, now);
    }
    // Low-battery owner ping, gated by the AlertGate: fires at <=20% CALIBRATED
    // charge while discharging (owner 2026-08-12 - NOT the raw T1 edge, which
    // lands around ~45% real), at most one per cooldown window even across the
    // T2 sleep's 5-minute wake-sniff boots (the persisted epoch survives them).
    // External power re-arms. Runs on the telemetry tick, right after the block
    // above refreshed g_battEstimate.
    if (g_battEstimate.onExternalPower) g_lowBattGate.noteExternalPower();
    if (pa.telemetryDue && g_orchMode && g_cfg.effective(Param::TgLowBattPing) &&
        g_lowBattGate.shouldPing(
            g_battEstimate.percent,
            g_battEstimate.chargeState == nimbus::power::ChargeState::Discharging,
            (uint32_t)time(nullptr)))
      lowBatteryPing();
    if (g_lowBattGate.persistEpoch() != g_lowBattSavedPingEp) {
      g_lowBattSavedPingEp = g_lowBattGate.persistEpoch();
      agent::store::setLowBattPingEpoch(g_lowBattSavedPingEp);
    }
    // A DEEP drain deliberately suppresses the clean T2 shutdown so the pack runs to the
    // real cutoff (the campaign wants the full curve). Only reachable via the TEST-only
    // DRAIN command; STORAGE (g_drainDeep=false) + production always honor T2.
    // Live-sync the runtime-configurable protection each power tick, so a web/AI
    // change to sleepMv/sleepOvr applies within one tick with no extra plumbing.
    if (pa.telemetryDue) {
      g_power.policyRef().setT2PackMv(agent::store::sleepMv());
      if (g_power.policyRef().t2Override() != agent::store::sleepOvr())
        g_power.policyRef().setT2Override(agent::store::sleepOvr());
    }
    // ⚠ the grace-pending gate is load-bearing (review, CRITICAL): deep sleep wipes
    // the policy's RAM state, so after a wake the debounce re-fires ~8 s into the
    // boot - long before the grace window's own check ever ran. Without this gate
    // the "90 s to react" was dead code and every timer/charger wake re-slept in ~10 s.
    const bool gracePending = s_wokeFromLowBatt && s_lowBattGraceUntil != 0;
    if (pa.shutdownT2 && !gracePending &&
        !(g_highLoadActive && g_drainDeep) && !agent::store::sleepOvr()) {
      agent::alogf("power: pack at/below %umV (debounced) - deep sleep "
                   "(charge to wake)", unsigned(agent::store::sleepMv()));
      enterLowBattSleep();
    }
  }

  // A low-battery WAKE gets a grace window (glance at the screen, plug a charger,
  // set the override via web/AI), then decides via the WAKE BAR - not the sleep
  // threshold. A drained pack RESTS UPWARD (measured: 6918-6992 mV after hitting
  // the 6000 floor), so "above sleepMv?" oscillates sleep->rest->wake->drain->
  // sleep forever (owner-observed on Board 2). stayAwakeAfterSleep() demands
  // wakeMv (default 7200, above any rested-empty reading) or a positive charging
  // inference; the reading is taken FRESH (g_power.last() can be minutes stale
  // at battery-saver telemetry cadence).
  if (s_wokeFromLowBatt && s_lowBattGraceUntil && now > s_lowBattGraceUntil) {
    s_lowBattGraceUntil = 0;
    const power::Sample fresh = g_monitor ? g_monitor->sample() : power::Sample{};
    const bool charging = fresh.onExternalPower ||
                          g_battEstimate.chargeState == power::ChargeState::Charging ||
                          g_battEstimate.chargeState == power::ChargeState::Full;
    const bool stay = power::stayAwakeAfterSleep(
        fresh.valid ? fresh.millivolts : 0, agent::store::sleepMv(),
        agent::store::wakeMv(), charging);
    if (!stay && !agent::store::sleepOvr() && !(g_highLoadActive && g_drainDeep)) {
      agent::alogf("power: %umV after the wake grace < wake bar %umV - back to sleep",
                   unsigned(fresh.millivolts), unsigned(agent::store::wakeMv()));
      enterLowBattSleep();   // noreturn
    }
    if (stay)
      agent::alogf("power: recovered (%umV >= %umV%s) - staying awake",
                   unsigned(fresh.millivolts), unsigned(agent::store::wakeMv()),
                   charging ? " / charging" : "");
    else
      agent::alogf("power: below the wake bar but override/drain active - staying awake");
    g_power.policyRef().clearT2();   // drop any latched decision from stale state
    s_wokeFromLowBatt = false;
  }

  // SD graceful degradation: an adaptive liveness probe demotes the memory tier
  // when a card that was present at boot vanishes (cold joint / pull) and promotes
  // it back on recovery - all WITHOUT a reboot (episodic appends fall to the RAM
  // ring, vectors re-cap). The tick self-rate-limits; the edge drives the cue.
  {
    static bool s_prevSdLost = false;
    agent::memory::tickSdHealth(now);
    const bool lost = agent::memory::sdLost();
    if (lost != s_prevSdLost) {
      s_prevSdLost = lost;
      agent::alogf("main: SD %s", lost ? "lost -> degraded (no reboot)" : "recovered -> SD tier");
      ::sfx::fire(lost ? nimbus::sfx::Ev::NetDegraded : nimbus::sfx::Ev::NetOk);
      // Timeline row. The "lost" row lands RAM-ring-only by definition (the SD
      // just vanished); the recovery row is durable - together they bracket the
      // degraded window for "what happened?" queries.
      agent::memory::captureEvent("sd", lost
          ? "SD card lost - memory demoted to RAM tier (no reboot)"
          : "SD card recovered - memory promoted back to SD tier");
    }
  }

  // Header BT/WiFi status glyphs are computed at render time (buildCtx), so they
  // go stale when the link state changes with no other event to repaint - the
  // classic case being WiFi/BLE coming up a few seconds AFTER boot, leaving the
  // status screen frozen on "wi- bt-". Detect a real change in the glyph levels
  // and schedule a coalesced StatusIdle repaint, but only while StatusIdle is the
  // live screen so this never yanks away a JobDetail / menu / pairing view.
  // DEBOUNCED (~2.5 s): the raw levels can flap with no real news - the BLE
  // glyph when WiFi-reconnect scans starve the shared 2.4 GHz radio and the
  // broker link drops/relinks every few seconds, and (before staConfigured()
  // was fixed to read stored config) the WiFi glyph on every retry. Each flap
  // repainted StatusIdle - a real panel refresh per bounce. A change must now
  // hold for the debounce window before it repaints; a boot-time link-up still
  // shows within ~2.5 s.
  {
    static int s_lastWifi = -1, s_lastBt = -1;
    static int s_pendWifi = -2, s_pendBt = -2;
    static uint32_t s_pendSinceMs = 0;
    const int w = net::staConnected() ? 2 : (net::staConfigured() ? 1 : 0);
    const int b = g_orchMode ? 0
                             : (net::ble::connected() ? 2
                                : (net::ble::enabled() ? 1 : 0));
    if (w == s_lastWifi && b == s_lastBt) {
      s_pendWifi = -2; s_pendBt = -2;            // settled back - cancel pending
    } else if (w != s_pendWifi || b != s_pendBt) {
      s_pendWifi = w; s_pendBt = b; s_pendSinceMs = now;   // new candidate
    } else if (now - s_pendSinceMs >= 2500) {    // held stable -> commit + repaint
      s_lastWifi = w; s_lastBt = b;
      s_pendWifi = -2; s_pendBt = -2;
      if (!g_menu.isOpen() &&
          g_lastScreen == uint8_t(attn::ScreenId::StatusIdle))
        g_sched.onIntent(uint8_t(attn::ScreenId::StatusIdle), false, now);
    }
  }

  // While the Software update submenu is showing, reseed its live status line
  // (~1 Hz) from the OTA engine so Checking../Up to date/Update found appear
  // WITHOUT any encoder event. Lives at loop-body scope (like the header
  // watcher above) - it used to sit inside the encoder-event pump, where it
  // only ever ran on knob motion, so an untouched knob never showed the check
  // result (prism v3.1.0 finding). The kick-grace guard keeps the drain's
  // "Checking..." on screen through the async checkTask spin-up instead of
  // overwriting it with the PRE-check state.
  if (g_menu.isOpen() && g_menu.showingUpdate()) {
    static uint32_t s_lastUpdSeed = 0;
    if (now - s_lastUpdSeed > 1000) {
      s_lastUpdSeed = now;
      const std::string st = otaupd::statusStr();
      const bool kickGrace = g_updCheckKickMs &&
                             (now - g_updCheckKickMs < 2500) && st != "checking";
      if (!kickGrace) {
        // One mapping for the line + affordances (CUM-193); the on-screen band
        // in renderMenu reads the same view.
        const nimbus::ota::UpdateView uv = otaViewNow();
        if (uv.showInstall)
          g_menu.setUpdateAvailable(std::string(otaupd::latestSeen().c_str()));
        if (uv.busy) g_updateAnim++;   // slide the indeterminate progress block
        if (!(st == "idle" && !g_updCheckKickMs)) {  // never blank the pre-check help
          g_menu.setUpdateStatus(uv.line);
          g_menuNeedsPaint = true;
        }
      }
    }
  }

  // Keep the on-screen Sign-in QR live (CUM-209). The Sign-in QR encodes a single-
  // use code with a TTL; the menu paints only on knob events, so a QR left on the
  // screen would keep showing a code that silently expires (a later scan then 401s).
  // While the ConfigQr screen is showing, ask for a repaint once the code goes stale
  // - renderMenu() -> configUrl() -> panelSigninCode() then re-mints it, so the QR
  // rotates ~once per TTL-window rather than per frame. Cheap poll (a millis compare).
  if (g_menu.isOpen() && g_menu.showingConfigQr() && net::panelCodeStale())
    g_menuNeedsPaint = true;

  // Flush a coalesced menu repaint once the menu's OWN refresh window elapses.
  // Gated on g_menuDoneAt so a busy/stuck status render can
  // never stall the menu: g_menuDoneAt only ever tracks the previous MENU frame,
  // which is elapsed in the common open case, so this fires on the next loop
  // (~3 ms) and the menu appears within one refresh. When a menu frame is still
  // streaming, it waits for that frame (protecting g_menuFb), never for status.
  if (g_menu.isOpen() && g_menuNeedsPaint && int32_t(now - g_menuDoneAt) >= 0)
    renderMenu();

  // ── RUNS IN BOTH MODES ────────────────────────────────────────────────────
  // These two blocks used to live inside the `if (g_orchMode)` gate below, so in
  // NOTIFIER mode they never ran. That silently disabled:
  //   • the die-temp poll + ThermalGuard - the guard written after the fried e-ink
  //     was inert in half the firmware's modes (dieTempC stale, Trip/Resume/Abort
  //     and the kGlobalLedKillC backstop could never fire), and
  //   • the settle machine - so a PRODUCTION `/api/storage` op started in Notifier
  //     mode NEVER reached its stop condition and drained PAST its target, and a
  //     drain recorded zero resting-mV samples.
  // They are mode-agnostic by construction (globals + the main task, which owns the
  // LEDs/ADC), so they belong here at loop-body level. Keep them ABOVE the gate.
  // Die-temperature poll + THERMAL PROTECTION (every 2 s, always on). The guard
  // governs the drain/storage LED load (trip -> off, cooled -> reduced, repeated
  // trips -> op aborted); the global backstop kills ANY ring output at
  // kGlobalLedKillC so a model-driven `led` pattern can't cook the panel either.
  // (Born from the fried e-ink, 2026-07-15 - see thermal_guard.h.)
  {
    const uint32_t tn = millis();
    if (uint32_t(tn - g_thermTickMs) >= 2000) {
      g_thermTickMs = tn;
      const float t = temperatureRead();
      g_dieTempC = t;
      if (g_highLoadActive) {
        using TG = nimbus::power::ThermalGuard;
        switch (g_thermal.onTemp(t)) {
          case TG::Decision::Trip:
            applyHighLoadRing(0);
            agent::alogf("thermal: TRIP at %.1fC die (trip %u) - LED load OFF, cooling",
                         double(t), unsigned(g_thermal.trips()));
            break;
          case TG::Decision::Resume:
            if (!g_hlSettling) applyHighLoadRing(hlBright());
            // report the ACTUAL resume level (min(reduced, per-run load)), not the
            // fixed constant - hlBright() caps the reduced level at the run load.
            agent::alogf("thermal: cooled to %.1fC - resuming at reduced load (%u/255)",
                         double(t), unsigned(hlBright()));
            break;
          case TG::Decision::Abort:
            g_highLoadActive = false; g_storageTargetMv = 0; g_drainDeep = false;
            g_ledOverrideActive = false; g_thermalAbortLatch = true;
            applyConfig();
            agent::alogf("thermal: ABORT at %.1fC die - repeated trips, drain/storage cancelled",
                         double(t));
            break;
          default: break;
        }
      } else if (solide::board().hasRing && t >= kGlobalLedKillC && !g_lightsOff) {
        // Global LED backstop: kill the WS2812 ring when the die runs hot, since a
        // sustained bright ring overheats adjacent electronics. This protects the
        // PHYSICAL ring only. On a ringless board there is no ring to overheat - the
        // "ring" is drawn on the panel and draws no LED power - and the S3 internal
        // die sensor reads high under normal Wi-Fi+BLE+TFT load, so this would trip
        // spuriously and FREEZE the on-screen ring (g_lightsOff stops the animator
        // while the panel keeps mirroring the last frame). So skip it when !hasRing;
        // real chip protection is the SoC's own thermal throttling, not this.
        g_lightsOff = true;
        solide::leds::clearFrame();
        solide::leds::off();
        agent::alogf("thermal: %.1fC die - ring forced OFF (global backstop; click restores)",
                     double(t));
      }
    }
  }
  // Battery drain/storage settle state machine: periodically drop the heavy LED load
  // for a few seconds to sample a NEAR-RESTING voltage (the calibration-grade reading),
  // then resume - or, for STORAGE, stop and hold once the resting voltage hits target.
  // Main task owns the LEDs + the ADC, so this is race-free.
  if (g_highLoadActive) {
    const uint32_t hnow = millis();
    if (!g_hlSettling && uint32_t(hnow - g_hlLastSettleMs) >= kHlSettleIntervalMs) {
      g_hlSettling = true; g_hlSettleStartMs = hnow;
      applyHighLoadRing(0);                       // drop the load, let the pack recover
    } else if (g_hlSettling && uint32_t(hnow - g_hlSettleStartMs) >= kHlSettleMs) {
      power::Sample s = g_monitor->sample();      // recovered, near-resting reading
      if (s.valid) { g_hlRestingMv = s.millivolts; g_hlRestingAtMs = hnow; }
      g_hlSettling = false; g_hlLastSettleMs = hnow;
      if (g_storageTargetMv && s.valid && s.millivolts <= g_storageTargetMv) {
        g_highLoadActive = false; g_storageTargetMv = 0; g_ledOverrideActive = false;
        g_thermal.disarm();
        applyConfig();
        agent::alogf("storage: reached target, holding at %umV pack", (unsigned)s.millivolts);
      } else {
        applyHighLoadRing(hlBright());            // resume - at the guard's level
      }
    }
  }

  if (g_orchMode) {
#ifdef NIMBUS_NOTIFIER_DEBUG
    static uint32_t s_hb = 0;
    if (now - s_hb > 4000) {
      s_hb = now;
      Serial.printf("ORCH alive sta=%d ip=%s heap=%u\n", int(net::staConnected()),
                    net::staIp().c_str(), unsigned(ESP.getFreeHeap()));
    }
#endif
    // Orchestrator mode: sub-session events are routed into g_router by the
    // EventSink on the Telegram poll task (under the net config lock). Here we
    // just drain the flags it set and drive the ring + panel the same way the
    // nsn path does - the job table has already been updated. The job table and
    // ask/reply surface render exactly like Notifier jobs (plan §3.6).
    bool ringDirty; bool screenRender; uint8_t screenId; bool screenAttn;
    {
      net::ConfigLockGuard lk;  // serialize with the sink's route()
      ringDirty = g_orchRingDirty; g_orchRingDirty = false;
      screenRender = g_orchScreenRender; g_orchScreenRender = false;
      screenId = g_orchScreenId; screenAttn = g_orchScreenAttn;
    }
    if (ringDirty) {
      // Refresh the session-cursor snapshot when the sub-session set changed, and
      // clamp the cursor so a completed job can't leave it dangling past the end.
      g_sessionList = agent::orchestrator::sessionInfos();
      g_lastActivityMs = now;   // a sub-agent just started/finished -> Calm blink
      if (!g_menu.isOpen() && !g_voiceWaiting) refreshRing();
    }
    // Apply a staged led/lights device action (DeviceSink, poll task -> here).
    // The main loop owns the LEDs, so patterns are only ever shown from this task.
    // Deferred while the menu is open (same as g_cfgPendDirty below): the ring is
    // held DARK there, so a staged led/lights override waits - its latch is already
    // set and the loop's black-push masks the ring, so it applies cleanly the moment
    // the menu closes rather than lighting the ring under the open menu.
    // Decoration expiry: a model `led` pattern past its per-mode hold releases
    // the ring back to composed status (measurement pins carry no deadline).
    if (g_ledOverrideActive && g_ledOvUntilMs != 0 &&
        int32_t(millis() - g_ledOvUntilMs) >= 0) {
      { net::ConfigLockGuard lk; g_ledOverrideActive = false; g_ledOvUntilMs = 0; }
      refreshRing();
    }
    if (g_devActDirty && !g_menu.isOpen()) {
      // Snapshot ALL override fields under the lock - the sink rewrites them on
      // the poll task (core 0) inside the same lock, and unlocked reads here
      // (core 1) could tear across two back-to-back led actions.
      bool ledOv; bool ovHasBright; uint8_t ovMode, ovR, ovG, ovB, ovBright;
      { net::ConfigLockGuard lk; g_devActDirty = false;
        ledOv = g_ledOverrideActive;
        ovMode = g_ledOvMode; ovR = g_ledOvR; ovG = g_ledOvG; ovB = g_ledOvB;
        ovHasBright = g_ledOvHasBright; ovBright = g_ledOvBright; }
      if (ledOv) {
        solide::leds::clearFrame();  // release the Full animator's raw frame
        // No explicit brightness in the action -> the MODE's ring brightness, not
        // whatever the ring happened to be at. Inheriting stale global brightness
        // meant a model pattern could run at a reveal's 255 on a Balanced board -
        // the "rainbow at full blast" field report.
        solide::leds::setBrightness(nimbus::power::clampBright(
            ovHasBright ? ovBright
                        : uint8_t(g_cfg.effective(nimbus::Param::RingBrightness)),
            agent::store::brightOvr()));
        solide::leds::Pattern p =
            ovMode == 1 ? solide::leds::Pattern::Spinner
          : ovMode == 2 ? solide::leds::Pattern::Pulse
          : ovMode == 3 ? solide::leds::Pattern::Flash
          : ovMode == 4 ? solide::leds::Pattern::Rainbow  // self-animating hue wheel (colour ignored)
                        : solide::leds::Pattern::Solid;
        solide::leds::show(p, ovR, ovG, ovB);
#ifdef NIMBUS_TEST
        // Keep the RENDER? oracle honest while a model pattern owns the ring.
        g_lastSeg = 0; g_lastSingle = true; g_lastDark = false;
        g_lastBright = ovHasBright ? ovBright : g_lastBright;
        tc::onRender(g_lastScreen, uint8_t(g_cfg.posture()), 0, true, false, g_lastBright);
#endif
      } else {
        // lights:off darkens via refreshRing()'s guard; lights:full / an attention
        // override recomposes the normal ring. Either way one refresh applies it.
        // (The menu-open case is already excluded by the guard above.)
        refreshRing();
      }
    }
    // Talk-to-configure drain: apply staged Config knobs through the SAME path
    // as the menu (selector/user pick, overrides, applyConfig + persistConfig).
    // Deferred while the menu is open - g_cfg.profile() then holds the pick
    // being edited and mutating it would corrupt the in-progress edit.
    if (g_cfgPendDirty && !g_menu.isOpen()) {
      int16_t posture, profile; int32_t hold; char theme[16];
      {
        net::ConfigLockGuard lk;
        g_cfgPendDirty = false;
        posture = g_cfgPendPosture;   g_cfgPendPosture = -1;
        profile = g_cfgPendProfile;   g_cfgPendProfile = -1;
        hold    = g_cfgPendAttnHold;  g_cfgPendAttnHold = -1;
        memcpy(theme, g_cfgPendTheme, sizeof(theme));
        g_cfgPendTheme[0] = 0;
      }
      if (profile >= 0) {
        g_selector.setUser(ProfileId(profile));   // the model speaks for the owner's pick
        g_cfg.setProfile(g_selector.resolve());   // forced > VBUS > user
      }
      if (posture >= 0) g_cfg.setOverride(Param::Posture, posture);
      if (hold >= 0)    g_cfg.setOverride(Param::AttnHoldMs, hold);
      if (theme[0]) {
        // Validate the slug against the real theme list (themeIndexOf maps
        // unknown to 0/teal - round-trip to reject typos instead).
        const int ti = nimbus::themeIndexOf(std::string(theme));
        if (nimbus::themeAt(ti) == std::string(theme))
          agent::store::setTheme(String(theme));
      }
      applyConfig();
      persistConfig();
    }
    // Orchestrator "working" heartbeat: refresh the ring on the turn-in-flight
    // edge so the Calm-level breathe appears when a turn starts and clears when it
    // ends, even for a bare turn with no sub-session change. Suppressed while a voice
    // turn owns the ring with its own waiting spinner (works in every posture).
    static bool s_lastWorking = false;
    const bool workingNow = agent::orchestrator::turnInFlight();
    if (workingNow != s_lastWorking) {
      s_lastWorking = workingNow;
      if (!g_menu.isOpen() && !g_voiceWaiting) refreshRing();
      // Turn just ENDED: settle the panel to the final state with ONE render (the
      // ambient intents below were held back while the turn ran). Not over the
      // screensaver - a settle there would flash the logo off and back.
      if (!workingNow && !g_menu.isOpen() && screenAmbientAllowedOverSaver(false))
        g_sched.onIntent(uint8_t(attn::ScreenId::StatusIdle), false, now);
    }
    // While a turn is IN FLIGHT, hold back AMBIENT screen intents - every job-state
    // edge was flashing a full 2.2 s refresh mid-processing (owner: "heavy
    // flickering while processing"). The ring carries the live activity; the panel
    // settles once on the falling edge above. ATTENTION intents (Error/NeedsInput/
    // Ask - a job waiting on the owner) still render immediately: that's the CTA
    // safety contract, never gate those.
    if (screenRender && !g_menu.isOpen() && (screenAttn || !workingNow) &&
        screenAmbientAllowedOverSaver(screenAttn))
      g_sched.onIntent(screenId, screenAttn, now);
    // Voice turn reply arrived on the poll task -> render it on the panel here.
    if (g_voiceReplyPending && !g_menu.isOpen()) {
      String reply;
      { net::ConfigLockGuard lk; reply = g_voiceReply; g_voiceReplyPending = false; }
      if (g_voiceWaiting) {   // stop the waiting spinner (repaint an override, don't strand it dark)
        g_voiceWaiting = false;
        if (g_ledOverrideActive) g_devActDirty = true;
        else { solide::leds::off(); refreshRing(); }
      }
      g_askOverride = String("Nimbus: ") + reply;
      g_askSticky = true; g_askPage = 0;   // hold until click; rotate pages (P2.3)
      renderScreen(attn::ScreenId::Ask, -1);
    }
    // Voice-wait watchdog: if no reply lands within 30 s (turn failed / dropped), stop
    // the spinner so the ring doesn't spin forever.
    if (g_voiceWaiting && !g_menu.isOpen() && (millis() - g_voiceWaitStart) > 30000) {
      // ^ menu-gated like the reply drain (review): painting the Ask screen +
      //   setting sticky while the menu owns the panel wedged the close path.
      g_voiceWaiting = false;
      if (g_ledOverrideActive) g_devActDirty = true;   // repaint, don't strand dark
      else { solide::leds::off(); refreshRing(); }
      g_askOverride = "No reply - try again.";
      g_askSticky = true; g_askPage = 0;   // hold until click (P2.3)
      renderScreen(attn::ScreenId::Ask, -1);
    }
    // Retention maintenance (docs/orchestrator-storage.md §4): every ~6 h prune SD
    // episodic day-streams + unreferenced blob sidecars older than 30 days. No-op
    // without an SD append-log or before the clock advances past the window; runs off
    // the loop so it never blocks a turn.
    static uint32_t s_lastRetention = 0;
    if (millis() - s_lastRetention > 6UL * 3600UL * 1000UL) {
      s_lastRetention = millis();
      agent::memory::pruneRetention(30);
    }
#ifdef NIMBUS_TEST
    // Under test the console is the sole Serial reader in Orchestrator mode; it
    // supersedes the debug TURN/IP mini-console (which is #if'd out below so the
    // two never both read Serial). TURN is handled by tc::pumpOrch().
    tc::pumpOrch();
#elif defined(NIMBUS_NOTIFIER_DEBUG)
    // Serial test channel: `TURN <text>` fires one real orchestrator turn (no
    // Telegram needed) so a live provider round-trip can be verified over WiFi.
    // Serial is free in Orchestrator mode (the nsn reader only runs in Notifier).
    static String s_cmd;
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (s_cmd.startsWith("TURN ")) {
          String t = s_cmd.substring(5);
          Serial.printf("ORCH TURN <- %s\n", t.c_str());
          // Route to tg_poll like the NIMBUS_TEST path - an inline handleMessage
          // here runs a 5-30 s TLS turn on the watchdog-subscribed main loop
          // (reboot) AND races the tg_poll-owned turn state + glass-box statics
          // across tasks (prism Release-A finding).
          if (!agent::telegram::injectMessage("serial", t))
        Serial.println("TURN busy - inbound queue full, resend");
        } else if (s_cmd == "IP") {
          Serial.printf("IP %s rssi=%d sta=%d\n", net::staIp().c_str(),
                        net::rssi(), int(net::staConnected()));
        }
        s_cmd = "";
      } else if (s_cmd.length() < 512) {
        s_cmd += c;
      }
    }
#endif
  } else {
    // Notifier mode: BLE is the ONLY nsn frame transport (USB serial is power +
    // flashing only - never a status transport, see AGENTS.md). Frames keep
    // flowing into the Router even while the menu is open (so the ring and job
    // table stay current); the menu just owns the panel until it closes.
#ifdef NIMBUS_TEST
    // Serial carries console commands only now (no frame data to tee out) -
    // the same pump Orchestrator mode uses.
    tc::pumpOrch();
#endif

    // BLE pairing screen - DORMANT under Just Works (pairingActive() only fires if
    // MITM is re-enabled). If enabled: while a central is mid-pairing the panel
    // shows the 6-digit passkey (the only channel on a silent-serial unit). Render
    // on the rising edge (and if the code changes for a retry); restore StatusIdle
    // when pairing ends. Preempts the menu: pairing is a rare, must-see event.
    {
      static bool     s_wasPairing = false;
      static uint32_t s_shownPk    = 0;
      const bool     pairing = net::ble::pairingActive();
      const uint32_t pk      = net::ble::pairingPasskey();
      if (pairing && (!s_wasPairing || pk != s_shownPk)) {
        renderScreen(attn::ScreenId::Pairing, -1);
        s_shownPk = pk;
      } else if (!pairing && s_wasPairing && !g_menu.isOpen()) {
        // Falling edge restore - but not OVER an open menu (the rising edge preempts
        // the menu intentionally; the restore should not clobber it). Guarded like
        // every other render site in loop().
        renderScreen(attn::ScreenId::StatusIdle, -1);
      }
      s_wasPairing = pairing;
    }

    // Cloud (cumulo-nimbus) pairing screen (Orchestrator mode). Same edge discipline
    // as BLE pairing above: show the claim code + QR on the rising edge (and if the
    // code changes for a retry), restore StatusIdle when pairing ends. Inactive in
    // Notifier (relay never runs there), so this is a no-op false branch then.
    {
      static bool        s_wasCloudPair = false;
      static std::string s_shownCode;
      const bool   cpair = nimbus::relay::pairingActive();
      const String code  = cpair ? nimbus::relay::claimCode() : String();
      if (cpair && (!s_wasCloudPair || code.c_str() != s_shownCode)) {
        renderScreen(attn::ScreenId::Pairing, -1);
        s_shownCode = code.c_str();
      } else if (!cpair && s_wasCloudPair && !g_menu.isOpen()) {
        renderScreen(attn::ScreenId::StatusIdle, -1);
      }
      s_wasCloudPair = cpair;
    }

    // Link-timeout only (no I/O): clears a stale ring if BLE goes quiet
    // without an explicit Offline frame (e.g. the central just disconnects).
    // Ambient hold scales with the ring level (Full lingers as a desk display,
    // Dark clears fast); calls-to-action hold much longer, for the tunable
    // Param::AttnHoldMs window (default 5 min), regardless of posture.
    if (g_notifier.tick(now, notifier::ambientHoldFor(g_cfg.posture()),
                        uint32_t(g_cfg.effective(Param::AttnHoldMs)))) {
      if (!g_menu.isOpen()) refreshRing();
      const attn::ScreenIntent& in = g_notifier.last().screen;
      if (in.render && !g_menu.isOpen() && screenAmbientAllowedOverSaver(in.attention))
        g_sched.onIntent(uint8_t(in.id), in.attention, now);
    }

    // BLE transport: drain queued FRAME bytes through the Mapper/Router.
    if (net::ble::drain(g_notifier, now)) {
      if (!g_menu.isOpen()) refreshRing();
      const attn::ScreenIntent& in = g_notifier.last().screen;
      if (in.render && !g_menu.isOpen() && screenAmbientAllowedOverSaver(in.attention))
        g_sched.onIntent(uint8_t(in.id), in.attention, now);
#ifdef NIMBUS_NOTIFIER_DEBUG
      Serial.printf("NSN(ble) jobs=%d attn=%d bright=%d\n", jobCount(),
                    int(g_notifier.last().anyAttention),
                    g_notifier.mapper().brightness());
#endif
    }
  }

  // Screensaver-restore render, deferred from saverKick(). saverKick fires from the
  // attention event tap, which the Router runs synchronously on the routing task -
  // during a turn that is tg_poll, and rendering there aborts on the NVS lock (deep
  // turn stack + cross-task NVS handle; backtrace decoded 2026-08-04). loop() is the
  // only task that renders, so the paint happens here. Mode-agnostic (both Notifier
  // and Orchestrator saverKick the same way).
  if (g_saverRestoreReq) {
    g_saverRestoreReq = false;
    if (g_lastScreen == uint8_t(attn::ScreenId::Screensaver) && !g_menu.isOpen())
      renderScreen(attn::ScreenId::StatusIdle, -1);
  }

  // Touch input. A tap is resolved against the tap regions of the frame ACTUALLY
  // on the panel and dispatched into the menu FSM and cursor, so its dirty-persist
  // block and request-flag drains below all run unchanged.
  // ⚠ TOUCHPOLL 0 disables the touch pump at runtime.
  if (g_screenIsTft) drainTouch(now);

  // The render scheduler only drives the panel while the menu is closed AND no
  // reply is being held (P2.3): a sticky reply must not be overwritten by the
  // next scheduled render - it dismisses on tap, like the menu's own gating.
  if (!g_menu.isOpen() && !g_askSticky) {
    render::RenderCommand cmd = g_sched.tick(now);
    if (cmd.render) {
      const attn::ScreenId screen = attn::ScreenId(cmd.screenId);
      const int cursorJob =
          (screen == attn::ScreenId::JobDetail && jobCount() > 0)
              ? int(g_cursor.index())
              : -1;
      // fullClear is a legacy slow-panel refresh hint; the color panel ignores it.
      renderScreen(screen, cursorJob, cmd.fullClear);
    }
  }

#ifdef NIMBUS_TEST
  // F9: a WiFi auth-fail surfaced by the onWifiReason handler paints a one-shot
  // attention Badge (the reason itself already went out on serial). Only when
  // the menu isn't holding the panel. This keeps the "no silent hang" contract:
  // a wrong PSK produces a visible error surface, not a dead screen.
  if (int r; tc::errorBadgePending(r) && !g_menu.isOpen()) {
    g_askSticky = false; g_askPage = 0; g_askOverride = "";  // dismiss a held reply
    // before painting over it (review: same wedge as the menu-close path).
    renderScreen(attn::ScreenId::StatusIdle, -1);
    tc::clearErrorBadge();
  }
#endif

  // Reveal window elapsed: drop the ring back to its normal posture rendering
  // (re-dim the Desk idle glow / release the Full treatment in Dark/Calm). One-
  // shot edge; skip the repaint while the menu holds the panel - it'll restore
  // when the menu closes (refreshRing() there) or on the next status render.
  // Theme flourish expired -> hand the ring back to live status.
  if (g_themePreviewUntilMs && int32_t(now - g_themePreviewUntilMs) >= 0) {
    g_themePreviewUntilMs = 0;
    if (!g_menu.isOpen()) refreshRing();
  }
  if (g_revealUntilMs && int32_t(now - g_revealUntilMs) >= 0) {
    g_revealUntilMs = 0;
    hw::setBrightnessHold(false);   // envelope done -> plan brightness owns again (P2.4)
    if (!g_menu.isOpen()) refreshRing();
  } else if (g_revealUntilMs && !g_menu.isOpen() && !g_lightsOff && !g_ledOverrideActive) {
    // Wake reveal IN PROGRESS: ease the ring brightness in (~kRevealFadeInMs) and
    // out (~kRevealFadeOutMs) across the window while keeping the composed status
    // colours - the transition breathes instead of snapping. Applied every loop
    // (leds::setBrightness scales the raw frame the Animator pushes at ~30 FPS).
    // applyRingPlan is brightness-held during the window so a working-driven
    // refreshRing() can't snap it back to full mid-fade (the flicker, P2.4).
    const uint32_t elapsed = now - (g_revealUntilMs - g_revealDurMs);
    uint16_t env;   // 0..256
    if (elapsed < kRevealFadeInMs)                          env = smoothstep256(elapsed, kRevealFadeInMs);
    else if (elapsed > g_revealDurMs - kRevealFadeOutMs)    env = smoothstep256(g_revealDurMs - elapsed, kRevealFadeOutMs);
    else                                                    env = 256;
    // ^ fade-out keyed to THIS window's duration (was kRevealMs - a shorter boot
    //   handoff window never eased out, it snapped at expiry).
    solide::leds::setBrightness(nimbus::power::clampBright(
        uint8_t((uint16_t(ringBrightByte()) * env) >> 8), agent::store::brightOvr()));
  }

#ifdef NIMBUS_TEST
  // Console SAVER command: bare SAVER forces the saver screen now (HIL/demo);
  // SAVER <min> sets + persists the idle threshold (0 disables).
  {
    int sv;
    if (tc::popSaverCmd(sv)) {
      if (sv < 0) {
        renderScreen(attn::ScreenId::Screensaver, -1);
        // TFT: blanking the backlight IS the saving; the drawn logo is only what
        // shows if something turns it back on.
        if (g_screenIsTft) hw::tft::setBacklight(nimbus::kBacklightRestPct);
      } else {
        agent::store::setSaverMin(uint16_t(sv));
        g_saver.setThresholdMin(uint16_t(sv));
        g_saver.noteActivity(now);
      }
    }
  }
#endif

  // A /savewifi request is the one exception to immediate TFT AP teardown: keep
  // AP+STA alive just long enough for the wizard to receive the exact LAN IP and
  // token-bearing URL. The explicit handoff acknowledgement shortens the fallback
  // window to four seconds (long enough to flush its HTTP response).
  if (g_orchMode && g_screenIsTft && net::consumeWifiJoinStarted()) {
    const bool alreadyUp = WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0u;
    g_apHandoffArmed = !alreadyUp;
    g_dropApAfterMs = alreadyUp ? now + 20000UL : 0;
  }
  if (g_orchMode && g_screenIsTft && net::consumeWifiHandoffReady()) {
    g_apHandoffArmed = false;
    g_dropApAfterMs = now + 4000UL;
    g_dropApPending = true;
  }

  // Keep the SoftAP consistent with the STA link (TFT only) - THE white-screen fix.
  // The AP's continuous beacon TX from the on-board antenna knocks this jumper-wired
  // panel into sleep, so the AP is DOWN while STA is up (panel stable; STA + LAN web
  // still work) and UP while STA is down (setup/recovery reachable). ⚠ Resolve from
  // the ACTUAL radio state, never from which event fired last: a GOT_IP immediately
  // followed by a DISCONNECT could otherwise tear the recovery AP down while offline
  // and never self-heal (prism). The event flags trigger an immediate check; a slow
  // periodic re-check self-heals any raced/missed event, so the AP can't be stranded.
  // ⚠ Orchestrator-only: Notifier runs the radio OFF (BLE owns the SRAM), so it has no
  // SoftAP to reconcile - without this gate the "offline" branch would fire every tick
  // and turn the radio back on in Notifier mode, defeating the Wi-Fi-off design. The
  // per-tick decision lives in the pure, host-tested decideSetupAp() (CUM-190): the
  // white-screen DROP stays TFT-only, but RESTORE now runs on any board, and a stalled
  // first-run join that is starving the AP is re-published without a physical restart.
  if (g_orchMode && (g_dropApPending || g_restoreApPending ||
                     int32_t(now - g_lastApReconcileMs) >= 3000)) {
    g_dropApPending = false;
    g_restoreApPending = false;
    g_lastApReconcileMs = now;
    const bool connected = WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0u;
    const bool apUp = (uint32_t)WiFi.softAPIP() != 0u;
    nimbus::wifi::SetupApInputs si;
    si.orchestrator = g_orchMode;
    si.tftBoard     = g_screenIsTft;
    si.staConnected = connected;
    si.apAddressed  = apUp;
    si.onboarded    = agent::store::onboarded();
    si.handoffGrace = g_apHandoffArmed ||
                      (g_dropApAfterMs != 0 && int32_t(now - g_dropApAfterMs) < 0);
    si.msSinceJoin  = net::msSinceJoinAttempt();
    switch (nimbus::wifi::decideSetupAp(si)) {
      case nimbus::wifi::SetupApAct::DropAp:
        net::dropSoftAP();
        g_dropApAfterMs = 0;
        agent::alog("[net] SoftAP dropped on a TFT board (white-screen mitigation)");
        break;
      case nimbus::wifi::SetupApAct::RestoreAp:
        g_dropApAfterMs = 0;
        net::restoreSoftAP();
        agent::alog("[net] Wi-Fi down - SoftAP restored so setup/recovery is reachable");
        break;
      case nimbus::wifi::SetupApAct::ProtectAp:
        net::publishSetupNetwork();
        agent::alog("[net] setup join stalled - setup network re-published (no restart needed)");
        break;
      case nimbus::wifi::SetupApAct::None:
        break;
    }
  }

  // Panel watchdog (TFT only). The ILI9341 can silently lose its configuration -
  // observed in the field as "it showed the UI, then went white" - and because an
  // idle device composes an IDENTICAL frame forever, nothing would ever repaint
  // it. This is the ONLY periodic render trigger on a TFT board, so it must stay
  // in loop(); the check itself is a few bytes over SPI when the panel is fine.
  if (g_screenIsTft && hw::tft::tickHealth(now)) {
    // ⚠ Route through the SAME renderer the foreground path uses.
    //
    // renderScreen() builds its own ScreenCtx and does NOT populate the menu -
    // rows, title, help and the adjusting flag all come from renderMenu(). So a
    // watchdog repaint while the menu was open redrew it as a BARE HEADER: the
    // Back chevron survived and every row vanished, five seconds after opening
    // any menu (owner-observed). renderMenu() also owns Sign-in, Self-test and
    // Battery, so those had the same hole.
    if (g_menu.isOpen()) renderMenu();
    else                 renderScreen(attn::ScreenId(g_lastScreen), -1);
  }

  // Screensaver entry: long-idle on the ambient screen -> the dotted-ring logo.
  // Gated on StatusIdle so a CTA badge, menu, detail, ask or QR screen can never
  // time out into the logo; every activity seam (input pop, real job edges, any
  // non-ambient render) resets the clock and saverKick() restores live status.
  // A ringless Notifier board draws the status ring on the panel, so the ring IS
  // the point - never blank it there. (Orchestrator still saves power by blanking;
  // the demo/a session wakes it via saverKick.)
  const bool ringIsTheScreen = g_screenIsTft && !solide::board().hasRing && !g_orchMode;
  if (!g_menu.isOpen() && !ringIsTheScreen && g_saver.due(now) &&
      g_lastScreen == uint8_t(attn::ScreenId::StatusIdle)) {
#ifdef NIMBUS_TEST
    Serial.printf("SAVERDBG fire now=%lu last=%lu thr=%u\n",
                  (unsigned long)now, (unsigned long)g_saver.lastActivityMs(),
                  (unsigned)g_saver.thresholdMin());
#endif
    // F28: drop any AMBIENT StatusIdle intent queued just before the saver armed
    // (header-glyph/telemetry watchers post gated on g_lastScreen==StatusIdle, but
    // the scheduler drain has no saver guard) - else tick() flashes it over the
    // logo. Attention intents are never dropped (the CTA contract).
    g_sched.clearPendingAmbient();
    renderScreen(attn::ScreenId::Screensaver, -1);
    // TFT: blanking the backlight IS the power saving - on this panel the
    // backlight is the idle draw, so drawing the logo at full brightness would
    // cost MORE than showing live status. saverKick() restores it on activity.
    // (Previously only the TEST-only SAVER command blanked, so a production
    // device lit the logo indefinitely.)
    if (g_screenIsTft) hw::tft::setBacklight(nimbus::kBacklightRestPct);
  }

  // Boot breathe-flourish window elapsed: recompose to the normal status ring.
  if (g_bootBreatheUntilMs && int32_t(now - g_bootBreatheUntilMs) >= 0) {
    g_bootBreatheUntilMs = 0;
    // Graceful handoff (owner: boot color jumps felt abrupt): ride the reveal
    // machinery - the first status ring eases in/out via its smoothstep envelope
    // instead of snapping from the white breathe to full-bright arcs.
    g_revealUntilMs = now + 1500;
    g_revealDurMs   = 1500;   // so the envelope's fade-in is keyed to THIS window,
                              // not kRevealMs (the bug: it started "0.9 s in", skipped
                              // the fade-in, held, then dipped to black before the snap)
    hw::setBrightnessHold(true);   // the envelope owns brightness for the handoff (P2.4)
    if (!g_menu.isOpen()) refreshRing();
  }

  // MENU OPEN -> the menu ring frame (dim fill-bar during its hold, else black)
  // owns the LEDs at the HIGHEST priority. This MUST win over voice/led/lights
  // state: an orchestrator led/lights override (e.g. a scheduled loop or
  // Telegram turn firing while the owner reads the menu) or a leftover voice
  // spinner sets a lower-precedence Pattern, and re-pushing the raw frame every
  // pass masks it (raw frame outranks segments + Pattern) and beats the
  // driver's ~500 ms staleness watchdog. Whatever override was latched shows
  // through naturally once the menu closes (the menu-close path runs clearFrame
  // + refreshRing, which re-establishes status / honours the override).
  if (g_menu.isOpen()) {
    menuRingRepaint(false);
  } else if (g_themePreviewUntilMs && int32_t(now - g_themePreviewUntilMs) < 0 &&
             !otaupd::installing() && !g_ledOverrideActive && !g_lightsOff &&
             !g_voiceWaiting && !ringHasAttention()) {
    // Theme just picked: flourish the new palette on the main screen so the change
    // is visible (an idle ring is otherwise dark). ⚠ Yields to a live job / CTA /
    // voice cue (the guards above) - a raw frame outranks the router's segments, so
    // without them the cosmetic preview would overpaint a Running/Approval/Error arc.
    // Re-pushed every pass to beat the driver's ~500 ms raw-frame staleness watchdog;
    // expires below -> refreshRing().
    const int L = NIMBUS_RING_LEDS;
    solide::ring::RGB buf[NIMBUS_RING_LEDS] = {};
    nimbus::ThemeColor pal[nimbus::kThemeMaxColors];
    const int n = nimbus::themePalette(std::string(agent::store::theme().c_str()), pal,
                                       nimbus::kThemeMaxColors);
    solide::leds::setBrightness(
        nimbus::power::clampBright(ringBrightByte(), agent::store::brightOvr()));
    for (int i = 0; i < L && n > 0; i++) {
      const nimbus::ThemeColor c = pal[i % n];
      buf[i] = {c.r, c.g, c.b};
    }
    solide::leds::showFrame(buf, L);
  } else if (!otaupd::installing() && !g_voiceWaiting && !g_ledOverrideActive &&
             !g_lightsOff) {
    // Active-posture ring animation (birth/ripple/collapse) needs a fresh frame
    // even between job-state edges - cheap + self-rate-limited (~30 FPS, no-op
    // outside the Full level) - but NOT while a direct pattern owns the ring:
    // the voice waiting spinner (this was the "no light while processing" bug -
    // in Full the animator overwrote the spinner within one 33 ms tick), an
    // orchestrator led/lights override, or an OTA install (the progress bar is
    // the sole raw-frame owner then - racing it was the heavy download flicker).
    hw::tickAnimation(now);
  }

  // On-screen ring animation (ringless boards): the ring IS the notifier display,
  // so repaint it at ~30 fps via a region push instead of waiting for a scheduler
  // event (which is why it looked frozen). Region-only, so it never runs the
  // ~31 ms full-frame blit; skipped over the menu or while the saver has blanked
  // the panel. tickAnimation() above already refreshed the ring frame it reads.
  static uint32_t s_lastRingPaint = 0;
  if (g_screenIsTft && !solide::board().hasRing && !g_menu.isOpen() &&
      g_lastScreen == uint8_t(attn::ScreenId::StatusIdle) &&
      hw::tft::backlight() > nimbus::kBacklightRestPct &&
      uint32_t(now - s_lastRingPaint) >= 33) {
    s_lastRingPaint = now;
    hw::tft::repaintRingRegion(buildCtx(-1));
  }

  delay(3);
}
