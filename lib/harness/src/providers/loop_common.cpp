#include "loop_common.h"

#include "nimbus/harness/log.h"
#include "wire.h"   // wire::lastExchange - did the failed request ever leave the device
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/transcript.h"

namespace agent {
namespace providers {

namespace orch = nimbus::orch;

// The fabric's host vocabulary - true iff makeStep() below can build a step for
// this host. The engine gates fabricOn + filters the hostList on this, so a
// CUSTOM head drops to the legacy single-shot path instead of failing "unknown
// host custom" with zero HTTP, and custom inside providerPriority no longer
// truncates a real failover ladder mid-switch.
bool fabricSupports(const std::string& host) {
  return host == "anthropic" || host == "openai" || host == "mistral";
}

// ---- CUM-477: one same-host resend for a request that never left -------------

namespace {
// How long the turn task may wait for the internal heap to come back after a short
// write (the failed connection's buffers are already freed; lwIP gives the rest
// back within a few hundred ms) - bounded so a starved device fails honestly.
constexpr uint32_t kHeapRecoverMs = 3000;
constexpr uint32_t kHeapPollMs = 100;

bool heapBelowFloor(const ProviderDeps& pd, const orch::HeadLoopConfig& cfg) {
  if (cfg.roundMinHeap && pd.freeHeap && pd.freeHeap() < cfg.roundMinHeap) return true;
  return cfg.roundMinLargest && pd.largestBlock && pd.largestBlock() < cfg.roundMinLargest;
}

// True once the heap is back past the round floor, waiting (bounded) when a delay
// seam exists. A transport "low memory" tag gets one settle pause even when the
// totals look fine at this instant.
bool heapRecovered(const ProviderDeps& pd, const orch::HeadLoopConfig& cfg, bool taggedLow) {
  if (taggedLow && pd.delayMs) pd.delayMs(kHeapPollMs);
  for (uint32_t waited = 0; heapBelowFloor(pd, cfg); waited += kHeapPollMs) {
    if (!pd.delayMs || waited >= kHeapRecoverMs) return false;
    pd.delayMs(kHeapPollMs);
  }
  return true;
}

// The error for a request that still could not be sent: memory when the heap is
// short (or the transport said so), else the plain network failure it was.
void markUnsent(orch::HeadStep& sr, const ProviderDeps& pd, const orch::HeadLoopConfig& cfg) {
  if (heapBelowFloor(pd, cfg) || wire::lastExchange().lowMem)
    sr.error = "low memory: the device could not send the request";
}
}  // namespace

orch::HeadStepFn resendUnsentOnce(const ProviderDeps& pd, const orch::HeadLoopConfig& cfg,
                                  orch::HeadStepFn inner, const char* who) {
  return [pd, cfg, inner, who](bool allowTools, const std::vector<orch::HeadToolResult>& prior,
                               uint32_t budgetMs, const std::string& capReason) -> orch::HeadStep {
    orch::HeadStep sr = inner(allowTools, prior, budgetMs, capReason);
    if (sr.ok || !wire::lastExchange().unsent) return sr;
    const unsigned heapAt = pd.freeHeap ? (unsigned)pd.freeHeap() : 0u;
    if (!heapRecovered(pd, cfg, wire::lastExchange().lowMem)) {
      hlog::logf("%s: request not sent and the heap did not recover (heap %u) - no resend",
                 who, heapAt);
      sr.error = "low memory: the device could not send the request";
      return sr;
    }
    hlog::logf("%s: request not sent (heap %u) - one same-host resend after the heap "
               "recovered (heap %u)",
               who, heapAt, pd.freeHeap ? (unsigned)pd.freeHeap() : 0u);
    orch::HeadStep sr2 = inner(allowTools, prior, budgetMs, capReason);
    if (!sr2.ok && wire::lastExchange().unsent) markUnsent(sr2, pd, cfg);
    return sr2;
  };
}

bool runFabricLoop(const ProviderDeps& pd, const std::vector<std::string>& hostList,
                   const std::string& instructions, const std::string& inputs,
                   std::string& outJson, std::string& err, const HeadTools& ht,
                   nimbus::orch::TokenUsage* usage, const FabricNotify& notify) {
  if (hostList.empty()) { err = "no runnable host"; return false; }

  // ONE canonical transcript for the whole turn, whichever provider serves a
  // given round. The controller records into it (hooks.transcript); every
  // host's step renders its wire from it.
  orch::Transcript tr;
  tr.addUser(inputs);

  // Per-host usage split (logged); `usage` still receives the summed total -
  // every factory adds into the same counter.
  // ⚠ Keep in sync with fabricSupports() below - a host listed here but not
  // there is silently excluded from fabric turns; the reverse fails the turn
  // with "unknown host" before any HTTP (the 2026-08-12 custom-head regression).
  auto makeStep = [&](const std::string& h) -> orch::HeadStepFn {
    if (h == "anthropic") return antLoopStep(pd, instructions, ht, usage, tr);
    if (h == "openai")    return oaiLoopStep(pd, instructions, ht, usage, tr);
    if (h == "mistral")   return misLoopStep(pd, instructions, ht, usage, tr);
    return nullptr;
  };

  size_t hostIdx = 0;
  int    switches = 0;
  const int kMaxSwitches = 2;
  orch::HeadStepFn cur = makeStep(hostList[0]);
  if (!cur) { err = "unknown host " + hostList[0]; return false; }

  // The wrapper IS the controller's step: retry/failover happens INSIDE one
  // logical round, so runHeadLoop's caps and recording are provider-agnostic.
  orch::HeadStepFn step = [&](bool allowTools,
                              const std::vector<orch::HeadToolResult>& prior,
                              uint32_t budgetMs,
                              const std::string& capReason) -> orch::HeadStep {
    for (;;) {
      orch::HeadStep sr = cur(allowTools, prior, budgetMs, capReason);
      if (sr.ok) return sr;
      // Device-side contract failure - identical on every host; do not burn
      // switches on it.
      if (sr.error == "schema parse") return sr;
      hlog::logf("fabric: %s step failed (%s) - same-host retry",
                 hostList[hostIdx].c_str(), sr.error.c_str());
      orch::HeadStep sr2 = cur(allowTools, prior, budgetMs, capReason);
      if (sr2.ok) return sr2;
      if (switches >= kMaxSwitches || hostIdx + 1 >= hostList.size())
        return sr2;   // exhausted - the controller fails soft to the caller
      const std::string from = hostList[hostIdx];
      const std::string to   = hostList[hostIdx + 1];
      orch::HeadStepFn next = makeStep(to);
      if (!next) return sr2;
      ++hostIdx;
      ++switches;
      hlog::logf("fabric: switching %s -> %s (round re-runs on the shared transcript; "
                 "%u tool-result bytes carry over)",
                 from.c_str(), to.c_str(), (unsigned)tr.toolBytes());
      if (notify) notify(from, to);
      cur = next;
      // loop: the SAME round re-runs on the new host. The transcript already
      // holds every executed tool result - nothing re-dispatches.
    }
  };

  orch::HeadLoopHooks hooks;
  hooks.step     = step;
  hooks.dispatch = ht.dispatch;
  hooks.nowMs    = pd.nowMs;
  hooks.freeHeap = pd.freeHeap;
  hooks.largestBlock = pd.largestBlock;
  hooks.log      = [](const std::string& m) { hlog::logf("%s", m.c_str()); };
  hooks.onText   = ht.onRoundText;
  hooks.spill    = ht.spill;
  hooks.transcript = &tr;

  orch::HeadOutcome res = runHeadLoop(ht.cfg, hooks);
  hlog::logf("fabric-loop: rounds=%d cap=%s ok=%d host=%s switches=%d heap=%u",
             res.rounds, res.capReason.c_str(), (int)res.ok,
             hostList[hostIdx].c_str(), switches,
             (unsigned)(pd.freeHeap ? pd.freeHeap() : 0));
  // Glass Box P3: hand the canonical transcript to the engine BEFORE the
  // failure return - a failed turn's middle is exactly what needs debugging.
  if (ht.onBrief) ht.onBrief(tr.renderBrief(kHeadBriefMax));
  if (!res.ok) { err = res.error.empty() ? std::string("loop failed") : res.error; return false; }
  outJson = res.finalTurn;
  return true;
}

}  // namespace providers
}  // namespace agent
