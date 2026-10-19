#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// An independent strict UTF-8 check for host tests (never the implementation's own):
// rejects invalid lead and continuation bytes, cut sequences, overlong forms, UTF-16
// surrogates and code points past U+10FFFF; optionally C0 / DEL / C1 controls too.
namespace testutf8 {

inline size_t leadLen(unsigned char c, uint32_t& cp) {
  if (c < 0x80) { cp = c; return 1; }
  if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; return 2; }
  if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; return 3; }
  if ((c & 0xF8) == 0xF0) { cp = c & 0x07; return 4; }
  return 0;
}

// The code point at s[i] and its length, or 0 when the sequence is not valid.
inline size_t decode(const std::string& s, size_t i, uint32_t& cp) {
  const unsigned char* u = (const unsigned char*)s.data();
  const size_t len = leadLen(u[i], cp);
  if (!len || i + len > s.size()) return 0;
  for (size_t k = 1; k < len; ++k) {
    if ((u[i + k] & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (u[i + k] & 0x3F);
  }
  static const uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
  if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return len;
}

inline bool isControl(uint32_t cp) { return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0); }

inline bool valid(const std::string& s, bool noControls = false) {
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = 0;
    const size_t len = decode(s, i, cp);
    if (!len || (noControls && isControl(cp))) return false;
    i += len;
  }
  return true;
}

}  // namespace testutf8
