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

}  // namespace nimbus::touch
