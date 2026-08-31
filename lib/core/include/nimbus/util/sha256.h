#pragma once
// Portable SHA-256 - no Arduino, host-tested (test/test_sha256). A small,
// self-contained FIPS 180-4 implementation used where the device needs a hash
// off the TLS stack: PKCE S256 code-challenge derivation for the outbound MCP
// OAuth flow (nimbus/orch/mcp_oauth.h). The device could reach mbedtls, but a
// portable implementation is what lets the whole OAuth decision layer be proven
// on the host against RFC test vectors (the class rule: hash the vectors, not
// one string). Not constant-time; it hashes public inputs (a random verifier),
// never a compared secret.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace nimbus {
namespace crypto {

class Sha256 {
 public:
  static constexpr size_t kDigestLen = 32;

  Sha256() { reset(); }

  void reset() {
    len_ = 0;
    fill_ = 0;
    static const uint32_t kInit[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::memcpy(h_, kInit, sizeof(h_));
  }

  void update(const uint8_t* data, size_t n) {
    len_ += n;
    while (n > 0) {
      size_t take = 64 - fill_;
      if (take > n) take = n;
      std::memcpy(buf_ + fill_, data, take);
      fill_ += take;
      data += take;
      n -= take;
      if (fill_ == 64) {
        block(buf_);
        fill_ = 0;
      }
    }
  }
  void update(const std::string& s) {
    update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }

  // Finalize into a 32-byte digest. The object is single-shot after this.
  void final(uint8_t out[kDigestLen]) {
    const uint64_t bitLen = len_ * 8;
    const uint8_t pad0 = 0x80;
    update(&pad0, 1);
    const uint8_t zero = 0x00;
    while (fill_ != 56) update(&zero, 1);
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; i++) lenBytes[i] = (uint8_t)(bitLen >> (56 - 8 * i));
    update(lenBytes, 8);  // triggers the final block
    for (int i = 0; i < 8; i++) {
      out[i * 4 + 0] = (uint8_t)(h_[i] >> 24);
      out[i * 4 + 1] = (uint8_t)(h_[i] >> 16);
      out[i * 4 + 2] = (uint8_t)(h_[i] >> 8);
      out[i * 4 + 3] = (uint8_t)(h_[i]);
    }
  }

  // One-shot convenience: raw 32-byte digest of a buffer as a std::string.
  static std::string digest(const uint8_t* data, size_t n) {
    Sha256 s;
    s.update(data, n);
    uint8_t d[kDigestLen];
    s.final(d);
    return std::string(reinterpret_cast<char*>(d), kDigestLen);
  }
  static std::string digest(const std::string& s) {
    return digest(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }

 private:
  static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
             ((uint32_t)p[i * 4 + 2] << 8) | ((uint32_t)p[i * 4 + 3]);
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + S1 + ch + K[i] + w[i];
      uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  }

  uint32_t h_[8];
  uint8_t  buf_[64];
  size_t   fill_ = 0;
  uint64_t len_ = 0;
};

}  // namespace crypto
}  // namespace nimbus
