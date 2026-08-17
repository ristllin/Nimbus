#pragma once
#include "nimbus/ring_plan.h"

// ring_out - device glue from a portable ring::Plan to solide::leds.
//
// Full ring level maps 1:1 onto the solide agent-segment API (agentStatus /
// agentAccent / agentProgress), which is the same status vocabulary the plan
// already speaks. The non-Full levels (Dark/Calm) want a single dim breathing
// LED, which the current solide::leds public API cannot express (segments own the
// whole ring); see docs/plan.md open question #4. Until an upstream pixel
// primitive lands, Dark/Calm approximate with a dim full-ring pulse when
// attention is present and go dark otherwise - documented, not final.

namespace nimbus::hw {

// Apply the plan to the LED ring. Full ring level drives nimbus::ring::Animator
// (birth grow-in / hue crossfade / done ripple / termination collapse) from
// job-status deltas - the raw-frame path (solide::leds::showFrame), unblocked
// 2026-07 by solide-drivers v0.2.0. Dark/Calm are UNCHANGED for now (still
// the Pattern::Pulse approximation; the single attention LED doesn't map onto
// the Animator's segment-arc model - see docs/plan.md open question #4 / task
// notes). Tracks which segment keys+statuses are live so keys dropped since the
// last call collapse out (Animator::terminated) instead of just vanishing.
void applyRingPlan(const ring::Plan& plan);

// Advance the Active-posture Animator and push a fresh frame when it's time
// (rate-limited internally to ~30 FPS - this call is cheap to make every main
// loop iteration). No-op in Dark/Calm levels or whenever applyRingPlan() hasn't
// put the Animator in control. Segment motion (ripple/grow/collapse) needs
// this EVEN between plan updates - that's the whole point of the animation.
void tickAnimation(uint32_t nowMs);

// Drop the animator's claim on the ring (clears the raw frame + the active
// latch) so another raw-frame owner (the OTA progress bar) can take it without
// the 30 FPS animator racing underneath. applyRingPlan() re-arms it later.
void stopAnimation();

// Set the Full-posture animation frame cadence from the RingFps knob (fps clamped
// 5..60). Wires Param::RingFps to real hardware - it was a no-op menu/web control
// while the cadence was a compile-time constant.
void setRingFps(int fps);

// While true, applyRingPlan() does NOT write the global LED brightness - the
// caller (the wake-reveal's eased envelope in loop()) owns it. Prevents the
// full-brightness snap mid-fade on every working-driven refreshRing() (P2.4).
void setBrightnessHold(bool held);

// Global LED-brightness safety cap (owner feature 2026-07-17): applyRingPlan clamps
// every plan to this. Main refreshes it each loop from the brightOvr knob -
// kBrightCap (60%) normally, 255 when the owner/AI accepted the heat risk.
void setBrightnessCap(uint8_t cap);

}  // namespace nimbus::hw
