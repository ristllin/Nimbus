#pragma once
#include <cstddef>
#include <cstdint>

#include "nimbus/profile.h"

// config_store - versioned serialization of Config (the active profile plus the
// sparse user overrides) for persistence across reboots. The device layer
// writes the blob to NVS/LittleFS; the format is portable and host-tested so a
// round-trip is provably lossless and corruption never crashes the loader.
//
// Layout (little-endian):
//   [ 'N' ][ 'C' ][ version=2 ][ profile:u8 ][ count:u8 ]
//   count x { [ param:u8 ][ value:i32 ] }        // only overridden params
//
// deserialize() is all-or-nothing: any bad magic/version/param/length leaves
// `out` untouched and returns false, so a corrupt blob falls back to defaults
// rather than a half-applied config.
//
// version history:
//   1 - original layout.
//   2 - Param::FullRefreshEveryN removed (e-ink deprecation), which renumbers
//       every param after it. Overrides are stored BY INDEX, so a v1 blob would
//       be misread; the bump makes deserialize() reject it wholesale and revert
//       the Tune overrides to defaults (the profile is mirrored separately in
//       config_nvs and survives). Any future param reorder MUST bump this.

namespace nimbus {

constexpr uint8_t  kConfigStoreVersion = 2;
constexpr size_t   kConfigHeaderBytes = 5;                 // NC ver profile count
constexpr size_t   kConfigRecordBytes = 5;                 // param + i32
constexpr size_t   kConfigMaxBytes =
    kConfigHeaderBytes + kParamCount * kConfigRecordBytes;

// Serialize c into out[cap]. Returns bytes written, or 0 if cap is too small.
size_t serializeConfig(const Config& c, uint8_t* out, size_t cap);

// Parse a blob into out. Returns false (out untouched) on any corruption.
bool deserializeConfig(const uint8_t* data, size_t len, Config& out);

}  // namespace nimbus
