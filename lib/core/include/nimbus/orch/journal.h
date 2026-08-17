#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/result_envelope.h"  // JobState + isTerminal

// Ported and adapted from Nuage-Solide src/agent/journal.{h,cpp}
// (Head Orchestrator v2). The LOGIC (add/find/complete/evict/gc/count/get, the
// 6-record ceiling + tag dedupe + seen-eviction, and reboot re-attach) is made
// portable here; the NVS Preferences I/O is hoisted behind a JournalStore seam,
// exactly like OrchMemory's MemoryStore. The device implements JournalStore
// against the "agjournal" NVS namespace (keys j0..j5); host tests use an
// in-memory store.
//
// Per-job journal. Persists enough state to resume polling after a reboot without
// re-dispatching (avoids duplicate billing / duplicate work): at boot the device
// re-attaches to any unfinished job by polling jobId alone (backend prefix before
// ':' routes the adapter). The portable guarantee is only that loadAll() faithfully
// restores every record with jobId byte-identical.
namespace nimbus {
namespace orch {

// Durable NVS shape - fixed-size char[] (not std::string) so the record
// serializes to a compact, bounded JSON blob whose worst case fits the serialize
// buffer. The sizes are the durable contract (device UI + restart depend on them).
struct JobRecord {
  char     tag[24];       // device correlation id, "job0003" - the dedupe key, stable across reboot
  char     jobId[96];     // "backend:remoteId" - the durable re-attach key
  char     backend[16];   // "openai" | "anthropic" | "mistral"
  char     category[16];  // "code" | "research" | "ops"
  char     model[40];     // per-session model (UI + restart)
  char     name[24];      // model-chosen display name ("css-fixer"); "" = unnamed
  char     prj[25];       // FileStore project tag ("" = none) - the Done branch
                          // auto-persists the full reply to <prj>/<name>-<tag>.md
  char     chatId[32];    // Telegram chat to deliver result
  JobState state = JobState::Unknown;
  bool     resultSeen = false;   // true once result delivered + LED cleared
  uint32_t dispatchedAt = 0;     // millis() at dispatch (advisory; resets on reboot)
};

// Persistence seam. Slots are 0..kAgentMaxJobs-1. Device backs this with NVS
// Preferences (namespace "agjournal", keys "j0".."j5"); tests use a std::map.
struct JournalStore {
  virtual ~JournalStore() = default;
  virtual std::string get(int slot) = 0;                 // "" if empty
  virtual void        put(int slot, const std::string& v) = 0;
  virtual void        remove(int slot) = 0;
  virtual void        clearNs() = 0;                     // wipe the whole namespace
};

// Serialize buffer: worst-case record ~293 B with jobId[96]. The 384 B ceiling
// leaves headroom; serialize() REFUSES to persist a record that would overflow it
// (returns the needed length, writes nothing usable) so a truncated / unparseable
// record can never silently corrupt the journal on reboot.
constexpr size_t kJournalSerializeBuf = 384;

// Free function so tests can pin the "serialize refuses over-buffer" boundary
// without a Journal instance. Returns the serialized length (bytes, excl. NUL).
// If need+1 > sz, writes nothing usable (buf[0]=0 when sz>0) and returns need.
size_t serializeRecord(const JobRecord& r, char* buf, size_t sz);
// Parse a record; returns false (out untouched-invalid) on bad JSON / empty tag.
bool deserializeRecord(const char* buf, JobRecord& out);

class Journal {
 public:
  // Load all slots from the store. `store` is borrowed (device owns lifetime).
  void begin(JournalStore* store);

  // Dedupe by TAG: update in place if the tag exists, else take the next free
  // slot. If full (count == kAgentMaxJobs), evict the OLDEST resultSeen record;
  // if none is seen, REFUSE the write (returns false - a full journal of live
  // jobs must not silently drop one). Persists the slot on success.
  bool write(const JobRecord& r);

  bool markSeen(const char* tag);          // result delivered; set resultSeen, persist
  bool update(const char* tag, JobState s);  // update in-flight state, persist

  // Number of ACTIVE records (!resultSeen). This is the count the device gates
  // dispatch on (dispatch a queued spawn only while count() < kMaxActiveInflight).
  int count() const;

  // Iterate the active (!resultSeen) records by index 0..count()-1.
  bool get(int idx, JobRecord& out) const;

  void gc();        // compact out resultSeen records (shift down, re-persist)
  void clearAll();  // wipe namespace + RAM (debug /clear-jobs)

  // Total slots occupied (active + seen), for tests / diagnostics.
  int slotsUsed() const { return count_; }

 private:
  int findSlot(const char* tag) const;
  void saveSlot(int i);

  JournalStore* store_ = nullptr;
  JobRecord     records_[kAgentMaxJobs];
  int           count_ = 0;
  // RAM-only insertion order (NOT persisted - the durable JobRecord shape is the
  // NVS contract and must not grow a field). seq_[i] is a monotonic stamp assigned
  // when slot i last took a NEW record; write() evicts the resultSeen slot with the
  // smallest seq_ so eviction genuinely targets the OLDEST seen record even after a
  // prior eviction reused a mid-array slot. On reboot, begin() stamps records in the
  // order they load (0,1,2,...), a stable within-session ordering.
  uint32_t      seq_[kAgentMaxJobs] = {};
  uint32_t      nextSeq_ = 0;
};

}  // namespace orch
}  // namespace nimbus
