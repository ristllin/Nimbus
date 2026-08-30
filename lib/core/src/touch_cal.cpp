#include "nimbus/touch_cal.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace nimbus::touch {

namespace {

// Split on ',' without allocating a stream. Empty fields are preserved so a
// malformed "200,,300,400" is rejected rather than silently shifting.
std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') { out.push_back(cur); cur.clear(); }
    else if (c != ' ') cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

// Strict unsigned parse: rejects empty, non-digits and anything over the ADC's
// 12-bit range. strtoul would accept "12abc" and "-1".
bool parseU16(const std::string& s, uint16_t& out) {
  if (s.empty() || s.size() > 5) return false;
  uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + uint32_t(c - '0');
  }
  if (v > 4095) return false;   // XPT2046 is 12-bit
  out = uint16_t(v);
  return true;
}

}  // namespace

bool parseCal(const std::string& s, Cal& out) {
  const auto f = split(s);
  if (f.size() < 4 || f.size() > 5) return false;

  Cal c;
  if (!parseU16(f[0], c.minX) || !parseU16(f[1], c.maxX) ||
      !parseU16(f[2], c.minY) || !parseU16(f[3], c.maxY))
    return false;

  // A zero or inverted span would divide by zero or flip the axis silently;
  // inversion is what the flags are for.
  if (c.minX >= c.maxX || c.minY >= c.maxY) return false;

  if (f.size() == 5) {
    uint16_t flags = 0;
    if (!parseU16(f[4], flags) || flags > 7) return false;
    c.swapXY  = (flags & 1) != 0;
    c.invertX = (flags & 2) != 0;
    c.invertY = (flags & 4) != 0;
  }

  out = c;
  return true;
}

std::string formatCal(const Cal& c) {
  const uint16_t flags = uint16_t((c.swapXY ? 1 : 0) | (c.invertX ? 2 : 0) |
                                  (c.invertY ? 4 : 0));
  return std::to_string(c.minX) + "," + std::to_string(c.maxX) + "," +
         std::to_string(c.minY) + "," + std::to_string(c.maxY) + "," +
         std::to_string(flags);
}

Cal boardDefaultCal(TouchKind kind) {
  Cal c;              // nominal min/max span (200..3900); unused on a capacitive panel
  // Both shipping boards mount the controller portrait-native under a landscape
  // panel, so both swap the axes. They differ on the invert, MEASURED per panel -
  // the seam that CUM-203 splits (a shared swap+invertY default mirrored one axis on
  // the resistive Solide out of the box):
  //   - Capacitive (Freenove FT6336U): swap + invertY. Bench-verified at CUM-189.
  //   - Resistive  (Solide S3 XPT2046): swap ONLY. In the CANONICAL (un-flipped)
  //     frame the Solide's landscape mount needs no invert; the 180 for an
  //     upside-down mount is the display flip's job (orientTouch), applied once on
  //     top and never folded in here (folding it in would double-apply). A fresh
  //     Solide boots with tftFlip=0, so this lands taps upright out of the box; it
  //     is exactly what a per-unit tap-the-crosses solves to once the flip is undone
  //     (a unit calibrated with tftFlip=1 stores swap+invertX+invertY = that plus the
  //     180). Held in one place so the fresh-boot default and the web "clear"
  //     fallback can never disagree.
  c.swapXY = true;
  if (kind == TouchKind::Capacitive) c.invertY = true;
  return c;
}

bool solveCornerCal(const RawSample c[4], Cal& out, uint16_t minSpan) {
  const RawSample& tl = c[0];
  const RawSample& tr = c[1];
  const RawSample& bl = c[2];
  const RawSample& br = c[3];
  // Averaged edge deltas (kept doubled - only signs and relative magnitudes matter):
  // how much each raw axis moves when only screen X (dx_*) or only screen Y (dy_*)
  // changes. Averaging the two parallel edges keeps one sloppy corner from flipping
  // the swap decision. Mirrors tools/tcal_wizard.py derive().
  const long dx_rawx = long(tr.x + br.x) - long(tl.x + bl.x);
  const long dx_rawy = long(tr.y + br.y) - long(tl.y + bl.y);
  const long dy_rawx = long(bl.x + br.x) - long(tl.x + tr.x);
  const long dy_rawy = long(bl.y + br.y) - long(tl.y + tr.y);

  const bool swap = std::labs(dx_rawy) > std::labs(dx_rawx);
  uint16_t xv[4], yv[4];
  long xDelta, yDelta;
  if (swap) {  // screen X rides raw Y, screen Y rides raw X
    xv[0] = tl.y; xv[1] = tr.y; xv[2] = bl.y; xv[3] = br.y;
    yv[0] = tl.x; yv[1] = tr.x; yv[2] = bl.x; yv[3] = br.x;
    xDelta = dx_rawy; yDelta = dy_rawx;
  } else {
    xv[0] = tl.x; xv[1] = tr.x; xv[2] = bl.x; xv[3] = br.x;
    yv[0] = tl.y; yv[1] = tr.y; yv[2] = bl.y; yv[3] = br.y;
    xDelta = dx_rawx; yDelta = dy_rawy;
  }

  uint16_t minX = xv[0], maxX = xv[0], minY = yv[0], maxY = yv[0];
  for (int i = 1; i < 4; ++i) {
    minX = std::min(minX, xv[i]); maxX = std::max(maxX, xv[i]);
    minY = std::min(minY, yv[i]); maxY = std::max(maxY, yv[i]);
  }
  // Reject a degenerate press set (all in one spot, or a shorted line): the driver
  // would divide by a near-zero span and every tap would land wrong.
  if (int(maxX) - int(minX) < int(minSpan) || int(maxY) - int(minY) < int(minSpan))
    return false;

  Cal r;
  r.minX = minX; r.maxX = maxX; r.minY = minY; r.maxY = maxY;
  r.swapXY = swap;
  // The driver maps lo->0 and hi->outMax, so a raw value that DECREASES as the
  // screen coordinate grows needs its axis inverted.
  r.invertX = xDelta < 0;
  r.invertY = yDelta < 0;
  out = r;
  return true;
}

void CalWizard::begin(int16_t w, int16_t h, int16_t inset) {
  w_ = w;
  h_ = h;
  // Keep the inset sane for small or odd sizes so targets never cross the middle.
  const int16_t maxInset = int16_t((w_ < h_ ? w_ : h_) / 3);
  inset_ = inset < 0 ? 0 : (inset > maxInset ? maxInset : inset);
  step_ = 0;
}

int16_t CalWizard::targetX(int i) const {
  // Order [tl, tr, bl, br]: left column for tl/bl, right column for tr/br.
  const bool right = (i == 1 || i == 3);
  return right ? int16_t(w_ - 1 - inset_) : inset_;
}

int16_t CalWizard::targetY(int i) const {
  const bool bottom = (i == 2 || i == 3);
  return bottom ? int16_t(h_ - 1 - inset_) : inset_;
}

bool CalWizard::recordRaw(uint16_t rawX, uint16_t rawY) {
  if (done()) return true;
  samples_[step_].x = rawX;
  samples_[step_].y = rawY;
  ++step_;
  return done();
}

bool CalWizard::solve(Cal& out) const {
  if (!done()) return false;
  return solveCornerCal(samples_, out);
}

Point orientTouch(Point p, bool displayFlipped, int16_t w, int16_t h) {
  if (p.down && displayFlipped) {
    p.x = static_cast<int16_t>(w - 1 - p.x);
    p.y = static_cast<int16_t>(h - 1 - p.y);
  }
  return p;
}

}  // namespace nimbus::touch
