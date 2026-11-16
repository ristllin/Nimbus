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

#include "nimbus/orch/request_budget.h"

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
  // Lane B: the fold trigger (bytes of history after thru that make a fold due) and
  // whether the history follows v5.1.1 (the floor branch: today's window, the 48 KB
  // compactAtKB trigger). deriveBudget leaves both at v5.1.1's.
  size_t   foldTriggerBytes = 48 * 1024;
  bool     floorBranch = true;
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

// ---- lane B: every field from the one request budget (plan v3 protocols) -------------
// The stored compactAtKB default (store.cpp): it means "derived".
inline constexpr uint32_t kCompactAtKbDefault = 48;
// No automatic fold trigger goes under this (RULINGS-v3 item 3).
inline constexpr size_t kFoldTriggerMinBytes = 24 * 1024;

// Every ContextBudget field derived from `rb` (requestBudgetFor), each at least its v5.1.1
// value for the same head (deriveBudget over rb.v511CtxTokens; budgets_v5_1_1.json), owner
// overrides verbatim as deriveBudget applies them:
//   system cap   max(32,768, 12% of the body bytes B), at most 56,000;
//   summary      6,144 at a 60k-token target and above, else 4,096;
//   tool result  max(8,192, 3% of B), at most 16,384;
//   recall       3% of B in 4,096-12,288;
//   replay       15% of B in 16,384-65,536, and never under the sending wire's v5.1.1 cap;
//   per turn     8,192;
//   message cap  12,288 at a 60k-token target and above, else 6,144;
//   historyMax   B - (tools 34 KB + system 24 KB + summary + per-turn 8 KB + slack 8 KB)
//                - replay - 5% of B (KB = 1,024 B);
//   soft         0.8 x historyMax (the fold trigger);
//   keep         max(0.5 x historyMax, soft - foldDigestCap(summary));
//   foldStep     soft - keep (one fold's input stays one string under kJsonStrGuard);
//   rows         max(64, historyMax / 192);
//   fold slice   max(v5.1.1's, foldStep).
// The floor branch (RULINGS-v3 item 3): when historyMax is at or below v5.1.1's, or its
// soft mark would fall under kFoldTriggerMinBytes, the history fields are v5.1.1's
// exactly (today's window, the 48 KB compactAtKB trigger) and floorBranch is set.
// recentConvBytes follows historyMaxBytes; toolTotalBytes and briefBytes stay v5.1.1's.
// ctxTokens is the head's real window (rb.windowTokens), not an allocation: a provider list
// that says less than v5.1.1's table (a 32K voxtral) lowers it. foldStepBytes is the one
// new mark (soft - keep) and may sit under v5.1.1's alias of the fold slice.
ContextBudget deriveRequestContext(const RequestBudget& rb, const BudgetOverrides& ov);

// The fold trigger an owner's compactAtKB setting gives: 0 = off (0); the floor branch:
// the setting as v5.1.1 reads it (KB x 1,024); kCompactAtKbDefault: the derived soft mark;
// any other value: that soft mark, clamped to [8 KB, historyMax - 8 KB].
size_t foldTriggerBytes(const ContextBudget& b, uint32_t compactAtKB);

}  // namespace orch
}  // namespace nimbus
