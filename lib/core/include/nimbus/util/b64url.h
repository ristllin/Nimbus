#pragma once
// Portable base64url ENCODER (RFC 4648 §5, '-'/'_' alphabet, no padding) - no
// Arduino, host-tested (test/test_sha256 covers it alongside the PKCE vectors).
// The decode side already lives in nimbus/util/b64_decode.h and cloud/relay_codec;
// this is the tiny encode counterpart the OAuth PKCE flow needs to turn random
// bytes into a code_verifier and a SHA-256 digest into a code_challenge.
#include <cstddef>
#include <cstdint>
#include <string>

namespace nimbus {
namespace b64 {

// Encode `n` bytes as base64url with no '=' padding (the form OAuth PKCE and
// JWT segments use). Standard base64url is a pure remap of standard base64.
inline std::string urlEncode(const uint8_t* data, size_t n) {
  static const char* kAlpha =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= n; i += 3) {
    uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8 | data[i + 2];
    out += kAlpha[(v >> 18) & 0x3f];
    out += kAlpha[(v >> 12) & 0x3f];
    out += kAlpha[(v >> 6) & 0x3f];
    out += kAlpha[v & 0x3f];
  }
  const size_t rem = n - i;
  if (rem == 1) {
    uint32_t v = (uint32_t)data[i] << 16;
    out += kAlpha[(v >> 18) & 0x3f];
    out += kAlpha[(v >> 12) & 0x3f];
  } else if (rem == 2) {
    uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8;
    out += kAlpha[(v >> 18) & 0x3f];
    out += kAlpha[(v >> 12) & 0x3f];
    out += kAlpha[(v >> 6) & 0x3f];
  }
  return out;
}

inline std::string urlEncode(const std::string& s) {
  return urlEncode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

}  // namespace b64
}  // namespace nimbus
