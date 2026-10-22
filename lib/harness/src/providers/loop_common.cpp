#include "loop_common.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>

#include "nimbus/harness/log.h"
#include "wire.h"   // wire::lastExchange - did the failed request ever leave the device
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/provider_park.h"   // the one per-provider park every seam shares
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
constexpr const char* kUnsentDevice = "unsent: the device could not finish sending its request";

// L19-14: the largest free internal block a resend of a request whose FIRST write
// left 0 bytes waits for. On 19 Oct the device failed slice 0 at a 7,156 B largest
// block with total free above the old resend threshold (16,096 B), resent 1.7 s later
// under the same conditions and failed again, then walked both host switches with
// ever larger bodies. A 4,096 B body slice and its TLS record need room above it.
constexpr uint32_t kUnsentResendLargest = 10240;

// Memory - not the network - is the reason a send failed: total free heap is below
// the round floor. Neither the largest block nor the transport's starved-start tag
// alone counts: a Freenove board idles near the block floor, and CUM-404 v2 forbids
// turning its genuine outages into fake low-memory failures (which skip failover).
// A total that stayed low for the whole wait is Resend::StayedLow, read at the call
// site. (A first write that left 0 bytes is the exception: Unsent, below.)
bool memoryShort(const ProviderDeps& pd, const orch::HeadLoopConfig& cfg) {
  return cfg.roundMinHeap && pd.freeHeap && pd.freeHeap() < cfg.roundMinHeap;
}

// The heap a resend of an unsent-at-start request needs: the largest block at
// kUnsentResendLargest (when the device reports it) and total free past the resend
// threshold.
bool blockRecovered(const ProviderDeps& pd, const orch::HeadLoopConfig& cfg) {
  if (pd.largestBlock && pd.largestBlock() < kUnsentResendLargest) return false;
  return !totalBelowResendFloor(pd, cfg);
}

// Wait (bounded, when a delay seam exists) for blockRecovered.
bool waitForBlock(const ProviderDeps& pd, const orch::HeadLoopConfig& cfg) {
  for (uint32_t waited = 0; !blockRecovered(pd, cfg); waited += kHeapPollMs) {
    if (!pd.delayMs || waited >= kHeapRecoverMs) return false;
    pd.delayMs(kHeapPollMs);
  }
  return true;
}

unsigned largestNow(const ProviderDeps& pd) { return pd.largestBlock ? (unsigned)pd.largestBlock() : 0u; }

// A request the device could not send is the device's fault: the same on every host.
void markUnsent(orch::HeadStep& sr) {
  sr.error = kUnsentDevice;
  sr.kind = orch::StepErr::Unsent;
  sr.httpStatus = 0;
}

// What is left of a round's budget after `t0` (UINT32_MAX = no deadline).
uint32_t budgetLeft(const ProviderDeps& pd, uint32_t budgetMs, uint32_t t0) {
  if (budgetMs == UINT32_MAX || !pd.nowMs) return budgetMs;
  const uint32_t used = pd.nowMs() - t0;
  return used >= budgetMs ? 0 : budgetMs - used;
}

unsigned heapNow(const ProviderDeps& pd) { return pd.freeHeap ? (unsigned)pd.freeHeap() : 0u; }

// A failed step without a typed kind (an older path) is typed from its error line.
void typeStep(orch::HeadStep& sr) {
  if (sr.kind != orch::StepErr::None) return;
  int st = 0;
  sr.kind = orch::classifyStepError(sr.error, &st);
  if (!sr.httpStatus) sr.httpStatus = st;
}
}  // namespace

namespace {
// L19-14: the first write left 0 bytes (the head, or the first body slice). The
// totals at that instant decide nothing (a few hundred bytes flipped the old verdict
// between "network" and "low memory"): it is Unsent, resent at most once and only
// after the largest block recovers, and it never switches host.
orch::HeadStep resendUnsentAtStart(const ResendCtx* ctx, orch::HeadStep sr, bool allowTools,
                                   const std::vector<orch::HeadToolResult>& prior, uint32_t budgetLeftMs,
                                   const std::string& capReason) {
  const ProviderDeps& pd = *ctx->pd;
  const unsigned largestAt = largestNow(pd);
  if (!waitForBlock(pd, *ctx->cfg)) {
    hlog::logf("%s: request not sent (first write, largest block %u) - the block did not recover, "
               "no resend", ctx->who, largestAt);
    markUnsent(sr);
    return sr;
  }
  hlog::logf("%s: request not sent (first write, largest block %u) - one same-host resend after "
             "the block recovered (largest %u)", ctx->who, largestAt, largestNow(pd));
  orch::HeadStep sr2 = (*ctx->inner)(allowTools, prior, budgetLeftMs, capReason);
  if (!sr2.ok && wire::lastExchange().unsent) markUnsent(sr2);
  return sr2;
}
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
    if (wire::lastExchange().unsentAtStart)
      return resendUnsentAtStart(ctx, sr, allowTools, prior, budgetLeft(pd, budgetMs, t0), capReason);
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
          (verdict == Resend::NoWait && (shortAtFailure || memoryShort(pd, *ctx->cfg)))) {
        sr.error = kUnsentLowMem;
        sr.kind = orch::StepErr::LowMemory;
      }
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
    if (!sr2.ok && wire::lastExchange().unsent && memoryShort(pd, *ctx->cfg)) {
      sr2.error = kUnsentLowMem;
      sr2.kind = orch::StepErr::LowMemory;
    }
    return sr2;
  };
}

// ---- the host park: the chat seam of the one per-provider park ------------------
// The fabric's park API (providers.h) is the Chat seam of nimbus::orch::providerPark()
// (provider_park.h): a billing or key refusal learned on ANY seam (embeddings, the
// caption step, a spawn, speech, the download scan) closes the host for chat turns
// too, and a chat refusal closes it for every other seam. The park remembers the
// key it was learned under; a new key is a new start.

namespace {
FabricSwitch g_lastSwitch;

uint32_t nowOf(const ProviderDeps& pd) { return pd.nowMs ? pd.nowMs() : 0; }
uint32_t keyFpOf(const ProviderDeps& pd, const std::string& host) {
  return orch::parkKeyFingerprint(pd.key ? pd.key(host.c_str()) : std::string());
}
orch::StepErr stepKindOf(const orch::ParkVerdict& v) {
  if (v.paused) return orch::StepErr::Paused;
  return v.kind == orch::ParkKind::Billing ? orch::StepErr::Billing : orch::StepErr::Auth;
}
orch::ParkKind parkKindOf(orch::StepErr k) {
  if (k == orch::StepErr::Billing) return orch::ParkKind::Billing;
  if (k == orch::StepErr::Auth) return orch::ParkKind::Auth;
  return orch::ParkKind::None;
}
}  // namespace

bool hostParked(const ProviderDeps& pd, const std::string& host, HostPark* why) {
  const orch::ParkVerdict v =
      orch::providerPark().available(host.c_str(), orch::PaidSeam::Chat, keyFpOf(pd, host), nowOf(pd));
  if (v.probe) hlog::logf("fabric: %s re-check due - one probe request", host.c_str());
  if (v.keyChanged) hlog::logf("fabric: %s un-parked (its key changed)", host.c_str());
  if (v.open) return false;
  if (why) *why = HostPark{stepKindOf(v), v.httpStatus, v.paused};
  return true;
}

void parkHost(const ProviderDeps& pd, const std::string& host, orch::StepErr kind, int httpStatus) {
  orch::providerPark().park(host.c_str(), orch::PaidSeam::Chat, keyFpOf(pd, host), parkKindOf(kind),
                            httpStatus, nowOf(pd));
  hlog::logf("fabric: %s parked (%s, HTTP %d) until its key changes or a re-check succeeds",
             host.c_str(), orch::stepErrSlug(kind), httpStatus);
}

// A failed round on `host`: a billing or auth refusal parks it; any other failure
// of the one PROBE request after the re-check time re-arms the park it already had
// (the refusal it was parked for was never answered otherwise - a 503 or a
// timeout on the probe must not silently un-park it).
void noteHostFailed(const ProviderDeps& pd, const std::string& host, orch::StepErr kind, int httpStatus) {
  if (orch::fabricRuleFor(kind).parkHost) {
    parkHost(pd, host, kind, httpStatus);
    return;
  }
  orch::providerPark().noteFailure(host.c_str(), nowOf(pd));
}

void unparkHost(const std::string& host) {
  if (orch::providerPark().noteSuccess(host.c_str(), orch::PaidSeam::Chat))
    hlog::logf("fabric: %s answered again - un-parked", host.c_str());
}

void clearHostParks() { orch::providerPark().clear(); }

const FabricSwitch& lastFabricSwitch() { return g_lastSwitch; }
void setTestFabricSwitch(const FabricSwitch& sw) { g_lastSwitch = sw; }

// The bench seams exist only in test and host builds: the production image has
// neither the setter nor the check, so nothing can script a provider answer there.
#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)
namespace {
struct TestOverride {
  std::string host;
  int status = 0;
  std::string body;
};
TestOverride g_testOverride;
}  // namespace

bool setTestStatusOverride(const std::string& host, int status, const std::string& body) {
  g_testOverride = TestOverride{host, status, body};
  hlog::logf("provider: TEST override on %s -> HTTP %d", host.c_str(), status);
  return true;
}
bool clearTestStatusOverride() {
  g_testOverride = TestOverride();
  return true;
}
bool testStatusOverride(const std::string& host, int& status, std::string& body) {
  if (g_testOverride.status == 0 || g_testOverride.host != host) return false;
  status = g_testOverride.status;
  body = g_testOverride.body;
  return true;
}
#endif

bool runFabricLoop(const ProviderDeps& pd, const std::vector<std::string>& hostList,
                   const std::string& instructions, const std::string& inputs,
                   std::string& outJson, std::string& err, const HeadTools& ht,
                   nimbus::orch::TurnSpend* spend, const FabricNotify& notify) {
  if (hostList.empty()) { err = "no runnable host"; return false; }

  // ONE canonical transcript for the whole turn, whichever provider serves a
  // given round. The controller records into it (hooks.transcript); every
  // host's step renders its wire from it.
  orch::Transcript tr;
  tr.addUser(inputs);

  // Per-host usage (lane T): each host's step adds into its OWN counter (hostUse),
  // and every host that billed is filed into `spend` once when the loop ends, with
  // the model configured for it - a switched turn never books one host's rounds (or
  // its cache writes) under another, and a failed turn still files what it billed.
  // ⚠ Keep in sync with fabricSupports() below - a host listed here but not
  // there is silently excluded from fabric turns; the reverse fails the turn
  // with "unknown host" before any HTTP (the 2026-08-12 custom-head regression).
  // CUM-477: each host's step is wrapped like the per-provider loops' - a request
  // the device could not finish sending is resent once on the same host after a
  // bounded wait for the heap, and reads as low memory when the heap never comes
  // back - so the fabric's own retry and any switch come after that wait. One
  // context per host in fixed storage sized to the three hosts the fabric speaks
  // (fabricSupports; the engine passes at most three), alive for the whole loop:
  // each wrapped step holds only its pointer, and nothing is allocated for them.
  // A host switched away from drops its step at once (it holds a copy of the deps).
  constexpr size_t kMaxHosts = 3;
  const size_t hostCount = std::min(hostList.size(), kMaxHosts);
  if (hostList.size() > kMaxHosts)
    hlog::logf("fabric: %u hosts offered, the first %u are used", (unsigned)hostList.size(), (unsigned)kMaxHosts);
  std::array<orch::HeadStepFn, kMaxHosts> inner;
  std::array<ResendCtx, kMaxHosts> resend{};
  std::array<orch::TokenUsage, kMaxHosts> hostUse{};   // fixed: nothing allocated for them
  auto wrap = [&](size_t i, orch::HeadStepFn step, const char* who) -> orch::HeadStepFn {
    inner[i] = std::move(step);
    resend[i] = ResendCtx{&pd, &ht.cfg, &inner[i], who};
    return resendUnsentOnce(&resend[i]);
  };
  auto makeStep = [&](size_t i) -> orch::HeadStepFn {
    const std::string& h = hostList[i];
    orch::TokenUsage* use = &hostUse[i];
    if (h == "anthropic") return wrap(i, antLoopStep(pd, instructions, ht, use, tr), "orchTurn(ant-loop)");
    if (h == "openai")    return wrap(i, oaiLoopStep(pd, instructions, ht, use, tr), "orchTurn(oai-loop)");
    if (h == "mistral")   return wrap(i, misLoopStep(pd, instructions, ht, use, tr), "orchTurn(mistral-loop)");
    return nullptr;
  };

  // A parked head (a billing or key refusal learned on any seam) is not asked again:
  // the turn starts on the first host that is open - no request, retry or notice
  // spent on it. When every host is closed the first one closed by a refusal is
  // tried anyway (the provider decides; the owner was already told) - but a PAUSED
  // host is never asked (the pause switch: the owner's pause, the spend ceiling).
  auto nextUnparked = [&](size_t from) -> size_t {
    for (size_t i = from; i < hostCount; i++)
      if (!hostParked(pd, hostList[i])) return i;
    return hostCount;
  };
  auto firstUnpaused = [&]() -> size_t {
    for (size_t i = 0; i < hostCount; i++) {
      HostPark why;
      if (!hostParked(pd, hostList[i], &why) || !why.paused) return i;
    }
    return hostCount;
  };
  size_t hostIdx = nextUnparked(0);
  if (hostIdx >= hostCount) hostIdx = firstUnpaused();
  if (hostIdx >= hostCount) {
    err = "paused: paid requests to " + hostList[0] + " are paused";
    hlog::logf("fabric: every host is paused - no request");
    if (ht.onStepError) ht.onStepError(hostList[0], err);
    return false;
  }
  int    switches = 0;
  const int kMaxSwitches = 2;
  orch::HeadStepFn cur = makeStep(hostIdx);
  if (!cur) { err = "unknown host " + hostList[hostIdx]; return false; }
  if (hostIdx > 0) {
    HostPark why;
    hostParked(pd, hostList[0], &why);
    g_lastSwitch = FabricSwitch{hostList[0], hostList[hostIdx], why.kind, why.httpStatus, true};
    hlog::logf("fabric: %s is parked (%s) - starting on %s", hostList[0].c_str(),
               orch::stepErrSlug(why.kind), hostList[hostIdx].c_str());
    if (notify) notify(hostList[0], hostList[hostIdx]);
  }

  // The wrapper IS the controller's step: retry/failover happens INSIDE one
  // logical round, so runHeadLoop's caps and recording are provider-agnostic.
  // Every failed attempt reaches the engine's failure chain with its host (lane T).
  auto noteFailed = [&](const orch::HeadStep& r) {
    if (ht.onStepError) ht.onStepError(hostList[hostIdx], r.error);
  };
  orch::HeadStepFn step = [&](bool allowTools,
                              const std::vector<orch::HeadToolResult>& prior,
                              uint32_t budgetMs,
                              const std::string& capReason) -> orch::HeadStep {
    for (;;) {
      // Lane DEF: each billed round counts toward the monthly spend ceiling at once (the
      // turn files its spend at its end), so a long loop that crosses it is refused at
      // its next request (wire::exchange) instead of running to the turn's end.
      const orch::TokenUsage before = hostUse[hostIdx];
      orch::HeadStep sr = cur(allowTools, prior, budgetMs, capReason);
      orch::notePendingUsage(hostList[hostIdx].c_str(),
                             pd.orchModel ? pd.orchModel(hostList[hostIdx].c_str()).c_str() : "",
                             before, hostUse[hostIdx]);
      if (sr.ok) { unparkHost(hostList[hostIdx]); return sr; }
      typeStep(sr);
      noteFailed(sr);   // every failed attempt, the no-retry exits below included (lane T)
      if (orch::isSpendFrozenError(sr.error)) {
        hlog::logf("fabric: %s step refused - %s (no retry, no switch)",
                   hostList[hostIdx].c_str(), orch::kSpendFrozenError);
        return sr;
      }
      // The typed rule (fabricRuleFor): a request unreadable anywhere (BadRequest:
      // an invalid body, a schema or size refused before the send) and a device
      // short of memory (CUM-404, CUM-477) fail the same way on every host, so no
      // switch is burned on them and the engine answers with the real cause.
      if (!orch::fabricRuleFor(sr.kind).switchHost && !orch::fabricRuleFor(sr.kind).retrySameHost) {
        hlog::logf("fabric: %s step failed (%s: %s) - no retry, no switch",
                   hostList[hostIdx].c_str(), orch::stepErrSlug(sr.kind), sr.error.c_str());
        noteHostFailed(pd, hostList[hostIdx], sr.kind, sr.httpStatus);
        return sr;
      }
      // A request that was not sent got its one same-host resend (or its memory
      // verdict) in the wrapper already: it is not sent a third time here. And
      // only a cause in transit is resent: a refusal of THIS request comes back
      // identical (and a cut round already had its headroom retry in the step).
      orch::HeadStep sr2 = sr;
      if (orch::fabricRuleFor(sr.kind).retrySameHost && !resend[hostIdx].settled) {
        hlog::logf("fabric: %s step failed (%s) - same-host retry",
                   hostList[hostIdx].c_str(), sr.error.c_str());
        // Gate G5: the failed step may have captured a served-model echo on its HTTP
        // 200; only the attempt that serves the round may name the model.
        hostUse[hostIdx].servedModel.clear();
        const orch::TokenUsage beforeRetry = hostUse[hostIdx];
        sr2 = cur(allowTools, prior, budgetMs, capReason);
        orch::notePendingUsage(hostList[hostIdx].c_str(),
                               pd.orchModel ? pd.orchModel(hostList[hostIdx].c_str()).c_str() : "",
                               beforeRetry, hostUse[hostIdx]);
        if (sr2.ok) { unparkHost(hostList[hostIdx]); return sr2; }
        typeStep(sr2);
        noteFailed(sr2);
      } else if (!orch::fabricRuleFor(sr.kind).retrySameHost) {
        hlog::logf("fabric: %s step failed (%s: %s) - no same-host retry",
                   hostList[hostIdx].c_str(), orch::stepErrSlug(sr.kind), sr.error.c_str());
      }
      const orch::FabricRule rule = orch::fabricRuleFor(sr2.kind);
      noteHostFailed(pd, hostList[hostIdx], sr2.kind, sr2.httpStatus);
      const size_t nextIdx = nextUnparked(hostIdx + 1);
      if (!rule.switchHost || switches >= kMaxSwitches || nextIdx >= hostCount)
        return sr2;   // exhausted - the controller fails soft to the caller
      const std::string from = hostList[hostIdx];
      const std::string to   = hostList[nextIdx];
      orch::HeadStepFn next = makeStep(nextIdx);
      if (!next) return sr2;
      const size_t prevIdx = hostIdx;
      hostIdx = nextIdx;
      ++switches;
      hlog::logf("fabric: switching %s -> %s (%s; round re-runs on the shared transcript; "
                 "%u tool-result bytes carry over)",
                 from.c_str(), to.c_str(), orch::stepErrSlug(sr2.kind), (unsigned)tr.toolBytes());
      g_lastSwitch = FabricSwitch{from, to, sr2.kind, sr2.httpStatus, false};
      if (notify) notify(from, to);
      cur = next;
      inner[prevIdx] = nullptr;   // the abandoned host's step and its deps copy
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
  // Every exit files what each host billed, once (nothing bills before the loop).
  if (spend) {
    for (size_t i = 0; i < hostCount; ++i)
      spend->add(hostList[i].c_str(), pd.orchModel ? pd.orchModel(hostList[i].c_str()).c_str() : "",
                 hostUse[i]);
    char line[256];
    orch::spendLine(*spend, line, sizeof(line));
    hlog::logf("fabric-spend: %s", line);
  }
  // Glass Box P3: hand the canonical transcript to the engine BEFORE the
  // failure return - a failed turn's middle is exactly what needs debugging.
  if (ht.onBrief) ht.onBrief(tr.renderBrief(kHeadBriefMax));
  if (!res.ok) return wire::loopFailed(res, outJson, err);   // CUM-621: a cut keeps its partial turn
  outJson = res.finalTurn;
  return true;
}

}  // namespace providers
}  // namespace agent
