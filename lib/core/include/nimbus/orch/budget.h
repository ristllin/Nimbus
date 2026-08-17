#pragma once
// ContextBudget - per-turn derived context allocations (owner ask 2026-08-05:
// "caps shouldn't be hardcoded - derived from the context length allowed for
// the task"). Every byte allocation that used to be an absolute constant is a
// RATIO of the head model's context window, with a floor and a cap; owner NVS
// overrides always win verbatim (then clamp to the same ranges the existing
// NVS clamps enforce).
//
// ANCHOR INVARIANT (pinned by test_orch_budget): deriveBudget(200000, {})
// reproduces today's shipped constants EXACTLY - 32768 system prompt / 4096
// summary / 3000 recent tail / 8192 per-result / 65536 tool total / 1200 brief
// / 65536 fold slice - so behavior on the current fleet (Anthropic 200K head)
// is byte-identical until a different window is configured. Ratios below are
// each constant over the 200K window's byte estimate (200000 * 4 = 800000 B).
//
// ROW COUNTS deliberately stay fixed constants (kRecentTurnsMax=12, brief 6
// rows, fold 400 rows): their cost is internal-SRAM row overhead (many SMALL
// strings under the 128 B extmem-spill threshold), not window size.
//
// Arduino-free, host-tested. Window resolution reuses modelCtxTokens()
// (compact.h) - no second table.

#include <cstddef>
#include <cstdint>

namespace nimbus {
namespace orch {

// Provider-usage-anchored byte estimate (no client tokenizer on-device; same
// convention as compaction - see docs/compaction.md).
constexpr uint32_t kBudgetBytesPerToken = 4;

// All per-turn derived allocations. Defaults are the 200K anchor values.
struct ContextBudget {
  uint32_t ctxTokens = 200000;         // resolved window this was derived from
  int      systemPromptBytes = 32768;  // assembleContext budget
  size_t   chatSummaryBytes = 4096;    // anchored CONVERSATION SUMMARY cap
  size_t   recentConvBytes = 3000;     // verbatim recent-tail byte cap
  size_t   toolResultBytes = 8192;     // per-tool-result clamp
  size_t   toolTotalBytes = 65536;     // cumulative tool-output budget
  size_t   briefBytes = 1200;          // sub-agent brief digest bytes
  size_t   foldSliceBytes = 65536;     // compaction fold digest slice bytes
};

// 0 == "auto/derive". Non-zero == owner override, wins verbatim then clamps.
struct BudgetOverrides {
  int maxContextBytes = 0;  // MemConfig.maxContextBytes (0 = auto)
  int toolResultCap = 0;    // NVS orchLoopRCap (0 = key absent = auto)
  int toolTotalCap = 0;     // NVS orchLoopTCap (0 = key absent = auto)
};

// Derive every allocation from the window. ctxTokens==0 falls back to the
// conservative compaction default window. Monotonic: a bigger window never
// shrinks an allocation (floors/caps preserve that).
ContextBudget deriveBudget(uint32_t ctxTokens, const BudgetOverrides& ov);

}  // namespace orch
}  // namespace nimbus
