#pragma once
// Owner-config persistence honesty (CUM-479; the class of CUM-242 / CUM-389 / CUM-408).
//
// The NVS partition is 0x5000 = 20 KB (default_16MB.csv): 5 pages of 126 32-byte
// entries, one page held back for garbage collection, so ~504 entries (~16 KB) carry
// every key in every namespace. Two failures shared that space badly:
//
//   1. Save routes answered {"ok":true} for writes that never landed. The store
//      setters were `void` wrappers over solide::memory::setString(), whose `false`
//      on a full partition was thrown away, so the web UI toasted "Saved" over a
//      dropped connector, host switch, or key.
//   2. Rebuildable caches (the per-provider model catalogs, mcat_<provider>, up to
//      3800 B = ~120 entries each, nearly a whole page) were packed into whatever
//      NVS was left after every verify, so five verified providers exhausted it.
//
// This header holds the PORTABLE half of the fix (host-tested in test/test_nvs_full):
//
//   - the one honest save answer (HTTP 507 + {"error":"storage_full","message":...}),
//   - the empty-string write verdict (the driver cannot judge an empty write),
//   - a per-task persist-failure ledger a save route opens a scope on, so EVERY NVS
//     write the request makes is covered, including ones nested inside a setter,
//   - the catalog-cache placement rule: catalogs live in flash files, never in NVS,
//     and any legacy mcat_ key is evicted (the boot heal for a device full today).
//
// No Arduino here: the device seam (src/sys/config_nvs.cpp + src/agent/store.cpp)
// binds these to solide::memory, LittleFS, and the FreeRTOS task handle.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "nimbus/orch/provider_slots.h"

namespace nimbus {
namespace store {

// ---- the honest save answer --------------------------------------------------
// HTTP 507 Insufficient Storage: the request was fine, the device could not store it.
inline constexpr int  kStorageFullStatus = 507;
inline constexpr char kStorageFullCode[] = "storage_full";
// Copy (AGENTS.md section 6): what happened, then the one next step. The web UI
// shows this sentence verbatim in its failure toast (include/web/ui_js.h carries the
// same text as its offline fallback; test_nvs_full pins the two together).
inline constexpr char kStorageFullMessage[] =
    "Couldn't save: device storage is full. Remove an unused provider key or "
    "connector and try again.";

// {"error":"storage_full","message":"<kStorageFullMessage>"}
std::string storageFullJson();

// The reply a save route sends once its writes are done. `persisted` false means at
// least one write this request made did not land: the answer is ALWAYS the 507
// storage_full body, never the route's ok body (a partial save is not a save).
struct SaveReply {
  int         status;
  std::string body;
};
SaveReply saveReply(bool persisted, int okStatus, const std::string& okBody);

// ---- string writes the driver cannot judge -----------------------------------
// solide::memory::setString() returns true for an EMPTY value whatever happened:
// Preferences::putString() reports the bytes written, which is 0 for "" on success
// AND on failure, so the driver ORs in v.isEmpty(). For an empty value the store
// therefore reads the key back. `presentAfter` = the key exists after the write,
// `readBackEmpty` = it reads back as "". A non-empty value trusts the driver.
inline bool stringWriteLanded(bool driverOk, bool valueEmpty, bool presentAfter,
                              bool readBackEmpty) {
  if (!valueEmpty) return driverOk;
  return driverOk && presentAfter && readBackEmpty;
}

// ---- the store's write primitives, over an NVS-shaped `Kv` --------------------
// The device binds Kv to solide::memory (src/agent/store.cpp); the host test binds a
// capacity-limited fake that refuses writes like a full partition (test_nvs_full).
// Kv needs: bool setString(const char*, const Str&); Str getString(const char*, const
// char* def); bool has(const char*); bool erase(const char*). `Str` is Arduino String
// on the device and std::string on the host (both have length()).

// Write a string; true iff it LANDED (an empty value is read back, see above).
template <typename Kv, typename Str>
inline bool writeString(Kv& kv, const char* key, const Str& v) {
  const bool drv = kv.setString(key, v);
  if (v.length()) return drv;
  const bool present = kv.has(key);
  const bool empty   = present && kv.getString(key, "\x01").length() == 0;
  return stringWriteLanded(drv, true, present, empty);
}

// Remove a key; true iff it is gone. An erase only FREES entries (it never
// allocates), so it lands even on a 100% full partition - which is what makes the
// storage-full remedy ("remove an unused provider key or connector") work there.
template <typename Kv>
inline bool eraseLanded(Kv& kv, const char* key) {
  if (!kv.has(key)) return true;
  kv.erase(key);
  return !kv.has(key);
}

// For keys whose getter default is "": an empty value is stored as ABSENT (an
// erase), never as an empty string, so a clear frees space instead of needing a
// fresh entry. Getters read an absent key as "" already, so nothing observable moves.
template <typename Kv, typename Str>
inline bool writeOrErase(Kv& kv, const char* key, const Str& v) {
  return v.length() ? writeString(kv, key, v) : eraseLanded(kv, key);
}

// A SHRINKING rewrite of a big value (the connectors blob after a delete). NVS
// writes the new entry before it erases the old one, so on a 100% full partition
// even a smaller value cannot land while the old one still holds its entries. Only
// when the plain write failed AND the new value is no larger than the stored one:
// erase, then write - the erase frees at least as many entries as the write needs.
// If the write still fails (another task's write took the freed room first), the
// old value is written back, best effort. NOT power-atomic: a power cut between the
// erase and the write loses the value (NVS's own new-before-erase order is exactly
// what a full partition rules out). The window is one write long and only opens on
// a full partition after a plain write already failed; removing a provider key (a
// pure erase) stays fully safe.
template <typename Kv, typename Str>
inline bool writeShrinkSafe(Kv& kv, const char* key, const Str& v) {
  if (!v.length()) return eraseLanded(kv, key);
  if (kv.setString(key, v)) return true;
  if (!kv.has(key)) return false;
  const Str old = kv.getString(key, "");
  if (v.length() > old.length()) return false;
  kv.erase(key);
  if (kv.setString(key, v)) return true;
  kv.setString(key, old);
  return false;
}

// ---- per-task persist-failure ledger -----------------------------------------
// Every store write reports its verdict; only FAILURES are recorded (the success path
// costs nothing). A save route opens a scope (remembers seq() and its task) before its
// first write and asks failedSince() before it answers. Failures are attributed to the
// task that made them, so a turn task's failed write does not turn an unrelated web
// save into a 507 - unless more than kRing failures land inside one request, when the
// answer is fail-closed (see failedSince). The web routes all run on the one AsyncTCP
// task, one at a time, so a scope sees exactly its own request's writes.
//
// Not internally locked: the device wraps every call in a spinlock (config_nvs.cpp);
// the host test is single-threaded.
class PersistLedger {
 public:
  static constexpr size_t kRing   = 8;    // recent failures kept for attribution
  static constexpr size_t kKeyMax = 16;   // NVS keys are <= 15 chars + NUL

  // Record one write that did not land. `key` is copied (truncated to 15 chars).
  void noteFailure(uintptr_t task, const char* key);

  // Failures recorded since boot (monotonic; a scope remembers it as its start).
  uint32_t seq() const { return seq_; }

  // Did `task` record a failure after `sinceSeq`? Fail-closed: if more failures
  // happened since `sinceSeq` than the ring holds, the ones that fell off cannot be
  // attributed, so the answer is true (a save is never reported ok on a guess).
  bool failedSince(uint32_t sinceSeq, uintptr_t task) const;

  // The most recent failing key ("" when nothing has failed). Diagnostics only.
  const char* lastKey() const;

 private:
  struct Rec {
    uint32_t  seq;
    uintptr_t task;
    char      key[kKeyMax];
  };
  Rec      ring_[kRing] = {};
  uint32_t seq_         = 0;
};

// ---- NVS usage, in bytes ------------------------------------------------------
// nvs_get_stats() counts 32-byte entries. `available` excludes the page NVS holds
// back for garbage collection, so it is the honest "room left" figure.
inline constexpr size_t kNvsEntryBytes = 32;
// The owner-config headroom rule: the largest single owner value - the connectors
// blob, capped at 3500 B = 1 header + 110 data entries = 3552 B - must be able to
// land, plus slack. NVS holds owner config and BOUNDED values only; nothing
// rebuildable and unbounded may live there (the catalogs moved to flash files; the
// mch_ choice lists stay, capped at 8 ids each). /api/state flags `low` below this so
// a filling device is visible before a save answers 507. It is a GUIDE, not a
// guarantee: a string must fit inside one 126-entry page, so free entries scattered
// across pages can still refuse a big value - the 507 contract is what stays exact.
inline constexpr uint32_t kOwnerHeadroomBytes = 4096;

struct NvsUsage {
  uint32_t usedBytes  = 0;
  uint32_t freeBytes  = 0;   // available for new data (GC page excluded)
  uint32_t totalBytes = 0;
  bool     low        = false;   // freeBytes < kOwnerHeadroomBytes
};
NvsUsage nvsUsageFromEntries(size_t usedEntries, size_t availableEntries,
                             size_t totalEntries);

// ---- provider catalog caches: flash files, never NVS --------------------------
// The capability catalog (GET /api/models) is a REBUILDABLE cache: every verify
// re-harvests it. It lives on LittleFS as /data/mcat_<slug>.json. Before CUM-479 it
// lived in NVS under mcat_<slug>; those keys are evicted on boot and on each verify.
//
// The per-provider JSON is still bounded so GET /api/models (which frames every
// provider's catalog in one response) stays a bounded payload. Flagships sort first,
// so a trim drops the lowest-priority models.
inline constexpr size_t kCatalogMaxBytes = 3800;

// Providers that can own a catalog cache: every canonical registry slot plus the
// free-form custom endpoint. Walk them with forEachCatalogProvider so a slot added
// to kProviderSlots is covered with no new list to update (the CUM-242 drift).
inline constexpr char kCustomProvider[] = "custom";
template <typename Fn>
inline void forEachCatalogProvider(Fn fn) {
  for (size_t i = 0; i < orch::kProviderSlotCount; i++) fn(orch::kProviderSlots[i].slug);
  fn(kCustomProvider);
}

// A slug is safe to embed in a file name / NVS key: 1-10 chars of [a-z0-9]
// ("mcat_" + 10 = the 15-char NVS key ceiling). Anything else has no cache path.
bool isCatalogSlug(const char* slug);
std::string catalogCachePath(const char* slug);   // "/data/mcat_<slug>.json", "" if unsafe
std::string legacyCatalogKey(const char* slug);   // "mcat_<slug>", "" if unsafe

// Evict every legacy NVS catalog cache. `Kv` needs `bool has(const char*)` and
// `bool erase(const char*)`. Erasing only frees entries (it never allocates), so this
// works on a 100% full partition - it is the heal for a device that is full today.
// Returns how many keys were present and are now gone.
template <typename Kv>
inline int evictLegacyCatalogs(Kv& kv) {
  int evicted = 0;
  forEachCatalogProvider([&](const char* slug) {
    const std::string key = legacyCatalogKey(slug);
    if (key.empty() || !kv.has(key.c_str())) return;
    if (eraseLanded(kv, key.c_str())) ++evicted;
  });
  return evicted;
}

}  // namespace store
}  // namespace nimbus
