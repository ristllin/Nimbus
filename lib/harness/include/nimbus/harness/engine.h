#pragma once
#include <cstdint>
#include <functional>
#include <map>
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
#include "nimbus/orch/loops.h"          // FireOutcome (scheduled-turn result)
#include "nimbus/orch/token_usage.h"
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
// per mid-turn provider switch (from, to) for the owner notice.
using FabricLoopFn = std::function<bool(
    const std::vector<std::string>& hostList, const std::string& instructions,
    const std::string& inputs, std::string& outJson, std::string& err,
    const HeadTools& ht, nimbus::orch::TokenUsage* usage,
    const std::function<void(const std::string& fromHost,
                             const std::string& toHost)>& notify)>;

// Registry: host name -> ProviderTurnFn. An unknown host fails with the same
// "host 'X' unavailable" error the device lambda produced, so it fails over.
class ProviderHosts {
 public:
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

    // Associative recall for the user's message (device: memory::recall -
    // embed + VDB search, fail-open). Nullable => no recall.
    std::function<std::vector<std::string>(const std::string& userText,
                                           const nimbus::orch::Principal& who)> recall;
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
    // Compile-time model choice list per provider ([AVAILABLE MODELS] block).
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
  };

  // Heap floors, defaulted to src/agent/agent_config.h's values so the
  // portable lib never includes that header.
  struct Tuning {
    uint32_t turnHardFloor   = 28000;  // ORCH_TURN_HARD_FLOOR
    uint32_t recallMinHeap   = 28000;  // ORCH_RECALL_MIN_HEAP (== the turn hard floor)
    uint32_t autoTurnMinHeap = 30000;  // ORCH_AUTO_TURN_MIN_HEAP
    uint32_t loopMinHeap     = 28000;  // ORCH_LOOP_MIN_HEAP
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
  // the bare "Done." fallback is suppressed (ApplyState.quietFallback).
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

  nimbus::orch::TokenUsage lastTurnUsage() const { return lastTurnUsage_; }
  nimbus::orch::TokenUsage sessionUsage() const { return sessionUsage_; }
  uint32_t turnCount() const { return turnCount_; }

  // W6: paint/clear the "head" ring arc from OUTSIDE a turn (the main-loop
  // reconciler), so the orchestrator's arc persists while its children run.
  // Running (blue comet) when lit, Offline when cleared.
  void setHeadArc(bool lit);

 private:
  std::string takePendingMemResults();
  std::string buildDynamicContext();
  std::string jobsSummaryText() const;
  static const char* channelOf(const std::string& chatId);
  void emitHead(uint8_t ringStatus);
  void deliver(const std::string& chatId, const std::string& text);
  uint32_t nowMs() const { return d_.platform.nowMs ? d_.platform.nowMs() : 0; }
  uint32_t freeHeap() const { return d_.platform.freeHeap ? d_.platform.freeHeap() : 0xFFFFFFFFu; }

  Deps   d_;
  Tuning t_;

  volatile bool turnInFlight_ = false;
  volatile uint32_t turnStartMs_ = 0;   // nowMs() at TurnGuard ctor (stuck-turn reaper)
  std::string curChat_;                 // routing chatId of the in-flight turn
  volatile bool toolReplied_ = false;   // a mid-loop reply already answered this turn
  bool scheduledTurn_ = false;          // a Local Loops / synthesis (unattended) turn is running
  bool quietTurn_ = false;              // dream: suppress the bare-"Done." fallback this turn
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
  // The most recent tool-loop transcript brief (Glass Box P3) - filled by
  // HeadTools::onBrief, consumed by the TurnDebugEv snapshot. Empty for a
  // single-shot turn that never entered a loop.
  std::string lastBrief_;
  // mem_query[] results from the previous turn, parked for the next turn's
  // inputs (deferred-result pattern).
  std::string pendingMemResults_;
  // A fallback switch recorded on the previous turn, surfaced to the model as a
  // one-line context note on the next turn (CUM-41). Consumed by buildDynamicContext.
  std::string pendingFallbackNote_;
};

}  // namespace agent
