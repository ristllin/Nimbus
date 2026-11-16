#pragma once
// ContextBudget - per-turn derived context allocations (owner ask 2026-08-05:
// "caps shouldn't be hardcoded - derived from the context length allowed for
// the task"). Every byte allocation that used to be an absolute constant is a
// RATIO of the head model's context window, with a floor and a cap; owner NVS
// overrides always win verbatim (then clamp to the same ranges the existing
// NVS clamps enforce).
//
// ANCHOR INVARIANT (pinned by test_orch_budget): deriveBudget(200000, {})
// reproduces the shipped constants EXACTLY - 32768 system prompt / 4096
// summary / 8192 recent tail (CUM-485: was 3000, and a dead knob - the device
// used a fixed constant) / 8192 per-result / 65536 tool total / 1200 brief /
// 65536 fold slice. Ratios below are each constant over the 200K window's byte
// estimate (200000 * 4 = 800000 B).
//
// ROW COUNTS deliberately stay fixed constants (kRecentTurnsMax=12, brief 6
// rows, fold 400 rows): their cost is internal-SRAM row overhead (many SMALL
// strings under the 128 B extmem-spill threshold), not window size.
//
// Arduino-free, host-tested. Window resolution reuses modelCtxTokens()
// (model_window.h) - no second table.
//
// ONE SEAM (lane P0 of the history fix, 16 Nov 2026): every consumer of the head's
// budget - the turn's caps, the history window, the fold slice, the results.get view
// cap, the web console's auto values, the device's brief and the VN's fold slice -
// reads headContextBudget(host, model, overrides), never deriveBudget directly
// (test_orch_budget pins it). Its body is today's derivation; the history fix changes
// the body, and every consumer follows at once.

#include <cstddef>
#include <cstdint>
#include <string>

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
  size_t   recentConvBytes = 8192;     // verbatim recent-window byte cap (CUM-485)
  size_t   toolResultBytes = 8192;     // per-tool-result clamp
  size_t   toolTotalBytes = 65536;     // cumulative tool-output budget
  size_t   briefBytes = 1200;          // sub-agent brief digest bytes
  size_t   foldSliceBytes = 65536;     // compaction fold digest slice bytes
  // The history fix's fields (lane P0). Each holds the figure the code applies today,
  // so a reader of the budget sees what the device does; nothing reads them yet.
  size_t   historyMaxBytes = 8192;     // the verbatim history's high water (recentConvBytes)
  size_t   historySoftBytes = 4096;    // where the overflow shift lands (half of high water)
  size_t   historyKeepBytes = 4096;    // the verbatim tail a fold keeps (half of high water)
  size_t   historyRows = 64;           // the history's row bound (HistoryBudget::highRows)
  size_t   replayBytes = 16384;        // the tool-round replay cap every wire keeps at least
  size_t   foldStepBytes = 65536;      // what one fold takes at most (foldSliceBytes)
  size_t   perTurnFloorBytes = 0;      // bytes held back for the per-turn block (none today)
  size_t   historyMessageCap = 6144;   // one history message's clip (kHistoryMessageCap)
  size_t   recallBytes = 32768;        // the recall block's bound: the system budget today
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

// The budget of a request the head `host` sends on `model`: the one seam above.
// Today's body: deriveBudget(modelCtxTokens(host, model), ov).
ContextBudget headContextBudget(const std::string& host, const std::string& model, const BudgetOverrides& ov);

}  // namespace orch
}  // namespace nimbus
