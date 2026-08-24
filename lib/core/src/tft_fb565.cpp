#include "nimbus/tft_render/fb565.h"

#include <algorithm>

#include "nimbus/font5x7.h"

// Portable RGB565 framebuffer. Every primitive clips to the panel, so callers
// can draw partially off-screen without guards.
//
// Pixels are STORED big-endian - the ILI9341's wire order - so data() blits
// with no conversion. Callers always pass and receive LOGICAL colours; the
// swap is confined to set()/get().

namespace nimbus::tft {

namespace {

// Letter spacing for the caption/label style (the web .k tracking).
constexpr int kLabelTrack = 1;

}  // namespace

bool Fb565::clipRect(int& x, int& y, int& w, int& h) const {
  if (w <= 0 || h <= 0 || x >= w_ || y >= h_) return false;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w <= 0 || h <= 0) return false;
  if (w > w_ - x) w = w_ - x;
  if (h > h_ - y) h = h_ - y;
  return w > 0 && h > 0;
}

std::string asciiSanitize(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    const uint8_t u = uint8_t(c);
    if (u >= 32 && u <= 126) out.push_back(char(u));
  }
  return out;
}

void Fb565::clear(uint16_t colour) {
  std::fill(buf_.begin(), buf_.end(), swap(colour));
}

void Fb565::set(int x, int y, uint16_t colour) {
  if (x < 0 || x >= w_ || y < 0 || y >= h_) return;
  buf_[size_t(y) * size_t(w_) + size_t(x)] = swap(colour);
}

uint16_t Fb565::get(int x, int y) const {
  if (x < 0 || x >= w_ || y < 0 || y >= h_) return 0;
  return swap(buf_[size_t(y) * size_t(w_) + size_t(x)]);
}

void Fb565::fillRect(int x, int y, int w, int h, uint16_t colour) {
  if (!clipRect(x, y, w, h)) return;
  const uint16_t v = swap(colour);
  for (int yy = y; yy < y + h; yy++) {
    auto* row = &buf_[size_t(yy) * size_t(w_) + size_t(x)];
    std::fill(row, row + w, v);
  }
}

void Fb565::hline(int x, int y, int w, uint16_t colour) { fillRect(x, y, w, 1, colour); }
void Fb565::vline(int x, int y, int h, uint16_t colour) { fillRect(x, y, 1, h, colour); }

void Fb565::rect(int x, int y, int w, int h, uint16_t colour) {
  if (w <= 0 || h <= 0) return;
  hline(x, y, w, colour);
  hline(x, y + h - 1, w, colour);
  vline(x, y, h, colour);
  vline(x + w - 1, y, h, colour);
}

void Fb565::fillRoundRect(int x, int y, int w, int h, int r, uint16_t colour) {
  if (w <= 0 || h <= 0) return;
  r = std::min(r, std::min(w, h) / 2);
  if (r <= 0) { fillRect(x, y, w, h, colour); return; }

  fillRect(x + r, y, w - 2 * r, h, colour);          // middle band, full height
  fillRect(x, y + r, r, h - 2 * r, colour);          // left flank
  fillRect(x + w - r, y + r, r, h - 2 * r, colour);  // right flank

  // Corners: one quarter-disc, mirrored. Comparing squared distance keeps this
  // integer-only (no sqrt) and portable/host-testable.
  const int r2 = r * r;
  for (int dy = 0; dy < r; dy++) {
    for (int dx = 0; dx < r; dx++) {
      const int ox = r - dx, oy = r - dy;
      if (ox * ox + oy * oy > r2) continue;
      set(x + dx,          y + dy,          colour);
      set(x + w - 1 - dx,  y + dy,          colour);
      set(x + dx,          y + h - 1 - dy,  colour);
      set(x + w - 1 - dx,  y + h - 1 - dy,  colour);
    }
  }
}

void Fb565::roundRect(int x, int y, int w, int h, int r, uint16_t colour) {
  if (w <= 0 || h <= 0) return;
  r = std::min(r, std::min(w, h) / 2);
  if (r <= 0) { rect(x, y, w, h, colour); return; }

  hline(x + r, y,         w - 2 * r, colour);
  hline(x + r, y + h - 1, w - 2 * r, colour);
  vline(x,         y + r, h - 2 * r, colour);
  vline(x + w - 1, y + r, h - 2 * r, colour);

  // Corner arc: the ring of pixels whose squared distance lands in [r-1, r].
  const int r2 = r * r, ri2 = (r - 1) * (r - 1);
  for (int dy = 0; dy < r; dy++) {
    for (int dx = 0; dx < r; dx++) {
      const int ox = r - dx, oy = r - dy;
      const int d = ox * ox + oy * oy;
      if (d > r2 || d <= ri2) continue;
      set(x + dx,         y + dy,         colour);
      set(x + w - 1 - dx, y + dy,         colour);
      set(x + dx,         y + h - 1 - dy, colour);
      set(x + w - 1 - dx, y + h - 1 - dy, colour);
    }
  }
}

void Fb565::text(int x, int y, const std::string& s, uint16_t colour, int scale) {
  if (scale < 1) scale = 1;
  const std::string clean = asciiSanitize(s);
  for (char ch : clean) {
    if (x >= w_) break;                        // rest of the string is off-panel
    if (x + 5 * scale > 0) {                  // cheap cull; set() clips the rest
      const uint8_t* g = nimbus::font::glyphFor(ch);
      for (int col = 0; col < 5; col++) {
        const uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
          if (!(bits & (1 << row))) continue;
          if (scale == 1) {
            set(x + col, y + row, colour);
          } else {
            fillRect(x + col * scale, y + row * scale, scale, scale, colour);
          }
        }
      }
    }
    x += 6 * scale;                           // 5px glyph + 1px gap, scaled
  }
}

int Fb565::textWidth(const std::string& s, int scale) {
  if (scale < 1) scale = 1;
  const int n = int(asciiSanitize(s).size());
  return n <= 0 ? 0 : n * 6 * scale - scale;  // no trailing gap
}

void Fb565::textClipped(int x, int y, const std::string& s, uint16_t colour,
                        int maxW, int scale) {
  const std::string clean = asciiSanitize(s);
  if (textWidth(clean, scale) <= maxW) { text(x, y, clean, colour, scale); return; }

  // Trim until the string plus an ellipsis fits. Truncating mid-word is fine
  // here; these are titles in a narrow card, not prose.
  const std::string ell = "..";
  const int ellW = textWidth(ell, scale);
  // If even the ellipsis does not fit, draw NOTHING rather than overflow the
  // caller's box - a stray ".." spilling into the next control reads as a
  // rendering fault.
  if (ellW > maxW) return;
  std::string cut = clean;
  while (!cut.empty() && textWidth(cut, scale) + ellW > maxW) cut.pop_back();
  text(x, y, cut + ell, colour, scale);
}

void Fb565::label(int x, int y, const std::string& s, uint16_t colour) {
  // Uppercase + letter-spaced at scale 1 - the web .k caption treatment.
  std::string up = asciiSanitize(s);
  for (char& c : up) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  for (char ch : up) {
    text(x, y, std::string(1, ch), colour, 1);
    x += 6 + kLabelTrack;
  }
}

int Fb565::labelWidth(const std::string& s) {
  const int n = int(asciiSanitize(s).size());
  return n <= 0 ? 0 : n * (6 + kLabelTrack) - kLabelTrack;
}

void Fb565::card(int x, int y, int w, int h, uint16_t fill, uint16_t border,
                 int radius) {
  fillRoundRect(x, y, w, h, radius, fill);
  roundRect(x, y, w, h, radius, border);
}

int Fb565::chipWidth(const std::string& s) {
  return textWidth(s, 1) + 2 * 6;   // web .badge: 8px horizontal padding, scaled
}

void Fb565::chip(int x, int y, const std::string& s, uint16_t colour,
                 uint16_t surface) {
  const std::string clean = asciiSanitize(s);
  const int w = chipWidth(clean);
  const int h = 7 + 2 * 4;          // glyph + 4px vertical padding
  // The web .badge look: the status colour at ~12% over the surface, with the
  // full-strength colour as the text.
  fillRoundRect(x, y, w, h, kPillRadius, tintFor(colour, surface));
  text(x + 6, y + 4, clean, colour, 1);
}

}  // namespace nimbus::tft
