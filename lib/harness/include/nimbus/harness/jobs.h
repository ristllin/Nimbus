#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/attention.h"        // nimbus::attn::Event (portable, lib/core)
#include "nimbus/harness/fabric.h"   // HeavyFabric / ManagedAgentAdapter
#include "nimbus/harness/hooks.h"    // Hooks - onSpawn/onResult observers
#include "nimbus/harness/platform.h" // Platform (nowMs / freeHeap)
#include "nimbus/orch/journal.h"     // Journal / JobRecord
#include "nimbus/orch/psram_alloc.h" // WorkingAllocator - the poll-envelope slot
#include "nimbus/orch/result_envelope.h"
#include "nimbus/orch/turn.h"        // Spawn
#include "nimbus/orch/world.h"       // SessionInfo

// jobs - the sub-agent JOB MACHINERY, lifted from src/agent/orchestrator.cpp
// (Stage F). One JobEngine owns the pending-spawn queue, the one-dispatch-per-
// cycle gate, the round-robin poll with exponential backoff, the fresh-results
// store + per-completion synthesis clock, the deferred ring-reap scheduler, and
// cancel/await/sessionInfos - every timer, cap, and owner-visible string moved
// BYTE-IDENTICAL (test_harness_jobs pins the load-bearing ones). Synthesis
// itself (the consolidation TURN) stays device-side behind the injected
// `synthesize` closure; the engine only owns the clock that decides WHEN it
// runs vs when raw results are drained.
//
// Representation change (documented, internal-only): the device's PendingSpawn
// queue was heap-malloc'd per entry (free the turn heap while pending) and
// FreshResult text was strdup'd; here both live in plain std::vector by value -
// small (≤ kAgentMaxJobs entries each), simpler, and PSRAM-irrelevant on host.
// Consequence: the old "Out of memory queuing that agent." malloc-failure path
// no longer exists (vector capacity is reserved up front).
namespace agent {

// A stable ring key from a job tag (FNV-1a) - the attention Router keys jobs by a
// uint32, not a string, so hash the tag into that key space. Exposed so the
// device glue can key its head-turn arc ("head") on the same hash.
uint32_t keyFromTag(const char* tag);

class JobEngine {
 public:
  struct Deps {
    Platform platform;                        // nowMs + freeHeap used
    HeavyFabric* fabric = nullptr;            // dispatch/poll/cancel routing
    nimbus::orch::Journal* journal = nullptr; // durable job records (device owns store)
    // Lifecycle observers (all nullable, observer-only): onSpawn fires once per
    // successful dispatch; onResult on every polled state change (terminal flag
    // set on Done/Error/expired). The device deliberately does NOT wire these
    // to sfx/ring (those already ride the event/fire sinks) - they exist for
    // log breadcrumbs, tests, and the future web trace tab.
    Hooks hooks;

    // Owner delivery (device: Telegram reply queue + full-queue logging).
    std::function<void(const std::string& chatId, const std::string& text)> deliver;
    // Attention-event sink (device: attn::Router via g_sinks.event). Nullable.
    std::function<void(const nimbus::attn::Event&)> event;
    // Sound cue, execution-only: "spawn" (device: sfx AgentSpawn). Nullable.
    std::function<void(const char* cue)> fire;
    // Backend accent hue for ring events (device: adapter_factory backendHue).
    // Nullable => 255 (white / unknown provider).
    std::function<uint8_t(const char* backend)> backendHue;

    // Sub-session spend attribution (nullable): fired ONCE per job when a
    // terminal poll carries real provider token usage (today only the OpenAI
    // Responses poll reports it; the Anthropic events poll has none - no data
    // is invented). Device wires store::recordProviderTokens(backend, in, out,
    // "spawn:<backend>").
    std::function<void(const std::string& backend, uint32_t tokensIn, uint32_t tokensOut)>
        recordSpawnUsage;

    // Recent-results spill (nullable - Context Fabric Stage 1): called with the
    // FULL result text for EVERY finished sub-agent, before any clipping. The
    // returned ring tag ("sub:<jobtag>") is referenced by overflow stubs and
    // clip markers so a bounded view stays widenable via results.get. Device
    // wires orchestrator::resultsPut.
    // `ns` = the owning tenant (the spawning chat's namespace) - ring reads are
    // scoped to it, so one chat's sub-agent result cannot leak into another's.
    std::function<std::string(const char* tag, const char* model, const std::string& fullText,
                              const std::string& ns)>
        spillResult;

    // Config reads (device: store::). firstSubProvider = first key'd provider
    // in subPriority order.
    std::function<std::string()> subPriority;
    std::function<bool(const std::string& provider)> providerHasKey;
    std::function<std::string(const std::string& provider)> subModel;
    std::function<bool(const std::string& provider, const std::string& model)> modelIsValid;

    // Connector-aware routing: given a spawn's skill/connector id, return the
    // provider that HOSTS that connector (e.g. "mistral" for a Studio Notion
    // connector), or "" if none/ambiguous. Used only when the spawn carries no
    // explicit provider - so a connector task lands on the provider that can
    // actually run it (heavy connector work then executes on lab compute).
    // Device wires this to the connectors registry; nullable => no routing.
    std::function<std::string(const std::string& skill)> connectorProvider;

    // Dynamic-skill capsule resolver (roadmap P2: "the skill field becomes a
    // real injection, not just a provider hint"). Given a Directive's skill id,
    // return the capsule body to PREPEND to the sub-agent instruction - or ""
    // for no capsule, which keeps today's provider-hint passthrough EXACTLY
    // (the skill string still rides Directive.skill either way). Device wires
    // this to skills::spawnCapsule (SD capsules with inject: spawn|both);
    // nullable => never inject.
    std::function<std::string(const std::string& skillId)> resolveSkill;

    // Current local date-time string for sub-agent task briefs ("" / null =>
    // no injection). Sub-agents run on provider hosts with NO clock context -
    // without this a research brief's "today" fell back to the model's training
    // prior (a news verifier confidently mis-dated by six months, owner
    // 2026-07-24). Nullable for host tests / clockless callers.
    std::function<std::string()> nowString;

    // Rendered context block of the SPAWNING chat for the sub-agent brief
    // (Release B4): "[CONTEXT]\n<min(N msgs, C chars) window>". Sub-agents run
    // on provider hosts with a fresh session and previously received ONLY the
    // task text - no idea what the owner and head were discussing. Nullable /
    // empty => no injection (host tests, notifier mode).
    std::function<std::string(const std::string& chatId)> chatContext;

    // v4.0.0 attachments: read a FileStore doc's TEXT for dispatch-time
    // splicing ("" on miss/refusal - the brief then carries an honest
    // not-found note). The SPAWNING chat's id is threaded so the device can
    // enforce the SAME read boundary as the files.read tool (a member must not
    // attach another tenant's private doc): the device resolves chatId ->
    // principal and checks readableBy. Text extensions only. Nullable => attach
    // refs are noted, not spliced.
    std::function<std::string(const std::string& chatId, const std::string& project,
                              const std::string& name)>
        readDoc;
    // v4.0.0 auto-persist: save a finished sub-agent's FULL reply as a doc in
    // its run project (rec.prj). Returns a short OUTCOME line appended to the
    // fresh result - "[saved: <project>/<file>]" or "[persist FAILED: <why>]" -
    // the feedback that makes multi-stage planning real. Nullable => no persist.
    std::function<std::string(const std::string& project, const std::string& name,
                              const std::string& tag, const std::string& text,
                              const std::string& chatId)>
        persistResult;
    // v4.1 provider file capture: a finished sub-agent run produced a binary FILE
    // (e.g. a Mistral code_interpreter PDF), carried in the poll's
    // ResultEnvelope.artifacts[] as {backend, file_id, file_name}. STREAM it to
    // SD (/mem/files/<project>/<name>) and register it so files.list/files.send
    // see it, then return a short OUTCOME line appended to the fresh result
    // ("[file saved: p/f]" / "[file FAILED: why]"). ⛔ Runs on tg_poll, ONE TLS
    // at a time (the mistral poll is cache-local, so no TLS is held here) - the
    // OTA streaming-download pattern, never a second concurrent connection.
    // Nullable => file refs are noted, not fetched.
    std::function<std::string(const std::string& backend, const std::string& fileId,
                              const std::string& fileName, const std::string& project,
                              const std::string& name, const std::string& tag,
                              const std::string& chatId)>
        fetchArtifact;

    // Synthesis stays device-side (it runs a full consolidation turn); the
    // engine owns only the coalesce/fallback clock that triggers it.
    std::function<void(const std::string& chatId)> synthesize;
    std::function<bool()> turnInFlight;
    // Reserved for parity with the device's ttsAnnounce helper; no job path
    // currently fires it (the device function is uncalled today). Nullable.
    std::function<void(const std::string& text)> ttsAnnounce;
  };

  // Numeric caps, defaulted to src/agent/agent_config.h's values so the
  // portable lib never includes that header.
  struct Tuning {
    int      maxActiveInflight = 4;      // AGENT_MAX_ACTIVE_INFLIGHT
    uint32_t dispatchMinHeap   = 28000;  // ORCH_DISPATCH_MIN_HEAP
    uint32_t pollIntervalMs    = 15000;  // AGENT_POLL_INTERVAL_MS
  };

  // Synthesis clock + reap grace (byte-moved constants).
  static constexpr uint32_t kSynthCoalesceMs = 3000;   // burst window: batch near-simultaneous
                                                       // completions into ONE synthesis turn
  static constexpr uint32_t kSynthFallbackMs = 60000;  // synthesis stuck (heap/provider) ->
                                                       // drain raw so results are never lost
  static constexpr uint32_t kReapGraceMs     = 6000;

  JobEngine(Deps d, Tuning t);
  // (Two ctors, not one defaulted arg: a `Tuning t = {}` default needs the
  // NSDMIs before the class is complete - clang rejects it.)
  explicit JobEngine(Deps d) : JobEngine(std::move(d), Tuning()) {}

  // Queue a spawn (job-limit refusal + sfx cue); the pump dispatches one/cycle.
  void enqueueSpawn(const nimbus::orch::Spawn& s, const std::string& chatId, bool quiet = false);

  // The JOB parts of the device's pollJobs(): reap, per-completion synthesis
  // clock, one dispatch, loop-closure synthesis, round-robin poll. Returns the
  // active job count (journal count).
  int pump();

  bool cancel(const char* tagOrJobId);
  // Aim the round-robin at the awaited tag + poll now (turn `await[0]`).
  void awaitTag(const std::string& firstTag);
  std::vector<nimbus::orch::SessionInfo> sessionInfos() const;

  // Fresh sub-agent results parked for the next consolidation turn.
  bool hasFreshResults() const { return !fresh_.empty(); }
  std::string takeFreshResults();
  void addFreshResult(const char* tag, const char* model, const std::string& text,
                      const std::string& ns = std::string());
  const std::string& freshChatId() const { return freshChatId_; }

  void noteSpawned() { nextDispatchAt_ = d_.platform.nowMs(); }  // dispatch-now nudge
  void scheduleReap(const char* tag, uint32_t graceMs = kReapGraceMs);
  void reapDone();
  void setAttnHoldMs(uint32_t ms) { attnHoldMs_ = ms; }  // fed from Param::AttnHoldMs
  int  activeJobCount() const;
  // Total live children = dispatched/running (journal) PLUS not-yet-dispatched
  // (pending queue) PLUS a dispatch in progress. The head-turn arc stays lit
  // while this is > 0 (W6), so it must cover every window: the entry is popped
  // from pending_ BEFORE dispatchSpawn and the journal record lands only AFTER
  // the provider call returns - for a synchronous Mistral sub that call IS the
  // whole run (≤60 s), so without dispatching_ the count read 0 mid-run and the
  // main-loop reconciler cleared the arc while the child was actually working.
  // Cross-task note: read from the main loop while tg_poll mutates - each term
  // is a single aligned word (int/size_t/bool), so a read is atomic on the S3;
  // a momentarily stale SUM is corrected by the next 5 s tick (same contract as
  // TurnEngine::turnInFlight()).
  int  activeCount() const {
    return (d_.journal ? d_.journal->count() : 0) + (int)pending_.size() +
           (dispatching_ ? 1 : 0);
  }
  int  pendingCount() const { return (int)pending_.size(); }  // queued-not-yet-dispatched (W5)

 private:
  // Pending-spawn queue entry (fixed char[] kept from the device struct so the
  // field truncation caps stay byte-identical).
  // ⚠ NON-POD (task/attach) - zero via these member initializers, NEVER memset.
  // enqueueSpawn used to memset the whole struct after emplace_back, nulling the
  // std::string's internal pointer; a LONG task survived (move-assign steals the
  // heap buffer) but a SHORT one (<=15 B, SSO) took the copy path and wrote
  // through the nulled pointer -> StoreProhibited. Live-caught 2026-08-12: a 3B
  // local model spawned with task "web_search" and panicked the board - months
  // of long-tasked cloud-model spawns never tripped it.
  struct PendingSpawn {
    char provider[16] = {};
    char model[40] = {};
    char category[16] = {};
    char skill[24] = {};
    char name[24] = {};   // model-chosen display name (owner-facing everywhere)
    char note[80] = {};
    char chatId[32] = {};
    char project[25] = {};  // FileStore run tag ("" = no auto-persist) - v4.0.0
    // v4.0.0: task is a heap std::string (PSRAM-backed on device for >=128 B) -
    // kSpawnTaskMax went 1024 -> 4096 and 6 pending x 4 KB char arrays would sit
    // on internal SRAM otherwise. Bounded by the parse caps upstream.
    std::string task;
    std::vector<std::string> attach;  // "<project>/<name>" docs to splice at dispatch
    bool quiet = false;  // scheduled/loop turn: suppress the "On it." spawn ack (in-RAM only)
  };
  // ringTag is the ACTUAL recent-results ring tag spillResult() returned (e.g.
  // "sub:job0007") - carried so an overflow stub points at the real entry instead
  // of reconstructing "sub:"+tag, which only matches while job tags stay short and
  // punctuation-free (prism 2026-08-05: make it correct-by-construction).
  struct FreshResult { std::string tag, model, text, ringTag; };
  struct ReapEntry { uint32_t key; uint32_t dueAt; };

  void dispatchSpawn(const PendingSpawn& p);
  void deliver(const std::string& chatId, const std::string& text);
  void emitJobState(const char* tag, const char* backend, nimbus::orch::JobState st);
  void emitJobCleared(uint32_t key);
  std::string firstSubProvider() const;
  uint32_t nowMs() const { return d_.platform.nowMs ? d_.platform.nowMs() : 0; }
  uint32_t freeHeap() const { return d_.platform.freeHeap ? d_.platform.freeHeap() : 0xFFFFFFFFu; }

  Deps   d_;
  Tuning t_;

  uint32_t tagSeq_  = 0;
  int      rrIndex_ = 0;                 // round-robin poll cursor
  uint32_t nextPollAt_ = 0;              // global poll gate
  uint32_t pollBackoffMs_;               // AGENT_POLL_INTERVAL_MS baseline

  // Stuck-job watchdog (RAM only - JobRecord is an NVS-persisted POD whose
  // layout must not change; a reboot resetting these clocks just restarts the
  // generous ceilings). Keyed by tag; entries clear on any terminal outcome.
  struct JobWatch { char tag[12] = {}; uint32_t firstErrMs = 0; bool parked = false; };
  JobWatch watch_[nimbus::orch::kAgentMaxJobs];
  JobWatch& watchFor(const char* tag);   // find-or-claim (evicts a free/stale slot)
  void watchClear(const char* tag);

  std::vector<PendingSpawn> pending_;
  uint32_t nextDispatchAt_ = 0;
  bool     dispatching_ = false;   // set around dispatchSpawn in pump (W6 arc gap)

  std::vector<FreshResult> fresh_;
  std::string freshChatId_;
  uint32_t freshSinceMs_ = 0;            // millis() of the OLDEST un-synthesized fresh
                                         // result; 0 = none pending (R5b topology)

  std::vector<ReapEntry> reap_;
  // Error arcs are calls-to-action: hold them for the tunable attention window
  // rather than Done's short grace - but NEVER forever.
  uint32_t attnHoldMs_ = 300000;         // 5 min default, mirrors the Param preset

  // 4 KB+ envelope: a member, NOT the poll-caller's stack (device: 16 KB task).
  // The poll envelope (16.5 KB with the v4 reply cap) - one PSRAM-backed slot
  // via WorkingAllocator, NOT an internal-SRAM member (the JobEngine is a
  // function-local static on the device).
  std::vector<nimbus::orch::ResultEnvelope,
              nimbus::orch::WorkingAllocator<nimbus::orch::ResultEnvelope>> envSlot_;
  nimbus::orch::ResultEnvelope& env_ref() { return envSlot_[0]; }
};

}  // namespace agent
