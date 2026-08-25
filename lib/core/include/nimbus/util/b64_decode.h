#pragma once
// Portable, streaming base64 decoder - no Arduino, host-tested. Feed it base64
// characters one at a time (across arbitrary chunk boundaries); it emits decoded
// bytes through a callback. Non-alphabet characters (whitespace, newlines, the
// '=' padding, a closing quote) are skipped, so a caller can stream base64 split
// over serial lines without stripping framing first. Used by the test-console
// FSPUT verb (src/test_console.cpp) to land an SD fixture, and unit-tested in
// test/test_b64_decode.
#include <cstddef>
#include <cstdint>

namespace nimbus {
namespace b64 {

inline int val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;  // padding, whitespace, framing - skipped
}

// Stateful bit-accumulator. feed() calls `out(byte)` zero or one time per input
// char. Carries partial state across calls, so chunk boundaries do not matter.
class StreamDecoder {
 public:
  template <class Out>
  void feed(char c, Out out) {
    const int v = val(c);
    if (v < 0) return;
    acc_ = (acc_ << 6) | static_cast<uint32_t>(v);
    bits_ += 6;
    if (bits_ >= 8) {
      bits_ -= 8;
      out(static_cast<uint8_t>((acc_ >> bits_) & 0xFF));
    }
  }

  // Decode a whole buffer, appending decoded bytes via out(). Returns the count.
  template <class Out>
  size_t feed(const char* data, size_t n, Out out) {
    size_t produced = 0;
    for (size_t i = 0; i < n; i++) feed(data[i], [&](uint8_t b) { out(b); produced++; });
    return produced;
  }

  void reset() { acc_ = 0; bits_ = 0; }

 private:
  uint32_t acc_ = 0;
  int bits_ = 0;
};

}  // namespace b64
}  // namespace nimbus
