#pragma once
#include <cstdint>
#include <string>

// relay_credential - the PURE, host-tested device-side policy for the cloud relay
// credential's expiry + re-mint (CUM-52, consuming the C6 contract on CUM-87). No
// Arduino, no TLS: it only reads the `exp`/`iat` hints out of the HS256 JWT (the
// relay owns signature verification) and decides WHEN the device should re-mint.
// The device glue in src/net/relay_client.cpp performs the actual POST + NVS write.
//
// Contract (CUM-87 v1):
//   - credential is an HS256 JWT: { sub, tv, iat, exp? }. `exp` is optional; a
//     credential with no `exp` is legacy / non-expiring.
//   - default lifetime DEVICE_CRED_TTL_SEC = 30 days; re-mint grace
//     DEVICE_CRED_REMINT_GRACE_SEC = 7 days.
//   - re-mint proactively at a JITTERED point in [50%, 80%] of the lifetime (so a
//     fleet that restarted together does not stampede the endpoint at once).
//   - an expired credential still WITHIN grace may be re-minted before reconnecting;
//     past grace, the device must re-pair.

namespace nimbus {
namespace cloud {

constexpr uint64_t kDeviceCredTtlSec   = 2592000;   // 30d (server default; informational)
constexpr uint64_t kRemintGraceSec     = 604800;    // 7d  (DEVICE_CRED_REMINT_GRACE_SEC)
constexpr uint32_t kRemintJitterLoPct  = 50;        // re-mint window start, % of lifetime
constexpr uint32_t kRemintJitterHiPct  = 80;        // re-mint window end,   % of lifetime

// Read the `exp` (unix seconds) from a JWT credential. Returns 0 when there is no
// `exp` claim (legacy, non-expiring) OR the token cannot be parsed - callers treat
// 0 as "no known expiry", never as "expired now".
uint64_t credentialExp(const std::string& jwt);

// Read the `iat` (issued-at, unix seconds); 0 if absent/unparseable. Needed for the
// lifetime = exp - iat that the jitter window is a fraction of.
uint64_t credentialIat(const std::string& jwt);

// The jittered proactive re-mint target (unix seconds): iat + f*(exp-iat), where f is
// a STABLE per-device fraction in [0.50, 0.80] derived from `seed` (e.g. a hash of the
// deviceId). Returns 0 when there is no usable lifetime (exp==0 or exp<=iat), i.e. a
// legacy credential has no proactive schedule.
uint64_t remintTarget(uint64_t iat, uint64_t exp, uint32_t seed);

// Proactive: the jittered target has passed and the credential is not yet expired -
// the device should re-mint at the next idle opportunity.
bool remintDueProactive(uint64_t iat, uint64_t exp, uint64_t now, uint32_t seed);

// Reconnect path: the credential is expired but still within `graceSec` - re-mint
// before reconnecting (the old credential's overlap window has not closed).
bool expiredWithinGrace(uint64_t exp, uint64_t now, uint64_t graceSec = kRemintGraceSec);

// Past grace (or exactly at the grace edge): re-mint is no longer allowed, the device
// must fall back to pairing.
bool expiredPastGrace(uint64_t exp, uint64_t now, uint64_t graceSec = kRemintGraceSec);

}  // namespace cloud
}  // namespace nimbus
