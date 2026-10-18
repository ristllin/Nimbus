#pragma once
#include <cstddef>
#include <cstring>
#include <string>

#include "nimbus/mem_cap.h"   // utf8CapLen - the backward boundary rule

// utf8 - the portable UTF-8 rules every provider-bound byte passes through.
//
// Every provider rejects a request body that is not valid UTF-8 with an HTTP 400
// that is decided before any output (so it is free, and identical on a retry and
// on every other provider). On 18 Oct one torn character in a files.search
// snippet (a byte-offset substr) failed the dream and two synthesis turns on all
// three hosts in turn. The class has three parts, all here:
//   - utf8Valid: strict validation (overlongs, surrogates, > U+10FFFF and torn
//     sequences are invalid, exactly what the providers' JSON decoders refuse);
//   - utf8Repair: replace each invalid sequence with U+FFFD, in place;
//   - boundary-safe cuts at BOTH ends (utf8Window, utf8CopyCap, utf8CapLen).
// Header-only and Arduino-free, so it is host-tested and shared by lib/core, the
// harness and the device.
namespace nimbus {

// What a lead byte announces: the sequence length (0 = no character starts with
// this byte) and the range its SECOND byte must fall in, which is where the
// overlong (E0, F0), surrogate (ED) and past-U+10FFFF (F4) forms are excluded.
struct Utf8Lead {
  unsigned char len, lo, hi;
};
inline Utf8Lead utf8Lead(unsigned char c) {
  if (c < 0x80) return {1, 0, 0};
  if (c < 0xC2) return {0, 0, 0};   // continuation byte, or an overlong 2-byte lead
  if (c < 0xE0) return {2, 0x80, 0xBF};
  if (c == 0xE0) return {3, 0xA0, 0xBF};
  if (c == 0xED) return {3, 0x80, 0x9F};
  if (c < 0xF0) return {3, 0x80, 0xBF};
  if (c == 0xF0) return {4, 0x90, 0xBF};
  if (c < 0xF4) return {4, 0x80, 0xBF};
  if (c == 0xF4) return {4, 0x80, 0x8F};
  return {0, 0, 0};                 // F5..FF never appear in UTF-8
}

// Length of the valid UTF-8 sequence starting at s[0] (1..4), or 0 when the bytes
// at s do not start one (a stray continuation byte, a forbidden lead byte, an
// overlong form, a surrogate, a code point past U+10FFFF, or a sequence cut short
// by the end of the buffer). n is the number of bytes available at s.
inline size_t utf8SeqLen(const unsigned char* s, size_t n) {
  if (n == 0) return 0;
  const Utf8Lead l = utf8Lead(s[0]);
  if (l.len <= 1) return l.len;
  if (n < l.len || s[1] < l.lo || s[1] > l.hi) return 0;
  for (size_t i = 2; i < l.len; i++)
    if ((s[i] & 0xC0) != 0x80) return 0;
  return l.len;
}

// Offset of the first invalid byte in [s, s+n), or n when all of it is valid.
inline size_t utf8InvalidAt(const char* s, size_t n) {
  const unsigned char* u = reinterpret_cast<const unsigned char*>(s);
  size_t i = 0;
  while (i < n) {
    const size_t k = utf8SeqLen(u + i, n - i);
    if (k == 0) return i;
    i += k;
  }
  return n;
}

inline bool utf8Valid(const char* s, size_t n) { return utf8InvalidAt(s, n) == n; }
inline bool utf8Valid(const std::string& s) { return utf8Valid(s.data(), s.size()); }

// How many bytes one invalid run at s covers: the lead byte plus the continuation
// bytes it legally accepted before the sequence broke (Unicode's "maximal
// subpart"), so a torn 3-byte character costs ONE replacement character, not three.
// Only called where utf8SeqLen(s, n) == 0, so the run is always shorter than the
// sequence its lead byte announced.
inline size_t utf8BadRun(const unsigned char* s, size_t n) {
  const Utf8Lead l = utf8Lead(s[0]);
  if (l.len <= 1) return 1;
  size_t k = 1;
  while (k < l.len && k < n) {
    const bool more = k == 1 ? (s[1] >= l.lo && s[1] <= l.hi) : (s[k] & 0xC0) == 0x80;
    if (!more) break;
    ++k;
  }
  return k;
}

// Replace every invalid sequence in `s` with U+FFFD, in place. Returns how many
// replacements it made (0 = `s` was already valid and is untouched). Idempotent:
// a repaired string is valid, so a second pass changes nothing.
inline size_t utf8Repair(std::string& s) {
  size_t bad = utf8InvalidAt(s.data(), s.size());
  if (bad == s.size()) return 0;
  std::string out;
  out.reserve(s.size() + 8);
  out.append(s, 0, bad);
  const unsigned char* u = reinterpret_cast<const unsigned char*>(s.data());
  size_t i = bad, count = 0;
  while (i < s.size()) {
    const size_t k = utf8SeqLen(u + i, s.size() - i);
    if (k) {
      out.append(s, i, k);
      i += k;
      continue;
    }
    out += "\xEF\xBF\xBD";
    ++count;
    i += utf8BadRun(u + i, s.size() - i);
  }
  s.swap(out);
  return count;
}

// A window of at most `width` bytes that starts about `before` bytes ahead of
// `at` (npos = the start), with BOTH ends on a character boundary: the start moves
// forward past continuation bytes, the end backs up to the last whole character.
// The old snippet code cut at raw byte offsets, so a window that began or ended
// inside a multi-byte character sent torn bytes to the provider (18 Oct).
inline std::string utf8Window(const std::string& s, size_t at, size_t before, size_t width) {
  size_t b = at == std::string::npos ? 0 : (at > before ? at - before : 0);
  if (b > s.size()) b = s.size();
  while (b < s.size() && (static_cast<unsigned char>(s[b]) & 0xC0) == 0x80) ++b;
  const size_t avail = s.size() - b;
  const size_t want = width < avail ? width : avail;
  size_t keep = static_cast<size_t>(
      utf8CapLen(s.data() + b, static_cast<int>(avail), static_cast<int>(want)));
  // A window that reaches the end of a buffer which was itself read short (a
  // capped file read) can end inside a character: drop that torn tail too.
  size_t lead = keep;
  for (int back = 0; back < 3 && lead > 0 &&
                     (static_cast<unsigned char>(s[b + lead - 1]) & 0xC0) == 0x80;
       ++back)
    --lead;
  if (lead > 0) {
    const unsigned char* u = reinterpret_cast<const unsigned char*>(s.data() + b + lead - 1);
    if (utf8SeqLen(u, keep - (lead - 1)) == 0) keep = lead - 1;
  }
  return s.substr(b, keep);
}

// Clip `s` to at most maxBytes, on a character boundary, with a visible note
// "\n[cut: kept K of N bytes]" INSIDE the cap, so the reader always knows text was
// dropped. Returns true when it clipped. A maxBytes too small for the note keeps
// the plain boundary-safe prefix.
inline bool utf8ClipNoted(std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return false;
  const size_t total = s.size();
  std::string note = "\n[cut: kept " + std::to_string(total) + " of " + std::to_string(total) +
                     " bytes]";   // the longest the note can be (kept <= total)
  if (note.size() >= maxBytes) {
    s.resize(static_cast<size_t>(
        utf8CapLen(s.data(), static_cast<int>(s.size()), static_cast<int>(maxBytes))));
    return true;
  }
  const int keep = utf8CapLen(s.data(), static_cast<int>(s.size()),
                              static_cast<int>(maxBytes - note.size()));
  s.resize(static_cast<size_t>(keep));
  s += "\n[cut: kept " + std::to_string(s.size()) + " of " + std::to_string(total) + " bytes]";
  return true;
}

// Copy at most cap-1 bytes of src into dst (cap includes the NUL), ending on a
// character boundary, NUL-terminated. Returns the bytes copied. cap 0 writes
// nothing.
inline size_t utf8CopyCap(char* dst, size_t cap, const char* src, size_t len) {
  if (cap == 0) return 0;
  size_t keep = len < cap - 1 ? len : cap - 1;
  if (keep < len)
    keep = static_cast<size_t>(utf8CapLen(src, static_cast<int>(len), static_cast<int>(keep)));
  std::memcpy(dst, src, keep);
  dst[keep] = 0;
  return keep;
}

}  // namespace nimbus
