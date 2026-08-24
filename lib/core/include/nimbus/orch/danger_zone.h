#pragma once
#include <cstring>

// Danger-zone confirm phrases + the identity-preservation policy - the ONE source
// of truth the web UI, the endpoints, and the tests all agree on (CUM-15). Pure +
// host-tested; the device glue (NVS erase, SD erase/format) lives in src/.
//
// Each destructive action has its OWN typed confirm phrase so one confirm can never
// trigger a heavier action than the owner meant. Factory Reset PRESERVES the device
// identity (the user-visible name) across the wipe - it answers "which device is
// this", not a setting - while everything else (keys, token, bonds, config) is fresh.

namespace nimbus {
namespace orch {

inline constexpr const char* kConfirmFactoryReset = "FACTORY RESET";
inline constexpr const char* kConfirmEraseStorage = "ERASE STORAGE";
inline constexpr const char* kConfirmFormatCard   = "FORMAT CARD";

// Exact, case-sensitive, non-empty match. A missing or wrong phrase is rejected.
inline bool confirmOk(const char* expected, const char* got) {
  return expected && got && got[0] && std::strcmp(expected, got) == 0;
}

// The NVS keys a factory reset PRESERVES (identity). Kept here so a test can pin
// that the device name survives and nothing else is on the keep-list by accident.
inline constexpr const char* kFactoryKeepKeys[] = {"nimbus_name"};
inline constexpr int kFactoryKeepKeyCount = 1;

}  // namespace orch
}  // namespace nimbus
