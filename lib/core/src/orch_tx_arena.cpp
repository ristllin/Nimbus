// The transcript arena (tx/arena.h): one flat block per chat, records up, slots down.
// Lane RB-T1. Nothing here allocates after init: append encodes straight into the block
// and appendLine copies a checked line (the destructor is inline in arena.h; see there).
#include <cstddef>
#include <cstring>

#include "nimbus/orch/tx/arena.h"

namespace nimbus {
namespace orch {
namespace tx {

namespace {

// The 32-byte header in front of every stored line.
struct Rec {
  uint64_t q;
  uint64_t t;
  uint32_t turn;
  uint32_t len;
  char kind;
  uint8_t flags;
  uint8_t pad[6];
};
static_assert(sizeof(Rec) == kArenaHeaderBytes, "the arena header is 32 bytes");

// Writes the encoder's bytes into the block up to `limit`, counts every byte offered.
struct BoundedSink final : LineSink {
  char* dst;
  size_t limit;
  size_t n = 0;
  BoundedSink(char* d, size_t l) : dst(d), limit(l) {}
  void put(const char* p, size_t len) override {
    if (n < limit) std::memcpy(dst + n, p, len < limit - n ? len : limit - n);
    n += len;
  }
};

constexpr size_t pad8(size_t n) { return (n + 7) & ~size_t(7); }

}  // namespace

uint32_t* TxArena::slots() const { return reinterpret_cast<uint32_t*>(base_ + cap_); }

uint8_t* TxArena::record(size_t index) const { return base_ + slots()[-1 - static_cast<ptrdiff_t>(index)]; }

Status TxArena::init(size_t capacity, ArenaAllocFn alloc, ArenaFreeFn release) {
  if (capacity < kArenaMinBytes || capacity > UINT32_MAX || !alloc)
    return Status::fail(Err::InvalidArgument, "tx::TxArena::init");
  if (base_ && release_) release_(base_);
  base_ = nullptr;
  cap_ = 0;
  clear();
  release_ = std::move(release);
  void* block = alloc(capacity);
  if (!block) return Status::fail(Err::Io, "tx::TxArena::init");
  base_ = static_cast<uint8_t*>(block);
  cap_ = capacity & ~size_t(7);
  return Status::success();
}

// Commit a record whose line already sits after its header at the end of the records.
void TxArena::place(const ArenaEntry& head, size_t lineBytes) {
  uint8_t* at = base_ + used_;
  std::memset(at + kArenaHeaderBytes + lineBytes, 0, pad8(lineBytes) - lineBytes);
  Rec r{};
  r.q = head.q;
  r.t = head.t;
  r.turn = head.turn;
  r.len = static_cast<uint32_t>(lineBytes);
  r.kind = kindLetter(head.kind);
  r.flags = head.flags;
  std::memcpy(at, &r, sizeof r);
  slots()[-1 - static_cast<ptrdiff_t>(count_)] = static_cast<uint32_t>(used_);
  used_ += kArenaHeaderBytes + pad8(lineBytes);
  ++count_;
  lastQ_ = head.q;
}

// q only grows: an item must be newer than every item appended since init or clear,
// including items already dropped through.
bool TxArena::inOrder(uint64_t q) const { return !(count_ || lastQ_) || q > lastQ_; }

Status TxArena::append(const Item& item) {
  if (!base_ || !inOrder(item.q)) return Status::fail(Err::InvalidArgument, "tx::TxArena::append");
  const size_t avail = freeBytes();
  const size_t room = avail > kArenaHeaderBytes + kArenaSlotBytes ? avail - kArenaHeaderBytes - kArenaSlotBytes : 0;
  BoundedSink sink(reinterpret_cast<char*>(base_ + used_ + kArenaHeaderBytes), room < kMaxLineBytes ? room : kMaxLineBytes);
  size_t lineBytes = 0;
  const Status st = encodeLineTo(item, sink, lineBytes);
  if (!st.ok()) return st;
  if (recordBytes(lineBytes) > avail) return Status::fail(Err::TooLarge, "tx::TxArena::append");
  ArenaEntry head;
  head.q = item.q;
  head.t = item.t;
  head.turn = item.turn;
  head.kind = item.kind;
  place(head, lineBytes);
  return Status::success();
}

Status TxArena::appendLine(const char* data, size_t len) {
  if (!base_) return Status::fail(Err::InvalidArgument, "tx::TxArena::appendLine");
  LineHeader h;
  const Status st = checkLine(data, len, h);
  if (!st.ok()) return st;
  if (!inOrder(h.q)) return Status::fail(Err::InvalidArgument, "tx::TxArena::appendLine");
  const size_t body = data[len - 1] == '\n' ? len - 1 : len;
  if (recordBytes(body + 1) > freeBytes()) return Status::fail(Err::TooLarge, "tx::TxArena::appendLine");
  ArenaEntry head;
  head.q = h.q;
  head.t = h.t;
  head.turn = h.turn;
  head.kind = h.kind;
  uint8_t* dst = base_ + used_ + kArenaHeaderBytes;
  std::memcpy(dst, data, body);
  dst[body] = '\n';
  place(head, body + 1);
  return Status::success();
}

Status TxArena::header(size_t index, ArenaEntry& out) const {
  if (index >= count_) return Status::fail(Err::NotFound, "tx::TxArena::header");
  Rec r;
  std::memcpy(&r, record(index), sizeof r);
  out.q = r.q;
  out.t = r.t;
  out.turn = r.turn;
  out.lineBytes = r.len;
  kindFromLetter(r.kind, out.kind);
  out.flags = r.flags;
  return Status::success();
}

Status TxArena::line(size_t index, const char*& data, size_t& len) const {
  if (index >= count_) return Status::fail(Err::NotFound, "tx::TxArena::line");
  const uint8_t* rec = record(index);
  Rec r;
  std::memcpy(&r, rec, sizeof r);
  data = reinterpret_cast<const char*>(rec + kArenaHeaderBytes);
  len = r.len;
  return Status::success();
}

Status TxArena::get(size_t index, Item& out) const {
  const char* data = nullptr;
  size_t len = 0;
  const Status st = line(index, data, len);
  if (!st.ok()) return Status::fail(Err::NotFound, "tx::TxArena::get");
  return decodeLine(data, len, out);
}

Status TxArena::setFlags(size_t index, uint8_t flags) {
  if (index >= count_) return Status::fail(Err::NotFound, "tx::TxArena::setFlags");
  record(index)[offsetof(Rec, flags)] = flags;
  return Status::success();
}

Status TxArena::find(uint64_t q, size_t& index) const {
  size_t lo = 0, hi = count_;
  while (lo < hi) {   // first index whose q >= the one sought
    const size_t mid = lo + (hi - lo) / 2;
    uint64_t at;
    std::memcpy(&at, record(mid), sizeof at);
    if (at < q)
      lo = mid + 1;
    else
      hi = mid;
  }
  uint64_t at = 0;
  if (lo < count_) std::memcpy(&at, record(lo), sizeof at);
  if (lo == count_ || at != q) return Status::fail(Err::NotFound, "tx::TxArena::find");
  index = lo;
  return Status::success();
}

Status TxArena::dropThrough(uint64_t q) {
  if (!base_) return Status::fail(Err::InvalidArgument, "tx::TxArena::dropThrough");
  size_t k = 0;   // items with q <= the one given: they are the oldest, q only grows
  while (k < count_) {
    uint64_t at;
    std::memcpy(&at, record(k), sizeof at);
    if (at > q) break;
    ++k;
  }
  if (!k) return Status::success();
  const uint32_t off = k < count_ ? slots()[-1 - static_cast<ptrdiff_t>(k)] : static_cast<uint32_t>(used_);
  std::memmove(base_, base_ + off, used_ - off);
  uint32_t* top = slots();
  for (size_t i = 0; i + k < count_; ++i) top[-1 - static_cast<ptrdiff_t>(i)] = top[-1 - static_cast<ptrdiff_t>(i + k)] - off;
  used_ -= off;
  count_ -= k;
  return Status::success();
}

void TxArena::clear() {
  used_ = 0;
  count_ = 0;
  lastQ_ = 0;
}

// ---- TxArenaPool ----------------------------------------------------------------------

Status TxArenaPool::init(size_t perChat, ArenaAllocFn alloc, ArenaFreeFn release) {
  if (perChat < kArenaMinBytes || !alloc) return Status::fail(Err::InvalidArgument, "tx::TxArenaPool::init");
  perChat_ = perChat;
  alloc_ = std::move(alloc);
  release_ = std::move(release);
  for (Slot& s : slots_) {
    s.key[0] = '\0';
    s.arena.clear();
  }
  return Status::success();
}

int TxArenaPool::slotOf(const char* chatKey) const {
  for (size_t i = 0; i < kResidentChats; ++i)
    if (slots_[i].key[0] && std::strcmp(slots_[i].key, chatKey) == 0) return static_cast<int>(i);
  return -1;
}

// An empty slot first, else the least recently used one.
size_t TxArenaPool::victim() const {
  size_t best = 0;
  for (size_t i = 0; i < kResidentChats; ++i) {
    if (!slots_[i].key[0]) return i;
    if (slots_[i].lastUse < slots_[best].lastUse) best = i;
  }
  return best;
}

Status TxArenaPool::acquire(const char* chatKey, TxArena*& out, bool& fresh) {
  out = nullptr;
  fresh = false;
  size_t n = 0;
  while (chatKey && n <= kChatKeyMax && chatKey[n]) ++n;
  if (!alloc_ || n == 0 || n > kChatKeyMax) return Status::fail(Err::InvalidArgument, "tx::TxArenaPool::acquire");
  int at = slotOf(chatKey);
  if (at < 0) {
    Slot& s = slots_[victim()];
    if (!s.ready) {
      const Status st = s.arena.init(perChat_, alloc_, release_);
      if (!st.ok()) return st;
      s.ready = true;
    }
    s.arena.clear();
    std::memcpy(s.key, chatKey, n + 1);
    fresh = true;
    at = static_cast<int>(&s - slots_);
  }
  slots_[at].lastUse = ++tick_;
  out = &slots_[at].arena;
  return Status::success();
}

Status TxArenaPool::forget(const char* chatKey) {
  const int at = chatKey ? slotOf(chatKey) : -1;
  if (at < 0) return Status::fail(Err::NotFound, "tx::TxArenaPool::forget");
  slots_[at].key[0] = '\0';
  slots_[at].arena.clear();
  return Status::success();
}

bool TxArenaPool::resident(const char* chatKey) const { return chatKey && slotOf(chatKey) >= 0; }

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
