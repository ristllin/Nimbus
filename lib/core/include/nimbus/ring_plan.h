#pragma once
#include "nimbus/duty.h"
#include <cstdint>

#include "nimbus/attention.h"
#include "nimbus/profile.h"
#include "solide/ring.h"

// ring_plan - posture-aware ring compositor decisions. Pure: composes the
// router's state + encoder cursor + effective config into a Plan; the device
// glue (src/hw/ring_out.cpp) turns Plans into pixels via solide::leds.
//
// Division of labor (the brief): the ring is INSTANT. Detents move the cursor
// on LEDs immediately; the e-ink follows after dwell. While the panel is busy
// rendering, the cursor LED shows a "panel syncing" shimmer.
//
// Ring levels (Posture{Dark,Calm,Full}):
//  - Full: full segment treatment - the allocator snapshot renders as per-job
//    arcs (provider accents, progress fill), voice stages override globally. When
//    idle (no jobs, no voice) it shows a single faint white breathe - a desk
//    "heartbeat" so the ring never looks dead - instead of going dark.
//  - Dark/Calm: ring dark; at most ONE LED lit (index/hue/anim/period from
//    config) for the top attention source (Calm adds a soft activity glow).
//    Voice stages take over that LED. Cursor activity temporarily lights a dim
//    positional glow that decays after cursorDecayMs. Both stay dark when idle.

namespace nimbus::ring {

// Sentinel key of the synthetic Full-posture desk idle-heartbeat segment (no real
// job). ring_out renders THIS segment at full per-status brightness (skipping the
// Idle 20% dim, which is meant for idle sessions) so the faint glow isn't double-dimmed.
constexpr uint32_t kIdleGlowKey = 0xFFFFFFFFu;
// Sentinel key of the synthetic Full-posture attention arc (an orchestrator ask /
// low-battery warning with no job of its own). Full used to render neither, so a
// reveal from Dark/Calm - where they DO light the single LED - showed LESS than Calm.
constexpr uint32_t kAttnGlowKey = 0xFFFFFFFEu;

struct SingleLed {
  bool     lit = false;
  uint8_t  index = 0;
  uint8_t  hue = 255;       // 0-254; 255 = white
  uint8_t  anim = 0;        // solide::ring::Anim as int
  uint16_t periodMs = 2400;
};

struct CursorGlow {
  bool    active = false;
  uint8_t index = 0;        // LED index of the cursor position
  bool    syncing = false;  // panel busy: render the shimmer variant
  uint8_t hue = 255;        // accent hue of the pointed-at session (255 = white)
};

struct Plan {
  Posture posture = Posture::Dark;
  uint8_t brightness = 30;  // global cap from config

  // Full ring level: allocator snapshot to hand to solide::leds' segment
  // renderer (count 0 = ring dark / idle).
  int segCount = 0;
  solide::ring::Slot segs[RING_MAX_SEGMENTS];

  // Dark/Calm levels: the one attention LED (unused in Full).
  SingleLed single;

  // Both postures.
  CursorGlow cursor;
  attn::VoiceStage voice = attn::VoiceStage::None;

  // Rainbow theme (owner 2026-07-16, "purely for looks"): the active theme is
  // "rainbow", so arcs render as a slowly ROTATING full-wheel hue cycle instead of
  // their static role hue. Set by the caller who knows the theme name (main.cpp's
  // theming block). alertHue rides along so the renderer can exempt the one
  // safety-critical signal - Error keeps its fixed alert hue + breathe (red stays
  // reserved even when the rest of the ring parties). Idle/white (255) is exempt
  // too (idle stays deliberately dull).
  bool    rainbow = false;
  uint8_t alertHue = 0;

  // Low-battery cue (owner 2026-07-29). The warning is OWNER-OPT-IN and, when on,
  // deliberately recessive: the renderer gates the whole frame to a brief pulse
  // each minute (nimbus/duty.h) instead of breathing continuously.
  // ⚠ The duty PHASE is NOT decided here. `single.lit` must stay true for the
  // whole low-battery window, because ring_out stops the animation tick the moment
  // it goes false - a plan-level blink would extinguish the cue permanently
  // instead of pulsing it.
  bool    lowBattCue   = false;   // arm the periodic envelope in the renderer
  uint8_t cueBrightPct = 100;     // cue brightness relative to the mode's own
};

class Cursor {
 public:
  // dir: +1 / -1 detents. Position wraps around ledCount.
  void onDetent(int dir, int ledCount, uint32_t nowMs);
  // Jump straight to a position. A knob can only ever step, but a TAP names its
  // target directly, so touch needs an absolute set. Same bookkeeping as a
  // detent (clamped, marks the cursor active) so the ring's locate cue behaves
  // identically however the cursor moved.
  void setIndex(int idx, int ledCount, uint32_t nowMs);
  void reset();
  bool activeAt(uint32_t nowMs, uint32_t decayMs) const;
  uint8_t index() const { return uint8_t(pos_); }
  uint32_t lastMoveMs() const { return lastMoveMs_; }

 private:
  int      pos_ = 0;
  uint32_t lastMoveMs_ = 0;
  bool     moved_ = false;
};

// The tail options of compose(), as NAMED fields. This struct exists because the
// old positional tail (7 defaulted scalars) allowed a catastrophic silent failure
// TWICE: a comment reflow swallowed `working, g_lastActivityMs,` at the main.cpp
// call sites (d3a800d, again on worktree-harness) and the call STILL COMPILED -
// the remaining args shifted left (1500 landed in the orchWorking bool -> forever
// true) and the ring breathed 24/7 on an idle Balanced board. With named fields a
// dropped line either fails to compile or falls back to these defaults - every
// default is the FAIL-DARK direction (no cue), never a stuck-on light.
struct ComposeOpts {
  uint32_t cursorDecayMs    = 1500;  // idle-panel cursor-glow lifetime
  bool     orchWorking      = false; // a turn is running now -> Calm soft breathe
  uint32_t lastActivityMs   = 0;     // millis() of last sub-agent start/finish
  uint32_t activityWindowMs = 1500;  // Calm blink window after lastActivityMs
  uint8_t  themeHue         = 127;   // tints the Calm working/activity cue
  bool     reveal           = false; // "wake the ring" click: FULL treatment briefly
  uint8_t  attnThemedHue    = 255;   // THEME-resolved top-attention hue; 255 = wire hue
  bool     lowBattCue       = false; // owner opted in to the low-battery light
                                     // (DEFAULT OFF = dark, the fail-safe direction)
  // Orchestrator fan-out split: while sub-sessions are live, Balanced shows the
  // SAME per-session segment split Full shows (at Balanced's own dimmer
  // brightness) instead of collapsing to the single working LED. Set ONLY by
  // the Orchestrator (main.cpp) - the Notifier's Calm stays deliberately
  // single-LED (the all-day-peripheral grammar), and the false default keeps
  // every host rig and the Notifier path byte-identical.
  bool     fanoutSegments   = false;
};

// Compose the current Plan. panelBusy renders the cursor shimmer and holds
// the glow past decay - but only within NIMBUS_CURSOR_SYNC_HOLD_MS of the
// last detent (the interaction's own dwell-render window); unrelated ambient
// or attention renders never resurrect a stale glow.
// opts.orchWorking / lastActivityMs / activityWindowMs drive the Calm-level
// activity cue (ignored in Dark and Full); opts.reveal is the "wake the ring"
// gesture (caller owns its timer); opts.attnThemedHue is the theme-resolved
// attention hue (what makes Dark/Calm honour the theme, owner R3).
Plan compose(const attn::Router& router, const Config& cfg, const Cursor& cursor,
             bool panelBusy, uint32_t nowMs, const ComposeOpts& opts = {});

}  // namespace nimbus::ring
