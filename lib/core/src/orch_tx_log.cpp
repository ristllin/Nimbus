// The transcript log (tx/log.h). Stub bodies from lane RB-B0a: each answers
// Err::NotImplemented until lane RB-T2 writes the log.
#include "nimbus/orch/tx/log.h"

namespace nimbus {
namespace orch {
namespace tx {

Status TxLog::open(HeadState&) { return Status::pending("tx::TxLog::open"); }
// A stub stores nothing, so the item is reported as held in RAM.
Status TxLog::append(const Item&, StoreState& state) {
  state = StoreState::NotStored;
  return Status::pending("tx::TxLog::append");
}
Status TxLog::appendBulk(uint32_t, const std::string&, std::string&) { return Status::pending("tx::TxLog::appendBulk"); }
Status TxLog::readBulk(const std::string&, size_t, size_t, std::string&) {
  return Status::pending("tx::TxLog::readBulk");
}
Status TxLog::fill(const HeadState&, TxArena&) { return Status::pending("tx::TxLog::fill"); }
Status TxLog::writeHead(const HeadState&) { return Status::pending("tx::TxLog::writeHead"); }
Status TxLog::rebuildHead(HeadState&) {
  (void)files_;   // the storage the real body reads; held so the seam's shape is final
  return Status::pending("tx::TxLog::rebuildHead");
}

Status txDir(const std::string&, std::string&) { return Status::pending("tx::txDir"); }
Status segmentPath(const std::string&, uint32_t, std::string&) { return Status::pending("tx::segmentPath"); }
Status headPath(const std::string&, std::string&) { return Status::pending("tx::headPath"); }
Status bulkPath(const std::string&, uint32_t, std::string&) { return Status::pending("tx::bulkPath"); }
Status digestArchivePath(const std::string&, const std::string&, std::string&) {
  return Status::pending("tx::digestArchivePath");
}
Status migratedPath(const std::string&, std::string&) { return Status::pending("tx::migratedPath"); }

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
