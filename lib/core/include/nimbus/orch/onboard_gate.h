#pragma once
#include "nimbus/orch/provider_slots.h"   // anySlotWhere: the caller enumerates the registry

// onboard_gate - when a device that boots with onboarded=false counts as already set
// up (QW5). Portable and host-tested (test/test_onboard_gate).
//
// The setup wizard arrived after devices were in the field, so boot marks a device as
// onboarded when it already looks configured, and an upgraded device is never dropped
// into first-run setup. Saved Wi-Fi alone used to be that sign. But the wizard's own
// Wi-Fi step saves Wi-Fi, so a wizard left before the provider step (the owner went to
// get a key and the device restarted) was silently marked finished: the wizard never
// came back, and the device sat with no assistant. The sign is now the wizard's own
// finish gate: saved Wi-Fi AND a verified provider (the same gate POST
// /api/onboard/complete enforces). Anything less resumes the wizard, which picks up at
// the first step that is not done.
namespace nimbus {
namespace orch {

inline bool migrateOnboarded(bool onboarded, bool wifiSaved, bool anyProviderVerified) {
  return !onboarded && wifiSaved && anyProviderVerified;
}

}  // namespace orch
}  // namespace nimbus
