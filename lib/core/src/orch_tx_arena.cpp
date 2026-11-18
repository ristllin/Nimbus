// The transcript arena (tx/arena.h). Stub bodies from lane RB-B0a: each answers
// Err::NotImplemented until lane RB-T1 writes the arena. (The destructor is inline in
// arena.h; see the note there.)
#include "nimbus/orch/tx/arena.h"

namespace nimbus {
namespace orch {
namespace tx {

Status TxArena::init(size_t, ArenaAllocFn, ArenaFreeFn) { return Status::pending("tx::TxArena::init"); }
Status TxArena::append(const Item&) { return Status::pending("tx::TxArena::append"); }
Status TxArena::get(size_t, Item&) const { return Status::pending("tx::TxArena::get"); }
Status TxArena::find(uint64_t, size_t&) const { return Status::pending("tx::TxArena::find"); }
Status TxArena::dropThrough(uint64_t) { return Status::pending("tx::TxArena::dropThrough"); }
void TxArena::clear() {
  used_ = 0;
  count_ = 0;
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
