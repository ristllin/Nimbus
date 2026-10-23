#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/caps.h"
#include "nimbus/orch/result_envelope.h"  // JobState + isTerminal

// Ported and adapted from Nuage-Solide src/agent/journal.{h,cpp}
// (Head Orchestrator v2). The LOGIC (add/find/complete/evict/gc/count/get, the
// 6-record ceiling + tag dedupe + seen-eviction, and reboot re-attach) is made
// portable here; the storage I/O is hoisted behind a JournalStore seam, exactly
// like OrchMemory's MemoryStore. The device backs it with one flash file
// (/data/agjournal.json, FileJournalStore below); until v4.5.16 it lived in the
// "agjournal" NVS namespace (keys j0..j5), which the file store reads once to
// migrate. Host tests use an in-memory store.
//
// Per-job journal. Persists enough state to resume polling after a reboot without
// re-dispatching (avoids duplicate billing / duplicate work): at boot the device
// re-attaches to any unfinished job by polling jobId alone (backend prefix before
// ':' routes the adapter). The portable guarantee is only that loadAll() faithfully
// restores every record with jobId byte-identical.
//
// It also keeps the last kJournalFinishedKeep FINISHED jobs (tag, name, project,
// the file the result was saved to, when it finished) across restarts: before
// them a restart left the head with an empty job table and its own note "waiting
// for the research results", so it ran the same paid jobs again and re-sent their
// reports (owner device, 19 Oct 2026).
namespace nimbus {
namespace orch {

// Durable shape - fixed-size char[] (not std::string) so the record serializes to a
// compact, bounded JSON blob whose worst case fits the serialize buffer. The sizes are
// the durable contract (device UI + restart depend on them, and a rollback to an
// NVS-journal release reads the same JSON).
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

// The finished-job ring: the last kJournalFinishedKeep jobs that reached a terminal
// state. Its records live in store slots kJournalFinishedSlot0.. after the active ones.
constexpr int kJournalFinishedKeep  = 8;
constexpr int kJournalFinishedSlot0 = kAgentMaxJobs;
constexpr int kJournalSlots         = kAgentMaxJobs + kJournalFinishedKeep;

// Persistence seam. Slots are 0..kJournalSlots-1: 0..kAgentMaxJobs-1 hold the active
// records, the rest the finished-job ring. The device backs it with one flash file
// (FileJournalStore); tests use a std::map. put and remove report whether they
// LANDED (a full file system refuses a put); the journal never erases a slot it still
// needs because of a write that did not land.
struct JournalStore {
  virtual ~JournalStore() = default;
  virtual std::string get(int slot) = 0;                 // "" if empty
  virtual bool        put(int slot, const std::string& v) = 0;
  virtual bool        remove(int slot) = 0;              // true when the slot is gone
  virtual void        clearNs() = 0;                     // wipe every slot
};

// One finished job, kept across restarts so the head knows the work is done.
struct FinishedJob {
  char     tag[24];          // "job0003"
  char     name[24];         // the model's display name ("" = unnamed)
  char     prj[25];          // the project the job ran under ("" = none)
  char     artifact[96];     // "<project>/<file>" the result was saved to ("" = none)
  char     finishedAt[32];   // local wall clock as the device shows it ("" = unset)
  char     chatId[32];       // the chat that asked for it
  uint32_t finishedEpoch = 0;   // Unix seconds (0 = the clock was not set)
  uint32_t seq = 0;             // ring order across restarts (newest = highest)
  bool     failed = false;      // ended in an error, a loss or a cancel
};

// Same contract as serializeRecord: nothing usable is written when it would not fit.
size_t serializeFinished(const FinishedJob& f, char* buf, size_t sz);
bool   deserializeFinished(const char* buf, FinishedJob& out);

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
  // Load all slots from the store. `store` is borrowed (device owns lifetime). Each
  // record remembers the store slot it came from (a slot left empty by an earlier gc
  // stays empty), and a second record with the same tag is dropped from the store.
  // Every record loaded here is RESTORED (restored()); an active record whose tag is
  // also in the finished ring was finished and delivered before the restart (finish()
  // writes the ring first), so it loads as seen: a result is delivered once.
  void begin(JournalStore* store);

  // Dedupe by TAG: update in place if the tag exists, else take a free slot. If full
  // (count == kAgentMaxJobs), evict the OLDEST resultSeen record; if none is seen,
  // REFUSE the write (a full journal of live jobs must not silently drop one).
  // Returns true only when the record is held AND its slot landed in the store; a
  // record whose write did not land is still kept in RAM for this run (the job keeps
  // being polled), so false means "not durable", never "forgotten".
  bool write(const JobRecord& r);

  // Result delivered: set resultSeen, persist. Returns false when the tag is unknown
  // or the write did not land (the RAM record is updated either way).
  bool markSeen(const char* tag);
  bool update(const char* tag, JobState s);  // update in-flight state, persist (same verdict)

  // Number of ACTIVE records (!resultSeen). This is the count the device gates
  // dispatch on (dispatch a queued spawn only while count() < kMaxActiveInflight).
  int count() const;

  // Iterate the active (!resultSeen) records by index 0..count()-1.
  bool get(int idx, JobRecord& out) const;

  // Drop the resultSeen records: erase their slots and forget them. Active records
  // never move, so no active record depends on a copy landing (the old shift-down gc
  // could erase a record whose shifted copy a full NVS had refused). A seen record
  // whose slot could not be erased stays held, so RAM always mirrors the store.
  void gc();
  void clearAll();  // wipe namespace + RAM (debug /clear-jobs)

  // Total slots occupied (active + seen), for tests / diagnostics.
  int slotsUsed() const { return count_; }

  // True when the record for `tag` was loaded by begin() (the job was started before
  // this boot), false for one written since, or an unknown tag.
  bool restored(const char* tag) const;

  // ---- the finished-job ring (L19-35) ---------------------------------------------
  // `tag` reached a terminal state: keep `f` in the ring (the oldest entry goes when
  // it is full), then mark the active record seen. The ring entry lands FIRST, so a
  // restart between the two writes still finds the job finished (begin() then loads
  // it as seen). The RAM ring and record update either way; true only when both
  // writes landed. An unknown tag still records `f` (a job whose record never landed).
  // `final` (when not Unknown) is the record's terminal state, stored with the seen
  // mark in the same write (one file rewrite, not two).
  bool finish(const char* tag, const FinishedJob& f, JobState final = JobState::Unknown);
  int  finishedCount() const { return finCount_; }
  // Newest first. `beforeBoot` (optional) says whether it finished before this boot.
  bool finished(int idx, FinishedJob& out, bool* beforeBoot = nullptr) const;
  // The highest NNNN of a "jobNNNN" tag held anywhere in the journal (-1 = none), so
  // a new boot never hands out a tag a restored or finished job still carries.
  long highestTagNumber() const;

 private:
  int  findSlot(const char* tag) const;
  int  freeStoreSlot() const;   // lowest store slot no held record uses (-1 = none)
  bool saveSlot(int i);         // true when record i landed in its store slot
  void loadFinished();          // begin(): the ring, oldest first
  int  finishedSlotFor(const char* tag) const;   // ring index of `tag` (-1 = none)
  int  finishedTarget();        // ring index a new entry takes (-1 = none)
  bool saveFinished(int i);     // true when ring entry i landed in its store slot

  JournalStore* store_ = nullptr;
  JobRecord     records_[kAgentMaxJobs];
  int           slot_[kAgentMaxJobs] = {};   // store slot of records_[i]
  int           count_ = 0;
  // RAM-only insertion order (NOT persisted - the durable JobRecord shape is the
  // NVS contract and must not grow a field). seq_[i] is a monotonic stamp assigned
  // when slot i last took a NEW record; write() evicts the resultSeen slot with the
  // smallest seq_ so eviction genuinely targets the OLDEST seen record even after a
  // prior eviction reused a mid-array slot. On reboot, begin() stamps records in the
  // order they load (0,1,2,...), a stable within-session ordering.
  uint32_t      seq_[kAgentMaxJobs] = {};
  uint32_t      nextSeq_ = 0;
  bool          restored_[kAgentMaxJobs] = {};   // RAM only: loaded by begin()

  // The ring, in load order. Heap, reserved once (about 2 KB: PSRAM on the device,
  // never static internal RAM).
  struct FinEntry {
    FinishedJob job;
    int         slot = 0;           // its store slot
    bool        beforeBoot = false; // loaded by begin()
  };
  std::vector<FinEntry> fin_;
  int           finCount_ = 0;
  uint32_t      nextFinSeq_ = 0;
};

// ---- the file store (agjournal off NVS, v4.6) ------------------------------------------
// Every slot lives in ONE flash file, rewritten whole and atomically (tmp + rename) on
// each change, so a crash leaves the previous image. The file is a JSON object keyed by
// slot number, each value the record's own JSON: {"0":{...},"6":{...}}.
//
// Migration follows the moved-store rule (nimbus/store/moved_store.h): load the file;
// when there is none, read the legacy NVS records (agjournal j0..j5) and write them to
// the file at once; NVS is never written again; the NVS records are erased only on a
// boot whose image was already marked valid, once the file reads back, so a rollback to
// v4.5.16 or earlier still finds its in-flight jobs.
// `absorbed` (optional, key "m"): the remote job ids already taken from the legacy NVS
// records (see FileJournalStore::load), so a later boot never takes one of them again.
std::string encodeJournalFile(const std::string (&slots)[kJournalSlots],
                              const std::vector<std::string>* absorbed = nullptr);
// False (slots untouched) when `blob` is not a journal image.
bool decodeJournalFile(const std::string& blob, std::string (&slots)[kJournalSlots],
                       std::vector<std::string>* absorbed = nullptr);
// The most absorbed ids a file keeps (the newest): six per migration or merge.
constexpr size_t kJournalAbsorbedKeep = 12;

struct JournalFileIo {   // the device binds LittleFS and Preferences; tests a fake
  virtual ~JournalFileIo() = default;
  virtual bool        readFile(std::string& out) = 0;        // true when the file exists
  virtual bool        writeFileAtomic(const std::string& v) = 0;
  virtual std::string readLegacySlot(int slot) = 0;          // NVS j<slot>, "" = absent
  virtual bool        eraseLegacy() = 0;                     // true when the records are gone
};

// File: the file, nothing left in NVS to merge. Merged: the file, plus running records an
// earlier release wrote to NVS after it (a rollback from this release, then an update
// back), added to it. Legacy: no file, the NVS records.
enum class JournalSource : uint8_t { None = 0, File, Legacy, Merged };

class FileJournalStore : public JournalStore {
 public:
  explicit FileJournalStore(JournalFileIo& io) : io_(io) {}
  // Load the image (see above). A legacy load is written to the file here; when that
  // write fails the records are still served from RAM and the NVS copy is untouched.
  // When both exist, a running NVS record the file does not hold (same remote job id)
  // is added to it: an earlier release wrote it after the file (a rollback, then this
  // release again), and dropping it would orphan a job that may still be billing. A tag
  // the file already uses for another job is renumbered past every tag in either. A
  // record the file took before (its remote id is in the file's absorbed list: the
  // migration's own copy, which may have finished since) is never taken again, so a
  // delivered job never comes back from its stale NVS copy.
  JournalSource load();
  // Erase the legacy NVS records when the rule allows it. True when they were erased.
  bool retireLegacy(bool imageValidAtBoot);
  // Whether the last write landed (false after a write the file system refused).
  bool durable() const { return durable_; }

  std::string get(int slot) override;
  bool        put(int slot, const std::string& v) override;
  bool        remove(int slot) override;
  void        clearNs() override;

 private:
  bool commit(int slot, const std::string& v);   // image with slot = v; kept on success
  int  mergeLegacy();                            // load(): running NVS records the file lacks
  JournalFileIo& io_;
  bool absorbed(const std::string& jobId) const;
  void noteAbsorbed(const std::string& jobId);
  std::string image(const std::string (&slots)[kJournalSlots]) const {
    return encodeJournalFile(slots, &absorbed_);
  }
  std::string    slots_[kJournalSlots];
  std::vector<std::string> absorbed_;   // remote ids taken from the legacy NVS records
  bool           durable_ = true;   // the last write landed
  bool           onDisk_ = false;   // the file holds exactly slots_
};

}  // namespace orch
}  // namespace nimbus
