#include "nimbus/orch/reset_plan.h"

namespace nimbus {
namespace orch {

const char* resetStepStr(ResetStep s) {
  switch (s) {
    case ResetStep::ResetNvs:        return "reset-nvs";
    case ResetStep::EraseCoreDump:   return "erase-coredump";
    case ResetStep::FinalSweep:      return "final-sweep";
    case ResetStep::ScrubInternalFs: return "scrub-internal-fs";
    case ResetStep::Restart:         return "restart";
    case ResetStep::Count:           break;
  }
  return "?";
}

bool resetStepMayStop(ResetStep s) { return s == ResetStep::ScrubInternalFs; }

// FULL reset (order and reasons: reset_plan.h): the internal file area erase
// first, then the settings and the crash dump, then the card's final sweep.
static constexpr ResetStep kFullTail[] = {
    ResetStep::ScrubInternalFs, ResetStep::ResetNvs, ResetStep::EraseCoreDump,
    ResetStep::FinalSweep,      ResetStep::Restart,
};
// Settings-only reset: unchanged behavior (NVS identity reset, crash dump, restart).
static constexpr ResetStep kSettingsTail[] = {
    ResetStep::ResetNvs, ResetStep::EraseCoreDump, ResetStep::Restart,
};

ResetPlan factoryResetTail(bool eraseData) {
  if (eraseData) return {kFullTail, sizeof(kFullTail) / sizeof(kFullTail[0])};
  return {kSettingsTail, sizeof(kSettingsTail) / sizeof(kSettingsTail[0])};
}

uint32_t scrubChunkCount(uint32_t partSize) {
  return partSize / kScrubChunkBytes + (partSize % kScrubChunkBytes ? 1u : 0u);
}

bool scrubChunk(uint32_t partSize, uint32_t index, uint32_t& offset, uint32_t& size) {
  if (index >= scrubChunkCount(partSize)) return false;
  offset = index * kScrubChunkBytes;
  const uint32_t left = partSize - offset;
  size = left < kScrubChunkBytes ? left : kScrubChunkBytes;
  return true;
}

}  // namespace orch
}  // namespace nimbus
