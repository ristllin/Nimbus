#include "nimbus/orch/seen_index.h"

#include <algorithm>
#include <new>

// seen_index - see the header (PRD section 7, lane RB-TG1).
namespace nimbus::orch {

namespace {
constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
}  // namespace

SeenIndex::~SeenIndex() {
  if (keys_ && release_) release_(keys_);
}

uint64_t SeenIndex::chatHash(std::string_view chat) {
  uint64_t h = kFnvOffset;
  for (unsigned char c : chat) {
    h ^= c;
    h *= kFnvPrime;
  }
  return h;
}

tx::Status SeenIndex::init(tx::ArenaAllocFn alloc, tx::ArenaFreeFn release) {
  if (keys_) {
    clear();
    return tx::Status::success();
  }
  if (!alloc || !release) return tx::Status::fail(tx::Err::InvalidArgument, "SeenIndex::init");
  void* block = alloc(blockBytes());
  if (!block) return tx::Status::fail(tx::Err::Io, "SeenIndex::init");
  keys_ = static_cast<Key*>(block);
  floors_ = reinterpret_cast<Floor*>(static_cast<char*>(block) + kSeenKeys * sizeof(Key));
  release_ = std::move(release);
  clear();
  return tx::Status::success();
}

void SeenIndex::clear() {
  count_ = 0;
  chats_ = 0;
}

uint64_t SeenIndex::floorOf(std::string_view chat) const {
  const uint64_t c = chatHash(chat);
  for (size_t i = 0; i < chats_; ++i)
    if (floors_[i].chat == c) return floors_[i].floor;
  return 0;
}

bool SeenIndex::seen(std::string_view chat, uint64_t updateId) const {
  if (!keys_ || updateId == 0) return false;
  const uint64_t c = chatHash(chat);
  for (size_t i = 0; i < count_; ++i)
    if (keys_[i].id == updateId && keys_[i].chat == c) return true;
  return updateId <= floorOf(chat);
}

// The floor table is full only with more than kSeenChats chats: the chat with the lowest floor
// gives its slot up (its ids are the oldest the index knows of).
void SeenIndex::raiseFloor(uint64_t chat, uint64_t to) {
  for (size_t i = 0; i < chats_; ++i)
    if (floors_[i].chat == chat) {
      floors_[i].floor = std::max(floors_[i].floor, to);
      return;
    }
  size_t slot = chats_;
  if (chats_ == kSeenChats) {
    slot = 0;
    for (size_t i = 1; i < chats_; ++i)
      if (floors_[i].floor < floors_[slot].floor) slot = i;
  } else {
    ++chats_;
  }
  floors_[slot] = Floor{chat, to};
}

// The smallest id leaves; its chat's floor rises to it.
void SeenIndex::evictOldest() {
  size_t old = 0;
  for (size_t i = 1; i < count_; ++i)
    if (keys_[i].id < keys_[old].id) old = i;
  raiseFloor(keys_[old].chat, keys_[old].id);
  keys_[old] = keys_[--count_];
}

void SeenIndex::note(std::string_view chat, uint64_t updateId) {
  if (!keys_ || updateId == 0 || seen(chat, updateId)) return;
  if (count_ == kSeenKeys) evictOldest();
  keys_[count_++] = Key{chatHash(chat), updateId};
}

void SeenIndex::rebuild(std::string_view chat, uint64_t hw, const std::vector<uint64_t>& rowIds) {
  if (!keys_) return;
  uint64_t lo = 0, hi = 0;
  for (uint64_t id : rowIds) {
    if (id == 0) continue;
    lo = lo ? std::min(lo, id) : id;
    hi = std::max(hi, id);
  }
  const uint64_t c = chatHash(chat);
  if (hi == 0 || hw > hi) raiseFloor(c, hw);   // no row in the window, or the fill fell short
  else if (lo > 1) raiseFloor(c, lo - 1);       // older than the window: taken
  for (uint64_t id : rowIds) note(chat, id);
}

tx::Status ownerRowIds(const tx::TxArena& arena, std::vector<uint64_t>& out) {
  out.clear();
  tx::Item it;
  for (size_t i = 0; i < arena.count(); ++i) {
    tx::ArenaEntry e;
    tx::Status st = arena.header(i, e);
    if (!st.ok()) return st;
    if (e.kind != tx::ItemKind::User) continue;
    if (!(st = arena.get(i, it)).ok()) return st;
    out.insert(out.end(), it.src.begin(), it.src.end());
  }
  return tx::Status::success();
}

tx::Status rebuildSeen(SeenIndex& index, std::string_view chat, const tx::HeadState& head, const tx::TxArena& arena) {
  std::vector<uint64_t> ids;
  const tx::Status st = ownerRowIds(arena, ids);
  if (!st.ok()) {
    index.rebuild(chat, head.hw, {});   // the rows cannot be read: the high-water mark floors all
    return st;
  }
  index.rebuild(chat, head.hw, ids);
  return tx::Status::success();
}

}  // namespace nimbus::orch
