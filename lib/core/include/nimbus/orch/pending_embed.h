#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/memory_tools.h"   // EmbedFail
#include "nimbus/orch/psram_alloc.h"

// pending_embed - memory writes that survive an embedding outage.
//
// A memory write needs an embedding before the vector store can take it
// (VectorMemory::add rejects anything that is not a full-width vector). When the
// embedder fails (out of credit, key rejected, offline), the fact used to be
// thrown away: Lumi lost 8 facts on 13-14 Oct 2026 to an OpenAI credit outage.
// This queue keeps the TEXT (never a vector, so a later model change cannot
// strand it) and the write seam stores it once embeddings work again, through the
// same rails as a direct write.
//
// Two engines, both portable and host-tested:
//   - PendingEmbeds: the bounded, durable queue (a sidecar blob on the SD card,
//     else LittleFS; PSRAM working set). When full it REFUSES new writes; it never
//     evicts a queued fact, because those are the owner's facts.
//   - EmbedPark: a RAM-only memo of "embeddings are down, since when, retry when",
//     so a parked device pays no doomed TLS embed per turn, per write or per drain.
namespace nimbus {
namespace orch {

enum class PendingOp : uint8_t { Write = 0, Update = 1 };

struct PendingEmbed {
  std::string id;            // contentId(content): the id the stored memory gets
  std::string ns;            // the authenticated caller's namespace at enqueue time
  PendingOp   op = PendingOp::Write;
  std::string oldId;         // Update: the memory to replace, by id ...
  std::string oldDesc;       // ... or by a short description of the old fact
  std::string content;       // the fact, at most kMemModelMax bytes
  float       importance = 0.5f;
  bool        permanent = false;
  std::string ttlName;       // the ttl CLASS name; the clamp is applied when stored
  std::string source = "self";
  uint32_t    createdAtHours = 0;
  bool        clockSynced = false;   // false: re-stamp with the real clock when stored
  uint16_t    attempts = 0;          // failed embed attempts so far
  uint8_t     lastFail = 0;          // the last EmbedFail (numbers are append-only)
};

class PendingEmbeds {
 public:
  // Bounds (owner-accepted 18 Oct): 64 entries or 48 KB in total, whichever comes
  // first, and 16 for any namespace other than the owner's.
  static constexpr size_t kMaxEntries = 64;
  static constexpr size_t kMaxBytes = 48 * 1024;
  static constexpr int    kMaxPerMember = 16;
  // Drain notes waiting for the next turn of their namespace (RAM only).
  static constexpr size_t kMaxNotes = 8;

  enum class Add { Added, Coalesced, Full, BytesFull, NsFull, TooLong, Empty };

  // Queue one entry. The same id twice coalesces into one entry (the higher
  // importance wins; permanent sticks). A full queue refuses: nothing queued is
  // ever evicted to make room. Content longer than kMemModelMax is refused
  // (TooLong), never cut: a cut fact would be stored as a different fact.
  Add enqueue(const PendingEmbed& e);

  // Oldest entry, or null when empty. Not removed: the drain pops it only after
  // the stored vector is durable.
  const PendingEmbed* front() const;
  const PendingEmbed* find(const std::string& id) const;
  bool pop(const std::string& id);
  // A failed attempt: count it, remember why, and move the entry to the back so
  // one entry the provider keeps refusing cannot block the others.
  void noteFailed(const std::string& id, EmbedFail why);

  size_t size() const { return entries_.size(); }
  bool   empty() const { return entries_.empty(); }
  size_t bytes() const;                       // sum of entryBytes over the queue
  int    countIn(const std::string& ns) const;
  // Entries visible under a read set (empty = unscoped), oldest first.
  std::vector<PendingEmbed> getAll(const std::vector<std::string>& nsAllow = {}) const;
  // Visit every entry, oldest first, by const reference (no copy): the degraded
  // keyword recall reads the queue on a device that may be short of memory.
  template <typename Fn>
  void forEachEntry(Fn&& fn) const {
    for (const PendingEmbed& e : entries_) fn(e);
  }
  int    removeNs(const std::string& ns);
  int    flushAll();

  bool dirty() const { return dirty_; }
  void markClean() { dirty_ = false; }

  // The serialized size of one record: what the byte bound counts.
  static size_t entryBytes(const PendingEmbed& e);

  // Blob: magic "PEQ1", a version byte, a count, then the records. deserialize is
  // tolerant: a truncated blob keeps its clean prefix and returns false; garbage
  // or an absent blob gives an empty queue. It never throws.
  std::string serialize() const;
  bool deserialize(const std::string& blob);

  // Drain notes: "queued memory <id> is now stored" for the next turn of the
  // namespace that wrote it. Bounded; the oldest note goes first when full.
  void addNote(const std::string& ns, const std::string& text);
  std::string takeNotes(const std::string& ns);

 private:
  struct Note { std::string ns, text; };
  int indexOf(const std::string& id) const;
  std::vector<PendingEmbed, WorkingAllocator<PendingEmbed>> entries_;   // FIFO
  std::vector<Note> notes_;
  bool dirty_ = false;
};

// ---- EmbedPark --------------------------------------------------------------
// What a failure class does to the next embed attempt.
enum class ParkRule : uint8_t {
  Clear,     // None: embeddings work; the park ends
  Retry,     // a local, momentary failure (the TLS slot was busy): no park
  Backoff,   // transient at the provider: 30 s, doubling, capped at 30 min
  Long,      // needs a person (credit, key, model, width, route): 30 min, then one probe
  NoChange,  // nothing to embed (empty text): says nothing about the provider
};
// Every EmbedFail maps to a rule; the host test walks the whole enum.
ParkRule parkRuleFor(EmbedFail kind);

class EmbedPark {
 public:
  static constexpr uint32_t kBackoffBaseMs = 30u * 1000u;
  static constexpr uint32_t kMaxParkMs = 30u * 60u * 1000u;

  // True while an attempt would be doomed. After the park time one attempt (the
  // probe) is let through; its result re-parks or clears.
  bool blocked(uint32_t nowMs) const;
  // Record an attempt's result (None = success). Returns true when this result
  // parked for a NEW reason (the caller may tell the owner once per episode).
  // `detail` is classifyEmbedFail's secret-safe detail (a known slug or digits),
  // kept so the parked words name the same cause.
  bool onResult(EmbedFail kind, uint32_t nowMs, const std::string& detail = "");
  void clear();   // a config or key change: the next attempt goes straight through

  bool      parked() const { return reason_ != EmbedFail::None; }
  EmbedFail reason() const { return reason_; }
  const std::string& detail() const { return detail_; }
  uint32_t  sinceMs() const { return sinceMs_; }
  uint32_t  retryAtMs() const { return retryAtMs_; }
  int       streak() const { return streak_; }

 private:
  EmbedFail reason_ = EmbedFail::None;
  std::string detail_;
  uint32_t  sinceMs_ = 0;
  uint32_t  retryAtMs_ = 0;
  int       streak_ = 0;
};

}  // namespace orch
}  // namespace nimbus
