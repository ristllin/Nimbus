#pragma once
#include <cstring>

// Danger-zone confirm phrases + the identity-preservation policy - the ONE source
// of truth the web UI, the endpoints, and the tests all agree on (CUM-15). Pure +
// host-tested; the device glue (NVS erase, SD erase/format) lives in src/.
//
// Each destructive action has its OWN typed confirm phrase so one confirm can never
// trigger a heavier action than the owner meant. Factory Reset SCRAPS EVERYTHING the
// owner set - keys, token, bonds, config, AND the user-visible device name - so a
// reset unit comes back as a clean, re-onboardable device with a fresh auto-generated
// name + mDNS (CUM-230). Only the board's PHYSICAL identity (config/identity_keys.h:
// panel model, mount flip, touch cal, OTA slug) survives, so it still boots on the
// right driver instead of a white screen it cannot correct from its own controls.

namespace nimbus {
namespace orch {

inline constexpr const char* kConfirmFactoryReset = "FACTORY RESET";
inline constexpr const char* kConfirmEraseStorage = "ERASE STORAGE";
inline constexpr const char* kConfirmFormatCard   = "FORMAT CARD";

// Exact, case-sensitive, non-empty match. A missing or wrong phrase is rejected.
inline bool confirmOk(const char* expected, const char* got) {
  return expected && got && got[0] && std::strcmp(expected, got) == 0;
}

// The NVS keys a factory reset PRESERVES beyond the board's physical identity.
// Intentionally EMPTY (CUM-230 ruling): nothing user-facing survives - not even the
// device name, which is user state tied to the OLD identity and would otherwise keep
// a reset unit advertising its pre-reset mDNS name (CUM-233). The mechanism stays so
// re-adding a keep-key is a one-line change with a matching test. The lone sentinel
// keeps the array a valid (non-zero-length) declaration; kFactoryKeepKeyCount is
// authoritative and iterators must stop at it.
inline constexpr const char* kFactoryKeepKeys[] = {nullptr};
inline constexpr int kFactoryKeepKeyCount = 0;

// After the whole-partition wipe, factory reset SEEDS the operating mode so the device
// boots into the Wi-Fi setup AP and the onboarding wizard is reachable. Leaving the
// mode key merely cleared defaults to Notifier - Wi-Fi off, no AP, no wizard, no way to
// onboard (the CUM-230 field bug). Mirrors sys::Mode::Orchestrator (1); the reset path
// static_asserts the two agree.
inline constexpr int kFactoryResetSeedMode = 1;

}  // namespace orch
}  // namespace nimbus
