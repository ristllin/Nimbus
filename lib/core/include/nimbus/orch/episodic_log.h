#pragma once
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "nimbus/orch/episodic.h"

// episodic_log - the SD system-of-record backing for EpisodicStore
// (docs/orchestrator-storage.md §3). Replaces the in-RAM 500-ring whole-blob
// rewrite with append-only JSONL day-streams + an in-RAM offset index + a
// PSRAM recent-window cache:
//
//   /mem/episodic/2026-07-04.jsonl   one EpisodicMessage per line (append-only)
//   /mem/episodic/sessions.jsonl     one EpisodicSession row per change (LWW by id)
//
//   * addMessage is O(1): seek-end, write one line, push one index record. No
//     whole-file rewrite -> the 500-cap that protected the rewrite is lifted.
//   * query is index-first: session/kind/time filters run off the in-RAM index
//     (never opening an out-of-window day file); textContains streams only the
//     survivors up to `limit`. A history that fits the recent ring answers from
//     RAM with ZERO filesystem reads.
//   * the index is rebuilt at boot by scanning the day-streams (one
//     system-of-record; no separate index.bin to keep consistent with the log
//     under power loss - a truncated last line is simply skipped, same tolerant
//     philosophy as the vector deserialize).
//
// Portable + Arduino-free: the store talks to an abstract EpiFs byte-file seam,
// so host tests inject an in-memory FS (and count its reads/writes to prove the
// O(1)-append + zero-read-fast-path properties). The device backs EpiFs with
// LittleFS/SD. Query semantics are byte-for-byte the InMemoryEpisodicStore's, so
// the existing query tests transfer.
namespace nimbus {
namespace orch {

// Abstract byte-file seam. Paths are absolute ("/mem/episodic/2026-07-04.jsonl").
// The device implements this over fs::FS; tests over an in-memory map.
class EpiFs {
 public:
  virtual ~EpiFs() = default;

  // Append `bytes` to the file at `path` (creating it + parent as needed). Returns
  // the byte offset of the START of the appended region (i.e. the pre-append size),
  // or -1 on failure. This offset is what the index records so a later readRange
  // can pull exactly this record back.
  virtual long append(const std::string& path, const std::string& bytes) = 0;

  // Read the whole file; "" if absent. Used only at boot (index rebuild).
  virtual std::string readAll(const std::string& path) const = 0;

  // Read `len` bytes starting at `offset`; "" if absent/out-of-range. The hot read
  // path - pulls one indexed record without loading the whole day file.
  virtual std::string readRange(const std::string& path, long offset, long len) const = 0;

  // Size of the file at `path` in bytes; 0 if absent. The boot scan needs this to
  // read a large day-stream's TAIL instead of pulling the whole file into RAM.
  virtual long size(const std::string& path) const = 0;

  // Basenames of the regular files directly under `dir` (no recursion, no dirs).
  // Empty if `dir` is absent.
  virtual std::vector<std::string> list(const std::string& dir) const = 0;

  // Remove the file at `path`. Returns true iff it existed and was removed.
  virtual bool remove(const std::string& path) = 0;

  // True iff a file exists at `path`. Used by prune() to distinguish "delete
  // succeeded / file already gone" from "delete failed, file survives" - the bool
  // from remove() alone conflates them, and dropping index records for a file that
  // is still on disk would let hydrate() replay the "pruned" rows on reboot.
  virtual bool exists(const std::string& path) const = 0;
};

// Boot-scan budget. Sized so the worst case stays well inside one watchdog period
// on the S3 while still covering far more history than the recent window needs:
// the index exists to make older rows reachable, and the newest are the ones any
// query actually wants first.
inline constexpr int    kHydrateMaxRows  = 4000;
inline constexpr size_t kHydrateMaxBytes = 256u * 1024;
// Most this scan will read from any ONE day-stream. A single file can be
// megabytes on a busy device, so the per-file bound matters as much as the total.
inline constexpr size_t kHydrateFileWindow = 128u * 1024;

// COLD-QUERY budget (v4.0.0). What the boot scan leaves unindexed is still on
// the card; an opt-in cold pass reads it back on demand. The budget is the
// safety property - this runs on `tg_poll` inside a turn, under the same 8 s
// watchdog, so a query can never walk a year of history in one call. It pages
// instead: each call reports the exact cursor to resume from.
// ⚠ The window is a HEAP allocation on whichever task is querying - the web
// task holds ~8 KB of stack and the device rests near 60 KB of free internal
// SRAM, so a big window is not a performance knob, it is a crash. A 128 KB
// window took the device off the LAN mid-query on the first hardware run.
// ⚠ The device callers (web + turn tasks) hold the shared `memory::Lock`
// across the whole query, so these budgets ALSO bound how long a deep search
// stalls the OTHER task. On the common ring-resident config the only SD work
// under the Lock is the cold pass (2 files / 128 KB ≈ sub-second, measured);
// the index-read cap below bounds the rarer large-history case. Dropping the
// Lock around the (immutable) day-file reads is a tracked follow-up.
inline constexpr int    kColdMaxFiles  = 2;
inline constexpr size_t kColdMaxBytes  = 128u * 1024;   // per call, all files
inline constexpr size_t kColdWindow    = 8u * 1024;     // one backward read
// A single record longer than the window would otherwise strand the rest of the
// file; the read escalates up to this before giving up on that window alone.
inline constexpr size_t kColdWindowMax = 32u * 1024;
// Records the INDEX pass will pull off the card in one call. Each read is an
// open + seek + read + close; measured ~1 ms on the bench card (the 4000-row
// unbounded walk that this cap replaced was the real hazard - it wedged the web
// task). This loop runs UNDER the shared `memory::Lock`, so the cap also bounds
// how long a large-history search stalls the other task; past it the query
// stops and reports the cursor, exactly like the cold pass.
inline constexpr int    kIndexReadsPerCall = 120;

// "2026-07-16.jsonl" -> epoch-day number (0 when the name is not a day-stream).
uint32_t dayNumFromName(const std::string& name);

// Report from a retention pass.
struct EpiPruneReport {
  std::vector<std::string> removedDayFiles;  // absolute paths of pruned day-streams
  std::vector<std::string> removedBlobs;     // basenames of pruned blob sidecars
  int                      keptMessages = 0; // messages surviving in the index
};

class AppendLogEpisodicStore : public EpisodicStore {
 public:
  // `dir` is the episodic directory ("/mem/episodic"); `recentCap` is the size of
  // the PSRAM recent-window cache (full messages kept resident for the zero-read
  // fast path). Construction does NOT touch the FS - call hydrate() once the FS is
  // mounted to rebuild the index/cache from the day-streams.
  AppendLogEpisodicStore(EpiFs& fs, std::string dir, int recentCap = 256)
      : fs_(fs), dir_(std::move(dir)), recentCap_(recentCap) {}

  // Rebuild the in-RAM index, recent-window cache and session table by scanning the
  // day-streams (chronological: day files sort by their YYYY-MM-DD name; within a
  // file, append order is chronological). Idempotent. Returns messages indexed.
  //
  // ⚠ BOUNDED, and it must stay that way. This runs inside setup(), on the task the
  // watchdog watches, BEFORE the device can serve anything. The unbounded version
  // read every day-stream whole and JSON-parsed every row: a board that had merely
  // ACCUMULATED enough history (~15 K rows, which a chatty month produces) could no
  // longer boot at all - the watchdog reset it mid-scan, forever, and no amount of
  // power-cycling helped. Retention prune could not save it either, because prune
  // runs after boot. A device must never be able to write itself into a state it
  // cannot start from.
  //
  // So: newest day FIRST, stop at `maxRows` / `maxBytes`, and call `yield` between
  // files so the caller can feed the watchdog. Older rows stay on disk and are
  // still readable by an explicit dated query - they are simply not indexed at
  // boot. Passing 0 for a budget means "no limit" (host tests, where there is no
  // watchdog and determinism matters more).
  int hydrate(int maxRows = kHydrateMaxRows, size_t maxBytes = kHydrateMaxBytes,
              const std::function<void()>& yield = nullptr);

  // ---- EpisodicStore ----
  void addSession(const EpisodicSession& s) override;
  bool setSessionStatus(const std::string& id, const std::string& status) override;
  void addMessage(const EpisodicMessage& m) override;
  std::vector<EpisodicSession> sessions(const std::string& status = "") const override;
  std::vector<EpisodicMessage> query(const MsgQuery& q) const override;
  std::vector<EpisodicMessage> query(const MsgQuery& q, EpiQueryInfo* info) const override;
  int messageCount() const override { return (int)index_.size() + unpersisted_; }

  // Oldest epoch-day present in the index - the floor of what a plain (hot)
  // query can see. 0 when the index is empty.
  uint32_t indexFloorDay() const;

  // Messages accepted into the RAM recent-window but NOT durably written (the FS
  // append failed - a mid-op SD loss). They stay queryable so the current
  // conversation's working set survives a card that vanished; older on-disk
  // history is what degrades. > 0 signals the device to demote the SD tier.
  int unpersistedCount() const { return unpersisted_; }

  // Highest numeric id suffix seen (the "m%08x" device scheme parses to this),
  // + 1, so the device resumes its id counter past ALL surviving history even
  // after a retention prune removed the oldest rows. 0 if the store is empty.
  uint32_t nextIdHint() const { return maxIdSuffix_ + (index_.empty() ? 0 : 1); }

  // Retention (§4): drop day-streams whose day is strictly older than
  // `cutoffDayNum` (days since the Unix epoch, i.e. tsHours/24), then
  // reference-count-scan `blobDir` and delete blob sidecars no surviving row
  // references. Returns what was removed.
  EpiPruneReport prune(uint32_t cutoffDayNum, const std::string& blobDir);

  // Path of the day-stream for a given epoch-day number ("/mem/episodic/Y-M-D.jsonl").
  std::string dayFile(uint32_t dayNum) const;
  // "YYYY-MM-DD" for an epoch-day number (civil calendar).
  static std::string civilDate(uint32_t dayNum);

 private:
  struct IdxRec {
    uint32_t tsHours = 0;
    uint32_t dayNum = 0;     // tsHours / 24 (epoch-day, == the day-file identity)
    uint32_t offset = 0;     // byte offset of the JSON within its day file
    uint32_t len = 0;        // JSON length (bytes, excluding the '\n')
    uint32_t idSfx = 0;      // epiIdSuffix(id) - orders rows for the `before` cursor
    MsgKind  kind = MsgKind::Message;
    std::string sessionId;   // cheap pre-read session filter
    std::string blobHash;    // blobHashOf(blobPath), "" if none - for the prune scan
  };

  // Read + decode a single record by its index entry; false if the line is
  // missing/garbage (tolerant - a torn last line just drops out of results).
  bool readRec(const IdxRec& r, EpisodicMessage& out) const;
  // Append one session row to sessions.jsonl (append-only; hydrate replays LWW).
  void appendSessionRow(const EpisodicSession& s);
  // True when the whole history is resident in the recent ring, so query can be
  // served from RAM with zero FS reads.
  bool ringResident() const { return (int)index_.size() <= recentCap_; }

 public:
  // Did the boot scan stop at its budget? True means older rows are on disk but
  // not indexed - surfaced so "where did my old history go" has an answer.
  bool hydrateTruncated() const { return hydrateTruncated_; }

 private:
  std::vector<EpisodicMessage> queryRing(const MsgQuery& q) const;

  // Walk the day-streams BELOW the index (or below `cur`), newest record first,
  // appending matches until `limit` or the call budget. Updates `info` with the
  // floor reached and the resume cursor. See the .cpp for the backward-window
  // read (a record straddling a window boundary is read whole by the next one).
  void queryCold(const MsgQuery& q, std::vector<EpisodicMessage>& out,
                 EpiQueryInfo& info) const;

  EpiFs& fs_;
  std::string dir_;
  int recentCap_;
  std::vector<IdxRec> index_;              // one per message, oldest-first
  std::vector<EpisodicMessage> recent_;    // last <=recentCap_ full messages
  std::vector<EpisodicSession> sessions_;  // LWW-by-id session table
  uint32_t maxIdSuffix_ = 0;               // high-water for nextIdHint()
  int unpersisted_ = 0;                    // recent_ entries the FS refused (SD loss)
  bool hydrateTruncated_ = false;          // boot scan hit its budget (older rows unindexed)
};

// JSONL codec for one EpisodicMessage (shared with the device seam + tests). encode
// emits a single line WITHOUT a trailing newline; decode is tolerant (false on a
// torn/garbage line). Exposed for direct testing.
std::string encodeEpisodicLine(const EpisodicMessage& m);
bool        decodeEpisodicLine(const std::string& line, EpisodicMessage& out);

}  // namespace orch
}  // namespace nimbus
