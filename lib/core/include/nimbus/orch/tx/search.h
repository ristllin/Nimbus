#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/migrate.h"   // DayRow, RowRole
#include "nimbus/orch/tx/status.h"

// tx/search.h - reading one chat's stored conversation back by words and by id (lane E2-SRCH;
// the engine-2 tools search and view, lib/harness plain/history.h).
//
// What is read:
//   - the chat's transcript segments (/mem/tx/<chatKey>/seg-*.jsonl): every item engine 2 ever
//     stored for that conversation, the spans compaction has summarized included (segments are
//     append-only, nothing leaves them), and the bulk payload of a result (TxLog::readBulk);
//   - optionally the earlier engine's day rows of the same chat (OldRows) older than the oldest row
//     the history import brought over verbatim: verbatimFloor() finds it (the first owner message of
//     the log that mirrors a day row). The import imports the newest rows and leaves the older ones
//     to its digest and to the line "... not shown: search finds them": those are the rows below it.
// Which conversation is read is the caller's: chatKey and the OldRows of that chat. No argument a
// model writes picks one (a cursor only positions inside it, and is checked).
//
// Reads only: TxFiles::readAt (at most kSliceBytes a call: each is one short card hold), size and
// list. Nothing is written, moved or rebuilt. A slice the card refuses ends the call with Io, and
// the caller says so ("cannot read" is never "absent", ruling 116).
//
// Order of reading: newest first. The transcript is read backward in slices, then the old rows
// through their own newest-first cursor. Order of the hits (lane E2-SRCH2): by how well they match
// (MatchRank), newest first among equals, over a whole window (SearchOutcome); the cursor
// continues inside the window, then into the next one: every match of the range exactly once.
//
// Matching: every word of the query (a run of letters and digits, ASCII case folded; bytes over
// 0x7f count as letters) occurs as a substring, case-insensitive, within one line of the text or a
// line and the one before it (matchesNear: in a long result of many lines the words are near each
// other). A query with no word
// matches nothing. Results the search and view tools returned earlier are never searched (their
// text repeats what they found), nor the items of the open turn (the question itself).
// Engine 2 only (tools/engine/engine2_only.txt). Portable, host-tested (test_tx_search).
namespace nimbus {
namespace orch {
namespace tx {

inline constexpr size_t kSliceBytes = 8192;              // one readAt: one card hold
inline constexpr size_t kSearchMaxHits = 12;
// One window (lane E2-SRCH2): the owner's month is about 1.8 MB of rows; at the card's measured rate
// (about 1 MB a second in 8 KB reads of under 11 ms each, unit F1) 6 MB is about 6 s of card time.
inline constexpr size_t kSearchWindowBytes = 6 * 1024 * 1024;
inline constexpr uint32_t kSearchCardMs = 12000;         // a window's card time, summed: a safety stop
inline constexpr size_t kSearchExcerptBytes = 160;
inline constexpr size_t kSearchRankedMax = 600;
inline constexpr size_t kVerbatimFloorScan = 256 * 1024; // verbatimFloor reads at most this much
inline constexpr size_t kOldPageRows = 32;
inline constexpr size_t kPhotoScanBytes = 8 * 1024 * 1024;   // findPhoto reads at most this much (TooLarge)

// What the card cost a call (the bench's measure).
struct ReadStats {
  uint32_t reads = 0;       // readAt calls (and old-row store queries)
  uint64_t bytes = 0;       // bytes read
  uint32_t totalMs = 0;     // card time summed
  uint32_t longestMs = 0;   // the longest single read
  void add(uint32_t ms, uint64_t b) {
    ++reads;
    bytes += b;
    totalMs += ms;
    if (ms > longestMs) longestMs = ms;
  }
};

// A TxFiles that counts every read into `stats` (ms from `nowMs`; null: no times). Writes are
// refused: nothing reached through it can change the card.
class ReadOnlyCounted : public TxFiles {
 public:
  ReadOnlyCounted(TxFiles& inner, std::function<uint32_t()> nowMs, ReadStats& stats)
      : in_(inner), now_(std::move(nowMs)), st_(stats) {}
  bool append(const std::string&, const char*, size_t) override { return false; }
  bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) override;
  bool size(const std::string& path, size_t& out) override { return in_.size(path, out); }
  bool writeWhole(const std::string&, const std::string&) override { return false; }
  bool rename(const std::string&, const std::string&) override { return false; }
  bool remove(const std::string&) override { return false; }
  bool list(const std::string& dir, std::vector<std::string>& names) override { return in_.list(dir, names); }

 private:
  TxFiles& in_;
  std::function<uint32_t()> now_;
  ReadStats& st_;
};

// ---- the earlier engine's rows of the chat ---------------------------------------------------
struct OldPageAsk {
  std::string before;        // the store's cursor: rows strictly older ("" = from the newest)
  uint64_t fromT = 0;        // epoch seconds, inclusive (0 = no bound)
  uint64_t toT = 0;          // epoch seconds, inclusive (0 = no bound)
  size_t max = kOldPageRows;
};
struct OldPage {
  std::vector<DayRow> rows;  // newest first
  std::string next;          // the cursor that continues below the page
  bool older = false;        // rows remain below the page
};
class OldRows {
 public:
  virtual ~OldRows() = default;
  // One page of this chat's rows, newest first. Io when the store did not answer.
  virtual Status page(const OldPageAsk& ask, OldPage& out, ReadStats& stats) = 0;
  // One row of this chat by id. NotFound when it has none; Io when the store did not answer.
  virtual Status row(uint64_t id, DayRow& out, ReadStats& stats) = 0;
};

// ---- search ----------------------------------------------------------------------------------
struct SearchAsk {
  std::string query;
  uint64_t fromT = 0;        // inclusive, epoch seconds (0 = none)
  uint64_t toT = 0;          // inclusive (0 = none)
  std::string before;        // a cursor a previous call handed back ("" = a new search)
  uint32_t openTurn = 0;     // its items are not searched (0 = none)
};

enum class HitSource : uint8_t { Person = 0, Assistant, Device, Summary, Tool, OldPerson, OldAssistant };

// How well a text matches (lane E2-SRCH2): the query's words as one phrase, every word in one line,
// every word in a line and the one before it. Anything less is not a hit.
enum class MatchRank : uint8_t { None = 0, Near = 1, Line = 2, Phrase = 3 };

struct SearchHit {
  std::string id;            // "r<q>" (an item of the transcript) or "m<row>" (an earlier-engine row)
  uint64_t t = 0;            // epoch seconds (0 = unknown)
  int16_t tzMinutes = 0;     // the item's own offset when it has one (an owner message), else 0
  bool hasTz = false;
  HitSource source = HitSource::Person;
  std::string tool;          // a result: its tool
  MatchRank rank = MatchRank::None;
  std::string excerpt;       // the best spot on one line, at most SearchLimits::excerptBytes
  bool cutBefore = false;    // the excerpt starts inside the text
  bool cutAfter = false;     // ... ends inside it
  bool outside = false;      // taken from outside content (a result stored as outside)
};

struct SearchLimits {
  size_t maxHits = kSearchMaxHits;            // hits a call returns
  size_t windowBytes = kSearchWindowBytes;    // what one window may read (lines, bulk payloads, old rows)
  uint32_t cardMs = kSearchCardMs;            // card time one window may spend (a safety stop)
  size_t excerptBytes = kSearchExcerptBytes;
  size_t rankedMax = kSearchRankedMax;        // the deepest rank a cursor reaches inside one window
};

struct SearchDeps {
  TxFiles* files = nullptr;
  std::string chatKey;
  OldRows* old = nullptr;          // null: the transcript only (a /clear generation, the daemon)
  std::function<uint32_t()> nowMs; // monotonic, the card time; null: none
  SearchLimits limits;
};

// One call reads a WINDOW of the conversation, newest first, from its start to its end: normally the
// whole range (the conversation, or the dates asked), else up to windowBytes (or cardMs), and ranks
// every match in it: Phrase, then Line, then Near, newest first among equals. It returns the ranks
// [k, k + maxHits). The cursor names the window (its start and end) and the next rank, so following
// cursors gives every match of a window once, then moves to the next window: every match of the range
// exactly once.
struct SearchOutcome {
  std::vector<SearchHit> hits;     // ranked: the best first
  uint32_t rankFrom = 0;           // the rank of hits[0] in its window (0: the first call of it)
  uint32_t matches = 0;            // every match in the window
  uint32_t further = 0;            // matches of the window ranked after the hits returned
  uint64_t furtherFromT = 0;       // the dates they span (0: unknown or none)
  uint64_t furtherToT = 0;
  uint64_t readFromT = 0;          // the oldest and newest stamped line the window read
  uint64_t readToT = 0;
  bool complete = false;           // the window reached the start of the range
  std::string next;                // the cursor of the next call ("" = nothing more)
  bool oldRead = false;            // the earlier engine's rows were reached
  uint32_t unreadBulk = 0;         // results whose stored text was gone (not searched)
  ReadStats card;
};

// Words of a query, as matching sees them (lower case). Pure.
std::vector<std::string> queryWords(const std::string& query);
// Every word occurs in `text` (case-insensitive substring). Pure.
bool matchesAll(const std::string& text, const std::vector<std::string>& words);
// Every word occurs within one line, or within a line and the one before it: in a long result of
// many lines the words must be near each other (a text of one line is matchesAll). What a hit is.
bool matchesNear(const std::string& text, const std::vector<std::string>& words);
// How `text` matches, and where its best spot begins (the phrase, else the line holding the most
// words). Pure.
MatchRank rankOf(const std::string& text, const std::vector<std::string>& words, size_t& spot);
// Is `cursor` one this module hands out ("" included)? Pure.
bool cursorOk(const std::string& cursor);

// One page of the search. InvalidArgument for a bad chat key, no files or a malformed cursor;
// Io when the card or the old store did not answer (the hits found so far stay in `out`, and
// out.next is where the next call starts again).
Status search(const SearchDeps& deps, const SearchAsk& ask, SearchOutcome& out);

// The oldest day row the import brought over verbatim: the m of the first owner message of the
// log that has one, reading segment 1 forward at most kVerbatimFloorScan bytes. 0: none found (no
// import item read): every earlier-engine row is below it.
Status verbatimFloor(TxFiles& files, const std::string& chatKey, uint64_t& floor);

// ---- view ------------------------------------------------------------------------------------
// These read through `files` as given: a caller that measures wraps it in ReadOnlyCounted.
// The stored item with this q (the first line of that q in file order). NotFound when the
// conversation holds none; Io when a read was refused.
Status findItem(TxFiles& files, const std::string& chatKey, uint64_t q, Item& out);

// The whole text an item stands for: a result's bulk payload or inline text, a message's or a
// note's text, a call's arguments. `total` is its size; `out` holds [offset, offset + limit) of it.
// NotFound when a bulk payload is no longer stored (retention); Io when it could not be read.
struct TextPage {
  size_t offset = 0;
  size_t limit = 0;
};
Status itemText(TxFiles& files, const std::string& chatKey, const Item& it, const TextPage& page, std::string& out,
                size_t& total);

// An owner message of this conversation that carries the photo `ref` ("p<ref8>"), newest first.
// found false: none in the conversation. Io when a read was refused; TooLarge past kPhotoScanBytes.
Status findPhoto(TxFiles& files, const std::string& chatKey, const std::string& ref, Item& owner, bool& found);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
