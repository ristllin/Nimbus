#pragma once
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "nimbus/orch/episodic.h"
#include "nimbus/orch/psram_alloc.h"   // WorkingAllocator - route the recent-ring + index cache to PSRAM (CUM-185)

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

  // Visit the basename of every regular file directly under `dir` (no recursion,
  // no dirs) without building a list. Retention and the index backfill walk
  // directories that hold hundreds of entries; a materialized list of short
  // strings is hundreds of small allocations, which on the device land on the
  // scarce internal heap. The default delegates to list(); the device overrides
  // it with a directory read that neither opens each entry nor keeps its name.
  virtual void forEachName(const std::string& dir,
                           const std::function<void(const char* name)>& fn) const {
    for (const auto& n : list(dir)) fn(n.c_str());
  }
};

// PSRAM-routed string and vector types (host: malloc). Every container the
// retention pass and the index backfill build uses these, so a pass over
// thousands of rows or blobs never drains the internal heap in small pieces.
using EpiPsString = std::basic_string<char, std::char_traits<char>, WorkingAllocator<char>>;
template <class T>
using EpiPsVec = std::vector<T, WorkingAllocator<T>>;

// Boot-scan budget. Sized so the worst case stays well inside one watchdog period
// on the S3. The BYTE budget is the one that binds: real rows average ~600 B on
// the card (trace rows run near 1 KB), so 256 KB indexes roughly 400 rows and the
// row cap is a backstop for tiny rows only. What the boot scan leaves out is
// indexed after boot by backfillStep() in small slices, so the boot budget sets
// how fast the device starts, not how much history the index ends up holding.
inline constexpr int    kHydrateMaxRows  = 4000;
inline constexpr size_t kHydrateMaxBytes = 256u * 1024;
// Most this scan will read from any ONE day-stream. A single file can be
// megabytes on a busy device, so the per-file bound matters as much as the total.
inline constexpr size_t kHydrateFileWindow = 128u * 1024;
// The walk stops once less than this is left of the byte budget: a tail read
// that small holds a row or two and only costs another file open.
inline constexpr size_t kHydrateMinTail = 4u * 1024;

// INDEX BACKFILL. After a truncated boot scan the index converges to the whole
// card in bounded slices driven from the device's loop task, so the index is a
// function of what is on the card, not of when the device last restarted.
// One slice reads one backward window and decodes a bounded number of rows.
inline constexpr size_t kBackfillSliceBytes = 8u * 1024;
inline constexpr int    kBackfillSliceRows  = 32;
// A window grows (reading the earlier bytes once, never re-reading) up to this
// for a single long record. A record longer than this cannot be indexed whole;
// it is skipped and the index stays marked incomplete, so retention keeps
// deferring the blob sweep rather than trusting a partial index.
inline constexpr size_t kBackfillWindowMax = 128u * 1024;
// Hard cap on index rows (PSRAM, about 100 B each: ~2 MB at the cap). The
// backfill stops there and the index stays marked incomplete: cold queries
// still reach the rest and retention still defers the blob sweep.
inline constexpr int    kIndexMaxRows = 20000;
// Consecutive failed reads of a file that still exists before the backfill
// gives up for this boot (the index stays marked incomplete).
inline constexpr int    kBackfillMaxReadErrors = 8;

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

// True when `name` is a day-stream in its canonical form ("YYYY-MM-DD.jsonl",
// the exact name dayFile() writes) and sets `day` (1970-01-01 is day 0, the file
// rows land in before the clock is set). Every walk of the episodic directory
// (boot scan, cold query, backfill, retention) uses this one rule, so a stray
// file is never indexed by one path and deleted by another.
bool parseDayStreamName(const char* name, uint32_t& day);

// Report from a retention pass. The name lists live in PSRAM: a first pass on a
// busy card removes hundreds of sidecars.
struct EpiPruneReport {
  EpiPsVec<EpiPsString> removedDayFiles;  // absolute paths of pruned day-streams
  EpiPsVec<EpiPsString> removedBlobs;     // basenames of pruned blob sidecars
  int  keptMessages = 0;                  // messages surviving in the index
  // The blob sweep did not run because the index does not yet cover every row
  // on the card: a blob referenced only by an unindexed row would read as an
  // orphan. It runs on a later pass once the index is complete.
  bool blobGcSkipped = false;
  int  blobsPresent = 0;                  // sidecars seen by the sweep (0 when skipped)
};

// Progress of the index backfill since the last hydrate().
struct EpiBackfillStats {
  int    rows = 0;          // rows added to the index
  int    slices = 0;        // steps that read the card
  size_t bytes = 0;         // bytes read off the card
  bool   complete = false;  // the index covers every row on the card
  bool   capped = false;    // stopped at the index cap (index stays incomplete)
  bool   stalled = false;   // stopped on repeated read errors or an over-long record
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
  // files so the caller can feed the watchdog. The walk also stops after the first
  // file it could read only part of (and when less than kHydrateMinTail of the
  // budget is left), so the boot index is always a CONTIGUOUS suffix of history:
  // no isolated older rows, no hole that a floor or a cursor would paper over.
  // Older rows stay on disk; backfillStep() indexes them after boot and the cold
  // query reaches them meanwhile. Passing 0 for a budget means "no limit" (host
  // tests, where there is no watchdog and determinism matters more).
  int hydrate(int maxRows = kHydrateMaxRows, size_t maxBytes = kHydrateMaxBytes,
              const std::function<void()>& yield = nullptr);

  // Index one more bounded slice of the history the boot scan left out: the
  // newest unindexed region first, one backward window (sliceBytes, growing
  // only for a single long record) and at most sliceRows rows. Returns true
  // while there is more to do; false once the index is complete (and then
  // hydrateTruncated() reads false), capped at the index cap, or stalled.
  // Safe to interleave with addMessage() and prune(). The device calls it from
  // its loop task, only while no turn is in flight.
  bool backfillStep(size_t sliceBytes = kBackfillSliceBytes,
                    int sliceRows = kBackfillSliceRows);
  const EpiBackfillStats& backfillStats() const { return bfStats_; }
  // Rows the last hydrate() indexed (the boot scan's share of the index).
  int bootIndexed() const { return bootIndexed_; }
  // The index holds every row on the card (the boot scan read everything, or the
  // backfill finished). Retention only sweeps blobs when this is true.
  bool indexComplete() const override { return !hydrateTruncated_; }
  // The backfill stopped because a file kept failing to read (not because of the
  // cap or an over-long record). resumeAfterReadErrors() clears that stop so a
  // later step retries where it left off; the device does so after a pause, as
  // a card that misread for a moment may read again. False when there was no
  // such stop.
  bool stalledOnReadErrors() const {
    return bfStats_.stalled && bfReadErrors_ >= kBackfillMaxReadErrors;
  }
  bool resumeAfterReadErrors() {
    if (!stalledOnReadErrors()) return false;
    bfStats_.stalled = false;
    bfReadErrors_ = 0;
    return true;
  }
  // Override the index cap (tests; the device keeps kIndexMaxRows).
  void setIndexCap(int rows) { indexCap_ = rows > 0 ? rows : kIndexMaxRows; }
  // Read-only walk of the index, oldest first: each row's day, byte offset,
  // length and id suffix. For diagnostics and the convergence tests.
  void forEachIndexed(const std::function<void(uint32_t day, uint32_t offset, uint32_t len,
                                               uint32_t idSfx)>& fn) const {
    for (const auto& r : index_) fn(r.dayNum, r.offset, r.len, r.idSfx);
  }

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
  // `cutoffDayNum` (days since the Unix epoch, i.e. tsHours/24), chosen by FILE
  // NAME so a day the index never saw still ages out; then, only when the index
  // is complete, reference-count-scan `blobDir` and delete blob sidecars no row
  // references. With an incomplete index the sweep is skipped and reported
  // (blobGcSkipped). Nothing under the Telegram archive root is ever removed
  // (isRetentionExempt). Every container it builds is PSRAM-routed.
  EpiPruneReport prune(uint32_t cutoffDayNum, const std::string& blobDir);

  // Path of the day-stream for a given epoch-day number ("/mem/episodic/Y-M-D.jsonl").
  std::string dayFile(uint32_t dayNum) const;
  // "YYYY-MM-DD" for an epoch-day number (civil calendar).
  static std::string civilDate(uint32_t dayNum);

 private:
  // CUM-185: the recent-window cache + index are the dominant Orchestrator-mode
  // internal-SRAM holder. On a busy device the ~512-row ring plus one IdxRec per
  // message hold hundreds of small (<128 B) std::strings, and the 128 B extmem
  // spill threshold keeps every one of them on the SCARCE internal heap - the
  // measured -53 KB off the largest contiguous internal block at Orchestrator
  // steady state (what a TLS handshake / AsyncTCP accept needs). PsString routes
  // those string bytes through the same PSRAM working-allocator seam the VDB uses
  // (host default = malloc, so host tests are unchanged; device = PSRAM), and the
  // two container buffers use WorkingAllocator too, so the whole cache lands in
  // the abundant 8 MB PSRAM. The public EpisodicMessage/EpisodicSession stay
  // std::string - conversion happens only at the ring boundary (transient, bounded
  // by the query limit), so the wire/query semantics are byte-identical.
  using PsString = std::basic_string<char, std::char_traits<char>, WorkingAllocator<char>>;

  struct IdxRec {
    uint32_t tsHours = 0;
    uint32_t dayNum = 0;     // tsHours / 24 (epoch-day, == the day-file identity)
    uint32_t offset = 0;     // byte offset of the JSON within its day file
    uint32_t len = 0;        // JSON length (bytes, excluding the '\n')
    uint32_t idSfx = 0;      // epiIdSuffix(id) - orders rows for the `before` cursor
    MsgKind  kind = MsgKind::Message;
    PsString sessionId;      // cheap pre-read session filter (PSRAM)
    PsString blobHash;       // blobHashOf(blobPath), "" if none - for the prune scan (PSRAM)
  };

  // PSRAM-resident mirror of EpisodicMessage for the recent-window cache. Same
  // fields; only the string storage differs (PsString -> PSRAM).
  struct CachedMsg {
    PsString id;
    PsString sessionId;
    uint32_t tsHours = 0;
    PsString role;
    MsgKind  kind = MsgKind::Message;
    PsString text;
    PsString blobPath;
    PsString tags;
  };

  // Ring-boundary conversions (defined in the .cpp). ps()/st() bridge the string
  // types; cacheOf()/msgOf() convert a whole message either way.
  static PsString      ps(const std::string& s);
  static std::string   st(const PsString& s);
  static CachedMsg        cacheOf(const EpisodicMessage& m);
  static EpisodicMessage  msgOf(const CachedMsg& c);

  // Read + decode a single record by its index entry; false if the line is
  // missing/garbage (tolerant - a torn last line just drops out of results).
  bool readRec(const IdxRec& r, EpisodicMessage& out) const;

  // One backward window over [.., end) of a day-stream: whole records only (the
  // torn head record is dropped and left for the next, older window). The read
  // grows backward without re-reading while a single record spans it, up to
  // `maxWindow`. Shared by the cold query and the backfill so both apply one
  // torn-head rule.
  enum class WinRead : uint8_t { Ok, Empty, Stubborn };
  struct BackWindow {
    std::string buf;        // whole records, newline-separated
    long        start = 0;  // file offset the read began at
    long        first = 0;  // file offset of buf[0] (the oldest whole record)
    size_t      bytesRead = 0;
  };
  WinRead readBackWindow(const std::string& path, long end, size_t window, size_t maxWindow,
                         BackWindow& w) const;

  // Sorted day numbers of every day-stream in dir_ (PSRAM vector).
  EpiPsVec<uint32_t> dayStreams() const;
  // Oldest indexed offset in `day`'s file, or -1 when no row of that day is indexed.
  long indexFloorOf(uint32_t day) const;
  // Backfill: forget the cursor (hydrate() starts over).
  void resetBackfill();
  // Backfill: index one window of `day`'s unindexed region below bfEnd_.
  bool backfillWindow(uint32_t day, const std::string& path, size_t sliceBytes, int sliceRows);
  // Retention: the reference-count sweep of `blobDir` (index must be complete).
  void sweepBlobs(const std::string& blobDir, EpiPruneReport& rep);
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
  // Offer every ring row (newest-first) to `col` until it says done.
  void queryRing(const MsgQuery& q, EpiCollector& col) const;

  // Walk the day-streams BELOW the index (or below `cur`), newest record first,
  // offering matches to `col` until it is done (unranked: `limit` reached) or
  // the call budget runs out. Updates `info` with the floor reached and the
  // resume cursor. See the .cpp for the backward-window read (a record
  // straddling a window boundary is read whole by the next one).
  void queryCold(const MsgQuery& q, EpiCollector& col, EpiQueryInfo& info) const;

  EpiFs& fs_;
  std::string dir_;
  int recentCap_;
  std::vector<IdxRec, WorkingAllocator<IdxRec>>       index_;   // one per message, oldest-first (PSRAM)
  std::vector<CachedMsg, WorkingAllocator<CachedMsg>> recent_;  // last <=recentCap_ full messages (PSRAM)
  std::vector<EpisodicSession> sessions_;  // LWW-by-id session table (small, internal ok)
  uint32_t maxIdSuffix_ = 0;               // high-water for nextIdHint()
  int unpersisted_ = 0;                    // recent_ entries the FS refused (SD loss)
  bool hydrateTruncated_ = false;          // index does not hold every row on the card

  // Backfill cursor. The walk goes newest day first; inside a day it moves the
  // exclusive upper bound of the unindexed region (bfEnd_) down to 0. Days newer
  // than the current one are complete (wholly indexed at boot, or appended live).
  int  indexCap_ = kIndexMaxRows;
  int  bootIndexed_ = 0;
  bool bfInit_ = false;
  EpiPsVec<uint32_t> bfDays_;              // day-streams captured at the first step, newest first
  size_t bfPos_ = 0;                       // bfDays_ index of the day being walked
  long   bfEnd_ = -1;                      // unindexed region of that day is [0, bfEnd_); -1 = not computed
  int    bfReadErrors_ = 0;                // consecutive failed reads of a file that still exists
  bool   bfSkipped_ = false;               // a record too long to index was skipped
  EpiBackfillStats bfStats_;
};

// JSONL codec for one EpisodicMessage (shared with the device seam + tests). encode
// emits a single line WITHOUT a trailing newline; decode is tolerant (false on a
// torn/garbage line). Exposed for direct testing.
std::string encodeEpisodicLine(const EpisodicMessage& m);
bool        decodeEpisodicLine(const std::string& line, EpisodicMessage& out);
// Same, over a span (no copy of the line). The JSON document behind every
// decode uses the PSRAM working allocator, so reading history back (boot scan,
// cold query, backfill) does not churn the internal heap.
bool        decodeEpisodicLine(const char* data, size_t len, EpisodicMessage& out);

}  // namespace orch
}  // namespace nimbus
