#include "nimbus/config_store.h"

namespace nimbus {

namespace {
void put32(uint8_t* p, int32_t v) {
  const uint32_t u = uint32_t(v);
  p[0] = uint8_t(u);
  p[1] = uint8_t(u >> 8);
  p[2] = uint8_t(u >> 16);
  p[3] = uint8_t(u >> 24);
}
int32_t get32(const uint8_t* p) {
  return int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                 (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
}
}  // namespace

size_t serializeConfig(const Config& c, uint8_t* out, size_t cap) {
  // Count overridden params first to size the blob.
  uint8_t n = 0;
  for (int i = 0; i < kParamCount; ++i)
    if (c.hasOverride(Param(i))) ++n;

  const size_t total = kConfigHeaderBytes + size_t(n) * kConfigRecordBytes;
  if (cap < total) return 0;

  out[0] = 'N';
  out[1] = 'C';
  out[2] = kConfigStoreVersion;
  out[3] = uint8_t(c.profile());
  out[4] = n;

  size_t off = kConfigHeaderBytes;
  for (int i = 0; i < kParamCount; ++i) {
    if (!c.hasOverride(Param(i))) continue;
    out[off] = uint8_t(i);
    put32(out + off + 1, c.effective(Param(i)));  // == the override value
    off += kConfigRecordBytes;
  }
  return total;
}

bool deserializeConfig(const uint8_t* data, size_t len, Config& out) {
  if (len < kConfigHeaderBytes) return false;
  if (data[0] != 'N' || data[1] != 'C') return false;
  if (data[2] != kConfigStoreVersion) return false;
  if (data[3] >= kProfileCount) return false;
  const uint8_t n = data[4];
  if (len < kConfigHeaderBytes + size_t(n) * kConfigRecordBytes) return false;

  // Validate every record before mutating `out` (all-or-nothing).
  size_t off = kConfigHeaderBytes;
  for (uint8_t i = 0; i < n; ++i) {
    if (data[off] >= kParamCount) return false;
    off += kConfigRecordBytes;
  }

  Config parsed;
  parsed.setProfile(ProfileId(data[3]));
  off = kConfigHeaderBytes;
  for (uint8_t i = 0; i < n; ++i) {
    parsed.setOverride(Param(data[off]), get32(data + off + 1));
    off += kConfigRecordBytes;
  }
  out = parsed;
  return true;
}

}  // namespace nimbus
