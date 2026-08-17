#pragma once
#include <cstdint>
#include <string>

#include "nimbus/qr.h"

// epd_render/fb - a portable 1-bit framebuffer for the 296x128 panel.
// Coordinates: (0,0) top-left, x right, y down, landscape. Bit semantics:
// 1 = black, 0 = white; rows packed MSB-first, kW/8 bytes per row. The device
// glue converts to GxEPD2's buffer expectations; golden tests hash/compare
// data() directly and tools/golden.py turns it into PNGs.

namespace nimbus::epd {

constexpr int kW = 296;
constexpr int kH = 128;
constexpr int kStride = kW / 8;          // 37 bytes per row
constexpr int kFbBytes = kStride * kH;   // 4736

class Fb {
 public:
  Fb() { clear(); }

  void clear(bool black = false);
  void set(int x, int y, bool black = true);
  bool get(int x, int y) const;

  void hline(int x, int y, int w, bool black = true);
  void vline(int x, int y, int h, bool black = true);
  void rect(int x, int y, int w, int h, bool black = true);      // outline
  void fillRect(int x, int y, int w, int h, bool black = true);
  void invertRect(int x, int y, int w, int h);

  // Text: embedded 5x7 monospace font (public-domain glyphs, ASCII 32-126),
  // drawn at integer scale (scale 1 => 6px advance, 8px line; scale 2 =>
  // 12/16). Unknown chars render as '?'. Returns the x after the last glyph.
  int text(int x, int y, const std::string& s, int scale = 1, bool black = true);
  static int textWidth(const std::string& s, int scale = 1);
  static int lineHeight(int scale = 1);

  // Draw a QR symbol at (x,y): each module is a scale*scale black square, on a
  // white background with the mandatory 4-module quiet zone (so scanners lock
  // on even against dark content). Total footprint = (qr.size + 8) * scale px.
  // Returns that footprint side length. Clips to bounds.
  int drawQr(int x, int y, const nimbus::qr::QrCode& qr, int scale = 1);
  static int qrFootprint(const nimbus::qr::QrCode& qr, int scale = 1);

  const uint8_t* data() const { return buf_; }
  uint8_t* data() { return buf_; }

 private:
  uint8_t buf_[kFbBytes];
};

// Transliterate UTF-8 text into the printable-ASCII subset the 5x7 font renders:
// smart quotes/dashes/ellipsis -> ASCII, accented Latin -> base letter, arrows/bullets
// -> ASCII, everything else (emoji, CJK, symbols) dropped. Without it every byte of a
// multi-byte UTF-8 char draws as its own '?' (e.g. a curly apostrophe -> "???"). Applied
// by Fb::text()/textWidth() and wrapText() so all on-screen text is clean by default.
std::string asciiSanitize(const std::string& in);

}  // namespace nimbus::epd
