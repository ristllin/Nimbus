#pragma once
#include <cstdint>
#include <string>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"

// plain/truth.h - the truth check bound into the plain engine (PRD section 8.8): the
// probe reads recent receipts from the transcript; in enforce mode it holds before
// delivery (beforeDeliver, HookSlot::Truth) and never after; in shadow mode it runs from
// the idle step (HookSlot::ShadowProbe) and writes counters only. Nothing is ever sent
// as a correction.
//
// Seam declared by lane RB-B0a; the body (plain_truth.cpp) is lane RB-G1's.
namespace agent {
namespace plain {

enum class TruthMode : uint8_t { Off = 0, ReceiptOnly = 1, Shadow = 2, Enforce = 3 };
inline const char* truthModeSlug(TruthMode m) {
  switch (m) {
    case TruthMode::Off: return "off";
    case TruthMode::ReceiptOnly: return "receipt";
    case TruthMode::Shadow: return "shadow";
    case TruthMode::Enforce: return "enforce";
  }
  return "off";
}

inline constexpr uint32_t kProbeTimeoutMs = 8000;

struct TruthDeps {
  TruthMode mode = TruthMode::ReceiptOnly;
};

Status installTruth(PlainHooks& hooks, const TruthDeps& deps);

}  // namespace plain
}  // namespace agent
