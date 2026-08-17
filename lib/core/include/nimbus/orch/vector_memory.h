#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/psram_alloc.h"

// vector_memory - associative (semantic) memory. A portable, device-sized
// engine: importance decay, TTL, dedup-on-write, permanent/creator
// exemptions, prune, browse. An ESP32 cannot run an embedder, so this engine
// takes ALREADY-EMBEDDED vectors (the provider /embeddings API is a device
// seam, Ph3 device wiring) and stores them INT8-QUANTIZED (locked decision:
// low vector size to fit the SD/PSRAM). Recall is brute-force cosine - fine
// for the thousands of small vectors a desk device accumulates.
//
// Splitting the (host-testable) math from the (network) embedding is deliberate:
// every rule below - cosine, dedup < 0.05, decay 0.95, prune < 0.05 / past-TTL,
// permanence - is unit-tested via pio test -e native by injecting vectors
// directly. Persistence to the SD is layered on top by the device (the engine
// holds the working set in RAM; a device store mirrors it), exactly like
// OrchMemory/Journal's seam pattern. Arduino-free.
namespace nimbus {
namespace orch {

// ---- reserved data namespaces (v3.7.0) --------------------------------------
// Declared here (the lowest layer that stores them) so vector_memory does not
// depend on the tool layer. kOwnerNs is the human who owns the device - ALL of
// their chats (web/serial/voice/telegram-owner) map to it, because they are one
// person; kSharedNs holds device-level facts every principal may READ but only
// an owner may WRITE; kMcpNs is the LAN MCP endpoint, which authenticates with
// one device token and carries no per-caller identity.
inline const char* kOwnerNs  = "owner";
inline const char* kSharedNs = "shared";
inline const char* kMcpNs    = "mcp";

// Map a routing chat to its data namespace. EVERY owner chat collapses to
// kOwnerNs - the owner is one person whether they arrive by web, serial, voice
// or their own Telegram chat, and splitting them would strand their memories
// per channel. A member's chat gets its own namespace, keyed by chat id.
inline std::string nsForChat(const std::string& chatId, bool isOwner) {
  if (isOwner) return kOwnerNs;
  return chatId.empty() ? std::string() : ("chat:" + chatId);
}

// A stored memory entry. `vec` is the int8-quantized embedding (see quantize()).
// Times are in HOURS since an arbitrary epoch, supplied by the caller (the
// device uses wall-clock hours; host tests pass explicit values).
struct VecEntry {
  std::string id;
  std::string content;
  float       importance = 0.5f;      // 0..1
  int32_t     ttlHours = 720;         // 30 days; -1 = never expires
  uint32_t    createdAtHours = 0;
  std::string source = "system";
  // v3.7.0 data boundary: WHOSE memory this is. Empty means legacy/unattributed
  // (loaded as the owner's - never as shared: a migration must not widen access).
  // Wire-compatible by construction: it rides the `source` field as
  // "source\x1Fns", so a pre-v3.7 image reads the blob without corruption (it
  // just shows an odd provenance label) and an OTA ROLLBACK preserves the tag.
  // Bumping the magic instead would have been catastrophic - deserialize()
  // clears the store before validating and the nightly dream then persists the
  // empty result.
  std::string ns;
  bool        creatorFlag = false;    // exempt from prune (user-authored)
  bool        permanentFlag = false;  // exempt from decay AND prune
  uint32_t    lastRecallHours = 0;    // wall-hours of the last search/recall HIT
                                      // (0 = never recalled). Usage bookkeeping -
                                      // stamped by const reads via a mutable field.
  std::vector<int8_t> vec;
};

// A recall hit (a search() result): the entry's content + its cosine
// DISTANCE to the query (0 = identical direction, up to 2). similarity = 1 - d.
// `score` is the composite recall score (recall() only; 0 for plain search()).
struct VecHit {
  std::string id;
  std::string content;
  float       importance = 0.5f;
  float       distance = 1.0f;
  float       score = 0.0f;
};

// Tunables for composite-ranked recall (recall()). Relevance x recency x
// importance, with query-time expiry filtering, near-duplicate collapse, and
// MMR diversity applied ONLY when the top-k scores are near-tied (so a single
// clearly-best answer for a narrow query is never diversified away).
struct RecallParams {
  int   k = 10;
  // v3.7.0 read boundary. Empty = UNSCOPED (every entry considered) - the
  // pre-namespace behavior, kept for maintenance passes (dream, prune) that
  // legitimately span the store. A turn ALWAYS passes its principal's set, so
  // one chat can never recall another's memories.
  std::vector<std::string> nsAllow;
  float relevanceThreshold  = 0.0f;   // min cosine similarity to include
  float recencyHalfLifeHours = 168;   // 1 week: recency weight halves each half-life
  float mmrLambda           = 0.7f;   // MMR relevance-vs-diversity weight
  static constexpr float kRecencyFloor = 0.35f;  // oldest memories still rank, not zeroed
  static constexpr float kPinnedRecencyFloor = 0.75f;  // permanent/creator hold recency up
  static constexpr float kTieEpsilon   = 0.05f;  // top-k score spread below this -> MMR
  static constexpr float kQueryDupDist = 0.10f;  // collapse hits within this cosine distance
};

class VectorMemory {
 public:
  // Semantic constants.
  static constexpr float kDuplicateThreshold = 0.05f;  // cosine distance < this => dup
  static constexpr float kMinImportance = 0.05f;       // prune floor
  static constexpr float kDecayFloor = 0.01f;          // importance never below this
  static constexpr float kDefaultDecay = 0.95f;

  // Fixed vector dimensionality for this store. All added/searched vectors must
  // match; mismatched vectors are rejected (the set-once embed-config contract
  // lives above this in Ph3 - here we just enforce a consistent width).
  void configure(int dims) { dims_ = dims > 0 ? dims : 256; }
  int  dims() const { return dims_; }
  int  size() const { return (int)entries_.size(); }

  // Cap the working set. 0 = unlimited (default). When add() would exceed the cap
  // it evicts the entry with the LOWEST retention score = importance * ttl-left
  // fraction (permanent/creator entries are exempt - never evicted). This keeps
  // the device's RAM/flash bounded (the whole VDB is a browse-able set) while
  // preserving what's both important and long-lived; frequently-recalled memories
  // survive because recall boosts their score (boostAccessed).
  void setMaxEntries(int n) { maxEntries_ = n < 0 ? 0 : n; }
  int  maxEntries() const { return maxEntries_; }

  // Retention score used for eviction: importance * clamp(ttl-left / ttl, 0, 1).
  // Permanent/creator -> +inf (never evicted); ttl<=0 (never expires) -> importance.
  float retentionScore(const VecEntry& e, uint32_t nowHours) const;

  // Quantize a float embedding (any range; typically L2-normalized to [-1,1]) to
  // int8 by v -> round(v * 127), clamped to [-127,127]. ~256 B for a 256-dim
  // vector. Static so the device embedding seam can quantize before add().
  static std::vector<int8_t> quantize(const std::vector<float>& v);

  // Add an entry (its .vec must be dims() wide). With dedup (the default): if
  // the nearest existing entry is within kDuplicateThreshold, SKIP the write and
  // bump that entry's importance to max(old, new); returns false (skipped).
  // Returns true when actually inserted. A wrong-width vector returns false.
  bool add(const VecEntry& e, bool dedup = true);

  // Top-k recall by ascending cosine distance. Empty store or
  // wrong-width query -> empty. k is clamped to the store size. Legacy path,
  // still used by dedup / update-targeting / corroboration.
  // nowHours > 0 filters expired entries with the same predicate as recall()
  // (query-time TTL); 0 = legacy unfiltered (clockless callers: dedup targeting).
  // `nsAllow` (v3.7.0) bounds what the caller may SEE; empty = unscoped, which
  // is what internal maintenance (dedup targeting) wants. The model-facing
  // memory.search always passes its principal's set.
  std::vector<VecHit> search(const std::vector<int8_t>& query, int k,
                             uint32_t nowHours = 0,
                             const std::vector<std::string>& nsAllow = {}) const;

  // Composite-ranked recall for turn injection: score = sim * recency *
  // importance, expired entries filtered at query time (so TTL is honoured even
  // if the nightly prune has not run), near-duplicates collapsed, and MMR
  // diversity applied only when the top-k scores are near-tied. Returns up to
  // p.k hits in selection order (best first), each carrying its composite score.
  std::vector<VecHit> recall(const std::vector<int8_t>& query, const RecallParams& p,
                             uint32_t nowHours) const;

  // Access-boost: reward recalled memories so they resist decay + eviction
  // (reinforce on access). For each id, importance += impBoost (clamped to 1.0); if
  // nowHours > 0, createdAtHours is refreshed to nowHours so the TTL clock resets
  // (extends life). Returns the number of entries boosted. Call with the ids that
  // search() returned after injecting them into a turn.
  int boostAccessed(const std::vector<std::string>& ids, float impBoost, uint32_t nowHours = 0);

  // Maintenance cadence: decay then prune.
  //  decayImportance: importance *= factor for non-permanent entries, floor
  //    kDecayFloor.
  //  pruneExpired: delete entries with importance < kMinImportance OR older than
  //    ttlHours (unless permanent/creator). Returns the count removed.
  void decayImportance(float factor = kDefaultDecay);
  int  pruneExpired(uint32_t nowHours);
  // Re-stamp entries created with a BOOT-RELATIVE clock (createdAtHours below
  // `threshold`, i.e. pre-SNTP) to `nowHours` at first sync - otherwise their
  // age jumps to ~half a century the moment the clock syncs and they expire
  // instantly (Release C3: closes the whole stamp class - write/update/unpin -
  // not just the boost path). Returns the number re-stamped.
  int  restampPreSync(uint32_t threshold, uint32_t nowHours);

  // Browse/admin (get_all / delete / flush / mark_permanent / dedupe).
  std::vector<VecEntry> getAll() const;               // importance-desc
  // v3.7.0: is this id inside the caller's read set? Every id-keyed mutation
  // (remove / markPermanent / markTemporary) must check it - filtering READS
  // alone would still let one principal delete or pin another's memory by
  // guessing or observing an id.
  // v3.7.0 quotas: how many entries this namespace holds, and how many of them
  // are permanent pins. Counted at the WRITE seam - a quota enforced only by a
  // later prune lets a tenant win the race and keep what it grabbed.
  uint32_t countIn(const std::string& ns) const;
  uint32_t pinsIn(const std::string& ns) const;

  bool idVisible(const std::string& id, const std::vector<std::string>& nsAllow) const;

  bool markPermanent(const std::string& id);
  // Reverse of markPermanent: clear the pin so the entry decays/expires again and
  // becomes evictable. Restores a finite TTL (defTtlHours, default 720 = 30 days)
  // and resets the TTL clock to nowHours so an old pinned entry isn't instantly
  // pruned. Returns false if the id is unknown.
  bool markTemporary(const std::string& id, int32_t defTtlHours = 720, uint32_t nowHours = 0);
  bool remove(const std::string& id);
  int  flushAll();
  int  flushNonPermanent();
  // Pairwise near-dup scan, keep the higher-importance twin. windowLimit > 0
  // bounds the scan to the NEWEST windowLimit entries (Release C2: the full
  // O(n²·dims) pass at the 5000 cap is ~3.2e9 int8 ops under the global memory
  // lock; older entries were deduped on previous nightly passes). 0 = full scan
  // (the web admin op keeps it - explicit owner action).
  int  deduplicate(int windowLimit = 0);

  // ---- persistence (SD file, Ph3 device seam) ----
  // Compact little-endian binary blob of dims + every entry (importance, ttl,
  // createdAt, flags, id/content/source, vector bytes). The device writes it to
  // /sd/mem/vectors.bin; host tests round-trip it in memory. deserialize() is
  // tolerant: a truncated/garbage blob restores whatever prefix parsed cleanly
  // and returns false (so the device can log a partial load) - it never throws.
  std::string serialize() const;
  bool deserialize(const std::string& blob);

 private:
  // Internal storage entry: identical to the public VecEntry except the int8 vector
  // buffer uses the PSRAM-routed WorkingAllocator (the dominant per-entry cost). The
  // small SSO strings stay on the internal heap. The public API keeps taking/returning
  // VecEntry (std::vector<int8_t> vec) so callers/tests are unchanged; add()/getAll()/
  // search() convert at the boundary (a cheap per-entry copy of the vector bytes).
  struct Stored {
    std::string id;
    std::string content;
    float       importance = 0.5f;
    int32_t     ttlHours = 720;
    uint32_t    createdAtHours = 0;
    std::string source = "system";
    std::string ns;              // v3.7.0 data boundary (see VecEntry::ns)
    bool        creatorFlag = false;
    bool        permanentFlag = false;
    // Access stamp (NOT content): search()/recall() are logically const reads,
    // so the usage bookkeeping is mutable. nowHours==0 callers (clockless dedup
    // probes) never stamp - an internal probe is not a "use".
    mutable uint32_t lastRecallHours = 0;
    std::vector<int8_t, WorkingAllocator<int8_t>> vec;
  };
  static VecEntry toPublic(const Stored& s);
  // Retention score on the raw fields, shared by the public retentionScore(VecEntry)
  // and the internal eviction loop (which works on Stored).
  static float retentionOf(float importance, int32_t ttlHours, uint32_t createdAtHours,
                           bool permanent, bool creator, uint32_t nowHours);
  // Expiry predicate shared by pruneExpired (nightly) and recall (query-time), so
  // the two can never disagree about what counts as expired.
  static bool isExpiredRaw(float importance, int32_t ttlHours, uint32_t createdAtHours,
                           bool permanent, bool creator, uint32_t nowHours);

  int findNearest(const std::vector<int8_t>& q, float& distOut) const;  // -1 if empty
  int indexOf(const std::string& id) const;

  int dims_ = 256;
  int maxEntries_ = 0;   // 0 = unlimited
  std::vector<Stored, WorkingAllocator<Stored>> entries_;
};

// Cosine distance between two equal-width int8 vectors: 1 - dot/(|a||b|).
// Returns 1.0 (orthogonal) if either is zero-length or widths differ. Exposed
// for tests.
float cosineDistanceI8(const std::vector<int8_t>& a, const std::vector<int8_t>& b);

}  // namespace orch
}  // namespace nimbus
