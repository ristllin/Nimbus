#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/status.h"

// seen_index - which Telegram updates engine 2 already took (PRD section 7, lane RB-TG1).
//
// Telegram re-serves a page until the next getUpdates confirms its offset. The device keeps that
// offset in RAM (the cursor is the truth) and checkpoints it to settings storage, but a full
// settings partition ignores the write (the 14 Oct replay loop) and a power cycle empties RAM and
// the RTC record, so after a reboot the same update can arrive again and again. Engine 2 stores
// the owner's message as a u item, with its update ids in u.src, before any model call. This
// index answers "was (chat, update id) already taken" from that record, so a re-served update is
// acked and dropped instead of answered twice.
//
// The rule:
//   - exact keys: the newest kSeenKeys (chat, update id) pairs noted. An update id is unique per
//     bot, so the chat only keeps the floors apart;
//   - a floor per chat: every id at or under it counts as seen. It rises only when a key leaves
//     the table (the smallest id leaves first: everything at or under it was taken long ago);
//   - seen(chat, id) = the exact key is held, or id <= floor(chat).
// A high-water mark alone is not enough: the round-boundary join (RB-TG2) can store a later
// text before an earlier photo that was queued, and a high-water rule would drop that photo.
//
// At boot the index is rebuilt from what the card holds (rebuildSeen): the owner rows' src ids
// of the chat's filled arena are noted, and head.json's hw (the highest update id stored on the
// chat) floors what the rows do not show:
//   - no owner row in the arena: floor = hw (the window starts after the chat's last message);
//   - rows: floor = the smallest row id - 1 (everything older than the window was taken); and
//     when hw is above every row (the fill did not reach the newest rows), floor = hw: a replay
//     loop is the worse failure.
// It reads the arena the fill already built (no card read of its own, no history window) and it
// writes nothing: no settings (NVS), no internal flash, no card. Its one block comes from the
// allocator it is given (PSRAM on the device). Portable, Arduino-free, host-tested
// (test_seen_index).
namespace nimbus::orch {

inline constexpr size_t kSeenKeys = 1024;
inline constexpr size_t kSeenChats = 32;

class SeenIndex {
 public:
  SeenIndex() = default;
  ~SeenIndex();
  SeenIndex(const SeenIndex&) = delete;
  SeenIndex& operator=(const SeenIndex&) = delete;

  // One block for the keys and the floors. Io when the allocator refuses (the index stays off:
  // seen() is false, note() does nothing, so intake runs as without it). Calling it again on a
  // ready index clears it.
  tx::Status init(tx::ArenaAllocFn alloc, tx::ArenaFreeFn release);
  bool ready() const { return keys_ != nullptr; }

  bool seen(std::string_view chat, uint64_t updateId) const;
  // Note an id taken into a u item (or joined into one). Id 0 is no update and is ignored.
  void note(std::string_view chat, uint64_t updateId);
  // The boot rebuild of one chat (the rule above). `rowIds`: the src ids of its owner rows.
  void rebuild(std::string_view chat, uint64_t hw, const std::vector<uint64_t>& rowIds);
  // Forget everything (a power cycle in a test).
  void clear();

  size_t size() const { return count_; }
  uint64_t floorOf(std::string_view chat) const;
  // The block this index holds, bytes (the PSRAM figure).
  static constexpr size_t blockBytes() { return kSeenKeys * sizeof(Key) + kSeenChats * sizeof(Floor); }

 private:
  struct Key {
    uint64_t chat;
    uint64_t id;
  };
  struct Floor {
    uint64_t chat;
    uint64_t floor;
  };
  static uint64_t chatHash(std::string_view chat);
  void raiseFloor(uint64_t chat, uint64_t to);
  void evictOldest();

  Key* keys_ = nullptr;
  Floor* floors_ = nullptr;
  size_t count_ = 0;
  size_t chats_ = 0;
  tx::ArenaFreeFn release_;
};

// The update ids of every owner row (u item) in a filled arena, in arena order.
tx::Status ownerRowIds(const tx::TxArena& arena, std::vector<uint64_t>& out);
// rebuild() for chat `chat` from its filled arena and its head.json state.
tx::Status rebuildSeen(SeenIndex& index, std::string_view chat, const tx::HeadState& head, const tx::TxArena& arena);

}  // namespace nimbus::orch
