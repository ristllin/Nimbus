#pragma once
#include <cstdint>

#include "nimbus/profile.h"   // Posture (backlightPctFor)

// ============================================================================
// duty - a pure on/off envelope over wall-clock ms.
//
// Exists for the low-battery ring cue. A warning that a battery is low must not
// itself be a reason the battery goes flat, and a continuous full-brightness red
// breathe on a 45-LED ring is exactly that: it is the brightest thing the device
// does, held indefinitely, precisely when there is least power to spend on it.
// So the cue is reduced to a brief pulse every minute.
//
// Kept portable and header-only (the `nimbus/saver.h` precedent) so the timing
// is host-testable with no hardware and no LEDs - see test/test_duty. The device
// never re-derives any of this.
// ============================================================================

namespace nimbus {

// The low-battery cue: ~3 s lit per minute. Owner-facing copy calls this a
// "dim red pulse"; these are the only two numbers behind it.
constexpr uint16_t kLowBattCueOnMs     = 3000;
constexpr uint16_t kLowBattCuePeriodMs = 60000;

// Relative brightness of the cue in the Full battery mode ONLY (percent).
// ⚠ Deliberately NOT applied in Dark/Calm: their profile brightness is already
// 10/255 and 30/255, and the driver's breathe floors at 0.15, so a further x0.25
// there lands at ~1/255 - indistinguishable from off. In those modes the mode's
// own brightness IS the dimming and the duty cycle alone does the rest.
constexpr uint8_t kLowBattCueBrightPct = 25;

// 1000 while inside the lit window, 0 during the gap.
//
// `periodMs == 0` means NO gating (always 1000) - that is the default everywhere,
// which is what keeps every existing animation byte-identical.
//
// Wrap-safe by construction: the modulo runs on unsigned arithmetic, so the only
// artifact at the ~49.7-day rollover is one shortened cycle (2^32 is not a whole
// multiple of periodMs), never a stuck-on or stuck-off state.
constexpr uint32_t dutyPermille(uint32_t nowMs, uint16_t onMs, uint16_t periodMs) {
  return periodMs == 0 ? 1000u : ((nowMs % periodMs) < onMs ? 1000u : 0u);
}

// Backlight level for a battery mode, percent.
//
// On a reflective e-ink panel the battery mode only changes LEDs. On a colour
// TFT the BACKLIGHT is the single largest continuous draw - larger than the ring
// - so a battery mode that dims the LEDs while leaving the screen at full
// brightness is not actually a battery mode. Dark is meant to disengage.
//
// Kept here rather than in a device file so it is host-testable and so the three
// levels sit next to each other where they can be compared.
// Backlight while the screen is RESTING. Deliberately not zero.
//
// A fully dark colour panel is indistinguishable from a dead one - and that is
// not hypothetical: the owner reported "and now it's black" as a fault symptom
// during a blank-screen investigation, when it was in fact this screensaver
// doing its job, and it cost real time to separate the two. A faint glow says
// "resting", where black says "broken".
//
// The power argument is weak in both directions: at this level the backlight
// draws a few percent of its lit value, so almost all of the saving is kept.
constexpr uint8_t kBacklightRestPct = 6;

constexpr uint8_t backlightPctFor(Posture p) {
  return p == Posture::Dark ? 35        // disengaged, still readable up close
       : p == Posture::Calm ? 65        // comfortable indoors
                            : 100;      // desk display, full brightness
}

}  // namespace nimbus
