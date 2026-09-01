#include "touch_input.h"

#include <Arduino.h>

#include <cstdlib>

#include "solide/board.h"         // board().touchKind - resistive vs capacitive
#include "solide/touch.h"
#include "solide/display_tft.h"   // kW/kH + flipped() - the 180 source of truth
#include "nimbus/touch_cal.h"     // orientTouch + belowUncalFloor (CUM-160/CUM-245)

namespace nimbus::hw::touch {

namespace {

// Past this, a press is a hold rather than a tap. Matches the encoder's
// BTN_LONGPRESS_MS so hold-to-talk feels the same on both input devices.
constexpr uint32_t kHoldMs = 600;

// A resistive panel wanders a few pixels under a stationary finger; movement
// beyond this cancels the tap (treated as a drag, which nothing consumes yet).
constexpr int kSlopPx = 18;
// Vertical travel before a drag counts as a scroll. Comfortably larger than the
// tap slop so the two can never be confused on a panel this size.
constexpr int kSwipeMinPx = 40;

// True while a fresh resistive panel is still uncalibrated: poll() then rejects any
// read whose raw pressure is below the uncalibrated floor, so a drifting/floating
// XPT2046 cannot stream phantom presses (CUM-245). Cleared once a cal is stored; the
// calibrated panel keeps the driver's own sensitive threshold.
bool     s_uncalFloor = false;

// The panel this build carries: only a RESISTIVE controller drifts per unit and needs
// the uncalibrated pressure floor. A capacitive panel reports a fixed pressure flag, so
// the floor never bites there anyway - this just makes that explicit and driver-true.
inline bool resistivePanel() {
  return solide::board().touchKind == solide::TouchKind::ResistiveSpi;
}

bool     s_down = false;
bool     s_holdFired = false;
uint32_t s_downMs = 0;
int16_t  s_downX = -1, s_downY = -1;
int16_t  s_lastX = -1, s_lastY = -1;

#ifdef NIMBUS_TEST
// A synthetic press is a QUEUE of poll-steps, not a pair of flags. The console
// injects press+release in ONE dispatch, so a flag pair would be set and
// cleared before poll() ever ran and no gesture would fire at all - the tap
// seam would look wired while being completely inert.
//   s_injHold  = stay down until injectUp() (press-and-hold)
//   s_injTicks = number of further polls to report "down" before releasing,
//                which must be >= 1 so press and release land on SEPARATE
//                polls, exactly as a real finger does.
bool    s_injActive = false, s_injHold = false;
int     s_injTicks = 0;
int16_t s_injX = -1, s_injY = -1;
#endif

inline int dist2(int ax, int ay, int bx, int by) {
  const int dx = ax - bx, dy = ay - by;
  return dx * dx + dy * dy;
}

}  // namespace

#ifdef NIMBUS_TEST
void injectDown(int16_t x, int16_t y) {
  s_injActive = true; s_injHold = true;   // held until injectUp()
  s_injX = x; s_injY = y; s_injTicks = 0;
}
void injectTap(int16_t x, int16_t y, int downPolls) {
  s_injActive = true; s_injHold = false;
  s_injX = x; s_injY = y;
  s_injTicks = downPolls < 1 ? 1 : downPolls;   // >= 1: press and release must
                                                // land on different polls
}
void injectUp() { s_injHold = false; s_injTicks = 0; }
#endif

void setUncalibratedFloor(bool on) { s_uncalFloor = on; }

Gesture poll() {
  bool    down = false;
  int16_t x = -1, y = -1;

#ifdef NIMBUS_TEST
  if (s_injActive) {
    // Report DOWN this poll; a non-hold press then expires so the NEXT poll
    // sees the release and the press/release pair becomes a real Tap.
    down = true; x = s_injX; y = s_injY;
    if (!s_injHold && --s_injTicks <= 0) s_injActive = false;
  } else {
#endif
    solide::touch::Point p = solide::touch::read();
    // Uncalibrated pressure floor (CUM-245): on a fresh resistive panel, reject a read
    // whose raw pressure is below the firmer uncalibrated floor, so a drifting/floating
    // XPT2046 cannot stream phantom presses into the UI. Applied here (not in the byte-
    // pinned driver) and only while uncalibrated; a calibrated or capacitive panel is
    // untouched. Demote to not-down so the rest of the gesture machine sees a clean up.
    if (nimbus::touch::belowUncalFloor(s_uncalFloor, resistivePanel(), p.pressure))
      p.down = false;
    // The display flip is the SINGLE source of truth for the 180 (nimbus::touch::
    // orientTouch, host-tested): the calibration maps to the un-flipped landscape,
    // and the flip is applied there exactly once, so taps can never double-apply or
    // land at the diagonally opposite spot. Only the real read is oriented; the
    // test-inject path above already carries logical (post-flip) coordinates.
    const nimbus::touch::Point tp = nimbus::touch::orientTouch(
        nimbus::touch::Point{static_cast<int16_t>(p.x), static_cast<int16_t>(p.y), p.down},
        solide::display_tft::flipped(), solide::display_tft::kW, solide::display_tft::kH);
    down = tp.down; x = tp.x; y = tp.y;
#ifdef NIMBUS_TEST
  }
#endif

  const uint32_t now = millis();
  Gesture g;

  if (down && !s_down) {                       // press
    s_down = true;
    s_holdFired = false;
    s_downMs = now;
    s_downX = x; s_downY = y;
    s_lastX = x; s_lastY = y;
    return g;                                  // nothing decided yet
  }

  if (down && s_down) {                        // still held
    s_lastX = x; s_lastY = y;
    if (!s_holdFired && now - s_downMs >= kHoldMs &&
        dist2(x, y, s_downX, s_downY) <= kSlopPx * kSlopPx) {
      s_holdFired = true;
      g.kind = Gesture::Kind::HoldStart;
      g.x = s_downX; g.y = s_downY;            // report where it STARTED
    }
    return g;
  }

  if (!down && s_down) {                       // release
    s_down = false;
    if (s_holdFired) {
      g.kind = Gesture::Kind::HoldEnd;
      g.x = s_downX; g.y = s_downY;
    } else if (dist2(s_lastX, s_lastY, s_downX, s_downY) <= kSlopPx * kSlopPx) {
      // A tap is reported at the PRESS point: on release the finger has often
      // slid, and the user means the thing they put their finger on.
      g.kind = Gesture::Kind::Tap;
      g.x = s_downX; g.y = s_downY;
    } else {
      // Beyond the slop radius. This used to emit NOTHING, so a drag was simply
      // ignored - which is why a long menu had no way to scroll without a knob.
      //
      // ⚠ Requires the movement to be mostly VERTICAL and past kSwipeMinPx. A
      // resistive panel drifts under a moving finger, so demanding the dominant
      // axis (rather than any displacement) is what keeps a sloppy tap from
      // registering as a scroll. Anything ambiguous still falls through to no
      // gesture at all, which is the safe outcome.
      const int dx = s_lastX - s_downX;
      const int dy = s_lastY - s_downY;
      if (abs(dy) >= kSwipeMinPx && abs(dy) > abs(dx)) {
        g.kind = dy < 0 ? Gesture::Kind::SwipeUp : Gesture::Kind::SwipeDown;
        g.x = s_downX; g.y = s_downY;
      }
    }
    s_holdFired = false;
  }
  return g;
}

}  // namespace nimbus::hw::touch
