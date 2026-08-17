#include "nimbus/orch/budget.h"

#include <algorithm>

#include "nimbus/orch/compact.h"     // kCtxDefaultTokens + the modelCtxTokens convention
#include "nimbus/orch/mem_config.h"  // MemConfig::kContextMin/Max - the owner-override clamp

namespace nimbus {
namespace orch {

namespace {

// The 200K-anchor window in bytes: every ratio is <anchor constant> / this, so
// scaling is exact integer math and deriveBudget(200000,{}) is the identity.
constexpr uint64_t kAnchorBytes = 200000ull * kBudgetBytesPerToken;

// value = anchorConst * windowBytes / kAnchorBytes, clamped. Linear + clamp =>
// monotonic in the window size (pinned by test_orch_budget).
size_t scaled(uint64_t anchorConst, uint64_t windowBytes, size_t floor_, size_t cap) {
  uint64_t v = anchorConst * windowBytes / kAnchorBytes;
  if (v < floor_) v = floor_;
  if (v > cap) v = cap;
  return (size_t)v;
}

int clampI(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

ContextBudget deriveBudget(uint32_t ctxTokens, const BudgetOverrides& ov) {
  ContextBudget b;
  if (ctxTokens == 0) ctxTokens = kCtxDefaultTokens;  // conservative, same as compaction
  b.ctxTokens = ctxTokens;
  const uint64_t wb = (uint64_t)ctxTokens * kBudgetBytesPerToken;

  // Floors keep a small-window model functional; caps repeat the existing NVS
  // clamp ceilings (store.cpp / MemConfig) so derive can never exceed what an
  // owner could set by hand.
  b.systemPromptBytes = (int)scaled(32768, wb, 8192, (size_t)MemConfig::kContextMax);
  b.chatSummaryBytes  = scaled(4096, wb, 2048, 8192);
  b.recentConvBytes   = scaled(3000, wb, 1500, 8192);
  b.toolResultBytes   = scaled(8192, wb, 512, 65536);
  b.toolTotalBytes    = scaled(65536, wb, 8192, 1048576);
  b.briefBytes        = scaled(1200, wb, 600, 4096);
  b.foldSliceBytes    = scaled(65536, wb, 16384, 131072);

  // Owner overrides win verbatim, then clamp to the same ranges the NVS
  // setters enforce - web/NVS and derive can never disagree about the range.
  if (ov.maxContextBytes > 0)
    b.systemPromptBytes = clampI(ov.maxContextBytes, MemConfig::kContextMin, MemConfig::kContextMax);
  if (ov.toolResultCap > 0)
    b.toolResultBytes = (size_t)clampI(ov.toolResultCap, 512, 65536);
  if (ov.toolTotalCap > 0)
    b.toolTotalBytes = (size_t)clampI(ov.toolTotalCap, 2048, 1048576);
  return b;
}

}  // namespace orch
}  // namespace nimbus
