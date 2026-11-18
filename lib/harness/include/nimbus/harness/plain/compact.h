#pragma once
#include <cstdint>
#include <string>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"

namespace agent {
class HttpTransport;
}  // namespace agent
namespace nimbus {
namespace orch {
namespace tx {
class TxLog;
class TxArena;
}  // namespace tx
}  // namespace orch
}  // namespace nimbus

// plain/compact.h - compaction (PRD section 3) on main's FoldDriver (ladder, backoff,
// daily cap, owner-first deferral kept). It runs only from the idle step (HookSlot::
// Compaction): no owner message waiting and at least 120 s of quiet, or 03:00 to 05:00
// local when resident is above 0.8 H. Never inside a turn.
//
// Seam declared by lane RB-B0a; the body (plain_compact.cpp) is lane RB-C2's.
namespace agent {
namespace plain {

inline constexpr uint32_t kCompactQuietS = 120;
inline constexpr uint32_t kCompactSliceMaxBytes = 24576;
inline constexpr uint8_t kCompactRequestsPerWindow = 2;
inline constexpr uint32_t kCompactRequestMs = 45000;
inline constexpr uint16_t kSummarizerMaxTokens = 400;

struct CompactDeps {
  HttpTransport* transport = nullptr;
  nimbus::orch::tx::TxLog* log = nullptr;
  nimbus::orch::tx::TxArena* arena = nullptr;
};

struct CompactOutcome {
  bool ran = false;
  uint32_t turnsFolded = 0;
  uint32_t digests = 0;
  uint8_t requests = 0;
};

// One slice: whole turns from thru forward until the rest is at or under L.
Status compactSlice(const CompactDeps& deps, const IdleCtx& ctx, CompactOutcome& out);
Status installCompaction(PlainHooks& hooks, const CompactDeps& deps);

}  // namespace plain
}  // namespace agent
