#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "nimbus/orch/tx/log.h"   // tx::TxFiles: the storage seam (an interface only, no engine-2 code)

// effect_ring.h - each chat's recent effects, stored (PRD section 8, rail 8 "Truth check";
// lane RB-FX). What the device actually did, one entry per effect: the time, the tool and a
// one-line receipt. It is the cross-turn, per-chat continuation of the per-turn ledger
// (turn_effects.h): the truth check (RB-G0, lib/core/src/orch_truth.cpp) reads it so a true
// "I logged it at 13:02" is not a hit, and the old engine's "recent actions" data block
// (RB-Q3a: the newest 12, at most 2,048 B) is rendered from it. Readers own every
// model-facing word; this file renders no text for the model.
//
// The contract, each part a property of test/test_effect_ring:
// - Per chat, the newest kRingEntries (50) entries; the 51st append drops the oldest.
// - An entry holds a time (epoch seconds, UTC; 0 when the clock is not set), a tool name of
//   at most kToolMax bytes and a receipt of at most kReceiptMax (160) bytes. Both are cleaned
//   at append (cleanText): one line, every control character (C0, DEL, C1, U+2028, U+2029,
//   tab included) becomes a space, a byte that is not part of valid UTF-8 becomes '?', and
//   the text is clipped on a UTF-8 boundary. Nothing the ring hands back can hold a newline.
// - One append per effect: append() makes exactly one TxFiles::append of one whole line
//   (at most kLineMax bytes, one bounded card piece under the gated card lock, Ruling 19).
//   Every kCompactLines lines the chat's file is rewritten once from RAM (the newest 50
//   entries, at most kRingEntries * kLineMax bytes: one writeWhole of <key>.ring.tmp, then
//   one rename over <key>.ring), so a file never grows past about 2 x 50 lines.
// - Crash safety: a cut at any point of an append loses at most that entry. A line counts
//   only with its '\n' and a good crc; a torn tail is dropped at load, and the next append
//   after a torn tail starts with "~\n" inside the same append, so the torn bytes stay a line
//   of their own and are never completed or glued onto the next entry. A cut or refusal
//   during the rewrite leaves the old file whole (the temp is deleted at the next use), or, on
//   a binding whose rename removes the target first, only the whole temp, which becomes the
//   file at the next use; until it has, nothing is written beside it.
// - Nothing is written for a chat before its file was read (and any temp settled): an entry
//   kept meanwhile is RamOnly. So RAM is the order of record: a refused append that landed
//   anyway, or a refusal followed by a stored append, never reorders or doubles an entry.
// - Survives a restart: a chat's ring is loaded from its file at its first use (append or
//   read) after boot, in kReadSlice pieces (one bounded card read each).
// - No card: when storage refuses (TxFiles::list answers false, an append is refused) or
//   there is no storage at all (files == nullptr), the ring runs from RAM: the append still
//   lands in RAM and answers AppendResult::RamOnly, the entry reads back with stored=false,
//   and backing() answers Backing::RamOnly (until the chat's next write lands), so a reader
//   can say so. A RAM-only entry is never
//   written later by itself; the next rewrite of the chat's file (once storage is back and
//   the file was loaded) carries the whole RAM ring, RAM-only entries included.
// - Chats never see each other's entries: every read and write names one chat id; each chat
//   has its own RAM ring and its own file /mem/fx/<key>.ring, where the key is an injective
//   function of the chat id (ringKey): a Telegram chat id ([-0-9]) is its own key, any
//   other id of at most kOtherIdMax bytes is "k" + the hex of its bytes. Any other chat id is
//   refused (AppendResult::Refused, nothing read or written).
// - Memory: RAM rings live in the working allocator (psram_alloc.h: PSRAM on the device),
//   kRingEntries * sizeof(RingEntry) (about 11 KB) per resident chat, at most kMaxChats
//   resident; the least recently used one is evicted (a stored chat is reloaded from its
//   file at its next use; a RAM-only chat's entries are lost with it). Loads, rewrites and
//   line buffers are bounded by kReadSlice and kRingEntries * kLineMax and are made inside
//   the call; on the device the binding calls the ring under the PSRAM-first scope
//   (agent::PsramFirstScope). The object itself holds a few pointers in internal RAM.
// - A refused allocation (ruling 30; the working allocator or plain new) is a designed state,
//   never an exception out of the ring. append answers AppendResult::NoMemory with nothing
//   kept or written (every allocation of an append comes before its write and its RAM entry);
//   a refused load leaves the chat not loaded (RamOnly, RAM untouched) and is tried again at
//   its next use; a refused rewrite is skipped and tried at the next append; visit and recent
//   answer 0. Nothing already stored is lost. No buffer grows by doubling: the load reserves
//   its kRingEntries block once.
// - Thread safety: every public call takes the ring's own mutex, so the ring may be shared by
//   the tasks that record and read effects. The binding's TxFiles takes the card lock per
//   storage call; never call the ring while holding the card lock unless that lock is
//   recursive.
//
// On disk (/mem/fx/<key>.ring, append-only between rewrites, outside the file index):
//   f1 <TAB> <epoch, decimal> <TAB> <tool> <TAB> <receipt> <TAB> <crc32, 8 lower-case hex> \n
// The crc covers every byte before the last tab. A line in any other form is dropped.
//
// Portable, Arduino-free, host-tested (test/test_effect_ring; nimbusd: test_hosted_effects).
namespace nimbus {
namespace orch {
namespace fx {

inline constexpr const char* kFxRoot = "/mem/fx";
inline constexpr size_t kRingEntries = 50;     // per chat
inline constexpr size_t kReceiptMax = 160;     // bytes; equals ledger::kSinkLineMax (RB-L1b)
inline constexpr size_t kToolMax = 48;         // bytes of a tool name
inline constexpr size_t kOtherIdMax = 23;      // bytes of a chat id that is not a Telegram id
inline constexpr size_t kKeyMax = 47;          // bytes of a key (and of a Telegram chat id)
inline constexpr size_t kMaxChats = 8;         // resident RAM rings
inline constexpr size_t kCompactLines = 2 * kRingEntries;   // lines in a file before a rewrite
inline constexpr size_t kReadSlice = 4096;     // one card read at load
// The longest line: "f1\t" + epoch (20) + "\t" + tool + "\t" + receipt + "\t" + crc (8) + "\n".
inline constexpr size_t kLineMax = 3 + 20 + 1 + kToolMax + 1 + kReceiptMax + 1 + 8 + 1;

// One effect as the ring holds it. Fixed size, so a RAM ring is one block.
struct RingEntry {
  int64_t epoch = 0;                 // seconds since 1970 UTC; 0: the clock was not set
  char tool[kToolMax + 1] = {};      // cleaned, NUL-terminated
  char receipt[kReceiptMax + 1] = {};// cleaned, NUL-terminated, one line
  bool stored = false;               // its line is on storage (appended, rewritten or loaded)
};

enum class Backing : uint8_t {
  Storage = 0,   // the chat's ring was loaded from (or confirmed absent on) storage
  RamOnly = 1,   // no storage, or storage refused: the ring lives in RAM only
};

enum class AppendResult : uint8_t {
  Stored = 0,    // in RAM and its line is on storage
  RamOnly = 1,   // in RAM only (no storage, or the append was refused)
  Refused = 2,   // a chat id the ring cannot key: nothing was kept
  NoMemory = 3,  // an allocation was refused: nothing was kept or written (ruling 30)
};

// Counters for evidence and tests (never reset; summed over every chat).
struct RingStats {
  uint32_t loads = 0;          // chat files read at first use
  uint32_t loaded = 0;         // entries taken from storage
  uint32_t dropped = 0;        // lines dropped at load (torn, bad crc, unknown form)
  uint32_t stored = 0;         // appends that reached storage
  uint32_t ramOnly = 0;        // appends kept in RAM only
  uint32_t refused = 0;        // appends refused (bad chat id)
  uint32_t rewrites = 0;       // file rewrites done (temp and rename)
  uint32_t evictions = 0;      // RAM rings evicted for room
  uint32_t noMemory = 0;       // calls that met a refused allocation (each a designed state)
};

// The cleaning every tool name and receipt goes through: one line (control characters,
// U+2028 and U+2029 become a space), invalid UTF-8 bytes become '?', clipped to at most
// `cap` bytes on a UTF-8 boundary. Readers may apply it to their own text.
std::string cleanText(const char* p, size_t n, size_t cap);
inline std::string cleanText(const std::string& s, size_t cap) { return cleanText(s.data(), s.size(), cap); }

// The storage key of a chat id: the id itself for [-0-9]{1,47}; "k" + lower-case hex of the
// bytes for any other id of 1 to kOtherIdMax bytes. False (out cleared) for anything else.
bool ringKey(const std::string& chatId, std::string& key);
// "/mem/fx/<key>.ring"; false when ringKey is.
bool ringPath(const std::string& chatId, std::string& path);

// One stored line (with its '\n') and its reading. decodeLine takes a line without its '\n'
// and answers false for any line not in the stored form (a torn tail, a bad crc).
std::string encodeLine(const RingEntry& e);
bool decodeLine(const char* p, size_t n, RingEntry& out);

class EffectRing {
 public:
  // `files` == nullptr: RAM only for the life of the ring. Otherwise the binding's storage
  // (the card through the gated card path on the device, the volume on nimbusd); it must
  // outlive the ring.
  explicit EffectRing(tx::TxFiles* files = nullptr);
  ~EffectRing();
  EffectRing(const EffectRing&) = delete;
  EffectRing& operator=(const EffectRing&) = delete;

  // Record one effect of `chatId`: cleans `tool` and `receipt`, keeps the entry in RAM and
  // appends its line (one TxFiles::append). Never blocks the caller's turn on a failure.
  AppendResult append(const std::string& chatId, int64_t epoch, const std::string& tool,
                      const std::string& receipt);

  // The newest `n` (at most kRingEntries) entries of `chatId`, oldest first, through `fn`
  // under the ring's lock (fn must not call the ring). Returns how many were visited; 0 for
  // a chat with none or a chat id the ring cannot key.
  size_t visit(const std::string& chatId, size_t n, const std::function<void(const RingEntry&)>& fn);
  // The same, copied out (out is cleared first).
  size_t recent(const std::string& chatId, size_t n, std::vector<RingEntry>& out);

  // Where `chatId`'s ring lives now (loads it first when it is not resident). RamOnly for a
  // chat id the ring cannot key.
  Backing backing(const std::string& chatId);

  RingStats stats() const;
  size_t residentChats() const;

 private:
  struct Slot;
  Slot* slotFor(const std::string& key);   // find or claim, then load or settle
  Slot* findSlot(const std::string& key);
  Slot* claimSlot(const std::string& key);  // a free or the least recently used slot
  void loadInto(Slot& s, const std::string& path);    // never throws: a refusal leaves it unloaded
  void loadFiles(Slot& s, const std::string& path);
  bool rewrite(Slot& s, const std::string& path);     // never throws: a refusal skips it
  bool rewriteFile(Slot& s, const std::string& path, bool& renaming);
  AppendResult appendLocked(const std::string& chatId, int64_t epoch, const std::string& tool,
                            const std::string& receipt);

  tx::TxFiles* files_;
  Slot* slots_ = nullptr;        // kMaxChats, in the working allocator
  uint32_t tick_ = 0;            // use order for eviction
  RingStats stats_;
  mutable std::mutex mu_;
};

}  // namespace fx
}  // namespace orch
}  // namespace nimbus
