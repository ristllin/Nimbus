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

// The resend waits for a margin ABOVE the round floor: the failed round was
// admitted at the floor, so resending at exactly the floor would just repeat it.
constexpr uint32_t kResendHeapMargin = 4096;

// Total free internal heap under the resend threshold (the round floor + margin).
bool totalBelowResendFloor(const ProviderDeps& pd, const orch::HeadLoopConfig& cfg) {
  return cfg.roundMinHeap && pd.freeHeap && pd.freeHeap() < cfg.roundMinHeap + kResendHeapMargin;
}

bool heapBelowFloor(const ProviderDeps& pd, const orch::HeadLoopConfig& cfg) {
  if (totalBelowResendFloor(pd, cfg)) return true;
  return cfg.roundMinLargest && pd.largestBlock && pd.largestBlock() < cfg.roundMinLargest;
}

// Whether the one resend may go, and if not, why - the reported error follows the
// reason (CUM-477). `StayedLow`: total free stayed under the resend threshold on
// every reading of the whole bounded wait - memory. `Blocked`: the wait ran out on
// anything else (the largest block alone, which a Freenove pins near its floor, or
// a total that recovered) - not memory. `NoWait`: there was no way to wait, so the
// readings at the failure are the only evidence (the failed connection's buffers
// may not be back yet).
enum class Resend : uint8_t { Go, StayedLow, Blocked, NoWait };

// Go once the heap is back past the resend thresholds, waiting (bounded) when a
// delay seam exists. A transport "low memory" tag gets one settle pause even when
// the totals look fine at this instant.
Resend resendVerdict(const ProviderDeps& pd, const orch::HeadLoopConfig& cfg, bool taggedLow) {
  if (taggedLow && pd.delayMs) pd.delayMs(kHeapPollMs);
  bool totalLowThroughout = true;   // one healthy total reading means the total recovered
  for (uint32_t waited = 0; heapBelowFloor(pd, cfg); waited += kHeapPollMs) {
    if (!pd.delayMs) return Resend::NoWait;
    totalLowThroughout = totalLowThroughout && totalBelowResendFloor(pd, cfg);
    if (waited >= kHeapRecoverMs) return totalLowThroughout ? Resend::StayedLow : Resend::Blocked;
    pd.delayMs(kHeapPollMs);
  }
  return Resend::Go;
}

constexpr const char* kUnsentLowMem = "low memory: the device could not send the request";

// Memory - not the network - is the reason a send failed: total free heap is below
// the round floor. Neither the largest block nor the transport's starved-start tag
// alone counts: a Freenove board idles near the block floor, and CUM-404 v2 forbids
// turning its genuine outages into fake low-memory failures (which skip failover).
// A total that stayed low for the whole wait is Resend::StayedLow, read at the call
// site.
bool memoryShort(const ProviderDeps& pd, const orch::HeadLoopConfig& cfg) {
  return cfg.roundMinHeap && pd.freeHeap && pd.freeHeap() < cfg.roundMinHeap;
}

// What is left of a round's budget after `t0` (UINT32_MAX = no deadline).
uint32_t budgetLeft(const ProviderDeps& pd, uint32_t budgetMs, uint32_t t0) {
  if (budgetMs == UINT32_MAX || !pd.nowMs) return budgetMs;
  const uint32_t used = pd.nowMs() - t0;
  return used >= budgetMs ? 0 : budgetMs - used;
}

unsigned heapNow(const ProviderDeps& pd) { return pd.freeHeap ? (unsigned)pd.freeHeap() : 0u; }
}  // namespace

orch::HeadStepFn resendUnsentOnce(const ResendCtx* ctx) {
  // One pointer: the closure fits std::function's inline storage, so wrapping the
  // step allocates nothing on the turn path (copying ProviderDeps would copy
  // ~20 std::function members per turn).
  return [ctx](bool allowTools, const std::vector<orch::HeadToolResult>& prior,
               uint32_t budgetMs, const std::string& capReason) -> orch::HeadStep {
    const ProviderDeps& pd = *ctx->pd;
    const uint32_t t0 = pd.nowMs ? pd.nowMs() : 0;
    ctx->settled = false;
    orch::HeadStep sr = (*ctx->inner)(allowTools, prior, budgetMs, capReason);
    if (sr.ok || !wire::lastExchange().unsent) return sr;
    ctx->settled = true;
    const unsigned heapAt = heapNow(pd);
    const bool shortAtFailure = memoryShort(pd, *ctx->cfg);
    const Resend verdict = resendVerdict(pd, *ctx->cfg, wire::lastExchange().lowMem);
    if (verdict != Resend::Go) {
      hlog::logf("%s: request not sent and the heap did not recover (heap %u) - no resend",
                 ctx->who, heapAt);
      // The error names the reason the resend was refused. After a wait the verdict
      // is the one source: total free that stayed under the resend threshold for the
      // whole wait is memory, anything else (the largest block alone, a total that
      // recovered) stays "network" so failover still runs (CUM-404 v2). With no way
      // to wait, a total below the round floor is the only evidence there is.
      if (verdict == Resend::StayedLow ||
          (verdict == Resend::NoWait && (shortAtFailure || memoryShort(pd, *ctx->cfg))))
        sr.error = kUnsentLowMem;
      return sr;
    }
    hlog::logf("%s: request not sent (heap %u) - one same-host resend after the heap "
               "recovered (heap %u)",
               ctx->who, heapAt, heapNow(pd));
    // The resend runs on what is left of THIS round's budget (F25: a round never
    // gets a fresh full budget), not a second whole one.
    orch::HeadStep sr2 =
        (*ctx->inner)(allowTools, prior, budgetLeft(pd, budgetMs, t0), capReason);
    // Still unsent: memory when it is short, else the plain network failure it was.
    if (!sr2.ok && wire::lastExchange().unsent && memoryShort(pd, *ctx->cfg))
      sr2.error = kUnsentLowMem;
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
  // CUM-477: each host's step is wrapped like the per-provider loops' - a request
  // the device could not finish sending is resent once on the same host after a
  // bounded wait for the heap, and reads as low memory when the heap never comes
  // back - so the fabric's own retry and any switch come after that wait. One
  // context per host, sized once (never reallocated) and alive for the whole loop:
  // each wrapped step holds only its pointer.
  std::vector<orch::HeadStepFn> inner(hostList.size());
  std::vector<ResendCtx> resend(hostList.size());
  auto wrap = [&](size_t i, orch::HeadStepFn step, const char* who) -> orch::HeadStepFn {
    inner[i] = std::move(step);
    resend[i] = ResendCtx{&pd, &ht.cfg, &inner[i], who};
    return resendUnsentOnce(&resend[i]);
  };
  auto makeStep = [&](size_t i) -> orch::HeadStepFn {
    const std::string& h = hostList[i];
    if (h == "anthropic") return wrap(i, antLoopStep(pd, instructions, ht, usage, tr), "orchTurn(ant-loop)");
    if (h == "openai")    return wrap(i, oaiLoopStep(pd, instructions, ht, usage, tr), "orchTurn(oai-loop)");
    if (h == "mistral")   return wrap(i, misLoopStep(pd, instructions, ht, usage, tr), "orchTurn(mistral-loop)");
    return nullptr;
  };

  size_t hostIdx = 0;
  int    switches = 0;
  const int kMaxSwitches = 2;
  orch::HeadStepFn cur = makeStep(0);
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
      // switches on it. Nor on memory: a request the device could not make or
      // send for want of heap fails the same way on any host (CUM-404, CUM-477),
      // and the engine answers it with the memory reply.
      if (sr.error == "schema parse" || sr.error.rfind("low memory", 0) == 0) return sr;
      // A request that was not sent got its one same-host resend (or its memory
      // verdict) in the wrapper already: it is not sent a third time here.
      orch::HeadStep sr2 = sr;
      if (!resend[hostIdx].settled) {
        hlog::logf("fabric: %s step failed (%s) - same-host retry",
                   hostList[hostIdx].c_str(), sr.error.c_str());
        sr2 = cur(allowTools, prior, budgetMs, capReason);
        if (sr2.ok) return sr2;
      }
      if (switches >= kMaxSwitches || hostIdx + 1 >= hostList.size())
        return sr2;   // exhausted - the controller fails soft to the caller
      const std::string from = hostList[hostIdx];
      const std::string to   = hostList[hostIdx + 1];
      orch::HeadStepFn next = makeStep(hostIdx + 1);
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
