#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/provider_park.h"

// health_rows - two Health rows the device could not show before (19 Oct triage):
// on 19 Oct /api/health read 9 ok and 1 degraded while the settings store had 0 B
// free and 266 failed writes and two providers were refusing for billing. The only
// storage row keyed on the profile-seed deferral, and the provider fault row was a
// test mask. The Providers row reads the one per-provider park. Pure and host-tested (test/test_health_rows); src/agent/health.cpp
// feeds the device's numbers, and system.health (the head's self-check) reads the
// same report.
namespace nimbus {
namespace orch {

struct HealthRow {
  std::string key, label, state, detail;   // state: "ok" | "degraded" | "absent" | "unknown"
};

// The settings store (NVS) as the reserve guard sees it.
struct SettingsStorageInputs {
  size_t freeEntries = 0;      // entries free right now
  size_t reserveEntries = 0;   // the reserve critical writes keep
  uint32_t failsSinceBoot = 0; // writes that did not land since start
  std::string lastFailKey;     // the last of them ("" when none)
  bool starved = false;        // the reserve could not be restored
};

// The Settings storage row: degraded when free entries are under the reserve, a
// save failed since start, or the reserve is starved, with
// "N saves failed since start, last <key>, M entries free". False (no row) on a
// healthy store, so a healthy device shows nothing new.
bool settingsStorageRow(const SettingsStorageInputs& in, HealthRow& out);

// The Providers row (the one per-provider park, nimbus/orch/provider_park.h): every
// provider the park holds closed (out of credit, a refused key, or paused), each in
// the park's own words with when it is checked again. Degraded while any is closed;
// ok ("every provider open") when every provider is open to every seam, so a
// provider that answers again clears it.
HealthRow providersRow(ProviderPark& park, uint32_t nowMs);

// The rows as text lines ("<label> | <state> | <detail>"), for the golden.
std::string healthRowsText(const std::vector<HealthRow>& rows);

}  // namespace orch
}  // namespace nimbus
