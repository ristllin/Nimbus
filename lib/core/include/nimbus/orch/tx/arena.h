#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/status.h"

// tx/arena.h - TxArena, the RAM side of one chat's transcript (PRD section 2): one flat
// PSRAM block of encoded items, each behind a 32-byte header, with no std::string per
// item. 384 KB for the active chat; the device keeps 3 chats resident, least recently
// used first out. The arena gets its memory through the allocator it is given, so the
// same code runs on the host with malloc and on the device with heap_caps PSRAM.
//
// Seam declared by lane RB-B0a; the body (orch_tx_arena.cpp) is lane RB-T1's.
namespace nimbus {
namespace orch {
namespace tx {

inline constexpr size_t kArenaHeaderBytes = 32;
inline constexpr size_t kActiveChatArenaBytes = 384 * 1024;
inline constexpr size_t kResidentChats = 3;

using ArenaAllocFn = std::function<void*(size_t)>;
using ArenaFreeFn = std::function<void(void*)>;

class TxArena {
 public:
  TxArena() = default;
  ~TxArena();
  TxArena(const TxArena&) = delete;
  TxArena& operator=(const TxArena&) = delete;

  // Take `capacity` bytes from `alloc` (PSRAM on the device). InvalidArgument for a
  // zero capacity or a null allocator; Io when the allocator returns null.
  Status init(size_t capacity, ArenaAllocFn alloc, ArenaFreeFn release);
  // Encode and append one item. TooLarge when it does not fit: the caller compacts or
  // drops the oldest closed turn, never this item.
  Status append(const Item& item);
  // Decode the item at `index` (0 = oldest resident).
  Status get(size_t index, Item& out) const;
  // The index of the item with sequence number `q`; NotFound when it is not resident.
  Status find(uint64_t q, size_t& index) const;
  // Forget every item with q <= `q` (after a compaction moved them into a digest).
  Status dropThrough(uint64_t q);
  void clear();

  size_t count() const { return count_; }
  size_t usedBytes() const { return used_; }
  size_t capacity() const { return cap_; }

 private:
  uint8_t* base_ = nullptr;
  size_t cap_ = 0;
  size_t used_ = 0;
  size_t count_ = 0;
  ArenaFreeFn release_;
};

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
