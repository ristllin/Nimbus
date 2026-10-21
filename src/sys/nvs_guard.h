#pragma once
// nvs_guard - the device seam of the settings-storage (NVS) reserve
// (nimbus/store/nvs_reserve.h, host-tested in test/test_nvs_full).
//
// Every NVS writer asks nvsAdmit() before a write that may grow the partition. A
// refusal is reported through notePersist by the caller like any failed write, so a
// save route's SaveScope answers 507 storage_full and /api/state shows the key.
// Erases and non-blob writes that do not grow a value are always admitted; a growing
// write of any class is refused when the partition cannot hold the new value, and a
// blob write (none ship today; a new one is admitted with NvsValKind::Blob and listed
// in test_nvs_full's raw-writer guard) must fit its whole span.
//
// The eviction pass (agent::store::nvsMaintain) runs on the main loop, never inside a
// web route: a refusal only raises a request flag here; the pass erases rebuildable
// caches until the reserve is back, and when it cannot, marks the device STARVED so
// owner-config writes are accepted again (only rebuildable and telemetry writes stay
// refused, logged once).

#include <cstddef>
#include <cstdint>

#include "nimbus/store/nvs_reserve.h"

namespace nimbus::sys {

// The namespace solide::memory opens (every agent::store key lives in it).
inline constexpr char kNvsNsSolide[] = "solide";

// True when the write may go ahead. `bytes` is the new value's size (a string without
// its NUL); `erase` is true for a removal. Reads the partition usage and the stored
// value's size (never the value). A refusal counts in nvsGuardStats() and raises the
// eviction request for the main-loop pass.
bool nvsAdmit(const char* ns, const char* key, nimbus::store::NvsValKind kind, size_t bytes,
              bool erase = false);

// Free entries right now (nvs_get_stats available_entries); 0 when the call fails.
size_t nvsAvailableEntries();

// The pass sets these; nvsAdmit reads them (both atomics, any task).
bool nvsStarved();
void setNvsStarved(bool starved);
// True once after a refusal was recorded (consumed by the main-loop pass).
bool takeNvsEvictRequest();

struct NvsGuardStats {
  uint32_t refused   = 0;   // writes refused by the reserve since boot
  uint32_t evicted   = 0;   // keys the pass erased since boot
  bool     starved   = false;
};
NvsGuardStats nvsGuardStats();
void          noteNvsEvicted(uint32_t n);

}  // namespace nimbus::sys
