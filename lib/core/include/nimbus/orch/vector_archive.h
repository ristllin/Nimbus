#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/psram_alloc.h"
#include "nimbus/orch/vector_memory.h"   // VecEntry / VecHit (shared record types)

// vector_archive - the cold store for memories that reached their TTL. When a
// live VectorMemory entry expires, the maintenance pass MOVES it here instead of
// deleting it (CUM-225), so the expensive part - the embedding - is preserved and
// the fact can be searched or restored later. This exists ONLY on the SD card:
// with no card the live engine drops entries at TTL exactly as before (the device
// simply never attaches this store as the prune sink), so an archive is a bonus of
// having storage, never a behavior the no-card device pays for.
//
// It is deliberately a SEPARATE engine, not a flag on VectorMemory, so archived
// entries are invisible to normal recall by construction (they are not in the live
// set) and the cold store can be capped, FIFO-evicted, and persisted on its own
// cadence without touching the hot path. Same shape as VectorMemory: int8-quantized
// vectors, brute-force cosine, PSRAM-resident working set, an SD blob for
// persistence. Portable, Arduino-free, host-tested (pio test -e native).
namespace nimbus {
namespace orch {

class VectorArchive {
 public:
  // Match the live store's width; every archived/searched vector must be this wide.
  void configure(int dims) { dims_ = dims > 0 ? dims : 256; }
  int  dims() const { return dims_; }
  int  size() const { return (int)entries_.size(); }

  // FIFO capacity. 0 = unlimited (default). When archive() would exceed the cap it
  // drops the OLDEST archived entry first (a cold store trades away the least-recently
  // expired, not the least important - importance already lost the entry its place in
  // the live set). Permanent/creator entries never reach here (they are exempt from
  // expiry), so there is nothing to protect from FIFO eviction.
  void setMaxEntries(int n) { maxEntries_ = n < 0 ? 0 : n; }
  int  maxEntries() const { return maxEntries_; }

  // Dirty tracking: the device persists the archive blob only when it changed, so a
  // dream pass that expires nothing (or a run with no SD) never rewrites the card.
  bool dirty() const { return dirty_; }
  void markClean() { dirty_ = false; }

  // Move an expired entry into the archive. `archivedAtHours` stamps when it was
  // archived (FIFO age + a user-facing "archived N hours ago"). If an entry with the
  // same id is already archived (a content hash that expired, was restored, and
  // expired again), it is replaced and moved to the most-recent position so restore
  // stays unambiguous. Preserves the int8 embedding verbatim. A wrong-width vector is
  // rejected (returns false) so the store's invariant holds. Returns true on store.
  bool archive(const VecEntry& e, uint32_t archivedAtHours);

  // Brute-force cosine search over the archive, top-k by ascending distance. `nsAllow`
  // bounds what the caller may SEE (same v3.7.0 read boundary as the live store); an
  // empty allow-list is unscoped (admin/maintenance). Wrong-width query or empty store
  // -> empty. Read-only: never stamps or mutates.
  std::vector<VecHit> search(const std::vector<int8_t>& query, int k,
                             const std::vector<std::string>& nsAllow = {}) const;

  // Take an entry OUT of the archive by id (for restore): copies it to `out`, removes
  // it, and returns true. False if the id is unknown OR not visible under `nsAllow`
  // (an empty allow-list is unscoped). The removal + copy is one step so a restore can
  // never leave a ghost in both stores.
  bool take(const std::string& id, VecEntry& out, const std::vector<std::string>& nsAllow = {});

  // Is this id inside the caller's read set? (Mirrors VectorMemory::idVisible - every
  // id-keyed op must check it so one principal can't touch another's archived memory.)
  bool idVisible(const std::string& id, const std::vector<std::string>& nsAllow) const;

  bool remove(const std::string& id);      // purge one entry (admin/browse)
  // Browse: FIFO order (newest-archived last). `nsAllow` bounds visibility (empty =
  // unscoped); use it so a browse never crosses the data boundary.
  std::vector<VecEntry> getAll(const std::vector<std::string>& nsAllow = {}) const;
  int  flushAll();

  // ---- persistence (SD file, device seam) ----
  // Compact little-endian blob: magic + dims + every entry (importance, ttl, created,
  // archivedAt, flags, id/content/source/ns, vec bytes). deserialize() is tolerant:
  // a truncated/garbage blob restores the clean prefix and returns false (never throws),
  // and a width mismatch is treated as corruption (clean prefix kept). The device writes
  // it to /mem/archive.bin; host tests round-trip it in memory.
  std::string serialize() const;
  bool deserialize(const std::string& blob);

 private:
  struct Stored {
    std::string id;
    std::string content;
    float       importance = 0.5f;
    int32_t     ttlHours = 720;
    uint32_t    createdAtHours = 0;
    uint32_t    archivedAtHours = 0;
    std::string source = "system";
    std::string ns;
    bool        creatorFlag = false;
    bool        permanentFlag = false;
    std::vector<int8_t, WorkingAllocator<int8_t>> vec;
  };
  static VecEntry toPublic(const Stored& s);
  int indexOf(const std::string& id) const;

  int dims_ = 256;
  int maxEntries_ = 0;    // 0 = unlimited
  bool dirty_ = false;
  std::vector<Stored, WorkingAllocator<Stored>> entries_;   // insertion order = FIFO age
};

}  // namespace orch
}  // namespace nimbus
