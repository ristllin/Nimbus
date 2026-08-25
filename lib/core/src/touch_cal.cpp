#include "nimbus/touch_cal.h"

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
  // panel, so the canonical mapping swaps the axes and inverts Y. Held in one place
  // so the fresh-boot default and the web "clear" fallback can never disagree.
  c.swapXY = true;
  c.invertY = true;
  (void)kind;         // both shipping models share this default today; the seam stays per-kind
  return c;
}

Point orientTouch(Point p, bool displayFlipped, int16_t w, int16_t h) {
  if (p.down && displayFlipped) {
    p.x = static_cast<int16_t>(w - 1 - p.x);
    p.y = static_cast<int16_t>(h - 1 - p.y);
  }
  return p;
}

}  // namespace nimbus::touch
