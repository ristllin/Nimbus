#pragma once
// nvs_reserve - which settings-storage (NVS) writes may spend the last free entries.
//
// The NVS partition is 20 KB (about 504 usable 32-byte entries) and every key in every
// namespace shares it. On a desk device in October 2026 per-turn rewrites (the usage
// ledger, the scratchpad, the verify cache, the sub-agent journal) filled it, and from
// then on the writes that keep the device safe failed with everything else: the
// Telegram offset (a replay loop of paid turns), the OTA rollback guard, the cloud
// credential. This header is the portable half of the fix (host-tested in
// test/test_nvs_full):
//
//   - every key has a CLASS: critical (the device's safety: offset, OTA guard,
//     credentials), owner config (what a person set), rebuildable (a cache that the
//     device can fetch or derive again) or telemetry (runtime state and counters);
//   - a RESERVE of kNvsReserveEntries free entries that only critical writes may use;
//   - erases and writes that do not grow a value never touch the reserve (they free
//     space or reuse it), so the storage-full remedy always works; a blob write must
//     fit its whole span, a rewrite included (the IDF leak below);
//   - when the reserve cannot be restored even after evicting the rebuildable caches
//     (the device is STARVED), owner-config writes are accepted again and only
//     rebuildable and telemetry writes stay refused, so a full device never answers
//     "storage full" to every save forever;
//   - the eviction plan: which rebuildable keys may go, in which order. Never the
//     managed-agent ids (antEnv, antAgents: evicting them orphans remote agents that
//     keep costing money) and never the model choice lists (mch_: wave 1 has no file
//     store for them yet).
//
// No Arduino: the device seam (src/sys/nvs_guard.cpp, src/agent/store.cpp) binds the
// decision to nvs_get_stats and the main-loop eviction pass.

#include <cstddef>
#include <cstdint>

namespace nimbus {
namespace store {

enum class NvsClass : uint8_t {
  Critical = 0,   // device safety: always written, may use the reserve
  Owner,          // owner config: refused at the reserve unless the device is starved
  Rebuildable,    // a cache the device can rebuild: refused at the reserve
  Telemetry,      // runtime state and counters: refused at the reserve
};
const char* nvsClassName(NvsClass c);   // "critical" | "owner" | "rebuildable" | "telemetry"

// Free entries only critical writes may use. The critical working set it must cover
// (test_nvs_full pins it): a cloud credential re-mint (about 12 entries) plus the OTA
// guard rewrite (about 7) plus the Telegram offset, with margin.
inline constexpr size_t kNvsReserveEntries = 24;

// One classification row. `prefix` rows match any key that starts with `key`.
// `evictOrder` > 0 marks a rebuildable key the main-loop pass may erase to restore the
// reserve; lower orders go first. 0 = never evicted.
struct NvsKeyClass {
  const char* ns;          // NVS namespace ("solide" holds every agent::store key)
  const char* key;
  bool        prefix;
  NvsClass    cls;
  uint8_t     evictOrder;
};

// The table, and the lookup. An unlisted key reads as Owner (the safe default: it is
// written when the device is starved), but test_nvs_full fails on any defined key that
// has no row, so "unlisted" only ever means a key from a future build.
const NvsKeyClass* nvsKeyTable(size_t* count);
const NvsKeyClass* findNvsKeyClass(const char* ns, const char* key);
NvsClass           classifyNvsKey(const char* ns, const char* key);

// ---- entry arithmetic --------------------------------------------------------
// NVS stores a string as one header entry plus its bytes and NUL in 32-byte data
// entries; a blob as an index entry plus, per chunk, a header and its data entries
// (one chunk below a page); an integer as one entry.
enum class NvsValKind : uint8_t { Int, Str, Blob };
size_t nvsValueEntries(NvsValKind kind, size_t bytes);   // `bytes` excludes a string's NUL

// The entries a blob write may take from nvs_get_stats' available count WHILE it is
// made. ESP-IDF writes the whole new version before it erases the old one, in chunks
// that follow the page tails: one data entry per 32 B, a chunk header per page the
// value reaches, the index entry, and, when the current page's tail is under a tenth
// of a page and too small for the value, that tail is skipped (up to 13 entries that
// still count as free but take nothing). test_nvs_full checks this span against a
// model of the IDF's chunk layout for every size up to 4,000 B and every start page.
size_t nvsBlobWriteSpan(size_t bytes);

// ---- the admission decision --------------------------------------------------
// Every growing write is checked against the available entries, critical ones
// included: a critical write may use the whole reserve but is refused when the
// partition cannot hold its new value at all. A blob write, new or rewrite, is
// admitted only when the available entries cover its whole span (nvsBlobWriteSpan):
// ESP-IDF 5.5.4's cleanup after a failed multi-page blob write erases the wrong chunk
// indices when the new copy is the second version, so one failed rewrite leaves every
// chunk it wrote stranded (counted used) until the next boot. Refused before the
// write, that path is never entered.
struct NvsAdmitIn {
  NvsClass cls          = NvsClass::Owner;
  size_t   available    = 0;   // nvs_get_stats available_entries (GC page excluded)
  size_t   newEntries   = 0;   // entries the new value takes
  size_t   oldEntries   = 0;   // entries the stored value takes (0 = absent)
  size_t   blobSpan     = 0;   // a blob write: nvsBlobWriteSpan(bytes); 0 = not a blob
  bool     erase        = false;
  bool     starved      = false;   // the main-loop pass could not restore the reserve
};
enum class NvsAdmit : uint8_t {
  Write = 0,        // go ahead (the driver may still refuse a truly full partition)
  RefuseReserve,    // would eat into the reserve: refused, reported as storage full
  RefuseNoRoom,     // the partition cannot hold the new value (or a blob's span) at all
};
NvsAdmit nvsAdmit(const NvsAdmitIn& in);

// ---- the eviction plan -----------------------------------------------------------
// Given the keys stored in the "solide" namespace, fills `out` with the indexes of the
// ones the main-loop pass may erase, in eviction order (lowest evictOrder first, then
// key order), and returns how many. fbRules counts only while it is a cloud copy
// (`fbRulesIsCloudCopy`, from fbSyncTs != 0); a local owner edit is never evicted.
size_t nvsEvictionPlan(const char* const* keys, size_t n, bool fbRulesIsCloudCopy,
                       size_t* out, size_t outCap);

// True when this write does not grow NVS: an erase, or a value no larger (in entries)
// than the one it replaces. Such writes never touch the reserve.
inline bool nvsWriteShrinks(bool erase, size_t newEntries, size_t oldEntries) {
  return erase || (oldEntries > 0 && newEntries <= oldEntries);
}

// The whole admission for one write, the way the device asks it: `available` is read
// (it costs an nvs_get_stats walk) for every write that needs it, a growing write of
// any class and every blob write, and never for an erase or a non-blob write that does
// not grow. `available` is a callable returning the available entries.
template <typename AvailableFn>
NvsAdmit nvsAdmitWrite(NvsClass cls, NvsValKind kind, size_t bytes, size_t oldEntries,
                       bool erase, bool starved, AvailableFn&& available) {
  NvsAdmitIn in;
  in.cls        = cls;
  in.newEntries = nvsValueEntries(kind, bytes);
  in.oldEntries = oldEntries;
  in.blobSpan   = kind == NvsValKind::Blob ? nvsBlobWriteSpan(bytes) : 0;
  in.erase      = erase;
  in.starved    = starved;
  if (!erase && (in.blobSpan || !nvsWriteShrinks(false, in.newEntries, oldEntries)))
    in.available = available();
  return nvsAdmit(in);
}

}  // namespace store
}  // namespace nimbus
