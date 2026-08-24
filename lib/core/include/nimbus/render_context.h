#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/attention.h"
#include "nimbus/power/power_monitor.h"
#include "nimbus/profile.h"

// screen_context - the display-agnostic context every screen is rendered from.
// The color panel renderer (tft_render) draws these structs directly; lib/core
// stays free of any board header so the render context remains host-testable.
//
// Layout conventions are the renderer's: a header strip (mode + posture +
// battery glyph when telemetry is valid) over a body region; JobDetail pages
// long text; Badge draws only a compact attention region.

namespace nimbus::render {

struct JobInfo {
  uint32_t key = 0;
  uint8_t  status = 0;    // solide::ring::Status
  uint8_t  progress = 0;  // 0-100
  uint8_t  accentHue = 255;
  std::string label;      // session title (nsn v2); may be empty (v1 has no labels)
  uint8_t  harness = 0;   // nsn v2 harness tag (nsn::kHarness*); 0 = unknown
};

struct ScreenCtx {
  // header
  std::string deviceName;              // this unit's identity (e.g. "Nimbus-2"), shown
                                       // top-left on every screen so multiple devices are
                                       // distinguishable at a glance. "" -> "Nimbus".
  const char* modeName = "notifier";   // "notifier" | "orchestrator"
  Posture     posture = Posture::Dark;
  const char* profileName = "balanced";
  power::Sample battery;               // valid=false hides the battery glyph
  // Battery analytics for the full-screen Battery view (drawBattery v2), from
  // nimbus::power::BatteryEstimate. -1 minutes = unknown (charging / no trend).
  int32_t     battMinutesToEmpty = -1;
  uint8_t     battHealthPct = 100;
  std::string battChargeState;         // "discharging"/"charging"/"full"/... ("" hides)
  bool networkDegraded = false;
  // Header radio status glyphs (top-right, left of the battery). 0 = off,
  // 1 = advertising / connecting, 2 = linked / up. Rendered as wi±/bt± tags.
  uint8_t wifiState = 0;
  uint8_t btState = 0;
  // Header line 2 (owner R4): sound state, spelled out - sfxLevel 0-3
  // (mute/low/med/high, the ACTIVE mode's level) + master volume 0-100.
  uint8_t sfxLevel = 0;
  uint8_t sfxVolume = 0;
  // Header L1 SD state token (owner ask): "ok" mounted / "none" not detected /
  // "lost" was mounted then dropped. Capacity lives on the menu's SD row.
  std::string sdShort;

  // jobs (StatusIdle summary + JobDetail)
  std::vector<JobInfo> jobs;
  int  cursorJob = -1;                 // index into jobs for JobDetail
  int  detailPage = 0;                 // TextPager page for JobDetail

  // On-screen ring mirror for boards with no physical LED ring. Empty = none;
  // 45 entries = the composited ring frame (segments + cursor glow), drawn by the
  // notifier screen as a dot-ring instead of driving WS2812s. POD RGB so
  // lib/core stays free of any solide/ header and remains golden-testable.
  struct RingLed { uint8_t r = 0, g = 0, b = 0; };
  std::vector<RingLed> ringLeds;
  bool micHeld = false;   // hold-to-talk is being pressed -> draw the button pressed

  // SessionDetail (Orchestrator cursor focus). The Orchestrator itself is
  // ALWAYS focus index 0 (sessionIsRoot) - the head agent you're always talking to
  // - so this screen never shows "nothing"; sub-sessions are focus indices 1..N.
  bool sessionIsRoot = false;          // true = the Orchestrator (head), not a sub-session
  std::string sessionTitle;            // sub-session task/category (root: shown as "Nimbus")
  std::string sessionProvider;         // "openai" | "anthropic" | "mistral" (root: active host)
  std::string sessionState;            // "running" | "queued" | "done" | ... (root: "ready"/"listening")

  // ask / voice / badge
  std::string askText;                 // Ask screen body
  attn::VoiceStage voice = attn::VoiceStage::None;
  bool badgeActive = false;            // Badge: something needs attention
  std::string badgeText;               // short, e.g. "INPUT?" / "APPROVE?"

  // setup info
  std::string apName, apPass, portalUrl;
  std::string fwVersion;  // firmware version string, shown small on the Setup screen
  bool apUp = false;      // the setup SoftAP is CURRENTLY up (reachable); torn down after
                          // STA joins. ConfigQr prefers the AP address while it holds
                          // (routable from a phone on the AP) and the LAN address once gone.
  std::string configUrl;  // encoded as the QR on the ConfigQr screen (LAN IP when joined)
  std::string setupUrl;   // encoded as the QR on SetupInfo - ALWAYS the SoftAP address
                          // (the scanning phone is on the setup AP; a LAN IP is
                          // unroutable from there - audit P1.2). "" -> configUrl.
  std::string netStatus;  // one-line live connectivity status under the ConfigQr QR
  std::string webToken;   // recovery-only device auth token. Normal setup/sign-in QRs
                          // carry it automatically; setup screens never require typing it.

  // BLE secure pairing (Pairing screen). The 6-digit passkey the Mac must enter,
  // shown while a central is mid-pairing. On a production (silent-serial) unit
  // this screen is the ONLY place the code appears, so the bonded link can
  // actually be completed.
  std::string pairingCode;

  // Cloud (cumulo-nimbus) pairing. When non-empty, the Pairing screen renders the
  // cloud claim variant instead of the BLE passkey: the 8-char pairingCode plus a
  // scannable QR of this claim URL. Empty leaves the BLE passkey layout unchanged.
  std::string claimUrl;

  // menu
  std::vector<std::string> menuItems;
  int menuSelected = 0;
  std::string menuTitle;  // breadcrumb path band; empty = legacy titleless layout
  std::string menuHelp;   // param help pane at the bottom; empty = no pane
  bool menuAdjusting = false;  // the selected row's value is being edited -> drawMenu
                               // INVERTS that row (P2.2)

  // self-test (SelfTest screen). Device fills rows from hw::runSelfTest(); the
  // portable renderer just lays them out. status: 0 PASS, 1 FAIL, 2 SKIP.
  struct SelfTestRow { std::string name; uint8_t status = 2; };
  std::vector<SelfTestRow> selfTest;
  std::string selfTestSummary;   // e.g. "8P/0F/3S"
};

}  // namespace nimbus::render
