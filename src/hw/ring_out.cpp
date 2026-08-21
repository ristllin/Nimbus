#include "hw/ring_out.h"
#include "nimbus/power/bright_cap.h"

#include <solide/leds.h>
#include <solide/board.h>   // board().hasRing - suppress the physical push on ringless boards

#include "nimbus/fault.h"    // resilience: simulated LED-ring fault (freezes the push)
#include "nimbus/ring_animator.h"
#include "nimbus/status_style.h"   // per-status animation pattern (single source)
#include "nimbus_config.h"  // NIMBUS_RING_LEDS

namespace nimbus::hw {

namespace {
// Keys + last-seen status applied on the previous call, so we can detect a
// Done TRANSITION (not just liveness) and free keys that vanish (Offline).
// Bounded by the job-table capacity (RING_MAX_SEGMENTS=8), NOT the physical
// LED count (NIMBUS_RING_LEDS=45) - those are different constants.
uint32_t            g_live[RING_MAX_SEGMENTS];
solide::ring::Status g_liveStatus[RING_MAX_SEGMENTS];
int                 g_liveN = 0;

int findLive(uint32_t key) {
  for (int i = 0; i < g_liveN; ++i)
    if (g_live[i] == key) return i;
  return -1;
}

// Active-posture raw-frame renderer. g_animActive tracks whether showFrame()
// currently owns the physical ring (vs. Passive's Pattern::Pulse); the periodic
// push lives in tickAnimation() so lifecycle motion (grow/ripple/collapse)
// keeps animating between plan updates, not just on job-state edges.
nimbus::ring::Animator g_anim;
bool                   g_animActive = false;
bool                   g_wasFullMode = false;   // last applyRingPlan drove Full segments
uint32_t               g_lastAnimTick = 0;
uint32_t               g_animTickMs = 33;  // frame cadence; driven by the RingFps knob
                                           // (was a hardcoded constant -> RingFps was a
                                           // no-op menu/web control; audit P11).
solide::ring::RGB      g_animBuf[NIMBUS_RING_LEDS];
ring::CursorGlow       g_cursorGlow;   // stashed from the last plan for the glow overlay

// A board with no physical ring (led.count == 1 status pixel) still composes the
// full 45-color frame into g_animBuf - the notifier renders it on the panel
// instead (see nimbus::hw::currentRingFrame + the TFT status screen). Suppress
// only the WS2812 push; everything upstream runs unchanged so the on-screen ring
// is a faithful mirror of what the LEDs would have shown.
inline bool ringIsPhysical() { return solide::board().hasRing; }
inline void pushRingFrame() {
  if (ringIsPhysical()) solide::leds::showFrame(g_animBuf, NIMBUS_RING_LEDS);
}
}  // namespace

void setRingFps(int fps) {
  if (fps < 5) fps = 5;
  if (fps > 60) fps = 60;
  g_animTickMs = 1000u / uint32_t(fps);
}

// Reveal brightness hold (P2.4): while the wake-reveal's eased envelope owns the
// global brightness (main loop writes it every iteration), applyRingPlan must NOT
// fight it - during an Orchestrator turn refreshRing() fires on every sub-agent
// edge, and each call snapped brightness back to full mid-fade = visible flicker.
static bool g_brightnessHeld = false;
void setBrightnessHold(bool held) { g_brightnessHeld = held; }

// 60%-of-255 unless overridden - sustained ~75% output has been shown to
// overheat and damage adjacent electronics. Main
// refreshes this every loop, so a config change applies within one iteration.
static uint8_t g_brightnessCap = nimbus::power::kBrightCap;
void setBrightnessCap(uint8_t cap) { g_brightnessCap = cap; }

void applyRingPlan(const ring::Plan& plan) {
  // Resilience: FAULT led freezes the physical ring - compose()/attention routing
  // upstream keep running, only the LED push is suppressed, so the rest of the
  // system is exercised as if the ring peripheral had gone dark.
  if (nimbus::fault::active(nimbus::fault::LED)) return;
  if (!g_brightnessHeld)
    solide::leds::setBrightness(plan.brightness > g_brightnessCap ? g_brightnessCap
                                                                  : plan.brightness);
  g_cursorGlow = plan.cursor;   // for tickAnimation's glow overlay (Active only)

  if (plan.posture != Posture::Full) {
    // ONE renderer, all postures: Dark/Calm now render the single attention cue
    // through the SAME Animator raw-frame path as Full (posture is a MODE, not a
    // fork to the driver's Pattern engine). This ends the Pattern::Pulse fork - it
    // was hardcoded to the driver's blue @ 2000 ms and gamma-corrected while the
    // Full path is linear (the audit's P12 gamma split), and it discarded
    // plan.single.hue/anim/period. Now a Dark/Calm ERROR breathes RED, the colour +
    // gamma match Full, and AttnPeriodMs is finally live on hardware.
    if (g_wasFullMode) {
      g_anim.clear();   // drop Full's segment table on the transition so a later
      g_liveN = 0;      // reveal / Full re-entry shows no ghost arcs (audit F5)
    }
    g_wasFullMode = false;
    g_anim.configure(NIMBUS_RING_LEDS, plan.posture, plan.brightness);
    g_anim.setRainbow(plan.rainbow, plan.alertHue);
    g_anim.setCueEnvelope(plan.lowBattCue ? nimbus::kLowBattCueOnMs : 0,
                          plan.lowBattCue ? nimbus::kLowBattCuePeriodMs : 0);
    g_anim.setSingleCue(plan.single.lit, plan.single.hue, plan.single.anim,
                        plan.single.periodMs);
    if (!plan.single.lit) {
      // Nothing needs you: release raw-frame ownership and turn the ring off.
      if (g_animActive) { solide::leds::clearFrame(); g_animActive = false; }
      solide::leds::off();
      // Zero the frame buffer too. On a ringless board the panel mirrors
      // g_animBuf (currentRingFrame) at 30 fps; without this it would keep
      // pushing whatever was last painted (a frozen "stuck" ring) after the
      // ring goes dark. Physical-ring boards are already off, so this is free.
      for (auto& px : g_animBuf) px = solide::ring::RGB{0, 0, 0};
      return;
    }
    g_animActive = true;   // tickAnimation drives the breathe + feeds the driver's
                           // 500 ms raw-frame staleness watchdog (RingFps clamps >= 5,
                           // so the tick is <= 200 ms - always fresh).
    g_anim.frame(millis(), g_animBuf, NIMBUS_RING_LEDS);   // paint now, not next tick
    pushRingFrame();
    return;
  }
  g_wasFullMode = true;

  // Full: drive the Animator's per-segment lifecycle from status DELTAS
  // (not the raw agent-segment API - showFrame() takes over the whole ring at
  // highest precedence, so the Full level now renders exclusively through the
  // Animator; solide::leds::agentStatus/agentAccent are not called here).
  g_anim.configure(NIMBUS_RING_LEDS, plan.posture, plan.brightness);
  g_anim.setRainbow(plan.rainbow, plan.alertHue);
  g_anim.setCueEnvelope(plan.lowBattCue ? nimbus::kLowBattCueOnMs : 0,
                        plan.lowBattCue ? nimbus::kLowBattCuePeriodMs : 0);
  g_animActive = true;

  const uint32_t now = millis();
  for (int i = 0; i < plan.segCount; ++i) {
    const solide::ring::Slot& s = plan.segs[i];
    // Reuse solide's own Status->{hue,anim} table (styleFor) for the default
    // color when the job carries no provider accent - keeps Active-posture
    // colors identical to what the old direct agentStatus() path showed, so
    // switching render paths doesn't silently change what jobs look like.
    const uint8_t hue = s.hasAccent ? s.accentHue : solide::ring::styleFor(s.status).hue;
    const int li = findLive(s.key);
    if (li < 0) {
      g_anim.born(s.key, hue, now);
    } else if (s.status == solide::ring::Status::Done &&
               g_liveStatus[li] != solide::ring::Status::Done) {
      g_anim.setHue(s.key, hue, now);     // crossfade to the DONE role hue so the
                                          // ripple sweeps themed, not in the previous
                                          // status's colour (review finding on P2.4)
      g_anim.done(s.key, now);            // success ripple -> ember, once
    } else {
      if (g_liveStatus[li] == solide::ring::Status::Done &&
          s.status != solide::ring::Status::Done)
        g_anim.revive(s.key);             // Done -> live again: cancel a mid-flight
                                          // ripple (stray done-sweep flicker, P2.4)
      g_anim.setHue(s.key, hue, now);     // no-op if unchanged (Animator dedupes)
    }
    // (progress fill is an e-ink concept - the ring carries status by colour +
    // motion, not a fractional bar; job.progress renders on the panel, not here.)
    // Per-status MOTION + relative brightness. The desk idle-HEARTBEAT (synthetic
    // key) renders at FULL per-status brightness: it's already dimmed by the global
    // kIdleGlowBrightness cap, and the Idle 20% (meant for idle SESSIONS) on top of
    // that crushed it to ~5/255 near-black (audit). Real idle sessions keep the 20%.
    const nimbus::StatusStyle ss = nimbus::statusStyle(s.status);
    // The low-battery cue is recessive: it carries its own relative brightness so
    // it reads as "for your information" rather than as an alarm. Keyed off
    // plan.lowBattCue, NOT off kAttnGlowKey - the orchestrator's "I have a
    // question" arc shares that key and must stay at full brightness.
    const uint8_t brightPct =
        (s.key == nimbus::ring::kIdleGlowKey) ? 100
        : (s.key == nimbus::ring::kAttnGlowKey && plan.lowBattCue) ? plan.cueBrightPct
        : ss.brightPct;
    g_anim.setAnim(s.key, uint8_t(ss.anim), brightPct);
  }

  // Free any key that was live last time but isn't in this plan (collapse
  // animation, not an instant vanish).
  for (int i = 0; i < g_liveN; ++i) {
    bool still = false;
    for (int j = 0; j < plan.segCount; ++j)
      if (plan.segs[j].key == g_live[i]) { still = true; break; }
    if (!still) g_anim.terminated(g_live[i], now);
  }

  // Record the new live set + statuses for the next delta.
  g_liveN = plan.segCount < RING_MAX_SEGMENTS ? plan.segCount : RING_MAX_SEGMENTS;
  for (int i = 0; i < g_liveN; ++i) {
    g_live[i] = plan.segs[i].key;
    g_liveStatus[i] = plan.segs[i].status;
  }
}

void stopAnimation() {
  if (!g_animActive) return;
  solide::leds::clearFrame();
  g_animActive = false;
}

void tickAnimation(uint32_t nowMs) {
  if (nimbus::fault::active(nimbus::fault::LED)) return;   // resilience: ring frozen
  if (!g_animActive) return;
  if (uint32_t(nowMs - g_lastAnimTick) < g_animTickMs) return;
  g_lastAnimTick = nowMs;
  g_anim.frame(nowMs, g_animBuf, NIMBUS_RING_LEDS);
  // Session-cursor glow: blink the focused session's LED in its accent hue on top
  // of the segment frame. g_cursorGlow.index is a PHYSICAL LED index - main.cpp
  // remaps the session-cursor ordinal to the focused arc's CENTRE via
  // arcCenterLed() (real layout() spans) before applyRingPlan; Full posture only
  // (Dark/Calm draw no per-segment glow, so the raw ordinal never renders there).
  int glowLed = -1;
  if (g_cursorGlow.active)
    glowLed = g_anim.arcCenter(int(g_cursorGlow.index));   // live spans = exact centre
  // No raw-ordinal fallback (audit): when arcCenter misses - right after a session
  // dies, or in single-cue Dark/Calm where no segments are placed - the ordinal is
  // a PHYSICAL index that lands on an unrelated LED (the "cursor crawls LED-by-LED"
  // bug the ordinal redesign fixed). A missing 33 ms glow beats a wrong one.
  if (glowLed >= 0 && glowLed < NIMBUS_RING_LEDS) {
    // Soft locate-pulse, not a hard blink (ambient grammar, owner 2026-07-16):
    // the centre LED breathes 15-100% at 800 ms - still finds the eye during the
    // ~8 s window, no strobe. (Also fixes the hue scale: hsv() wants a 16-bit
    // wheel, so the raw 8-bit accent hue rendered red-ish for every theme.)
    const uint8_t lvl = solide::ring::breatheLevel(nowMs, 800);
    const solide::ring::RGB g = (g_cursorGlow.hue == 255)
        ? solide::ring::RGB{lvl, lvl, lvl}
        : solide::ring::hsv(uint16_t(g_cursorGlow.hue) * 257, 255, lvl);
    g_animBuf[glowLed] = g;
  }
  pushRingFrame();
}

// The current 45-entry composited ring frame (segments + cursor glow + envelopes),
// exactly what the physical LEDs would show. On a ringless board the notifier
// draws these on the panel instead. Always valid; kept fresh by tickAnimation().
// ⚠ MUST be called on the MAIN task. g_animBuf is written by tickAnimation()/
// applyRingPlan(), which also run on the main loop, so the read is race-free ONLY
// because rendering is on that same task. If TFT rendering ever moves to its own
// task, snapshot g_animBuf under a lock instead of exposing it directly.
const solide::ring::RGB* currentRingFrame() { return g_animBuf; }
int currentRingCount() { return NIMBUS_RING_LEDS; }

void paintRingSolid(uint8_t r, uint8_t g, uint8_t b) {
  for (auto& px : g_animBuf) px = solide::ring::RGB{r, g, b};
}

}  // namespace nimbus::hw
