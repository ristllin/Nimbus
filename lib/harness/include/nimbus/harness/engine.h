#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/attention.h"           // nimbus::attn::Event (head Running/Offline arc)
#include "nimbus/harness/apply.h"       // ApplyDeps / ApplyState / applyTurn (Stage E)
#include "nimbus/harness/compose.h"     // ComposeInputs / composeInstructions (Stage D)
#include "nimbus/harness/config.h"      // HarnessConfig
#include "nimbus/harness/head_tools.h"  // HeadTools (mid-turn tool loop context)
#include "nimbus/harness/hooks.h"       // Hooks / TurnDebugEv
#include "nimbus/harness/jobs.h"        // JobEngine (fresh results + keyFromTag)
#include "nimbus/harness/platform.h"    // Platform (nowMs / freeHeap / delayMs)
#include "nimbus/harness/conversation.h"   // FoldSlice - the history fold slice (histV2)
#include "nimbus/orch/episodic.h"       // EpisodicMessage (historyRows)
#include "nimbus/orch/failure_chain.h"  // FailureChain - the turn's causes
#include "nimbus/orch/fallback_rules.h" // ErrorClass - the pending [FALLBACK] note's class
#include "nimbus/orch/loops.h"          // FireOutcome (scheduled-turn result)
#include "nimbus/orch/token_usage.h"
#include "nimbus/orch/turn_spend.h"     // TurnSpend - the turn's per-host spend
#include "nimbus/orch/tool_registry.h"  // ToolRegistry::Spec (toolSpecs closure)

// engine - the TURN ORCHESTRATION, lifted from src/agent/orchestrator.cpp
// (Stage G, the final lift). One TurnEngine owns the full turn pipeline: the
// TurnGuard (in-flight flag + head ring arc), associative recall, World-prompt
// composition, host pick + per-host "host|convId" conversation state, the
// per-provider token-budget failover, the head tool-loop wiring, the
// retry/failover ladder (same-host fresh-conv retry, priority walk, the
// no-retry-once-a-tool-ran guard), usage/turn-counter accounting, parse +
// reply SALVAGE, and the portable applyTurn threading - plus maybeConsolidate
// (auto-synthesis), handleMessage's turn core, injectScheduledTurn, and the
// stuck-turn reaper. Every owner-visible string and log line moved
// BYTE-IDENTICAL (test_harness_turn pins the load-bearing ones). The device
// keeps only glue: sinks, NVS/PSRAM surfaces, the Telegram /loops intercept,
// and thin public-API forwards.
namespace agent {

// One stateful head-orchestrator turn on a provider host: send system
// (instructions) + user (inputs), get the strict orch_turn JSON back. convId is
// in-out (provider conversation continuity; "" = fresh). tools non-null =>
// run the bounded multi-turn tool-use loop instead of the single shot. The
// DEVICE registers three String<->std::string wrappers around the existing
// orchTurn* adapters (adapters themselves untouched); tests register fakes.
using ProviderTurnFn = std::function<bool(
    std::string& convId, const std::string& instructions, const std::string& inputs,
    std::string& outJson, std::string& err, const HeadTools* tools,
    nimbus::orch::TokenUsage* usage)>;

// The engine-owned multi-provider fabric loop (Stage 2 phase 5): the device
// registers providers::runFabricLoop bound to its ProviderDeps. hostList is the
// priority-ordered, pre-filtered (keyed + in-budget) host set; notify fires once
// per mid-turn provider switch (from, to) for the owner notice. `spend` (lane T)
// receives what EACH host billed, filed under its own model - a TurnSpend, not a
// summed TokenUsage, so a caller that still passes a sum does not compile.
using FabricLoopFn = std::function<bool(
    const std::vector<std::string>& hostList, const std::string& instructions,
    const std::string& inputs, std::string& outJson, std::string& err,
    const HeadTools& ht, nimbus::orch::TurnSpend* spend,
    const std::function<void(const std::string& fromHost,
                             const std::string& toHost)>& notify)>;

namespace providers {
struct HostPark;   // why a host is parked (providers.h)
}

// Registry: host name -> ProviderTurnFn. An unknown host fails with the same
// "host 'X' unavailable" error the device lambda produced, so it fails over.
class ProviderHosts {
 public:
  // Lane K's billing and auth park on the paths outside the fabric loop: the
  // single-shot ladder and the context fold skip a parked host and feed the park
  // with every answer and failure, so a refused host is asked once per episode on
  // every path, not once per turn. The device binds these to providers::hostParked,
  // noteHostFailed and unparkHost over its ProviderDeps (the park's key and model
  // fingerprint is the fabric's). Unset = no host is ever parked.
  std::function<bool(const std::string& host, providers::HostPark* why)> parked;
  std::function<void(const std::string& host, nimbus::orch::StepErr kind, int httpStatus)> failed;
  std::function<void(const std::string& host)> answered;
  bool isParked(const std::string& host, providers::HostPark* why = nullptr) const {
    return parked && parked(host, why);
  }
  // A failed request there, typed from its error line (classifyStepError).
  void noteFailed(const std::string& host, const std::string& err) const;
  void noteAnswered(const std::string& host) const {
    if (answered) answered(host);
  }

  // Optional fabric loop - when set AND cfg.loop.midTurnFailover() is true, the
  // engine routes LOOP turns here; ProviderTurnFn stays the single-shot (and
  // gate-off) path.
  FabricLoopFn fabric;
  // Which hosts the fabric can actually drive (device wires
  // providers::fabricSupports). Absent = every registered host. Without this
  // gate a CUSTOM head entered the fabric, whose step table only knows the
  // three cloud providers, and the turn failed "unknown host" with zero HTTP
  // (2026-08-12 regression - the head-on-custom path was unreachable under
  // default knobs since the fabric landed).
  std::function<bool(const std::string&)> fabricSupports;

  void add(std::string host, ProviderTurnFn fn) { hosts_[std::move(host)] = std::move(fn); }
  bool has(const std::string& host) const {
    auto it = hosts_.find(host);
    return it != hosts_.end() && (bool)it->second;
  }
  bool run(const std::string& host, std::string& convId, const std::string& instructions,
           const std::string& inputs, std::string& outJson, std::string& err,
           const HeadTools* tools, nimbus::orch::TokenUsage* usage) const {
    auto it = hosts_.find(host);
    if (it == hosts_.end() || !it->second) {
      err = "host '" + host + "' unavailable";
      return false;
    }
    return it->second(convId, instructions, inputs, outJson, err, tools, usage);
  }

 private:
  std::map<std::string, ProviderTurnFn> hosts_;
};

// Per-chat provider-conversation map (Release B2). The NVS value holds
// "chat=host|convId;chat2=host|convId;..." (legacy single "host|convId" values
// carry no chat key and are DISCARDED - one fresh conversation per chat at
// upgrade, the same reset a failover does routinely). Bounded at 8 entries,
// oldest dropped first. Pure string ops - host-tested.
std::string convMapGet(const std::string& raw, const std::string& chat,
                       const std::string& host);
std::string convMapSet(std::string raw, const std::string& chat,
                       const std::string& host, const std::string& convId);

// CUM-404: may a chat/turn be admitted, given the internal-heap facts? BOTH the
// total-free starvation backstop AND the largest-contiguous-block guard must clear -
// fragmentation, not total free, is what fails a turn's mbedTLS handshake allocation,
// so a device that is fragmented-but-ample (free high, largest low) is correctly
// refused while a healthy-but-low-total device is admitted. Pure + host-tested; a
// largestBlock of 0xFFFFFFFF (the harness sentinel for an unwired closure) makes the
// largest-block half a no-op, so a test or host build with no largest hook behaves as
// a total-free gate only. Mirrors nimbus::cloud::relayCanDial's two-floor shape.
inline bool turnHeapOk(uint32_t freeInternal, uint32_t largestBlock,
                       uint32_t freeFloor, uint32_t largestFloor) {
  return freeInternal >= freeFloor && largestBlock >= largestFloor;
}

// CUM-523: tools that can only deliver over Telegram (reply.telegram, files.send).
// A turn that cannot deliver over Telegram (HarnessConfig::telegramDeliverable)
// is never offered them.
bool isTelegramOnlyTool(const std::string& name);

// histV2: where a chat's verbatim history starts (row ids; "" = none).
struct HistoryMarks {
  std::string thru;        // the newest row the fold summary covers
  std::string softStart;   // the overflow shift: rows up to it are hidden until a fold
};

class TurnEngine {
 public:
  struct Deps {
    HarnessConfig cfg;
    Platform platform;      // nowMs + freeHeap + delayMs (the 400 ms retry pause)
    Hooks hooks;            // onTurnDebug -> the device's /api/lastturn PSRAM capture
    JobEngine* jobs = nullptr;  // fresh results + sessionInfos ([ACTIVE SESSIONS])
    ProviderHosts hosts;
    ApplyDeps apply;        // the device-built execution table (Stage E)

    // Owner delivery (device: Telegram reply queue + full-queue logging).
    std::function<void(const std::string& chatId, const std::string& text)> deliver;
    // Attention-event sink (device: attn::Router via g_sinks.event). Nullable.
    std::function<void(const nimbus::attn::Event&)> event;
    // Sound cue, execution-only: "turnstart" (device: sfx TurnStart). Nullable.
    std::function<void(const char* cue)> fire;

    // Associative recall for the user's message (device: memory::recallFor -
    // embed + VDB search; when embeddings cannot serve it, a keyword scan that
    // says so in RecallOut::degraded). Nullable => no recall.
    std::function<nimbus::orch::RecallOut(const std::string& userText,
                                          const nimbus::orch::Principal& who)> recall;
    // Notes for this chat from the pending-embed drain ("[MEMORY] queued mem_write
    // ... -> now stored"), taken once; they join the turn's MEMORY RESULTS.
    // Nullable => none.
    std::function<std::string(const std::string& chatId)> memNotes;
    // Gather the LIVE prompt inputs (devName/hostLabel/directive/running
    // memory/HAL+fault hardware manifest/sessions/scratchpad). The engine
    // fills tools/loopOn/recalled itself so "advertised == callable" is a
    // structural property, then calls agent::composeInstructions.
    // chatId = the turn's routing chat, so the device can fill the per-chat
    // RECENT CONVERSATION window (Release B1). Pseudo-chats (web/serial/voice)
    // get their own windows for free.
    std::function<ComposeInputs(const std::string& chatId)> composeInputs;
    // Registry tool advertisements for THIS turn's caller (device:
    // memory::registry().toolSpecsFor(who)). W14: principal-scoped - an
    // admin-only tool is not advertised to a member/guest turn, because it
    // would refuse at dispatch anyway ("advertised == callable"). The engine
    // passes the same Principal it uses for recall + dispatch.
    std::function<std::vector<nimbus::orch::ToolRegistry::Spec>(
        const nimbus::orch::Principal&)> toolSpecs;
    // One MCP JSON-RPC request -> response (device: memory::handleMcp - the
    // SAME Lock + dispatch + persist path external MCP clients use).
    std::function<std::string(const std::string& jsonRpcRequest,
                              const nimbus::orch::Principal& who)> mcpDispatch;
    // Per-provider capability + connector catalog (device: connectors::catalog).
    std::function<std::string()> connectorsCatalog;
    // W15: the ambient [SKILLS] index (device: skills::indexText) - one line per
    // capsule so the model can PULL a playbook with skill.get. A skill the
    // per-turn context never NAMES is a skill the model will never reach for.
    std::function<std::string()> skillsIndex;
    // Model choice list per provider for the [AVAILABLE MODELS] block. MUST be the
    // same derivation the spawn validator accepts (device: the shared
    // nimbus::orch::effectiveModelChoices over the harvested list - CUM-486), so
    // the head is never offered a model that validation coerces away.
    std::function<std::string(const std::string& provider)> modelChoices;
    // Episodic auto-capture of the owner's message (session row + user row with
    // the engine-sanitized "from:<name>" tag). Nullable.
    std::function<void(const std::string& chatId, const std::string& text,
                       const std::string& fromTag)> episodicCaptureUser;
    // The first allow-listed chat (scheduled-turn default target).
    std::function<std::string()> firstAllowedChat;
    // Journal compaction after a consolidation turn (device: g_journal.gc()).
    std::function<void()> journalGc;
    // Recent-results spill for clamped tool results (Context Fabric, nullable):
    // full text -> the results ring; returns the results.get handle the
    // truncation marker embeds. Device: orchestrator::resultsPut("tool", ...).
    // `ns` is the OWNING tenant (the turn Principal's namespace) - reads of the
    // ring are scoped to it, so a clamped result cannot leak across chats.
    std::function<std::string(const std::string& name, const std::string& fullText,
                              const std::string& ns)> spillResult;

    // ---- histV2: the turn-structured history (conversation.h) --------------
    // All nullable. histV2 is on only when the switch says so AND rows can be
    // read and the user row's id comes back; otherwise the device's window and
    // the previous capture stay byte-identical.
    std::function<bool()> histV2;
    // The chat's conversation rows (message and media kinds), newest first,
    // strictly older than `before` ("" = from the newest), at most `limit`.
    std::function<std::vector<nimbus::orch::EpisodicMessage>(
        const std::string& chatId, size_t limit, const std::string& before)> historyRows;
    // Capture the owner's row (text = what the sender sent, tags = from:, att:,
    // src:, sent:) and return its id: the in-flight turn the window leaves out.
    std::function<std::string(const std::string& chatId, const std::string& text,
                              const std::string& tags)> captureUserRow;
    // A seedless turn's trigger row (role system), written just before the
    // turn's first assistant row, so a silent routine leaves nothing.
    std::function<void(const std::string& chatId, const std::string& text,
                       const std::string& tags)> captureTrigger;
    // The chat's fold anchor and overflow shift ({thru, softStart}), and the
    // shift's advance (persisted with the turn).
    std::function<HistoryMarks(const std::string& chatId)> historyMarks;
    std::function<void(const std::string& chatId, const std::string& softStart)> noteHistoryShift;
    // Minutes east of UTC for day markers and send times (unwired = UTC).
    std::function<int32_t()> utcOffsetMin;
  };

  // Heap floors, defaulted to src/agent/agent_config.h's values so the
  // portable lib never includes that header. (CUM-404: total-free floors are a
  // conservative starvation backstop; turnMinLargestBlock is the real gate.)
  struct Tuning {
    uint32_t turnHardFloor      = 12000;  // ORCH_TURN_HARD_FLOOR (total-free backstop)
    uint32_t recallMinHeap      = 12000;  // ORCH_RECALL_MIN_HEAP (== the turn hard floor)
    uint32_t autoTurnMinHeap    = 14000;  // ORCH_AUTO_TURN_MIN_HEAP (+2000 above the interactive floor)
    uint32_t loopMinHeap        = 12000;  // ORCH_LOOP_MIN_HEAP
    uint32_t turnMinLargestBlock = 5000;  // ORCH_TURN_MIN_LARGEST_BLOCK (largest contiguous
                                          // internal block a turn's mbedTLS handshake needs;
                                          // CUM-404 v2 == relay_heap.h kRelayHeapFloorLargest,
                                          // CYD-safe. NOT provider_verify's one-shot 8000)
  };

  TurnEngine(Deps d, Tuning t);
  // (Two ctors, not one defaulted arg: a `Tuning t = {}` default needs the
  // NSDMIs before the class is complete - clang rejects it.)
  explicit TurnEngine(Deps d) : TurnEngine(std::move(d), Tuning()) {}

  // Run ONE orchestrator turn: recall for the user's message, assemble the
  // World prompt, pick the host (with same-host retry + cross-host failover),
  // parse the turn, apply its fields. userText is the raw owner message (empty
  // for synthesis turns) - used only to drive associative recall.
  bool runTurn(const std::string& inputs, const std::string& chatId,
               const std::string& userText);

  // The TURN CORE of the device's handleMessage (the /loops owner-command
  // intercept, clearAsk and drainStaged stay device-side, called before this).
  void handleMessage(const std::string& text, const std::string& fromName,
                     const std::string& chatId);

  // Run a Local Loops scheduled turn (mirrors the auto-synthesis turn but with
  // a [SCHEDULED LOOP] preamble; recall runs for the prompt). loopId (optional,
  // additive) feeds the spend-attribution tag "loop:<id>"; empty => "loop".
  // quietOk (additive; the DREAM loop): an all-empty turn delivers nothing -
  // the bare "Done." fallback is suppressed (ApplyState.quietFallback), and every
  // engine notice is held. The reserved dream goes further: its own reply, ask and
  // correction lines are never sent either, but come back in FireOutcome.held.
  // once (W20): a self-set single wakeup - renders a [WAKEUP] preamble instead
  // (calling a one-shot the model armed for itself a "recurring task" is a lie
  // the honesty rails would then have to argue with). ownerReminder (W22): the
  // one-shot was set by the OWNER via /remind - [REMINDER] framing, since "you
  // scheduled this for yourself" would be false.
  nimbus::orch::FireOutcome injectScheduledTurn(const std::string& chatId,
                                                const std::string& prompt,
                                                const std::string& name,
                                                const std::string& loopId = "",
                                                bool quietOk = false,
                                                bool once = false,
                                                bool ownerReminder = false);

  // The auto-synthesis (consolidation) turn - wired as the JobEngine's
  // synthesize closure by the device.
  void maybeConsolidate(const std::string& chatId);

  // Stuck-turn reaper: if a turn has been in flight longer than the loop
  // budget + 2 min margin, presume the turn task dead - free the head arc +
  // clear the flag. Returns true when it fired (caller repaints).
  bool reapStuckTurn(uint32_t nowMs);

  // v3.6.0 context fold (plans/compaction-plan.md, prism-revised design): ONE
  // single-shot summarization call for a chat. Deliberately NOT a turn - no
  // delivery, no recall, no tool loop, no conv-map read/write, no assistant
  // capture, no turnInFlight/ring state, and it never touches lastTurnUsage_
  // (Local Loops metering isolation; its spend is ledgered as "compact").
  // The caller (device pump, on tg_poll) supplies the previous anchored summary
  // and the episodic digest, stores the returned summary FIRST, then resets the
  // chat's provider chain via clearChatConv. Fail-soft: false on heap/budget
  // gate, provider failure, or an empty/unparseable summary - the caller's
  // breaker decides what happens next.
  // Deferred = a gate (heap/budget/turn-in-flight) said "not now" - retry on a
  // later pump pass, and the caller must NOT count it against the breaker (a
  // low-heap window after a turn burned the breaker as if the provider failed -
  // live-caught by the L15 degraded-fold row). Failed = a real attempt failed.
  enum class FoldResult : uint8_t { Ok = 0, Deferred, Failed };

  // TRUE when a fold could run right now (heap, keyed host, budget, no turn in
  // flight). The caller MUST check this before doing anything visible or
  // expensive: runFold's own gates fire after the notice + digest would already
  // have been paid, and a persistent defer (low heap, month-long budget cap)
  // otherwise repeats them every pump pass - an owner-visible notice loop.
  bool canFoldNow() const;
  // Keyed, in-budget fold hosts (orchHost first, then priority; <=3). Public so
  // tests can pin the ladder order without scripting a provider.
  std::vector<std::string> foldHostCandidates() const;
  FoldResult runFold(const std::string& chatId, const std::string& prevSummary,
                     const std::string& digest, std::string& outSummary);

  // Reset ONE chat's provider conversation (fold write-order step 2; also the
  // reactive-overflow recovery). Other chats' threads stay intact - unlike the
  // web convReset, which clears the whole map.
  void clearChatConv(const std::string& chatId);

  // P6: the tool loop is the default turn path, but it must have headroom above
  // the single-TLS + per-round floor. This single predicate decides BOTH what
  // the prompt advertises (composeInstructions) and whether the loop actually
  // runs (runTurn), so "advertised == callable" holds even when memory pressure
  // forces a single-shot turn.
  bool loopActiveNow() const;
  // CUM-523: the Telegram answer sampled for the turn in flight (the last turn's when none
  // is). The turn's tool list, channel hint, compose wording and capability line
  // all follow it; device text served mid-turn (skill bodies, tool results)
  // reads it too, so one turn never disagrees with itself.
  bool turnTelegram() const { return turnTelegram_; }
  // CUM-523: files.send for the turn in flight - its Telegram sample AND a chat
  // files.send can deliver to (a public-mode guest turn has the first only).
  bool turnFileSend() const { return turnFileSend_; }
  // The same gate against a supplied heap reading - runTurn gates on the
  // turn-ENTRY heap so recall's transient dip can't defer the loop (see the
  // rationale at the top of runTurn).
  bool loopActiveAt(uint32_t heap) const;

  // CUM-211: is ANY orchestrator provider actually routable right now - a
  // registered host (openai/anthropic/mistral, or a configured custom/LAN
  // endpoint) that also has a key? Budget-agnostic on purpose: an over-budget
  // provider IS configured and has its own honest reply, so it must not read as
  // "no provider". handleMessage checks this BEFORE running a paid/failing turn,
  // so an unroutable chat gets a deterministic local "no provider set up" reply
  // instead of a silent drop or a misleading 401 - on every channel, and
  // precisely when nothing else works (a local message, no round-trip needed).
  bool anyProviderConfigured() const;

  bool turnInFlight() const { return turnInFlight_; }
  uint32_t headJobKey() const { return keyFromTag("head"); }
  bool inScheduledTurn() const { return scheduledTurn_; }
  const std::string& lastInstructions() const { return lastInstructions_; }
  const std::string& currentChat() const { return curChat_; }
  // A mid-loop reply.telegram/reply.speak already answered THIS turn's chat -
  // applyTurn then suppresses its bare "Done." fallback (device sendToChat
  // reports it here).
  void noteToolReplied() { toolReplied_ = true; }

  // The last turn's billed total, on EVERY exit (a failed turn's spend too): a
  // scheduled fire and the dream report their own spend, never a previous turn's.
  nimbus::orch::TokenUsage lastTurnUsage() const { return lastTurnUsage_; }
  // The claim check of the current (or last) turn: its effect ledger and flags.
  const ClaimCheck& claimCheck() const { return claims_; }
  // The tags of the assistant row being captured now: S's fx: tag, and under
  // histV2 its out: kind (reply / ask / tts).
  std::string assistantRowTags() const;
  // histV2 on for this engine (the switch and the seams it needs are wired).
  bool histV2On() const;
  // H3: the next fold's slice of this chat - the turns after the fold anchor
  // beyond the verbatim history's low water (half the head's history budget, the
  // same budget the window shifts to), rendered up to `capBytes`
  // (conversation.h foldSlice). Empty = nothing worth a summary.
  FoldSlice historyFoldSlice(const std::string& chatId, size_t capBytes, bool manual);
  // Engine notices a quiet background turn held back since boot (never delivered).
  uint32_t quietNoticesHeld() const { return quietNoticesHeld_; }
  nimbus::orch::TokenUsage sessionUsage() const { return sessionUsage_; }
  uint32_t turnCount() const { return turnCount_; }

  // W6: paint/clear the "head" ring arc from OUTSIDE a turn (the main-loop
  // reconciler), so the orchestrator's arc persists while its children run.
  // Running (blue comet) when lit, Offline when cleared.
  void setHeadArc(bool lit);

 private:
  // runTurn with the turn's one Telegram sample: handleMessage and the scheduled
  // turn sample before they write their hint / delivery line into the inputs.
  bool runTurn(const std::string& inputs, const std::string& chatId,
               const std::string& userText, bool telegram);
  // CUM-523: the live Telegram predicate for a turn on `chatId`
  // (HarnessConfig::telegramDeliverable; unset = true). Sampled ONCE, at turn
  // entry; everything after reads turnTelegram().
  bool telegramOn(const std::string& chatId) const;
  // This chat's parked [MEMORY RESULTS] block ("" when none), consumed.
  std::string takePendingMemResults(const std::string& chatId);
  // After a turn: keep this chat's freshly parked results (stamped with the park
  // order) or drop its empty slot, then evict the oldest parked chats past the cap.
  void parkMemResults(const std::string& chatId);
  // CUM-538: this chat's "[SPAWN REFUSED]" block ("" when none).
  std::string takeSpawnNotes(const std::string& chatId);
  // The [SPAWN REFUSED] block around a chat's refusal lines.
  static std::string refusedBlock(const std::string& notes, bool mayHaveRun);
  // Lane L: the [SPAWN CAPACITY] sentence on how keyed providers run subs.
  std::string syncCapacityText() const;
  // A billing refusal's owner notice: once per episode (billingNoticeDue, shared
  // with the embedding seam), held without taking the latch on a quiet turn.
  void deliverBillingNotice(const std::string& chatId, const std::string& host,
                            const std::string& text);
  // Lane L: an answer that was not applied (cut or unreadable) but whose
  // salvaged text claims sub-agents: the ledger line and the head's note, as
  // apply gives a promise without a spawn.
  void noteUnappliedSpawnClaim(const std::string& chatId);
  std::string buildDynamicContext();
  std::string buildStableCatalogs();
 public:
  // The head the next turn will run on - the ONE resolution runTurn and the
  // [YOUR MODEL] line use (explicit orchHost, else the first keyed priority
  // host, else the verified router head). The device sizes the RECENT
  // CONVERSATION window for this head (CUM-485) instead of a store-side guess
  // that does not know the router fallback.
  std::string headHost() const;
 private:
  std::string jobsSummaryText() const;
  static const char* channelOf(const std::string& chatId);
  void emitHead(uint8_t ringStatus);
  void deliver(const std::string& chatId, const std::string& text);
  uint32_t nowMs() const { return d_.platform.nowMs ? d_.platform.nowMs() : 0; }
  uint32_t freeHeap() const { return d_.platform.freeHeap ? d_.platform.freeHeap() : 0xFFFFFFFFu; }
  // Largest contiguous free INTERNAL block (CUM-404). Unwired => 0xFFFFFFFF, so the
  // largest-block guard is a no-op on a host/test build that does not script it.
  uint32_t largestFreeBlock() const {
    return d_.platform.largestFreeBlock ? d_.platform.largestFreeBlock() : 0xFFFFFFFFu;
  }

  Deps   d_;
  Tuning t_;

  volatile bool turnInFlight_ = false;
  volatile uint32_t turnStartMs_ = 0;   // nowMs() at TurnGuard ctor (stuck-turn reaper)
  std::string curChat_;                 // routing chatId of the in-flight turn
  volatile bool toolReplied_ = false;   // a mid-loop reply already answered this turn
  volatile bool turnTelegram_ = true;   // CUM-523: this turn's one Telegram sample (read cross-task)
  volatile bool turnFileSend_ = true;   // CUM-523: and whether files.send can deliver on it
  bool scheduledTurn_ = false;          // a Local Loops / synthesis (unattended) turn is running
  bool quietTurn_ = false;              // dream: suppress the bare-"Done." fallback this turn
  bool dreamTurn_ = false;              // the reserved dream loop: memory-only surface (lane M)
  uint32_t quietNoticesHeld_ = 0;       // engine notices a quiet turn held back (tests, diagnostics)
  std::string heldReply_;               // the dream turn's output, held (FireOutcome.held)
  // True while a quiet scheduled turn runs: every engine notice is held (deliver)
  // and the one-notice billing latch is left armed (lane M x lane K).
  bool holdingNotices() const { return scheduledTurn_ && quietTurn_; }
  // CUM-536: this turn's [FRESH RESULTS] include a result whose connector never
  // ran. Set by the entry wrappers from takeFreshResults; while set, the turn's
  // memory writes (mem_write[] and the loop's memory.write tool) are refused.
  bool unverifiedTurn_ = false;
  // Lane L: the entry wrappers fold sub-agent results into this turn (external
  // content); runTurn seeds externalContentSeen_ from it, and a tool whose
  // output is external (orch::toolTaint) sets it mid-turn. The leak-repair gate
  // keeps recovered actions only while it is false.
  bool freshInTurn_ = false;
  bool externalContentSeen_ = false;
  bool lastTurnSpoke_ = false;   // the last applied turn delivered a reply, an ask or a tool reply
  // Spend-attribution tag + hook source for the CURRENT turn, set by the entry
  // wrappers (handleMessage "turn"/Owner, maybeConsolidate "synthesis",
  // injectScheduledTurn "loop:<id>") and reset to the owner default after.
  std::string attribution_ = "turn";
  TurnSource  turnSource_ = TurnSource::Owner;

  nimbus::orch::TokenUsage sessionUsage_;   // running sum of billed tokens since boot
  uint32_t                 turnCount_ = 0;  // non-empty turns since boot
  nimbus::orch::TokenUsage lastTurnUsage_;  // real usage of the last completed turn (loops)
  std::string lastReply_;               // the last delivered reply (loop repeat-hash)
  // Owner-visible protection-override note for THIS turn - appended to the
  // delivered reply so a risk flip can never be silent (cleared per turn).
  std::string riskNote_;

  // The most recent composed World system prompt (for PROMPT? / diagnostics).
  std::string lastInstructions_;
  ClaimCheck claims_;               // lane S: this turn's effect ledger and claim check
  int freshSourced_ = 0;            // sub-agent results folded into this turn: with a lookup
  int freshUnsourced_ = 0;          // ...and live-data ones with none (set by the entry wrappers)
  // The most recent tool-loop transcript brief (Glass Box P3) - filled by
  // HeadTools::onBrief, consumed by the TurnDebugEv snapshot. Empty for a
  // single-shot turn that never entered a loop.
  std::string lastBrief_;
  // mem_query[] results from a chat's previous turn, parked for THAT chat's next
  // turn (deferred-result pattern). Keyed by chat id (lane M): an engine-global
  // slot leaked one principal's memory results into another chat's turn. At most
  // kPendingMemChats chats park results at once.
  struct PendingMem {
    std::string body;
    uint32_t seq = 0;   // park order: the oldest parked chat is evicted first
  };
  std::map<std::string, PendingMem> pendingMemResults_;
  uint32_t pendingMemSeq_ = 0;
  static constexpr size_t kPendingMemChats = 8;
  // A fallback switch recorded on the previous turn, surfaced to the model as a
  // one-line context note on the next turn (CUM-41). Consumed by buildDynamicContext,
  // which labels it with whose turn it was and how long ago (lane T): the 18 Oct
  // dream's unlabelled note was read by the next owner turn as about itself.
  std::string pendingFallbackPath_;    // "anthropic -> openai -> mistral" ("" = none)
  nimbus::orch::ErrorClass pendingFallbackReason_ = nimbus::orch::ErrorClass::None;
  std::string pendingFallbackWhose_;   // "nightly dream", "routine 'X'", ...
  uint32_t    pendingFallbackMs_ = 0;  // nowMs() when it was recorded
  std::string loopName_;               // the scheduled turn in flight (its routine name)

  // histV2 (conversation.h): the in-flight user row, a seedless turn's trigger
  // row (written before its first assistant row), and the out: kind of the
  // assistant row being captured.
  std::string curUserRowId_;
  std::string triggerText_, triggerTags_;
  bool triggerPending_ = false;
  std::string outKind_;
  bool nextSpoken_ = false;            // apply's speak() just ran: the next capture is the tts line
  bool replyPending_ = false;          // the turn has a reply apply has not captured yet
  void setTrigger(const std::string& text, const std::string& src);
  // The capture of a turn's own owner row (H1 tags, H7 duplicate source).
  // False = a delivery already in the history: the turn is dropped.
  bool captureOwnerRow(const std::string& chatId, const std::string& text, const std::string& fromTag);
  // H2/H3: the RECENT CONVERSATION block for this turn ("" = none).
  std::string historyBlock(const std::string& chatId, size_t highBytes);
  // Capture one assistant row with its out: kind (and the trigger row first).
  void captureAssistantRow(const std::string& chatId, const std::string& text, const char* outKind);
  void flushTrigger(const std::string& chatId);   // the pending trigger row, once
  // The chat's turns after o.startAfter, read a page at a time (repeats folded).
  std::vector<HistTurn> loadTurns(const std::string& chatId, const HistoryOptions& o, size_t maxRows) const;
  struct ApplyCapture;   // swaps apply's capture and speak hooks for one applyTurn

  // Lane T: the turn's spend per host and its failure chain - one allocation per
  // boot (large enough to land in PSRAM on the device), reset at every turn start.
  struct TurnFacts {
    nimbus::orch::TurnSpend    spend;
    nimbus::orch::FailureChain chain;
    bool                       metered = false;   // the spend reached the ledger
    bool                       switched = false;  // this turn recorded a [FALLBACK] switch
  };
  std::unique_ptr<TurnFacts> facts_;
  TurnFacts& facts();
  // Meter `spend` under `tag` through BudgetConfig::recordSpend (or, unwired, each
  // slot through recordTokens) - the one way spend reaches a ledger.
  void meterSpend(const nimbus::orch::TurnSpend& spend, const std::string& tag);
  // The model-facing label of the turn in flight ("nightly dream", ...).
  std::string turnLabel() const;
};

}  // namespace agent
