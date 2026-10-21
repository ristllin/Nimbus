#include "sys/nvs_guard.h"

#include <atomic>

#include <nvs.h>

namespace nimbus::sys {

namespace {
std::atomic<bool>     s_starved{false};
std::atomic<bool>     s_evictReq{false};
std::atomic<uint32_t> s_refused{0};
std::atomic<uint32_t> s_evicted{0};

// Entries the stored value takes now (0 when absent). Sizes only: a NULL buffer makes
// NVS report the length without copying the value.
size_t storedEntries(const char* ns, const char* key) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return 0;   // no namespace yet: absent
  nvs_type_t t = NVS_TYPE_ANY;
  size_t entries = 0;
  if (nvs_find_key(h, key, &t) == ESP_OK) {
    size_t len = 0;
    using nimbus::store::NvsValKind;
    using nimbus::store::nvsValueEntries;
    if (t == NVS_TYPE_STR && nvs_get_str(h, key, nullptr, &len) == ESP_OK)
      entries = nvsValueEntries(NvsValKind::Str, len ? len - 1 : 0);   // len counts the NUL
    else if (t == NVS_TYPE_BLOB && nvs_get_blob(h, key, nullptr, &len) == ESP_OK)
      entries = nvsValueEntries(NvsValKind::Blob, len);
    else
      entries = 1;   // an integer of any width
  }
  nvs_close(h);
  return entries;
}
}  // namespace

size_t nvsAvailableEntries() {
  nvs_stats_t st = {};
  if (nvs_get_stats(nullptr, &st) != ESP_OK) return 0;
  return st.available_entries;
}

bool nvsAdmit(const char* ns, const char* key, nimbus::store::NvsValKind kind, size_t bytes,
              bool erase) {
  using namespace nimbus::store;
  if (erase) return true;   // an erase only frees entries
  // The stats are read for every growing write (critical ones too) and every blob
  // write; a non-blob write that does not grow skips them (nvsAdmitWrite).
  const NvsAdmit a = nvsAdmitWrite(classifyNvsKey(ns, key), kind, bytes, storedEntries(ns, key),
                                   s_starved.load(), [] { return nvsAvailableEntries(); });
  if (a == NvsAdmit::Write) return true;
  s_refused.fetch_add(1);
  s_evictReq.store(true);   // the main-loop pass frees rebuildable entries for the next try
  return false;
}

bool nvsStarved() { return s_starved.load(); }
void setNvsStarved(bool starved) { s_starved.store(starved); }
bool takeNvsEvictRequest() { return s_evictReq.exchange(false); }
void noteNvsEvicted(uint32_t n) { s_evicted.fetch_add(n); }

NvsGuardStats nvsGuardStats() {
  NvsGuardStats s;
  s.refused = s_refused.load();
  s.evicted = s_evicted.load();
  s.starved = s_starved.load();
  return s;
}

}  // namespace nimbus::sys
