#include "nimbus/speaker_memory.h"

namespace nimbus {
namespace sfx {

bool CueGate::allow(SoundKind kind, uint32_t dmaFree, uint32_t dmaLargest, uint32_t nowMs) {
  if (kind == SoundKind::Asked) return speakerMemoryOk(kind, dmaFree, dmaLargest);
  const bool holding = holdMs_ != 0 && (uint32_t)(nowMs - holdFrom_) < holdMs_;
  if (!holding && speakerMemoryOk(kind, dmaFree, dmaLargest)) return true;
  ++skipped_;
  return false;
}

void CueGate::finished(bool played, bool allocFailed, uint32_t nowMs) {
  if (played) {
    holdMs_ = 0;
    return;
  }
  if (!allocFailed) return;
  holdMs_ = holdMs_ == 0 ? kHoldFirstMs : (holdMs_ >= kHoldMaxMs / 2 ? kHoldMaxMs : holdMs_ * 2);
  holdFrom_ = nowMs;
}

bool CueGate::logDue(uint32_t nowMs) {
  if (skipped_ == 0) return false;
  if (logged_ && (uint32_t)(nowMs - lastLog_) < kLogEveryMs) return false;
  logged_ = true;
  lastLog_ = nowMs;
  return true;
}

}  // namespace sfx
}  // namespace nimbus
