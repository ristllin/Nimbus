#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/status.h"

// tx/arena.h - TxArena, the RAM side of one chat's transcript (PRD section 2): one flat
// PSRAM block of encoded items, each behind a 32-byte header, with no std::string per
// item. 576 KB for the active chat; the device keeps 3 chats resident, least recently
// used first out (TxArenaPool). The arena gets its memory through the allocator it is
// given, so the same code runs on the host with malloc and on the device with heap_caps
// PSRAM (Platform::allocLarge).
//
// Layout (lane RB-T1, lib/core/src/orch_tx_arena.cpp): records grow up from the start of
// the block, each a 32-byte header {q, t, turn, line bytes, kind, flags} and the stored
// line (tx/item.h, newline included) padded to 8 bytes; a table of 4-byte record offsets
// grows down from the end. An item therefore costs recordBytes(line) of the block and
// nothing anywhere else. get, line and header are O(1); find is a binary search on q.
//
// Memory rule: init allocates once through the allocator; append, appendLine, line,
// header, find, dropThrough, setFlags and clear never allocate (proved by the counting
// allocator in test/test_tx_arena). get decodes into the caller's Item, whose strings
// allocate as any std::string does: on the device call it under the PSRAM-first scope, or
// read line() bytes directly.
namespace nimbus {
namespace orch {
namespace tx {

inline constexpr size_t kArenaHeaderBytes = 32;
inline constexpr size_t kArenaSlotBytes = 4;
// Sized to the default profile, full (ruling 83 item 8; lane RB-T4): the largest head's due mark
// (gpt-5.6-luna, 320,000 B) plus one request's growth takes up to 514,496 B of arena on short chat
// turns (test_tx_budget); 576 KB keeps a 12% margin. Three resident chats: 1,728 KB of PSRAM.
inline constexpr size_t kActiveChatArenaBytes = 576 * 1024;
inline constexpr size_t kResidentChats = 3;
inline constexpr size_t kArenaMinBytes = 64;

// Bytes of the block one item takes: header, line padded to 8, and its slot.
inline constexpr size_t recordBytes(size_t lineBytes) {
  return kArenaHeaderBytes + ((lineBytes + 7) & ~size_t(7)) + kArenaSlotBytes;
}

using ArenaAllocFn = std::function<void*(size_t)>;
using ArenaFreeFn = std::function<void(void*)>;

// Flags kept beside an item in RAM; never part of the stored line.
enum ItemFlag : uint8_t {
  kItemNotStored = 1,   // the card refused its append: it stays in RAM (PRD section 2)
};

// The header of one resident item, read without decoding its line.
struct ArenaEntry {
  uint64_t q = 0;
  uint64_t t = 0;
  uint32_t turn = 0;
  uint32_t lineBytes = 0;   // the stored line, newline included
  ItemKind kind = ItemKind::User;
  uint8_t flags = 0;        // ItemFlag bits
};

class TxArena {
 public:
  TxArena() = default;
  // Inline on purpose: an out-of-line destructor that calls a std::function needs unwind
  // tables, and the device link keeps those even for an object nothing references.
  ~TxArena() {
    if (base_ && release_) release_(base_);
  }
  TxArena(const TxArena&) = delete;
  TxArena& operator=(const TxArena&) = delete;

  // Take `capacity` bytes from `alloc` (PSRAM on the device; capacity() is it rounded
  // down to 8). InvalidArgument for a capacity under kArenaMinBytes or over 4 GB, or a
  // null allocator; Io when the allocator returns null. A second init releases the
  // first block. `release` may be null (the block is then never given back).
  Status init(size_t capacity, ArenaAllocFn alloc, ArenaFreeFn release);
  // Encode and append one item, straight into the block. Its q must be above lastQ()
  // (q starts at 1; InvalidArgument otherwise, or before init, or when encodeLine
  // refuses the item). TooLarge when it does not
  // fit: `where` is "tx::encodeLine" when the line itself passes kMaxLineBytes, else
  // "tx::TxArena::append" (the arena is full: the caller compacts or drops the oldest
  // closed turn, never this item). Nothing is changed on failure.
  Status append(const Item& item);
  // Append one stored line as is (the restart fill): Torn when its framing or crc fails,
  // InvalidArgument when checkLine cannot read its k, q, turn and t or q is not above
  // lastQ(), TooLarge
  // when it does not fit. The line may come with or without its newline; the arena keeps
  // it with one. Only the header is read (tx/item.h checkLine): a line whose other
  // values cannot be read is kept, and get() answers InvalidArgument for it.
  Status appendLine(const char* data, size_t len);
  // Decode the item at `index` (0 = oldest resident). NotFound past the end.
  Status get(size_t index, Item& out) const;
  // The stored line of the item at `index` (newline included), valid until the arena
  // changes. NotFound past the end.
  Status line(size_t index, const char*& data, size_t& len) const;
  // The header of the item at `index`. NotFound past the end.
  Status header(size_t index, ArenaEntry& out) const;
  // Replace the ItemFlag bits of the item at `index`. NotFound past the end.
  Status setFlags(size_t index, uint8_t flags);
  // The index of the item with sequence number `q`; NotFound when it is not resident.
  Status find(uint64_t q, size_t& index) const;
  // Forget every item with q <= `q` (after a compaction moved them into a digest); the
  // rest move down in place. InvalidArgument before init.
  Status dropThrough(uint64_t q);
  // Forget every item; the block is kept.
  void clear();

  size_t count() const { return count_; }
  // Bytes of the block in use: records and their slots.
  size_t usedBytes() const { return used_ + count_ * kArenaSlotBytes; }
  size_t capacity() const { return cap_; }
  // Bytes a further append may still take (compare with recordBytes(line)).
  size_t freeBytes() const { return cap_ - usedBytes(); }
  // The highest q appended since init or clear (dropThrough keeps it); 0 before any.
  uint64_t lastQ() const { return lastQ_; }

 private:
  uint8_t* base_ = nullptr;
  size_t cap_ = 0;
  size_t used_ = 0;
  size_t count_ = 0;
  uint64_t lastQ_ = 0;
  ArenaFreeFn release_;

  uint32_t* slots() const;
  uint8_t* record(size_t index) const;
  void place(const ArenaEntry& head, size_t lineBytes);
  bool inOrder(uint64_t q) const;
};

// The resident chats (PRD section 2: 3 chats, least recently used first out). Each slot
// owns one TxArena of `perChat` bytes, allocated on the slot's first use and reused, not
// freed, when its chat is evicted. Chat keys are at most kChatKeyMax bytes.
class TxArenaPool {
 public:
  static constexpr size_t kChatKeyMax = 47;

  // InvalidArgument for a null allocator or perChat under kArenaMinBytes. Allocates
  // nothing yet; called again, it empties every slot and each takes a new block (and gives
  // its old one back) on its next use.
  Status init(size_t perChat, ArenaAllocFn alloc, ArenaFreeFn release);
  // The arena of `chatKey`. `fresh` is true when the chat was not resident: the arena is
  // empty and the caller fills it from the log. Evicts the least recently used chat when
  // every slot is taken. InvalidArgument for an empty or over-long key or before init;
  // Io when the first allocation of a slot fails.
  Status acquire(const char* chatKey, TxArena*& out, bool& fresh);
  // Drop `chatKey` from residence (its slot is emptied, its block kept). NotFound when it
  // is not resident.
  Status forget(const char* chatKey);
  // Is `chatKey` resident? Does not count as a use.
  bool resident(const char* chatKey) const;

 private:
  struct Slot {
    TxArena arena;
    char key[kChatKeyMax + 1] = {0};
    uint32_t lastUse = 0;
    bool ready = false;
  };
  Slot slots_[kResidentChats];
  size_t perChat_ = 0;
  ArenaAllocFn alloc_;
  ArenaFreeFn release_;
  uint32_t tick_ = 0;

  int slotOf(const char* chatKey) const;
  size_t victim() const;
};

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
