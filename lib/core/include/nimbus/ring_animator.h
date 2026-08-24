#pragma once
#include "nimbus/duty.h"
#include <cstdint>

#include "nimbus/profile.h"   // Posture
#include "solide/ring.h"      // RGB, hsv, breatheLevel, cometHead/Falloff (host-tested)

// ring_animator - the MOTION layer of the LED experience (docs/led-ux.md).
//
// ring_plan decides the STEADY state (which segments, what color). Animator
// renders those decisions with purposeful TRANSITIONS: per-segment lifecycle
// (birth grow-in, done ripple, termination collapse, hue crossfade) in Full, and
// a single dim breathing cue in Dark/Calm. (Boot/connect LED feedback is driven
// separately from main.cpp via the driver's self-animating Pattern - it does not
// route through here.)
//
// Pure + deterministic: the clock is passed in, so a full RGB frame is a
// function of (events so far, nowMs). Host-tested by asserting exact LED colors
// at chosen instants during each transition - the golden-frame equivalent for
// the ring. Device glue pushes the frame to a raw WS2812 primitive
// (solide::leds::showFrame - a small upstream addition; until then this layer
// ships dark-launched and host-verified).

namespace nimbus::ring {

using solide::ring::RGB;

#ifndef ANIM_MAX_SEGMENTS
#define ANIM_MAX_SEGMENTS 8
#endif

// Candidate "working" (Running) animations (CUM-42). These are alternate renderings
// of the one animated steady state the ambient grammar allows - the processing slide -
// behind a selector, so Roy can A/B them on the ring, the on-screen ring, and the web
// simulator and pick one. Every variant is a DETERMINISTIC function of (nowMs, arc
// geometry) so the three surfaces render byte-identically and the host tests can pin
// exact frames. The default is CometTail, which reproduces the shipped comet exactly,
// so nothing changes on any device until a variant is selected.
enum class RunStyle : uint8_t {
  CometTail   = 0,  // variant 1 (default): sliding head + smoothly fading tail
  CometSparks = 1,  // variant 2: comet head + sparse trailing embers that linger and die
  DualComet   = 2,  // variant 3: two comet heads chasing on opposite sides of the arc
  BreatheArc  = 3,  // variant 4: the whole arc breathes gently in its hue
  Fireflies   = 4,  // variant 5: sparse per-LED twinkles drifting across the arc
};
constexpr uint8_t kRunStyleCount = 5;

class Animator {
 public:
  void configure(int ledCount, Posture posture, uint8_t brightness);

  // ---- per-segment lifecycle (key = stable job id) ----
  void born(uint32_t key, uint8_t hue, uint32_t nowMs);       // grow-in + rainbow settle
  void setHue(uint32_t key, uint8_t hue, uint32_t nowMs);     // crossfade to new hue
  // Per-status steady-state MOTION + relative brightness (nimbus::statusStyle): Running
  // slides (Comet); WaitingInput/AwaitingApproval/Error breathe (~2.6 s, no strobes);
  // Done fades to an ember; Idle is a static dim glow.
  // anim is a solide::ring::Anim; brightPct 0..100. Idempotent (cheap to call each plan).
  void setAnim(uint32_t key, uint8_t anim, uint8_t brightPct);
  void done(uint32_t key, uint32_t nowMs);                    // success ripple -> ember
  void revive(uint32_t key);   // Done -> running again: cancel a mid-flight ripple
                               // (kills the stray done-sweep amid a working comet, P2.4)
  void terminated(uint32_t key, uint32_t nowMs);              // collapse + free the slot
  void clear();  // drop ALL segments immediately (no collapse) - call when leaving
                 // Full posture so a later reveal/Full re-entry shows no ghost arcs
                 // of sessions that ended while the ring was Dark/Calm (audit F5).

  // Dark/Calm SINGLE-CUE mode (the "one renderer, all postures" refactor): instead
  // of segment arcs, the ring shows ONE dim breathing cue over the whole ring - the
  // plan's single-LED attention - rendered by THIS same raw-frame path (same table,
  // gamma and constants as Full), replacing the driver's hardcoded Pattern::Pulse
  // fork. frame() renders the cue whenever the configured posture is not Full.
  // hue: 0-254, 255=white. anim: solide::ring::Anim (Breathe/Solid; strobes banned).
  void setSingleCue(bool lit, uint8_t hue, uint8_t anim, uint16_t periodMs);
  // Rainbow theme ("purely for looks", owner 2026-07-16): arcs cycle a rotating
  // full-wheel gradient instead of their static role hue. alertHue = the theme's
  // Error hue - segments carrying it keep their FIXED colour (red stays reserved),
  // as does white/255 (idle stays dull).
  void setRainbow(bool on, uint8_t alertHue);
  // Periodic on/off envelope over the WHOLE frame (the low-battery cue).
  // periodMs == 0 = no gating, which is the default - every existing animation
  // renders byte-identically unless a caller explicitly arms this.
  // ⚠ Gating the whole FRAME rather than a segment's brightness is deliberate:
  // Seg::bright is honoured only in the Steady phase, so a per-segment dim would
  // still flash at full brightness through the ~350 ms birth grow-in.
  void setCueEnvelope(uint16_t onMs, uint16_t periodMs) {
    envOnMs_ = onMs;
    envPeriodMs_ = periodMs;
  }

  // Select the "working" (Running) animation variant (CUM-42). Idempotent; affects
  // only the Comet/Running steady state - needs-you states keep their calm breathe.
  void setRunStyle(RunStyle s) { runStyle_ = s; }
  RunStyle runStyle() const { return runStyle_; }

  // Render the current frame into out[0..n) at nowMs. n should be ledCount.
  void frame(uint32_t nowMs, RGB* out, int n);

  // Arc-centre LED of the ordinal-th ACTIVE segment in the frame just rendered
  // (-1 = no such segment). The cursor glow resolves through THIS - computing a
  // separate layout in main drifted off-centre whenever the animator's own
  // segment set differed mid-grow/collapse (owner: "indicator not centered").
  int arcCenter(int ordinal) const {
    if (ordinal < 0 || ordinal >= lastPlaced_) return -1;
    return (lastSpans_[ordinal].start + lastSpans_[ordinal].len / 2) % lastN_;
  }

  int liveCount() const;  // segments not yet fully collapsed

 private:
  enum class Phase : uint8_t { Grow, Steady, Ripple, Dying };
  struct Seg {
    bool     used = false;
    uint32_t key = 0;
    Phase    phase = Phase::Steady;
    uint32_t phaseStart = 0;
    uint8_t  hue = 0;         // current target hue
    uint8_t  fromHue = 0;     // crossfade source
    uint32_t hueStart = 0;    // crossfade start (0 = no active crossfade)
    uint8_t  anim = 1;        // solide::ring::Anim (1 = Solid) - steady-state motion
    uint8_t  bright = 100;    // per-status relative brightness 0-100
  };

  int find(uint32_t key) const;
  int alloc(uint32_t key);
  RGB scaled(RGB c) const;  // apply brightness cap

  struct SingleCue {
    bool     lit = false;
    uint8_t  hue = 255;
    uint8_t  anim = 1;          // solide::ring::Anim (1 = Solid)
    uint16_t periodMs = 2600;
  };

  int      leds_ = 45;
  Posture  posture_ = Posture::Full;
  uint8_t  bright_ = 30;
  RunStyle runStyle_ = RunStyle::CometTail;   // CUM-42 selector; default = shipped comet
  SingleCue single_;
  bool     rainbow_ = false;      // active theme is "rainbow" -> cycling hues
  uint8_t  rainbowAlert_ = 0;     // the theme's alert hue (exempt from cycling)
  uint16_t envOnMs_ = 0;          // whole-frame duty envelope; 0 period = off
  uint16_t envPeriodMs_ = 0;
  Seg      segs_[ANIM_MAX_SEGMENTS];
  solide::ring::Span lastSpans_[ANIM_MAX_SEGMENTS] = {};  // spans of the last frame
  int      lastPlaced_ = 0;                              // (for arcCenter)
  int      lastN_ = 1;
};

}  // namespace nimbus::ring
