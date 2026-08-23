#include "nimbus/cloud/relay_credential.h"

#include <vector>

#include <ArduinoJson.h>

#include "nimbus/cloud/relay_codec.h"   // b64Decode (standard base64)

namespace nimbus {
namespace cloud {

namespace {

// Decode a JWT base64url segment (RFC 7515: '-'/'_' alphabet, no padding) by
// translating it to standard base64 and reusing the host-tested b64Decode. Returns
// false on any malformed input.
bool b64UrlDecode(const std::string& seg, std::vector<uint8_t>& out) {
  std::string b64;
  b64.reserve(seg.size() + 3);
  for (char c : seg) {
    if (c == '-') b64 += '+';
    else if (c == '_') b64 += '/';
    else b64 += c;
  }
  while (b64.size() % 4 != 0) b64 += '=';   // restore the stripped padding
  return b64Decode(b64.c_str(), b64.size(), out);
}

// Pull one unsigned numeric claim out of a JWT payload. Returns 0 when the token is
// malformed, the claim is missing, or it is not a positive integer.
uint64_t claim(const std::string& jwt, const char* key) {
  const size_t dot1 = jwt.find('.');
  if (dot1 == std::string::npos) return 0;
  const size_t dot2 = jwt.find('.', dot1 + 1);
  if (dot2 == std::string::npos || dot2 <= dot1 + 1) return 0;
  const std::string payloadSeg = jwt.substr(dot1 + 1, dot2 - dot1 - 1);

  std::vector<uint8_t> raw;
  if (!b64UrlDecode(payloadSeg, raw) || raw.empty()) return 0;

  ArduinoJson::JsonDocument doc;
  if (ArduinoJson::deserializeJson(doc, raw.data(), raw.size())) return 0;
  ArduinoJson::JsonVariantConst v = doc[key];
  if (!v.is<uint64_t>() && !v.is<int64_t>() && !v.is<double>()) return 0;
  const int64_t n = v.as<int64_t>();
  return n > 0 ? (uint64_t)n : 0;
}

}  // namespace

uint64_t credentialExp(const std::string& jwt) { return claim(jwt, "exp"); }
uint64_t credentialIat(const std::string& jwt) { return claim(jwt, "iat"); }

uint64_t remintTarget(uint64_t iat, uint64_t exp, uint32_t seed) {
  if (exp == 0 || iat == 0 || exp <= iat) return 0;   // no usable lifetime (legacy)
  const uint64_t life = exp - iat;
  // Offset in [lo%, hi%] of the lifetime. Integer math keeps it deterministic across
  // builds (no float): base = lo% of life, then up to (hi-lo)% more, scaled by seed.
  const uint32_t span = kRemintJitterHiPct - kRemintJitterLoPct;      // 30
  const uint64_t base = life * kRemintJitterLoPct / 100;              // 0.50 * life
  const uint64_t extra = life * ((uint64_t)(seed % (span + 1))) / 100; // 0..0.30 * life
  return iat + base + extra;
}

bool remintDueProactive(uint64_t iat, uint64_t exp, uint64_t now, uint32_t seed) {
  const uint64_t target = remintTarget(iat, exp, seed);
  if (target == 0) return false;          // legacy: no proactive schedule
  return now >= target && now < exp;      // in the window, not yet expired
}

bool expiredWithinGrace(uint64_t exp, uint64_t now, uint64_t graceSec) {
  if (exp == 0) return false;             // legacy: never "expired"
  return now >= exp && now < exp + graceSec;
}

bool expiredPastGrace(uint64_t exp, uint64_t now, uint64_t graceSec) {
  if (exp == 0) return false;             // legacy: never "expired"
  return now >= exp + graceSec;
}

}  // namespace cloud
}  // namespace nimbus
