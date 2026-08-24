#include "nimbus/render_text.h"

#include <cstdint>

namespace nimbus::render {

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

}  // namespace nimbus::render
