#pragma once
#include <cstdint>
#include <string>

// ============================================================================
// touch_cal - the portable encode/parse for a resistive-touch calibration.
//
// A resistive panel's raw ADC range varies per unit, so the mapping from counts
// to pixels has to be MEASURED on the device and then stored. Without a way to
// set it after flashing, a wrong guess is indistinguishable from broken
// hardware: every tap simply lands somewhere else.
//
// Wire format is one short string so it fits an NVS key and a form field:
//   "minX,maxX,minY,maxY,flags"      flags: bit0 swapXY, bit1 invertX, bit2 invertY
// e.g. "200,3900,240,3850,4" = the defaults with Y inverted.
//
// Parsing is here (portable, host-tested) rather than in the console or the web
// handler so both surfaces agree and neither can drift.
// ============================================================================

namespace nimbus::touch {

struct Cal {
  uint16_t minX = 200, maxX = 3900;
  uint16_t minY = 200, maxY = 3900;
  bool swapXY = false, invertX = false, invertY = false;

  bool operator==(const Cal& o) const {
    return minX == o.minX && maxX == o.maxX && minY == o.minY && maxY == o.maxY &&
           swapXY == o.swapXY && invertX == o.invertX && invertY == o.invertY;
  }
};

// Parse "minX,maxX,minY,maxY[,flags]". Returns false and leaves `out` untouched
// on anything malformed - a half-applied calibration is worse than the default,
// because it looks deliberate.
//
// Rejects min >= max on either axis: that would divide by zero or invert the
// axis silently, and inversion has its own explicit flag.
bool parseCal(const std::string& s, Cal& out);

// The inverse, for round-tripping through NVS and the web UI.
std::string formatCal(const Cal& c);

// Which touch controller a board carries. A portable mirror of solide::TouchKind
// so lib/core stays driver-free and this default is host-testable.
enum class TouchKind : uint8_t { Resistive = 0, Capacitive = 1 };

// The per-board-model DEFAULT touch calibration (CUM-189). Applied when NVS has no
// per-unit calibration (a freshly flashed board) AND when the owner clears the
// calibration from the web UI. This is the SINGLE source both the boot path
// (src/main.cpp) and the web-clear path (src/net/webui.cpp) read, so "clear" and
// "fresh boot" can never restore different orientations. Measured at board
// bring-up.
//
// Only the ORIENTATION FLAGS are board-model here: a capacitive panel (FT6336U)
// reports pixel coordinates, so its min/max span is unused; a resistive panel
// (XPT2046) keeps its driver-measured min/max span and this only pins the flags it
// already defaults to. Both shipping boards mount the controller portrait-native
// under a landscape panel, so the canonical mapping swaps the axes and inverts Y.
// A board that measures a different orientation changes ONLY this one function.
Cal boardDefaultCal(TouchKind kind);

// ============================================================================
// Orientation (CUM-160): reconciling touch with the display's 180 flip.
//
// A calibrated touch point is in the CANONICAL (un-flipped) landscape frame - the
// Cal above maps the controller's fixed axes onto that frame and NOTHING else. The
// panel can be turned 180 (which end of the landscape is up), a display-only MADCTL
// change tracked by store::tftFlip. If touch did not follow, taps would land at the
// diagonally opposite point - the field-reported "touch reversed 180".
//
// The rule this function enforces: the DISPLAY FLIP is the SINGLE source of truth
// for the 180. It is applied here, once, on top of the calibration - never baked
// into the Cal (which would double-apply against this and put taps 180 out). Both
// the device poll (src/hw/touch_input.cpp) and any host test go through this one
// function, so the two can never drift.
// ============================================================================

struct Point {
  int16_t x = -1, y = -1;
  bool down = false;
};

// Apply the 180 display flip - and ONLY the 180 - to a calibrated landscape point.
// `w`,`h` are the landscape screen dimensions. A point that is not down passes
// through untouched (there is no live coordinate to mirror).
Point orientTouch(Point p, bool displayFlipped, int16_t w, int16_t h);

}  // namespace nimbus::touch
