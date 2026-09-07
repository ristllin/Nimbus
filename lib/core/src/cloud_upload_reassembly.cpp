#include "nimbus/cloud/upload_reassembly.h"

namespace nimbus {
namespace cloud {

void UploadReassembly::begin(const std::string& id, uint32_t totalLen, uint32_t cap) {
  status_ = Status::Active;
  id_ = id;
  totalLen_ = totalLen;
  cap_ = cap;
  received_ = 0;
  expectedSeq_ = 0;
}

UploadReassembly::ChunkOutcome UploadReassembly::chunk(
    const std::string& id, uint32_t seq, uint32_t off, const uint8_t* data, size_t len,
    const std::function<bool(const uint8_t*, size_t)>& sink) {
  if (status_ != Status::Active) return ChunkOutcome::NotActive;
  if (id != id_) return ChunkOutcome::WrongId;
  // Strict in-order, gapless: both the sequence number and the byte offset must be the
  // next expected value. A gap or a replay is refused rather than silently mis-assembled.
  if (seq != expectedSeq_ || off != received_) return ChunkOutcome::OutOfOrder;
  // 64-bit math so a near-UINT32_MAX offset plus a chunk length cannot wrap.
  const uint64_t end = static_cast<uint64_t>(received_) + len;
  if (end > cap_) {
    status_ = Status::Failed;
    return ChunkOutcome::OverCap;
  }
  if (totalLen_ != 0 && end > totalLen_) {
    status_ = Status::Failed;
    return ChunkOutcome::OverTotal;
  }
  if (len && !sink(data, len)) {
    status_ = Status::Failed;
    return ChunkOutcome::SinkFailed;
  }
  received_ = static_cast<uint32_t>(end);
  expectedSeq_++;
  return ChunkOutcome::Accepted;
}

bool UploadReassembly::end(const std::string& id) {
  if (status_ != Status::Active || id != id_) return false;
  if (totalLen_ != 0 && received_ != totalLen_) {
    status_ = Status::Failed;
    return false;
  }
  status_ = Status::Done;
  return true;
}

void UploadReassembly::abort() {
  status_ = Status::Idle;
  id_.clear();
  totalLen_ = 0;
  cap_ = 0;
  received_ = 0;
  expectedSeq_ = 0;
}

}  // namespace cloud
}  // namespace nimbus
