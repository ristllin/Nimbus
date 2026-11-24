#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/status.h"

// tx/log.h - the transcript on storage (PRD section 2):
//   /mem/tx/<chatKey>/seg-000001.jsonl   items, one line each (tx/item.h), rolled at 1 MB
//   /mem/tx/<chatKey>/head.json          a cache of HeadState, written by temp and rename
//   /mem/tx/<chatKey>/b/<turn>.txb       bulk payloads, one append each, 30 days
//   /mem/tx/<chatKey>/digests-YYYY-MM.md archived digests
//   /mem/tx/<chatKey>/migrated.json      the migration marker
// None of it is in the file index; older firmware ignores the folder. Nothing goes to NVS.
//
// The device and the hosts reach storage through TxFiles, so the same log runs over the
// card (through the existing gated card append), nimbusd's volume and a host fake.
//
// The log (lane RB-T2, lib/core/src/orch_tx_log.cpp):
// - Segments are append-only. One stored item is exactly one TxFiles::append of its whole
//   line; nothing in a segment is ever rewritten. A segment rolls before a line would take
//   it past kSegmentRollBytes, so every segment ends on a line end unless a cut tore its
//   last append.
// - A torn tail (a cut inside an append) is dropped by every reader: a line counts only
//   with its '\n' and a good crc. open() notices one at the end of the open segment and the
//   next append starts with "~\n" (inside the same append), so the torn bytes become a line
//   of their own that readers drop, the next item is never glued onto them, and a tail that
//   lacked only its '\n' is never completed into a second line of a q stored again.
// - q is assigned by the caller (HeadState::nextQ is where to start). The log does not
//   refuse a q out of order (a replay after a cut may write one again); readers keep the
//   first line of each q in file order and the arena refuses q that do not increase.
// - head.json is only a cache with its own crc. open() reads head.json.tmp first (a whole
//   one exists only when a cut or refusal stopped its rename, so it is the newer), then
//   head.json, and trusts one only when the line ending at `end` is the one it names
//   (end_q, below next_q) and the line before the fill start is covered by thru; then it
//   catches up on the lines appended after it. Otherwise open() rebuilds it from the
//   segments, newest segment first, each read forward in kFillSliceBytes slices, until the
//   newest s, x, u and t items are found, then locates thru's line, and writes the rebuilt
//   head back (best effort). A chat with no digest yet is read whole, as its fill is.
// - The restart fill is one sequential read from (thruSeg, thruOffset) to the end of the
//   open segment in kFillSliceBytes slices: 80 to 130 KB on Lumi's shape, about 16 reads.
// - A refused append (no card, lock not taken, memory floor) answers NotStored and leaves
//   no whole line behind; the caller keeps the item in RAM flagged kItemNotStored. Lane
//   E2-BFIX1: store() keeps the q order by holding every later item too (flagged the same
//   way) and writes the held items, oldest first, before the next item it is given and at
//   flushHeld() (the turn's close, idle time): a refused turn lands whole once storage takes
//   a line again. While items are held, append() and appendLine() refuse (a line of a higher
//   q may not go first). Only when storage refuses the oldest held item and then takes the
//   new one (a refusal of that line alone) are the held items given up (lostCount()): the
//   stored transcript then has a hole and the engine names it (an h item of class gap on
//   the first stored item after the episode). After open() answered Io the log stays
//   closed: the caller opens it again when storage returns (card inserted, lock free).
// - Memory: open, fill, writeHead, the rebuild and bulk reads use buffers of up to
//   kMaxLineBytes + kFillSliceBytes for the call, and append keeps one line buffer; on the
//   device every call runs under the PSRAM-first allocation scope (PRD section 1).
// - Every path is built from a checked chat key: [a-z0-9_-] (lower case: FAT folders ignore
//   case), 1 to kChatKeyMaxBytes bytes. A bulk handle names only a turn number and offsets,
//   never a path, and is not bound to a file generation: a handle read after its turn's file
//   was pruned answers NotFound or other bytes of the same chat.
namespace nimbus {
namespace orch {
namespace tx {

inline constexpr const char* kTxRoot = "/mem/tx";
inline constexpr size_t kSegmentRollBytes = 1024 * 1024;
inline constexpr size_t kFillSliceBytes = 8192;       // the restart fill reads in these slices
inline constexpr uint32_t kBulkRetentionDays = 30;
inline constexpr uint32_t kRestartFillBudgetMs = 3000; // restart to full transcript, card time
inline constexpr size_t kChatKeyMaxBytes = TxArenaPool::kChatKeyMax;   // 47
inline constexpr uint32_t kMaxSegment = 999999;       // seg-NNNNNN: six digits
inline constexpr size_t kHeadMaxBytes = 16384;        // head.json, crc tail included
inline constexpr size_t kHeadMaxPinned = 64;          // HeadState::pinned entries
inline constexpr size_t kHeadMaxRefusedPhotos = 16;  // HeadState::refusedPhotos entries
inline constexpr size_t kHeadMaxString = 128;         // profile and each pinned entry
inline constexpr size_t kBulkMaxBytes = 512 * 1024;   // one bulk payload
inline constexpr size_t kBulkChunkBytes = 32 * 1024;  // one bulk append: about 50 ms of card lock (Ruling 19)

// The storage the log runs over. Every call is one bounded operation; false is a
// refusal (no card, lock not taken, write failed) and never throws. A binding keeps to:
// - append creates the file (and its folders) when missing and adds `len` bytes at its end;
//   a refusal may leave part of the bytes behind (the log copes), never reorder them;
// - readAt past the end answers true with got = 0; a missing file answers false;
// - size of a missing file answers false;
// - writeWhole creates or replaces the file;
// - rename replaces an existing `to` (a binding whose filesystem cannot, removes `to`
//   first: a cut between the two leaves only head.json.tmp, which open() reads);
// - list of a missing folder answers true with no names, only when the folder is known to be
//   absent; a listing that failed answers false (never "empty"); names are bare (no folder).
class TxFiles {
 public:
  virtual ~TxFiles() = default;
  virtual bool append(const std::string& path, const char* data, size_t len) = 0;
  virtual bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) = 0;
  virtual bool size(const std::string& path, size_t& out) = 0;
  virtual bool writeWhole(const std::string& path, const std::string& data) = 0;
  virtual bool rename(const std::string& from, const std::string& to) = 0;
  virtual bool remove(const std::string& path) = 0;
  virtual bool list(const std::string& dir, std::vector<std::string>& names) = 0;
};

// head.json: a cache, rebuilt from the segments when it is missing, torn or stale.
// On disk: {"v":1,"next_q":N,"thru":N,"stub":N,"seg":N,"end":N,"end_q":N,"thru_seg":N,"thru_off":N,
// "hw":N,"m_hi":N,"profile":"...","pinned":["..."],"last_req_at":N,"open_turn":N,
// "attempts":N,"z":"<crc32>"} and a newline; the crc is the line crc of tx/item.h.
struct HeadState {
  uint8_t v = 1;
  uint64_t nextQ = 1;
  uint64_t thru = 0;            // the newest q a digest covers (the newest s item's thru)
  uint64_t stub = 0;            // the stub pointer (the newest x item's stub)
  uint32_t seg = 1;             // the open segment number
  uint64_t end = 0;             // bytes of the open segment when head.json was written
  uint64_t endQ = 0;            // q of the line that ends at `end` (0 = no line)
  uint32_t thruSeg = 1;         // the segment the restart fill starts in
  uint64_t thruOffset = 0;      // byte offset in thruSeg just past thru's line (fill start)
  uint64_t hw = 0;              // highest Telegram update id seen on this chat (u.src)
  uint64_t mHi = 0;             // newest day-stream row mirrored (u.m; re-entry rule)
  std::string profile;          // tx::profileSlug of the newest t item
  std::vector<std::string> pinned;   // kept only here: a rebuild cannot recover it
  uint64_t lastReqAt = 0;       // epoch seconds of the last head request on this chat
  uint32_t openTurn = 0;        // 0 = no turn open (the newest t item has an e after it)
  uint8_t attempts = 0;         // attempts of the open turn (its t item's attempt)
  // Photo refs a provider refused as pixels in this chat (lane E2-REP4; kept only here, at most
  // kHeadMaxRefusedPhotos, the oldest given up first): each renders as its line after a restart too.
  std::vector<std::string> refusedPhotos;
};

// Where an item went: a failed append leaves the item in RAM, not stored, and the
// model sees one device line about it (PRD section 2, "no card").
enum class StoreState : uint8_t { Stored = 0, NotStored = 1 };

// What a restart fill did (tx::TxLog::fill).
struct FillReport {
  size_t lines = 0;         // lines appended to the arena
  size_t torn = 0;          // lines dropped for framing or crc (torn tails, garbage)
  size_t invalid = 0;       // crc-valid lines checkLine or decodeLine cannot read: dropped
  size_t skipped = 0;       // lines at or below thru, or not above the arena's last q
  size_t dropped = 0;       // resident items given up because the arena was full
  uint64_t droppedThru = 0; // the newest q given up that way (0 = none): name it as omitted
  size_t reads = 0;         // TxFiles::readAt calls
  uint64_t bytes = 0;       // bytes read
};

struct ScanFacts;   // what a scan learns for head.json (orch_tx_log.cpp)

class TxLog {
 public:
  TxLog(TxFiles& files, const std::string& chatKey) : files_(files), chatKey_(chatKey) {}

  // Find the open segment and the state: head.json (or its temp) when it checks out, with
  // the lines after it caught up; else a rebuild from the segments. A chat with no segment
  // yet answers a fresh HeadState, but only when no head file naming stored items and no
  // first segment can be read (else Io: the listing missed them). InvalidArgument for an
  // unsafe chat key; Io when the storage refuses or cannot tell (no card, a failed listing):
  // the log stays closed and every append answers NotStored.
  Status open(HeadState& head);
  // One append of one encoded item to the open segment (rolling at kSegmentRollBytes).
  // NotStored when the storage refused or store() holds items: the caller keeps the item in RAM. TooLarge or
  // InvalidArgument when encodeLine refuses the item: nothing is written.
  Status append(const Item& item, StoreState& state);
  // The same for a line already encoded (the arena's bytes: no allocation, no re-encode).
  // Torn or InvalidArgument when checkLine refuses it.
  Status appendLine(const char* line, size_t len, StoreState& state);
  // The persist point: append `item` to `arena`, write the held items first (flushHeld),
  // then its arena line to storage, and flag it kItemNotStored in the arena when storage did
  // not take it (state NotStored, status ok: the item is held in RAM and the transcript goes on). An arena refusal (TooLarge when
  // full: the caller compacts or names an omission; InvalidArgument for q out of order) is
  // returned and nothing is written anywhere.
  Status store(const Item& item, TxArena& arena, StoreState& state);
  // Append one payload to b/<turn>.txb in appends of at most kBulkChunkBytes; `handle`
  // ("b/<turn>.txb@<off>+<len>") names where it landed. NotStored when storage refused; InvalidArgument for an empty
  // payload or turn 0; TooLarge over kBulkMaxBytes (nothing written).
  Status appendBulk(uint32_t turn, const std::string& payload, std::string& handle);
  // Read [offset, offset + limit) of a bulk payload, clipped to its end (view and exact
  // paging; an offset at or past the end reads nothing). InvalidArgument for a handle this
  // log did not write; NotFound when the file no longer holds the payload (retention).
  // Needs no open().
  Status readBulk(const std::string& handle, size_t offset, size_t limit, std::string& out);
  // Remove b/<turn>.txb for every turn below `beforeTurn` (30-day retention: the caller
  // finds the first turn younger than kBulkRetentionDays). Io when listing is refused.
  Status pruneBulk(uint32_t beforeTurn, size_t& removed);
  // Restart fill: one sequential read from (head.thruSeg, head.thruOffset) to the end of
  // the newest segment into `arena`, in kFillSliceBytes slices. Lines at or below head.thru
  // or not above arena.lastQ() are skipped (so a second fill only tops up). When the arena
  // is full, its oldest resident turn is given up (report.dropped, report.droppedThru) so
  // the newest turns are the ones resident; the turn being filled is never given up (a line
  // of it that does not fit is dropped and counted instead). `head` is trusted as given:
  // pass the one open() returned. Allocates one buffer of kMaxLineBytes + kFillSliceBytes.
  Status fill(const HeadState& head, TxArena& arena, FillReport& report);
  Status fill(const HeadState& head, TxArena& arena);
  // Write head.json by temp and rename (turn close, after a compaction). The log sets
  // seg, end, thruSeg and thruOffset itself (it locates the line of a new thru by reading
  // forward from the last one), nextQ is at least one above the highest q it stored, and
  // thru and stub never move back. InvalidArgument before open(), for thru or stub at or
  // past next_q, or for a head over the limits above (16 KB counted after escaping); Io
  // when the storage refused (head.json is a cache: nothing is lost).
  Status writeHead(const HeadState& head);
  // Rebuild HeadState from the segments alone (ignores head.json). pinned and lastReqAt
  // beyond the newest a or e item cannot be recovered.
  Status rebuildHead(HeadState& out);

  const std::string& chatKey() const { return chatKey_; }
  bool isOpen() const { return open_; }
  // The log stops writing until the next open() (a binding that holds items it has not yet
  // ordered after the stored ones; lane E2-STORE). Nothing on storage changes.
  void close() {
    open_ = false;
    lost_ += held_;   // the caller's reopen merges what RAM holds (E2-BFIX1: no hold over a closed log)
    held_ = 0;
    heldFrom_ = 0;
  }
  uint32_t segment() const { return seg_; }
  uint64_t segmentBytes() const { return segBytes_; }
  // The highest q this log has seen stored (open() and successful appends).
  uint64_t lastQ() const { return maxQ_; }
  // Lane E2-BFIX1: write the items store() holds (flagged kItemNotStored in `arena`, q from
  // the oldest held one) in q order; stops at the first refusal, the rest stay held. Held
  // items the arena no longer has (a reload, a dropped turn) are given up (lostCount()).
  Status flushHeld(TxArena& arena);
  // Items store() holds in RAM for storage, in q order (0: none).
  uint32_t heldCount() const { return held_; }
  // Items given up since this log was made: held, then never written (a hole on storage).
  uint32_t lostCount() const { return lost_; }

 private:
  TxFiles& files_;
  std::string chatKey_;
  std::string dir_;
  std::string line_;            // the encode buffer, reused (keeps its capacity)
  bool open_ = false;
  bool tailTorn_ = false;       // the open segment does not end on a line end
  bool needsSync_ = false;      // an append was refused: check the segment size first
  uint32_t seg_ = 1;
  uint64_t segBytes_ = 0;
  uint64_t maxQ_ = 0;
  uint64_t lastLineQ_ = 0;      // q of the last whole line of the open segment
  uint64_t thru_ = 0;
  uint64_t stub_ = 0;
  uint32_t thruSeg_ = 1;
  uint64_t thruOff_ = 0;
  uint64_t heldFrom_ = 0;       // q of the oldest held item (0: nothing held)
  uint32_t held_ = 0;
  uint32_t lost_ = 0;

  Status writeLine(const char* line, size_t len, uint64_t q, StoreState& state);
  Status writeChecked(const char* line, size_t len, uint64_t q, StoreState& state);
  Status writeArenaLine(const TxArena& arena, size_t at, uint64_t q, StoreState& state);
  Status syncTail();
  Status segments(std::vector<uint32_t>& out);
  Status provenFresh();
  Status readHeadFile(const std::string& path, HeadState& out);
  Status checkHead(const HeadState& head, const std::vector<uint32_t>& segs);
  bool lastLineOk(const HeadState& head);
  bool lineBefore(const std::string& path, uint64_t end, LineHeader& lh);
  bool thruLineOk(const HeadState& head);
  bool lastLine(const std::string& path, uint64_t end, size_t cap, std::string& tail, size_t& start);
  void applyLater(const ScanFacts& f, HeadState& head);
  Status locateBackward(uint64_t thru, const std::vector<uint32_t>& segs, uint32_t& seg, uint64_t& off);
  Status catchUp(HeadState& head, const std::vector<uint32_t>& segs);
  Status rebuildFrom(const std::vector<uint32_t>& segs, HeadState& out);
  Status locateThru(uint64_t thru, uint32_t fromSeg, uint64_t fromOff, const std::vector<uint32_t>& segs,
                    uint32_t& seg, uint64_t& off);
  Status adopt(const HeadState& head, const std::vector<uint32_t>& segs);
};

// The paths above, for one chat. InvalidArgument for an empty or unsafe chat key, segment
// 0 or over kMaxSegment, turn 0, or a month that is not "YYYY-MM".
Status txDir(const std::string& chatKey, std::string& out);
Status segmentPath(const std::string& chatKey, uint32_t seg, std::string& out);
Status headPath(const std::string& chatKey, std::string& out);
Status bulkPath(const std::string& chatKey, uint32_t turn, std::string& out);
Status digestArchivePath(const std::string& chatKey, const std::string& yyyymm, std::string& out);
Status migratedPath(const std::string& chatKey, std::string& out);
// Is `chatKey` one the paths accept?
bool chatKeyOk(const std::string& chatKey);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
