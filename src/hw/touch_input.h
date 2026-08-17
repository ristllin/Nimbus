#pragma once
#include <cstdint>

// touch_input - turns panel taps into the SAME abstract gestures the EC11
// encoder produces.
//
// The point of this file is that nothing downstream learns touch exists. The
// menu FSM (nimbus::SettingsMenu) already speaks onRotate/onClick/onLongPress,
// and main.cpp already drains its request flags and persists its dirty state;
// a tap is translated into one of those calls and every existing path runs
// unchanged. That is why adding a whole new input device needs no FSM change.
//
// Gestures recognised (deliberately few - this is a resistive panel on a desk
// device, not a phone):
//   Tap        - press and release inside the slop radius, under the hold time
//   Hold       - still down past kHoldMs (drives press-and-hold to talk)
//   Release    - the end of a hold, so recording can stop on lift
//   Swipe      - release displaced vertically past kSwipeMinPx
//
// ⚠ Swipe is SAFE to add precisely because a drag beyond the slop radius already
// produced NOTHING: it fails the tap test and no gesture was emitted. So this
// fills a dead zone rather than competing with Tap, and a mis-read swipe can
// never activate a menu row - which is the one failure this input layer must not
// have (with no knob, activating a row the owner did not touch is unrecoverable
// confusion).

namespace nimbus::hw::touch {

struct Gesture {
  enum class Kind : uint8_t { None, Tap, HoldStart, HoldEnd, SwipeUp, SwipeDown };
  Kind    kind = Kind::None;
  int16_t x = -1, y = -1;   // where it happened (panel px)
};

// Poll the controller and report at most one gesture per call. Cheap; call it
// every loop. Returns Kind::None when nothing happened.
Gesture poll();

// TEST-only injection, mirroring the ENC/SW console seam: lets the HIL suites
// drive the UI with no finger. A queued point is consumed by the next poll().
#ifdef NIMBUS_TEST
// A synthetic press is a QUEUE of poll-steps, not a flag pair: the console
// injects press+release in one dispatch, and a flag pair would be set and
// cleared before poll() ever ran, so no gesture would fire at all.
void injectTap(int16_t x, int16_t y, int downPolls = 1);  // press, then release
void injectDown(int16_t x, int16_t y);                    // press and HOLD
void injectUp();                                          // release a hold
#endif

}  // namespace nimbus::hw::touch
