#pragma once
#include <cstdint>

#include "solide/ring.h"

// attention - the single funnel for "something happened". Both modes emit
// semantic events here; the router classifies them (attention vs ambient),
// maintains the job table (solide::ring::Allocator), and fans each event out
// as pure data:
//   - a ScreenIntent for the scheduler (attention => immediate path),
//   - a NotifyIntent for the mode layer (Telegram ping, Orchestrator only),
//   - ringDirty, telling the caller to recompute the ring plan (ring_plan.h).
//
// Attention events: WaitingInput, AwaitingApproval, Error (job states),
// IncomingAsk, LowBattery-T1. Everything else is ambient and touches only the
// panel (coalesced). In the Dark/Calm ring levels the ring shows just the single
// highest-priority attention source; topAttention() reports it.

namespace nimbus::attn {

// Voice pipeline stages (shown on the ring, one glyph max on the panel).
enum class VoiceStage : uint8_t { None = 0, Recording, Processing, Speaking };

// Stable ids for every screen the renderer can draw. Shared vocabulary between
// the router, scheduler and renderer. Wire numbers are frozen (HIL mirrors them
// positionally); do not reorder or renumber.
enum class ScreenId : uint8_t {
  StatusIdle = 0,   // ambient status (mode, jobs summary, battery)
  JobDetail,        // cursor-selected job, paged text
  Badge,            // Dark/Calm attention badge (small region, partial-friendly)
  Menu,
  Battery,          // telemetry screen
  Ask,              // orchestrator ask/reply
  VoiceGlyph,       // single partial-refresh glyph for voice stages
  SetupInfo,        // captive-portal / provisioning info
  IdleArt,          // 3-color art (explicit, slow refresh)
  ConfigQr,         // full-screen QR to the config web page (append-only!)
  SessionDetail,    // Orchestrator: the encoder-cursor's focused sub-session (append-only!)
  Pairing,          // BLE secure pairing: big passkey the Mac must enter (append-only!)
  SelfTest,         // hardware health-check results (menu-triggered) (append-only!)
  Screensaver,      // long-idle logo screen: dotted-ring mark + device name (append-only!)
  TokenDetail,      // full recovery sign-in code; opened from Connectivity (append-only!)
  TouchCal,         // on-device tap-the-crosses touch calibration (CUM-189) (append-only!)
};

struct Event {
  enum class Type : uint8_t {
    JobState,     // key + status (solide::ring::Status); Offline frees the job
    JobProgress,  // key + value 0-100
    IncomingAsk,  // orchestrator has a question for the user
    AskCleared,   // user answered / dismissed
    Voice,        // stage transition
    LowBattery,   // battery policy T1 edge (level = percent)
    BatteryOk,    // T1 released
    NetworkDegraded,
    NetworkOk,
  };
  Type     type;
  uint32_t key = 0;      // JobState/JobProgress: stable job key
  uint8_t  status = 0;   // JobState: solide::ring::Status as int
  uint8_t  value = 0;    // JobProgress pct / LowBattery percent
  VoiceStage stage = VoiceStage::None;
  // JobState provider accent. hasAccent gates whether accentHue is applied, so
  // every hue 0-254 AND 255 (white - the "unknown provider" colour) is a valid
  // accent; without it, 255 could not be distinguished from "no accent".
  bool     hasAccent = false;
  uint8_t  accentHue = 255;
};

struct ScreenIntent {
  bool     render = false;
  ScreenId id = ScreenId::StatusIdle;
  bool     attention = false;
};

struct NotifyIntent {
  bool notify = false;
  Event::Type reason = Event::Type::JobState;
};

struct Decision {
  ScreenIntent screen;
  NotifyIntent notify;
  bool         ringDirty = false;
};

class Router {
 public:
  // Route one event; updates internal state and returns the fan-out.
  Decision route(const Event& e, uint32_t nowMs);

  // Optional observer invoked for EVERY event entering route(), before state
  // updates. Both operating modes converge on the shared router (notifier BLE
  // feed, orchestrator sink, battery policy), so this is the single seam for
  // cross-cutting event consumers (the SFX engine). Must be cheap and
  // non-blocking - it runs on the routing task. Plain function pointer keeps
  // the core allocation-free.
  using EventTap = void (*)(const Event& e);
  void setEventTap(EventTap tap) { tap_ = tap; }

  // ---- state queries for ring_plan / screens ----
  const solide::ring::Allocator& jobs() const { return jobs_; }
  VoiceStage voiceStage() const { return voice_; }
  bool askPending() const { return askPending_; }
  bool lowBattery() const { return lowBattery_; }
  bool networkDegraded() const { return netDegraded_; }

  // The single most important attention source right now (drives the Passive
  // one-LED plan). Returns false if nothing needs attention.
  struct Attention {
    // ⚠ APPEND-ONLY. topAttention() builds these with POSITIONAL brace-init, so a
    // field inserted before `hue` silently lands the hue in the wrong member AND
    // still compiles.
    enum class Src : uint8_t { None, Job, Ask, LowBattery };

    bool active = false;
    solide::ring::Status status = solide::ring::Status::Offline;
    uint8_t hue = 255;  // suggested hue (state style; ask/battery overrides)
    // WHICH source won. Needed because a low battery and a failed job both
    // surface as Status::Error with hue 0 - byte-identical - and callers must be
    // able to treat them differently (the low-battery cue is owner-opt-in and
    // deliberately subtle; a job failure is not). lowBattery() cannot break the
    // tie either: a job Error OUTRANKS low battery here, so it can be true while
    // the winning attention is the job.
    Src src = Src::None;
  };
  Attention topAttention() const;

  // Fallback expiry (the main-loop safety net). The happy-path reap runs on the
  // tg_poll / broker task (orchestrator scheduleReap, notifier Mapper::timeout);
  // if THAT task stalls or a clearing Offline is dropped, a red Error/CTA arc
  // could otherwise strand for hours. This ages out every attention source whose
  // dwell exceeds maxAgeMs - attention-status jobs by their Slot::enteredAt, and
  // the ask latch by askSince_ - so nothing red can persist past the window
  // regardless of the reap path's health. Returns true if it cleared anything.
  // (lowBattery_ is left alone: when real battery HW exists it is genuinely
  // driven and cleared by BatteryOk; the NullMonitor stub never sets it.)
  bool forceExpireAttention(uint32_t nowMs, uint32_t maxAgeMs);

  // Expiry TOMBSTONES (owner red-ring root-cause fix). The broker sends FULL-SNAPSHOT
  // frames on every event, so after forceExpireAttention() frees a stale attention
  // arc, the next neighbor-driven frame re-adds the SAME key at the SAME status with
  // a fresh enteredAt - red flaps back every backstop cycle until the broker's own
  // CTA TTL. A tombstone remembers (key,status) at expiry; route() SUPPRESSES an
  // identical re-add until a DIFFERENT status arrives for that key (a real state
  // change revives it - also clears on Offline), or kTombstoneTtlMs passes (safety
  // valve - never suppress forever).
  static constexpr uint32_t kTombstoneTtlMs = 15u * 60u * 1000u;

 private:
  struct Tombstone { uint32_t key = 0; uint8_t status = 0; uint32_t at = 0; bool used = false; };
  Tombstone tombs_[RING_MAX_SEGMENTS];
  void tombstoneSet(uint32_t key, uint8_t status, uint32_t nowMs);
  void tombstoneClear(uint32_t key);
  bool tombstoneBlocks(uint32_t key, uint8_t status, uint32_t nowMs);

  solide::ring::Allocator jobs_;
  VoiceStage voice_ = VoiceStage::None;
  bool askPending_ = false;
  uint32_t askSince_ = 0;   // ms when askPending_ went true (for forceExpireAttention)
  bool lowBattery_ = false;
  bool netDegraded_ = false;
  EventTap tap_ = nullptr;
};

// Pure classification helper (exposed for tests): is a job status an
// attention state?
bool isAttentionStatus(solide::ring::Status s);

}  // namespace nimbus::attn
