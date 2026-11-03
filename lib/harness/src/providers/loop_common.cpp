#include "loop_common.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <memory>
#include <new>   // std::bad_alloc: a thrown round still files what the loop billed

#include "nimbus/harness/log.h"
#include "nimbus/harness/output_caps.h"   // sameHostNonReasoningModel - the cut step-down
#include "wire.h"   // wire::lastExchange - did the failed request ever leave the device
#include "image_parts.h"   // R1: LoopImages - the turn's photos seed the transcript
#include "nimbus/orch/compact.h"   // isContextOverflowError - a provider's window refusal
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/head_switch.h"     // parkPause: reading the park without asking it
#include "nimbus/orch/provider_park.h"   // the one per-provider park every seam shares
#include "nimbus/orch/reply_guard.h"   // cutRetryNote - what a cut round is told
#include "nimbus/orch/transcript.h"
#include "nimbus/orch/turn.h"   // salvageTurnText - what a cut round wrote

namespace agent {
namespace providers {

namespace orch = nimbus::orch;

// The fabric's host vocabulary - true iff makeStep() below can build a step for
// this host. The engine gates fabricOn + filters the hostList on this, so a
// CUSTOM head drops to the legacy single-shot path instead of failing "unknown
// host custom" with zero HTTP, and custom inside providerPriority no longer
// truncates a real failover ladder mid-switch.
bool fabricSupports(const std::string& host) {
  return host == "anthropic" || host == "openai" || host == "mistral" || host == "zai";
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

// ---- web search hosted by the provider (providers.h) -----------------------------
namespace {
// A read of the park (parkPause), not an ask (hostParked): this runs on every tool
// listing, and asking would spend a parked provider's one re-check on nothing.
bool searchHostOpen(const ProviderDeps& pd, const char* host) {
  return pd.key && !pd.key(host).empty() &&
         !orch::pauseHolds(orch::parkPause(host, orch::PaidSeam::Chat, pd.nowMs ? pd.nowMs() : 0, /*dueIsOpen=*/true));
}
}  // namespace

bool hostedWebSearchAvailable(const ProviderDeps& pd) {
  return searchHostOpen(pd, "mistral") || (pd.zaiHead && searchHostOpen(pd, "zai"));
}

websearch::Result hostedWebSearch(const ProviderDeps& pd, const std::string& query, int maxResults) {
  websearch::Result res;
  res.hosted = true;
  res.err = "no provider with a built-in web search has a key";
  if (searchHostOpen(pd, "mistral")) {
    res = mistralWebSearch(pd, query);
    if (res.ok) return res;
    hlog::logf("web.search: mistral failed (%s)", res.err.c_str());
  }
  if (pd.zaiHead && searchHostOpen(pd, "zai")) {
    ProviderDeps zai = pd;
    pd.zaiHead(zai);
    const bool billedBefore = res.billed;
    res = zaiWebSearch(zai, query, maxResults);
    res.billed = res.billed || billedBefore;
  }
  return res;
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

void wireParkLog() {
  static const bool kWired = [] {
    orch::providerPark().setLog([](const char* line) { hlog::logf("%s", line); });
    return true;
  }();
  (void)kWired;
}

bool hostParked(const ProviderDeps& pd, const std::string& host, HostPark* why) {
  wireParkLog();
  const orch::ParkVerdict v =
      orch::providerPark().peek(host.c_str(), orch::PaidSeam::Chat, keyFpOf(pd, host), nowOf(pd));
  if (v.keyChanged) hlog::logf("fabric: %s un-parked (its key changed)", host.c_str());
  if (v.open) return false;
  if (why) *why = HostPark{stepKindOf(v), v.httpStatus, v.paused, v.cause};
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
//
// Cumulo closes only on the reason its body names, and the wire already heard that body
// (noteParkRefusal). A step kind here is typed from a status alone, and a status cannot
// say: a gateway's 403 with no reason is one failed call, not a rejected key.
void noteHostFailed(const ProviderDeps& pd, const std::string& host, orch::StepErr kind, int httpStatus) {
  if (host != "cumulo" && orch::fabricRuleFor(kind).parkHost) {
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

namespace {
// With every host of a turn closed: the one to ask (only a host whose re-check fell
// due since the first walk: its request is the probe), else `count`; and the first closed by a refusal (out of credit or a refused
// key), whose kind and status name the turn's refusal when nothing is asked.
struct ClosedPick {
  size_t ask;
  size_t refused;
  orch::StepErr kind = orch::StepErr::None;
  int status = 0;
};
ClosedPick pickWhenAllClosed(const ProviderDeps& pd, const std::vector<std::string>& hostList, size_t count) {
  ClosedPick pick{count, count};
  for (size_t i = 0; i < count; i++) {
    HostPark why;
    if (!hostParked(pd, hostList[i], &why)) return ClosedPick{i, count};   // its re-check is due now
    if (why.paused || pick.refused < count) continue;
    pick.refused = i;
    pick.kind = why.kind;
    pick.status = why.httpStatus;
  }
  return pick;
}
}  // namespace

namespace {
// What a host's tool loop can still do for a round that was too big or ran out of room: refit
// the prompt, one trim for a provider's window, one more run with the cut note. The fabric and
// a single host's loop (the Cumulo router head, the hosted Virtual Nimbus, failover off) share
// it. One turn long, on the turn's stack: the saved cut lives on the heap (it is big).
class RoundRecovery {
 public:
  RoundRecovery(orch::Transcript& tr, const HeadTools& ht, const std::string& instructions, const std::string& inputs)
      : tr_(tr), ht_(ht), instructions_(instructions), inputs_(inputs) {}

  // `lead` opens each request's input; `rebuild` makes the host's step anew on instructions().
  void bind(std::function<std::string()> lead, std::function<void()> rebuild) {
    lead_ = std::move(lead);
    rebuild_ = std::move(rebuild);
  }
  // The prompt and input the requests send: the engine's until a refit shortens them.
  const std::string& instructions() const { return fitted_ ? fitInstructions_ : instructions_; }
  std::string seed() const { return (lead_ ? lead_() : std::string()) + cutNote_ + (fitted_ ? fitInputs_ : inputs_); }
  // The seed differs from what the engine handed in, whichever host runs it.
  bool seedChanged() const { return fitted_ || !cutNote_.empty(); }

  // The prompt side of a refit (turn_fit.h): what the next request renders changes, so the
  // host's step is rebuilt on it. Never runs while the step is on the stack.
  size_t fitPrompt(size_t want, orch::FitScope scope) {
    if (!ht_.fitPrompt) return 0;
    const size_t freed = ht_.fitPrompt(want, scope, fitInstructions_, fitInputs_);
    if (!freed) return 0;
    fitted_ = true;
    tr_.setSeedText(seed());
    rebuild_();
    return freed;
  }
  std::function<size_t(size_t, orch::FitScope)> fitHook() {
    return [this](size_t want, orch::FitScope scope) { return fitPrompt(want, scope); };
  }

  // A provider refused the request for its size: nothing ran and nothing was billed. Trim
  // along the whole ladder and say to run it once more; the turn's photos are read as their
  // transcriptions on this host from here on. A second refusal in a row takes the caller's rules.
  bool overflowRetry(const std::string& host, const std::string& why) {
    if (overflowTried_ || !orch::isContextOverflowError(why)) return false;
    overflowTried_ = true;
    const size_t total = instructions().size() + (fitted_ ? fitInputs_.size() : inputs_.size()) + tr_.toolBytes();
    const size_t want = orch::overflowTrimBytes(why, total);
    const size_t freed = orch::trimForOverflow(tr_, want, fitHook(), [&]() -> size_t {
      return imageparts::readAsText(tr_.images(), host);
    });
    hlog::logf("fabric: %s refused the request for its size - gave up %u of %u B, one retry", host.c_str(),
               (unsigned)freed, (unsigned)want);
    return freed > 0;
  }

  // The next attempt at a cut round is told what happened and answers in less room.
  void noteCut(const orch::HeadStep& sr) {
    savedCut_.reset(new orch::HeadStep(sr));
    orch::Salvage kept;
    orch::salvageTurnText(sr.finalTurn, kept);
    cutNote_ = orch::cutRetryNote(sr.error, kept.reply.empty() ? sr.text : kept.reply);
    tr_.setSeedText(seed());
  }
  // True the first time per run of failures: the round may run once more with the note.
  bool cutRerunOnce() {
    if (cutRerun_) return false;
    cutRerun_ = true;
    return true;
  }
  // What a cut round wrote outlives a later failure of another kind: the owner gets those
  // words, not a bare cause.
  const orch::HeadStep& failure(const orch::HeadStep& last) const { return savedCut_ ? *savedCut_ : last; }

  // A round moved to another host: that host gets its own trim and its own rerun.
  void newHost() { overflowTried_ = cutRerun_ = false; }

  // The round is served. What was recovered is over: a later round's failure is about
  // itself, it may have its own trim and rerun, and it is not told about this cut.
  void served() {
    overflowTried_ = cutRerun_ = false;
    savedCut_.reset();
    heldHost_.clear();
    heldCause_.clear();
    if (cutNote_.empty()) return;
    cutNote_.clear();
    tr_.setSeedText(seed());
  }

  // A cut's cause reaches the failure chain only if nothing recovers it (the chain's first
  // entry is what the owner hears). `note` files a failed attempt, with what it took to flush.
  void holdCut(const std::string& host, const std::string& why) {
    if (!heldHost_.empty() && heldHost_ != host) flushCause();
    if (!heldHost_.empty()) return;   // the first of a run explains it
    heldHost_ = host;
    heldCause_ = why;
  }
  void flushCause() {
    if (heldHost_.empty()) return;
    if (ht_.onStepError) ht_.onStepError(heldHost_, heldCause_);
    heldHost_.clear();
    heldCause_.clear();
  }

 private:
  orch::Transcript& tr_;
  const HeadTools& ht_;
  const std::string& instructions_;
  const std::string& inputs_;
  std::function<std::string()> lead_;
  std::function<void()> rebuild_;
  std::string fitInstructions_, fitInputs_;
  bool fitted_ = false;
  bool overflowTried_ = false, cutRerun_ = false;
  std::unique_ptr<orch::HeadStep> savedCut_;
  std::string cutNote_;
  std::string heldHost_, heldCause_;
};

void loopHooks(orch::HeadLoopHooks& hooks, const ProviderDeps& pd, const HeadTools& ht, orch::Transcript& tr,
               RoundRecovery& rec) {
  hooks.dispatch = ht.dispatch;
  hooks.nowMs    = pd.nowMs;
  hooks.freeHeap = pd.freeHeap;
  hooks.largestBlock = pd.largestBlock;
  hooks.log      = [](const std::string& m) { hlog::logf("%s", m.c_str()); };
  hooks.onText   = ht.onRoundText;   // round prose -> engine (Glass Box A4)
  hooks.spill    = ht.spill;         // clamped-result spill -> results ring
  hooks.transcript = &tr;            // the controller records; step() renders
  if (ht.fitPrompt) hooks.fitPrompt = rec.fitHook();
}
}  // namespace

bool runHostLoop(const HostLoop& loop, const std::string& instructions, const std::string& inputs,
                 std::string& outJson, std::string& err) {
  const ProviderDeps& pd = loop.pd;
  const HeadTools& ht = loop.ht;
  const char* who = loop.who;
  // The CANONICAL transcript: the controller records prose, calls and results into it, and
  // every round's request is rendered fresh from it. Stateless by construction: no provider
  // conversation to create or track in loop mode.
  orch::Transcript tr;
  tr.addUser(inputs);
  const imageparts::LoopImages photos(tr);   // R1: the turn's photos ride the seeded message
  RoundRecovery rec(tr, ht, instructions, inputs);
  // CUM-477: one same-host resend of a request that never left.
  orch::HeadStepFn inner = loop.make(instructions, tr);
  const ResendCtx resend{&pd, &ht.cfg, &inner, who};
  const orch::HeadStepFn send = resendUnsentOnce(&resend);
  rec.bind(nullptr, [&] { inner = loop.make(rec.instructions(), tr); });

  orch::HeadLoopHooks hooks;
  hooks.step = [&](bool allowTools, const std::vector<orch::HeadToolResult>& prior, uint32_t budgetAtEntry,
                   const std::string& capReason) -> orch::HeadStep {
    const uint32_t stepStart = pd.nowMs ? pd.nowMs() : 0;
    for (;;) {
      orch::HeadStep sr = send(allowTools, prior, budgetLeft(pd, budgetAtEntry, stepStart), capReason);
      if (sr.ok) {
        rec.served();
        return sr;
      }
      // Too big to send: the controller refits and runs it again (head_loop.h).
      size_t budget = 0, size = 0;
      if (orch::parseBodyBudgetError(sr.error, budget, size)) return sr;
      typeStep(sr);
      if (rec.overflowRetry(loop.host, sr.error)) continue;
      if (sr.kind != orch::StepErr::Cut) return rec.failure(sr);
      rec.noteCut(sr);
      if (!rec.cutRerunOnce()) return rec.failure(sr);
      hlog::logf("%s: cut - the round runs once more with the note", who);
    }
  };
  loopHooks(hooks, pd, ht, tr, rec);

  const orch::HeadOutcome res = runHeadLoop(ht.cfg, hooks);
  hlog::logf("%s: rounds=%d cap=%s ok=%d heap=%u", who, res.rounds, res.capReason.c_str(), (int)res.ok,
             (unsigned)(pd.freeHeap ? pd.freeHeap() : 0));
  // Glass Box P3: hand the canonical transcript to the engine BEFORE the failure return: a
  // failed turn's middle is exactly what needs debugging.
  if (ht.onBrief) ht.onBrief(tr.renderBrief(kHeadBriefMax));
  if (!res.ok) return wire::loopFailed(res, outJson, err);   // CUM-621: a cut keeps its partial turn
  outJson = res.finalTurn;
  return true;
}

bool runFabricLoop(const ProviderDeps& pd, const std::vector<std::string>& hostList,
                   const std::string& instructions, const std::string& inputs,
                   std::string& outJson, std::string& err, const HeadTools& ht,
                   nimbus::orch::TurnSpend* spend, const FabricNotify& notify) {
  if (hostList.empty()) { err = "no runnable host"; return false; }

  // ONE canonical transcript for the whole turn, whichever provider serves a
  // given round. The controller records into it (hooks.transcript); every
  // host's step renders its wire from it.
  orch::Transcript tr;   // seeded below, once the host the turn starts on is known

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
  // context per host in fixed storage sized to the hosts one turn walks (the engine
  // passes at most three of the fabric's four), alive for the whole loop:
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
  RoundRecovery rec(tr, ht, instructions, inputs);
  auto makeStep = [&](size_t i) -> orch::HeadStepFn {
    const std::string& h = hostList[i];
    const std::string& ins = rec.instructions();
    orch::TokenUsage* use = &hostUse[i];
    if (h == "anthropic") return wrap(i, antLoopStep(pd, ins, ht, use, tr), "orchTurn(ant-loop)");
    if (h == "openai")    return wrap(i, oaiLoopStep(pd, ins, ht, use, tr), "orchTurn(oai-loop)");
    if (h == "mistral")   return wrap(i, misLoopStep(pd, ins, ht, use, tr), "orchTurn(mistral-loop)");
    if (h == "zai" && pd.zaiHead) {
      ProviderDeps zai = pd;   // the step keeps its own copy, as every step does
      pd.zaiHead(zai);
      return wrap(i, zaiLoopStep(zai, ins, ht, use, tr), "orchTurn(zai-loop)");
    }
    return nullptr;
  };

  // A parked head (a billing or key refusal learned on any seam) is not asked again:
  // the turn starts on the first host that is open - no request, retry or notice
  // spent on it. A host closed by a refusal (out of credit, or a refused key) is never
  // asked outside its timed re-check or a key change: with no open host the turn ends
  // with no request and the park's cause (zero requests to a parked provider). A
  // PAUSED host is never asked at all (the pause switch: the owner's pause, the spend
  // ceiling).
  auto nextUnparked = [&](size_t from) -> size_t {
    for (size_t i = from; i < hostCount; i++)
      if (!hostParked(pd, hostList[i])) return i;
    return hostCount;
  };
  size_t hostIdx = nextUnparked(0);
  if (hostIdx >= hostCount) {
    const ClosedPick pick = pickWhenAllClosed(pd, hostList, hostCount);
    hostIdx = pick.ask;
    if (hostIdx >= hostCount) {
      const size_t named = pick.refused < hostCount ? pick.refused : 0;
      if (pick.refused < hostCount) {
        err = orch::parkedRefusalError(hostList[named], pick.kind, pick.status);
        hlog::logf("fabric: every host is parked or paused - no request (%s parked: %s)", hostList[named].c_str(),
                   orch::stepErrSlug(pick.kind));
      } else {
        err = "paused: paid requests to " + hostList[0] + " are paused";
        hlog::logf("fabric: every host is paused - no request");
      }
      if (ht.onStepError) ht.onStepError(hostList[named], err);
      return false;
    }
  }
  // The seeded input opens with the lead of the host each request really runs on (HFM),
  // then the note of a cut round (reply_guard.h cutRetryNote).
  orch::HeadStepFn cur;
  rec.bind([&] { return ht.inputLead ? ht.inputLead(hostList[hostIdx]) : std::string(); },
           [&] { cur = makeStep(hostIdx); });
  tr.addUser(rec.seed());
  const imageparts::LoopImages photos(tr);   // R1: the turn's photos ride the seeded message
  int    switches = 0;
  const int kMaxSwitches = 2;
  bool   steppedDown = false;   // the one same-host non-reasoning step after a cut
  cur = makeStep(hostIdx);
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
  // A cut's and a provider overflow's cause are noted only when nothing recovers them.
  auto noteFailed = [&](const orch::HeadStep& r) {
    if (r.kind == orch::StepErr::Cut) {
      rec.holdCut(hostList[hostIdx], r.error);
      return;
    }
    rec.flushCause();
    if (ht.onStepError) ht.onStepError(hostList[hostIdx], r.error);
  };
  orch::HeadStepFn step = [&](bool allowTools,
                              const std::vector<orch::HeadToolResult>& prior,
                              uint32_t budgetAtEntry,
                              const std::string& capReason) -> orch::HeadStep {
    const uint32_t stepStart = pd.nowMs ? pd.nowMs() : 0;
    for (;;) {
      const uint32_t budgetMs = budgetLeft(pd, budgetAtEntry, stepStart);   // a rerun has only what is left
      // Lane DEF2: a host whose own monthly limit was reached during this turn (its billed
      // rounds count at once) is not asked again: the round moves to the next open host,
      // quietly (the owner hears about the limit once, from the limits store), and the
      // progress carries over; with no open host left the turn ends with the pause.
      if (orch::providerPark().pausedBy(hostList[hostIdx].c_str()) & orch::kPauseProviderLimit) {
        orch::HeadStep paused;
        paused.ok = false;
        paused.kind = orch::StepErr::Paused;
        paused.error = "paused: paid requests to " + hostList[hostIdx] + " are paused";
        noteFailed(paused);
        const size_t nextIdx = nextUnparked(hostIdx + 1);
        orch::HeadStepFn next = nextIdx < hostCount && switches < kMaxSwitches ? makeStep(nextIdx) : nullptr;
        if (!next) return paused;
        hlog::logf("fabric: %s reached its monthly limit - the round moves to %s",
                   hostList[hostIdx].c_str(), hostList[nextIdx].c_str());
        g_lastSwitch = FabricSwitch{hostList[hostIdx], hostList[nextIdx], orch::StepErr::Paused, 0, true};
        if (notify) notify(hostList[hostIdx], hostList[nextIdx]);
        const size_t prevIdx = hostIdx;
        hostIdx = nextIdx;
        ++switches;
        cur = next;
        inner[prevIdx] = nullptr;   // the abandoned host's step and its deps copy
        if (ht.inputLead) tr.setSeedText(rec.seed());
        continue;
      }
      // Lane DEF: each billed round counts toward the monthly spend ceiling at once (the
      // turn files its spend at its end), so a long loop that crosses it is refused at
      // its next request (wire::exchange) instead of running to the turn's end.
      const orch::TokenUsage before = hostUse[hostIdx];
      orch::HeadStep sr = cur(allowTools, prior, budgetMs, capReason);
      orch::notePendingUsage(hostList[hostIdx].c_str(),
                             pd.orchModel ? pd.orchModel(hostList[hostIdx].c_str()).c_str() : "",
                             before, hostUse[hostIdx]);
      if (sr.ok) {
        unparkHost(hostList[hostIdx]);
        rec.served();
        return sr;
      }
      // A round too big to SEND never left the device: the controller folds older
      // tool rounds and runs it again (head_loop.h refitToBudget). It is not a host
      // failure and not a cause in the engine's chain (the owner hears the chain's
      // first cause); if no refit fits, the engine records the refusal at turn end.
      size_t budget = 0, size = 0;
      if (orch::parseBodyBudgetError(sr.error, budget, size)) return sr;
      typeStep(sr);
      // The provider's context window: trim and retry once on this host. Not a failure yet: if
      // the retry fails it says why itself. A second refusal takes the rules below (another
      // host may have the room, and it runs the trimmed request).
      if (rec.overflowRetry(hostList[hostIdx], sr.error)) continue;
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
        if (sr2.ok) {
          unparkHost(hostList[hostIdx]);
          rec.served();
          return sr2;
        }
        typeStep(sr2);
        noteFailed(sr2);
      } else if (!orch::fabricRuleFor(sr.kind).retrySameHost) {
        hlog::logf("fabric: %s step failed (%s: %s) - no same-host retry",
                   hostList[hostIdx].c_str(), orch::stepErrSlug(sr.kind), sr.error.c_str());
      }
      const orch::FabricRule rule = orch::fabricRuleFor(sr2.kind);
      noteHostFailed(pd, hostList[hostIdx], sr2.kind, sr2.httpStatus);
      // Ruling 3 Nov 09:15Z: a reasoning head that out-grew its round (Lumi:
      // Large 4 cut at 4,125 then 5,500 tokens for a 135-byte reply, 38,656
      // billed as reasoning) gets ONE same-host step on a non-reasoning model
      // before a different provider is asked. Same provider, so no owner
      // notice: nothing the owner sees changes, and the round that spiralled
      // is exactly the round a non-reasoning model answers in a few hundred
      // tokens (measured 3 Nov, evidence/defect5/). The step-down holds for
      // the rest of the turn: the configured head already spiralled once.
      if (sr2.kind == orch::StepErr::Cut && !steppedDown) {
        const std::string headModel =
            pd.orchModel ? pd.orchModel(hostList[hostIdx].c_str()) : std::string();
        const char* alt = providers::sameHostNonReasoningModel(hostList[hostIdx], headModel);
        if (alt && *alt && headModel != alt) {
          steppedDown = true;
          hlog::logf("fabric: %s (%s) cut at its output cap - one same-host step on %s "
                     "before another provider",
                     hostList[hostIdx].c_str(), headModel.c_str(), alt);
          ProviderDeps altPd = pd;   // the step keeps its own copy, as every step does
          const std::string altModel = alt;
          auto origModel = pd.orchModel;
          altPd.orchModel = [origModel, altModel](const char* h) {
            return std::string(h) == "mistral" ? altModel
                                              : (origModel ? origModel(h) : std::string());
          };
          cur = wrap(hostIdx, misLoopStep(altPd, rec.instructions(), ht, &hostUse[hostIdx], tr),
                     "orchTurn(mistral-loop)");
          continue;   // the SAME round re-runs on the stepped-down model
        }
      }
      const size_t nextIdx = nextUnparked(hostIdx + 1);
      const bool nowhereToGo = !rule.switchHost || switches >= kMaxSwitches || nextIdx >= hostCount;
      // A cut round had its headroom retry in the step. The next attempt, here or on the
      // next host, is told what happened and answers in less room; with nowhere to go
      // the round runs once more here (Lumi, 31 Oct: the only open host, 3m56s, no answer).
      if (sr2.kind == orch::StepErr::Cut) {
        rec.noteCut(sr2);
        if (nowhereToGo && rec.cutRerunOnce()) {
          hlog::logf("fabric: %s cut with nowhere to go - the round runs once more with the note",
                     hostList[hostIdx].c_str());
          continue;
        }
      }
      // Exhausted: the controller fails soft to the caller.
      if (nowhereToGo) return rec.failure(sr2);
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
      rec.newHost();
      if (ht.inputLead || rec.seedChanged()) tr.setSeedText(rec.seed());
      // loop: the SAME round re-runs on the new host. The transcript already
      // holds every executed tool result - nothing re-dispatches.
    }
  };

  orch::HeadLoopHooks hooks;
  hooks.step = step;
  loopHooks(hooks, pd, ht, tr, rec);

  // Every exit files what each host billed, once (nothing bills before the loop).
  auto fileSpend = [&] {
    for (size_t i = 0; i < hostCount; ++i)
      spend->add(hostList[i].c_str(), pd.orchModel ? pd.orchModel(hostList[i].c_str()).c_str() : "",
                 hostUse[i]);
  };
  orch::HeadOutcome res;
  try {
    res = runHeadLoop(ht.cfg, hooks);
  } catch (const std::bad_alloc&) {
    // A failed allocation inside a round (the next request being built after a round
    // billed and ran tools): the rounds already billed are filed on the way out, so
    // the turn's memory end meters them (F2: they were never metered).
    if (spend) fileSpend();
    throw;
  }
  if (!res.ok) rec.flushCause();   // nothing recovered it
  hlog::logf("fabric-loop: rounds=%d cap=%s ok=%d host=%s switches=%d heap=%u",
             res.rounds, res.capReason.c_str(), (int)res.ok,
             hostList[hostIdx].c_str(), switches,
             (unsigned)(pd.freeHeap ? pd.freeHeap() : 0));
  if (spend) {
    fileSpend();
    std::string line(400, '\0');   // model ids included: logged whole, built off the stack
    line.resize(orch::spendLine(*spend, &line[0], line.size()));
    hlog::log(("fabric-spend: " + line).c_str());
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
