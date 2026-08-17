#include "nimbus/epd_render/fb.h"

#include <cstring>

#include "nimbus/font5x7.h"

// Portable 1-bit framebuffer. Every primitive clips to the panel bounds, so
// callers can draw partially off-screen text/rects without guards. Bit
// packing matches the header contract: 1 = black, rows MSB-first (pixel x
// lives in bit 7 - (x & 7) of byte y * kStride + x / 8).

namespace nimbus::epd {

namespace {

// ---- font5x7 ----------------------------------------------------------------
// The atlas now lives in nimbus/font5x7.h so the colour touch renderer shares
// one copy (a pure move - the goldens below are byte-identical across it).
using nimbus::font::glyphFor;


// Clip a rect to the framebuffer in place; false when nothing remains.
bool clipRect(int& x, int& y, int& w, int& h) {
  if (w <= 0 || h <= 0 || x >= kW || y >= kH) return false;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w <= 0 || h <= 0) return false;
  if (w > kW - x) w = kW - x;
  if (h > kH - y) h = kH - y;
  return true;
}

}  // namespace

void Fb::clear(bool black) {
  std::memset(buf_, black ? 0xFF : 0x00, size_t(kFbBytes));
}

void Fb::set(int x, int y, bool black) {
  if (x < 0 || x >= kW || y < 0 || y >= kH) return;
  uint8_t& b = buf_[y * kStride + (x >> 3)];
  const uint8_t m = uint8_t(0x80u >> (x & 7));
  if (black) b |= m; else b &= uint8_t(~m);
}

bool Fb::get(int x, int y) const {
  if (x < 0 || x >= kW || y < 0 || y >= kH) return false;
  return (buf_[y * kStride + (x >> 3)] >> (7 - (x & 7))) & 1;
}

void Fb::hline(int x, int y, int w, bool black) { fillRect(x, y, w, 1, black); }

void Fb::vline(int x, int y, int h, bool black) { fillRect(x, y, 1, h, black); }

void Fb::rect(int x, int y, int w, int h, bool black) {
  if (w <= 0 || h <= 0) return;
  hline(x, y, w, black);
  if (h > 1) hline(x, y + h - 1, w, black);
  if (h > 2) {
    vline(x, y + 1, h - 2, black);
    if (w > 1) vline(x + w - 1, y + 1, h - 2, black);
  }
}

void Fb::fillRect(int x, int y, int w, int h, bool black) {
  if (!clipRect(x, y, w, h)) return;
  for (int yy = y; yy < y + h; ++yy) {
    for (int xx = x; xx < x + w; ++xx) {
      uint8_t& b = buf_[yy * kStride + (xx >> 3)];
      const uint8_t m = uint8_t(0x80u >> (xx & 7));
      if (black) b |= m; else b &= uint8_t(~m);
    }
  }
}

void Fb::invertRect(int x, int y, int w, int h) {
  if (!clipRect(x, y, w, h)) return;
  for (int yy = y; yy < y + h; ++yy) {
    for (int xx = x; xx < x + w; ++xx) {
      buf_[yy * kStride + (xx >> 3)] ^= uint8_t(0x80u >> (xx & 7));
    }
  }
}

namespace {
// Append one Unicode code point to `out` as printable ASCII (0+ chars).
void appendCp(std::string& out, uint32_t cp) {
  if (cp < 0x80) { out += char(cp); return; }
  switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: out += '\''; return;  // ' ' ' '
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: out += '"';  return;  // " " " "
    case 0x2013: case 0x2014: case 0x2015: case 0x2212: out += '-';  return;  // - -- - -
    case 0x2026: out += "..."; return;                                        // ellipsis
    case 0x2022: case 0x00B7: case 0x25CF: out += '*'; return;                // bullets
    case 0x00A0: case 0x2009: case 0x202F: out += ' '; return;                // thin/nbsp space
    case 0x20AC: out += "EUR"; return;
    case 0x00A9: out += "(c)"; return;  case 0x2122: out += "(TM)"; return;
    case 0x2192: out += "->"; return;   case 0x2190: out += "<-"; return;
    default: break;
  }
  if (cp >= 0xC0 && cp <= 0xFF) {   // Latin-1 accented letters -> base letter
    static const char base[] = "AAAAAA_CEEEEIIIIDNOOOOO_OUUUUY__aaaaaa_ceeeeiiiidnooooo_ouuuuy_y";
    char b = base[cp - 0xC0];
    if (b != '_') { out += b; return; }
    switch (cp) {
      case 0xC6: out += "AE"; return;  case 0xE6: out += "ae"; return;
      case 0xDF: out += "ss"; return;
      case 0xDE: out += "Th"; return;  case 0xFE: out += "th"; return;
      default: return;  // x / division sign -> drop
    }
  }
  // Anything else (emoji, CJK, box-drawing, other symbols) -> drop silently.
}
}  // namespace

std::string asciiSanitize(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  size_t i = 0, n = in.size();
  while (i < n) {
    unsigned char c = (unsigned char)in[i];
    uint32_t cp; int len;
    if      (c < 0x80)        { cp = c;        len = 1; }
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
    else if ((c >> 3) == 0x1E){ cp = c & 0x07; len = 4; }
    else { ++i; continue; }   // stray continuation / invalid lead byte -> skip
    bool ok = true;
    for (int k = 1; k < len; ++k) {
      if (i + k >= n || ((unsigned char)in[i + k] & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | ((unsigned char)in[i + k] & 0x3F);
    }
    if (!ok) { ++i; continue; }
    i += len;
    appendCp(out, cp);
  }
  return out;
}

int Fb::text(int x, int y, const std::string& raw, int scale, bool black) {
  if (scale < 1) scale = 1;
  const std::string s = asciiSanitize(raw);
  for (const char ch : s) {
    // Cheap cull; set()/fillRect() still clip partially visible glyphs.
    if (x + 5 * scale > 0 && x < kW && y + 7 * scale > 0 && y < kH) {
      const uint8_t* g = glyphFor(ch);
      for (int col = 0; col < 5; ++col) {
        const uint8_t bits = g[col];
        for (int row = 0; row < 7; ++row) {
          if (!(bits & (1u << row))) continue;
          if (scale == 1) {
            set(x + col, y + row, black);
          } else {
            fillRect(x + col * scale, y + row * scale, scale, scale, black);
          }
        }
      }
    }
    x += 6 * scale;  // 5px glyph + 1px gap, scaled
  }
  return x;
}

int Fb::textWidth(const std::string& s, int scale) {
  if (scale < 1) scale = 1;
  // Measure the SANITIZED length so centering matches what text() actually draws
  // (a multi-byte UTF-8 char collapses to its ASCII transliteration).
  return int(asciiSanitize(s).size()) * 6 * scale;
}

int Fb::lineHeight(int scale) {
  if (scale < 1) scale = 1;
  return 8 * scale;
}

int Fb::qrFootprint(const nimbus::qr::QrCode& qr, int scale) {
  if (scale < 1) scale = 1;
  return (qr.size + 8) * scale;  // symbol + 4-module quiet zone each side
}

int Fb::drawQr(int x, int y, const nimbus::qr::QrCode& qr, int scale) {
  if (scale < 1) scale = 1;
  if (qr.size <= 0) return 0;
  const int foot = qrFootprint(qr, scale);
  // Paint the whole footprint white first (quiet zone + background) so the code
  // stays scannable even over other panel content.
  fillRect(x, y, foot, foot, /*black=*/false);
  const int quiet = 4 * scale;
  for (int my = 0; my < qr.size; ++my)
    for (int mx = 0; mx < qr.size; ++mx)
      if (qr.module(mx, my))
        fillRect(x + quiet + mx * scale, y + quiet + my * scale, scale, scale,
                 /*black=*/true);
  return foot;
}

}  // namespace nimbus::epd
