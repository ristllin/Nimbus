#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "nimbus/harness/conversation.h"   // ConvSpan
#include "nimbus/orch/episodic.h"          // EpisodicMessage

// conv_cache - a chat's conversation rows after its fold anchor, kept in RAM between turns
// (lane H of the history fix, 16 Nov 2026).
//
// The history window used to re-read the chat's rows off the card every turn, newest
// first, up to a few hundred rows, under memory::Lock: a 490-row read took about 60 s on
// Lumi. A window that holds a whole day or more (206 KB on a 360 KB head) cannot be read
// that way. The cache holds the rows instead:
//   - per chat, conversation kinds only (what Deps::historyRows returns), at most kChats
//     chats (least recently used goes), each bounded in bytes (the head's historyMax plus
//     a quarter) and, on the v5.1.1 floor, in rows;
//   - brought up to date on every access with ONE read of the rows newer than the newest
//     it holds (the after cursor): those are the rows just written, still in the store's
//     RAM ring, so a steady-state turn reads no card row. A read that comes back full (the
//     cache is more than a page behind) drops the entry: it is filled again newest first;
//   - filled NEWEST FIRST with the before cursor down to the fold anchor (thru), one page
//     per call: one on a turn, one per idle pump pass (RULINGS-v3 item 4: the first turn
//     after a boot with a 400-row backlog carries the newest exchanges, not the oldest).
//     The fill is done only when it reaches thru, a /clear or the chat's start: a store
//     whose index does not reach that far yet (its backfill indexes the card after a boot)
//     leaves the rows below named as unread until a later fill reads them;
//   - trimmed when a fold moves thru (rows at or before it go), and dropped when thru moves
//     back; a /clear boundary row ends the conversation (older rows go);
//   - patched by the photo-record overlay (row_overlay.h's patch journal: a record that
//     arrived after the row was read replaces its text here too).
// What the cache does not hold of the rows after thru is reported as a span (rows
// evicted for the bound, counted; rows not read yet, not counted), so the request names
// it instead of losing it silently.
//
// Portable (lib/harness): the device, the Virtual Nimbus and the lab use the same class
// through the engine. Thread-safe (one mutex): the turn and the idle pump may run on
// different threads on the Virtual Nimbus.
namespace agent {

struct HistoryQuery;

class ConvCache {
 public:
  static constexpr size_t kChats = 3;
  static constexpr size_t kPageRows = 24;   // one read (a page) of a chat's rows
  // The after cursor from a chat's very first row (no fold anchor yet).
  static constexpr const char* kBeforeFirstRow = "m00000000";
  // Idle fill passes in a row that may read nothing new before the idle pump leaves the
  // chat to its turns (each turn still tries one page): a store whose index has not
  // reached the rows yet would otherwise be read every pump pass for nothing.
  static constexpr uint8_t kIdleStalls = 3;

  // One read of a chat's rows (Deps::historyRows bound to the chat).
  using ReadFn = std::function<std::vector<nimbus::orch::EpisodicMessage>(const HistoryQuery&)>;

  struct Bound {
    size_t bytes = 0;   // 0 = no byte bound
    size_t rows = 0;    // 0 = no row bound
  };

  // What one access did and found.
  struct View {
    std::vector<nimbus::orch::EpisodicMessage> rows;   // after thru, oldest first (a copy)
    bool complete = false;   // every row after thru is in `rows` or `span`
    ConvSpan span;           // rows after thru not in `rows` (evicted, or not read yet)
    int pages = 0;           // historyRows calls this access made
  };

  // Bring `chat`'s entry up to date against `thru` and return its rows: the delta read,
  // then at most `fillPages` pages of the newest-first fill, each only while `pageAllowed`
  // (when set) says so. The engine allows the first while its turn has read no card row
  // yet and each later one while the page before read none (the store's RAM ring served
  // it), so a turn makes at most one page of card reads in all.
  View access(const std::string& chat, const std::string& thru, const Bound& bound, int fillPages,
              const ReadFn& read, const std::function<bool()>& pageAllowed = std::function<bool()>());
  // One fill page for the most recently used chat that is still filling and not stalled
  // (the idle pump). False when there is none (nothing was read).
  bool fillStep(const std::function<ReadFn(const std::string& chat)>& readerFor);
  // The chat's newest `n` rows held, newest first (no read; [] when the chat is not held).
  std::vector<nimbus::orch::EpisodicMessage> newest(const std::string& chat, size_t n) const;

  // Diagnostics and tests.
  size_t chats() const;
  size_t bytesOf(const std::string& chat) const;
  bool holds(const std::string& chat) const;
  void drop(const std::string& chat);   // the next access fills the chat again

 private:
  struct Entry {
    std::string chat;
    std::string thru;                 // the anchor the rows are after
    std::vector<nimbus::orch::EpisodicMessage> rows;   // oldest first
    std::string newestId;             // the newest row read (kept when a fold trims every row)
    std::string fillBefore;           // the next fill page's before cursor ("" = from the newest)
    bool complete = false;            // the fill reached thru, a /clear or the chat's start
    bool started = false;             // the first fill page ran
    size_t bytes = 0;
    ConvSpan span;                    // evicted rows (counted) and the unread rest
    uint32_t lastUse = 0;
    uint32_t patchSeq = 0;            // row_overlay's patch journal, applied up to here
    uint8_t stalls = 0;               // fill pages in a row that read nothing new
  };

  Entry* find(const std::string& chat);
  const Entry* find(const std::string& chat) const;
  Entry& open(const std::string& chat, const std::string& thru);
  static void reset(Entry& e, const std::string& thru);
  void syncThru(Entry& e, const std::string& thru);
  void applyPatches(Entry& e);
  int readNewer(Entry& e, const ReadFn& read);
  bool fillPage(Entry& e, const Bound& bound, const ReadFn& read);
  static bool gapAfterThru(const Entry& e, const ReadFn& read);
  void enforce(Entry& e, const Bound& bound);
  static size_t rowBytes(const nimbus::orch::EpisodicMessage& m);
  View viewOf(const Entry& e) const;

  mutable std::mutex mu_;
  std::vector<Entry> entries_;
  uint32_t tick_ = 0;
  Bound lastBound_;   // the bound of the latest access (the idle fill uses it)
};

}  // namespace agent
