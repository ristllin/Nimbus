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

// L19-36: the sentence every sub-agent brief ends with: the reply cap, stated (the
// sub was never told one existed, and wrote reports past it that reached the head
// cut, presented as complete).
inline constexpr const char* kSubReplyCapNote =
    "\n\n[Reply size: the first 16 KB of your reply reaches the head directly; a longer reply is "
    "saved whole as a file the head can read, so finish your full report and do not pad or repeat it.]";

// A stable ring key from a job tag (FNV-1a) - the attention Router keys jobs by a
// uint32, not a string, so hash the tag into that key space. Exposed so the
// device glue can key its head-turn arc ("head") on the same hash.
uint32_t keyFromTag(const char* tag);

// The job engine's owner delivery composed with episodic capture (CUM-482).
// Job-engine messages - spawn acks, "Couldn't start that agent on ...", sub
// failures, raw fallbacks - bypassed the chat history, so the web chat (which
// renders that history) never showed them and they vanished on reload. The
// device wires JobEngine::Deps::deliver through this: `send` delivers and
// returns false ONLY when the outbound screen suppressed the message; `capture`
// records it to the chat's history. A screened-out message is never captured
// (its text must not resurface through the history window).
std::function<void(const std::string& chatId, const std::string& text)> capturingDelivery(
    std::function<bool(const std::string& chatId, const std::string& text)> send,
    std::function<void(const std::string& chatId, const std::string& text)> capture);

// The tag on every captured job-engine delivery row. Those rows render in the web
// chat but stay OUT of every model-facing conversation read (the head's window,
// the sub-agent brief, the fold): a raw fallback carries sub-agent web content,
// and the model never read job messages there before they were captured.
extern const char kJobDeliveryTag[];   // "src:jobs"
bool isJobDelivery(const std::string& rowTags);   // whole comma-list item match

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
    // terminal poll carries real provider token usage (the OpenAI Responses poll,
    // and the Anthropic session read at a finished poll; no data is invented). The
    // synchronous subs are metered at their dispatch instead (seam_meter.h). The
    // device and the hosted instance report it to the seam meter (the monthly spend
    // ceiling, filed under "spawn:<backend>"). `model` is the job's JobRecord.model:
    // the sub-agent's model or selector, or the literal "default" when none was
    // resolved (resolved to the provider's sub model). The usage rides as a
    // TokenUsage (prompt, completion and the cache traffic, in the provider's native
    // meaning), so every part prices at its rate.
    std::function<void(const std::string& backend, const std::string& model,
                       const nimbus::orch::TokenUsage& usage)>
        recordSpawnUsage;

    // Lane QW4 round 3: the background stop list (helpers whose stop is not yet
    // confirmed), saved whole after every change and read once at the first pump, so a
    // restart keeps asking them to stop and counts them. Nullable: RAM only.
    std::function<std::string()> loadStopping;
    std::function<bool(const std::string& text)> saveStopping;

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
    // in subPriority order, then the router keys (cumulo, then zai).
    std::function<std::string()> subPriority;
    std::function<bool(const std::string& provider)> providerHasKey;
    std::function<std::string(const std::string& provider)> subModel;
    std::function<bool(const std::string& provider, const std::string& model)> modelIsValid;
    // The provider's model choice list as CSV - the same derivation modelIsValid
    // reads (device: effectiveModelChoices). Lets a spawn whose model the provider
    // does not list fall back to a model that IS listed (validated coercion);
    // "" means the provider has no list to check against (custom). Nullable =>
    // the old fallback to the provider's sub-agent default.
    std::function<std::string(const std::string& provider)> modelChoices;

    // Connector-aware routing: given a spawn's skill/connector id, return the
    // provider that HOSTS that connector (e.g. "mistral" for a Studio Notion
    // connector), "any" for a connector any provider runs, or "" when the skill is
    // no enabled connector. A provider is used only when the spawn carries no
    // explicit provider - so a connector task lands on the provider that can
    // actually run it (heavy connector work then executes on lab compute) - and a
    // connector task never runs on the Cumulo router (CUM-541).
    // Device wires this to the connectors registry; nullable => no routing.
    std::function<std::string(const std::string& skill)> connectorProvider;

    // Where a FILE task (skill "code_interpreter" or "deliver-pdf", no explicit
    // provider) runs: the provider of the device's PDF route ("openai" for its
    // code_interpreter, "anthropic" for a sandbox whose outputs the device
    // collects, "" when no sandbox route exists). Mistral's sandbox errors on PDF,
    // so a file task never lands there by default (critique A7). Nullable: then
    // openai when keyed, else the usual routing.
    std::function<std::string()> fileSpawnProvider;

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
    // Unix seconds for the finished-job record (L19-35; 0 = clock unset). Nullable.
    std::function<uint32_t()> nowEpoch;

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
    uint32_t dispatchMinHeap   = 12000;  // ORCH_DISPATCH_MIN_HEAP (== the turn hard floor:
                                         // a dispatch makes the same one TLS handshake)
    uint32_t pollIntervalMs    = 15000;  // AGENT_POLL_INTERVAL_MS
  };

  // Synthesis clock + reap grace (byte-moved constants).
  static constexpr uint32_t kSynthCoalesceMs = 3000;   // burst window: batch near-simultaneous
                                                       // completions into ONE synthesis turn
  static constexpr uint32_t kSynthFallbackMs = 60000;  // synthesis stuck (heap/provider) for this
                                                       // long AFTER the batch drained -> deliver
                                                       // raw so results are never lost (CUM-480)
  static constexpr uint32_t kReapGraceMs     = 6000;

  JobEngine(Deps d, Tuning t);
  // (Two ctors, not one defaulted arg: a `Tuning t = {}` default needs the
  // NSDMIs before the class is complete - clang rejects it.)
  explicit JobEngine(Deps d) : JobEngine(std::move(d), Tuning()) {}

  // Queue a spawn (job-limit refusal + sfx cue); the pump dispatches one/cycle.
  // Returns true only when the spawn joined the queue: the turn's effect ledger
  // (the claim check) and spawnedOut count accepted spawns, never intents.
  bool enqueueSpawn(const nimbus::orch::Spawn& s, const std::string& chatId, bool quiet = false);

  // The JOB parts of the device's pollJobs(): reap, per-completion synthesis
  // clock, one dispatch, loop-closure synthesis, round-robin poll. Returns the
  // active job count (journal count).
  int pump();

  bool cancel(const char* tagOrJobId);
  // Lane QW4 (CUM-774): true while a job's stop is asked but not yet confirmed by its
  // provider (it is on the background stop list: asked again, polled, counted).
  bool stopping(const char* tagOrJobId) const;
  int stoppingCount() const { return (int)stopping_.size(); }
  static constexpr int kMaxStopping = 8;   // the stop list's room (a full one counts the estimate)
  // Aim the round-robin at the awaited tag + poll now (turn `await[0]`).
  void awaitTag(const std::string& firstTag);
  std::vector<nimbus::orch::SessionInfo> sessionInfos() const;
  // L19-35: the jobs that finished before this boot (the journal's finished ring), one
  // line each, newest first, for the turn context; "" when there are none.
  std::string finishedBeforeRestartText() const;

  // ringTag is the ACTUAL recent-results ring tag spillResult() returned (e.g.
  // "sub:job0007") - carried so an overflow stub points at the real entry instead
  // of reconstructing "sub:"+tag, which only matches while job tags stay short and
  // punctuation-free (prism 2026-08-05: make it correct-by-construction).
  // `text` is the (UTF-8 capped) finding itself; `hint` is the model-facing
  // widen handle appended after it (" [full N B: results.get(...)]", or "") - kept
  // apart so the owner rendering can drop it while the seed stays byte-identical.
  struct FreshResult {
    std::string tag, model, text, hint, ringTag;
    std::string provenance;    // " [tools: n, web: m]" / " [no tools ran]" ("" = engine line)
    bool failed = false;   // an engine failure line, not a sub-agent's finding
    bool unverified = false;   // its connector never ran (CUM-536), or a live task ran no tool
  };
  // Fresh sub-agent results parked for the next consolidation turn.
  bool hasFreshResults() const { return !fresh_.empty(); }
  // L24: a turn takes these before it knows it can run. One that then ends for memory
  // before asking any provider puts back what it took (a copy taken just before the
  // take), in front of any that arrived since, so the next turn sees them; never a drop.
  // The stuck-synthesis clock comes back as the take found it.
  std::vector<FreshResult> peekFresh() const { return fresh_; }
  void restoreFresh(std::vector<FreshResult> taken);
  // The same for a chat's spawn-refusal notes (what takeSpawnNotes returned).
  void restoreSpawnNotes(const std::string& chatId, const std::string& notes, bool mayHaveRun);
  // `unverified` (optional, CUM-536) reports whether any taken result opened with
  // the connector-not-called marker, so the turn that reads them can refuse to
  // store their contents as memory.
  std::string takeFreshResults(bool* unverified = nullptr);
  // The same, also counting the results by provenance for the claim check:
  // `sourced` had a lookup behind them, `unsourced` are marked unverified.
  std::string takeFreshResults(bool* unverified, int* sourced, int* unsourced);
  // The same parked results rendered for the OWNER, without consuming them: the
  // raw fallback's text when the summary step cannot run (CUM-480). A one-line
  // preamble, then each sub-agent's title and its findings - none of the
  // model-facing scaffolding ([FRESH RESULTS] header, job tags, results.get
  // handles). Bounded like the seed: past the budget a result folds to one line.
  std::string ownerResultsText() const;
  // Deliver an owner-facing results text through the job engine's own delivery
  // sink. The synthesis turn's failure path uses it so its raw fallback lands
  // exactly like the stuck-synthesis one (same seam, same device handling).
  void deliverResults(const std::string& chatId, const std::string& text) { deliver(chatId, text); }
  // `failed` marks the engine's OWN failure lines (provenance out of band: a
  // sub-agent reply that merely starts with "FAILED: " is still a finding).
  // `unverified` marks a result whose task asked for a connector that never ran
  // (CUM-536); the text already opens with the honest marker.
  void addFreshResult(const char* tag, const char* model, const std::string& text,
                      const std::string& ns = std::string(), bool failed = false,
                      bool unverified = false);
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

  // CUM-538: spawns that never started (queue full, no provider, the provider
  // refused) - the owner hears each at once, but the head used to learn nothing
  // and could claim a capacity it never had. One line per refusal with the real
  // reason, kept per SPAWNING chat: only that chat's next turn reads (and
  // consumes) them, so one tenant's task text never reaches another's turn. The
  // byte budget is per chat too, so one chat's unread backlog never strips
  // another chat's refusal of its reason. A dispatch that timed out is a "may
  // have started" line, not a refusal: the sub most likely ran and its result is
  // lost, so a re-spawn would pay for the same work twice. `mayHaveRun`
  // (optional) reports whether the taken notes carry such a line.
  std::string takeSpawnNotes(const std::string& chatId, bool* mayHaveRun = nullptr);
  // Lane L: a spawn intent the turn could not hand to the queue (no task, an
  // unreadable spawn list, over the per-turn cap, a reply that promised one)
  // - noted for the chat's next turn exactly like a refusal, and kept in the
  // spawn ledger. The owner line rides the turn's own reply (apply).
  void noteSpawnIntentRefused(const std::string& chatId, const std::string& name,
                              const std::string& task, const std::string& reason);
  // The spawn ledger (UNIFIED 3.17): the device's own record of the sub-agents a
  // chat started recently - queued, running, finished, failed or refused - from
  // the job table, so the head never has to keep that in its free-text notes
  // and re-check it with file or session tools. "" when the chat has none.
  std::string spawnLedgerText(const std::string& chatId) const;
  static constexpr size_t kLedgerMax = 12;              // entries kept (all chats)
  static constexpr uint32_t kLedgerWindowMs = 2u * 60 * 60 * 1000;   // shown for 2 h
  // Sub-agents whose dispatch timed out recently (within kMaybeRunningMs): they
  // may still be running on their provider with no job here to wait for. The
  // [SPAWN CAPACITY] line counts them as possibly running.
  int maybeRunningCount() const;
  static constexpr uint32_t kMaybeRunningMs = 10u * 60 * 1000;

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
  // Which dispatched jobs asked for live data (a status, today's figures):
  // read when the result lands, RAM only (a reboot forgets it: no marker).
  struct LiveTag { char tag[24]; bool live; };
  struct ReapEntry { uint32_t key; uint32_t dueAt; };

  void dispatchSpawn(const PendingSpawn& p);
  // Where a spawn runs: the provider (explicit hint, connector routing, then
  // firstSubProvider), its adapter and the model. A provider with no key of its
  // own runs on the Cumulo router when the router offers its model (CUM-541).
  // False when it cannot start; the owner was told why and the head noted it.
  bool routeSpawn(const PendingSpawn& p, std::string& provider, std::string& model,
                  ManagedAgentAdapter*& adapter);
  // L19-8: a provider the one park closed gets no spawn: move to the first open
  // sub provider, or refuse with the park's words. False when refused.
  bool moveOffClosedProvider(const PendingSpawn& p, const std::string& host, std::string& provider,
                             std::string& model);
  // L19-36: save a reply longer than the envelope whole to the card and end the
  // envelope with a marker naming the file and the real size. True when saved.
  bool spillWholeReply(const nimbus::orch::JobRecord& rec, const std::string& whole, size_t dropped,
                       std::string* savedPath = nullptr);
  // L19-35: the job reached a terminal state: keep it in the journal's finished ring
  // (with the file its result was saved to), then mark its record seen.
  void finishJob(const nimbus::orch::JobRecord& rec, const std::string& artifact,
                 nimbus::orch::JobState final);
  // SPAWN-JOURNAL-DURABILITY: the ack for a job whose record did not store.
  void deliverNotDurableAck(const PendingSpawn& p, const std::string& chatId, const std::string& suffix);
  std::string spawnProvider(const PendingSpawn& p) const;
  bool keylessToRouter(std::string& provider, std::string& model) const;
  // Validated coercion (subagents F5): the asked model when the provider lists
  // it, else the provider's sub default when listed, else the cheapest listed
  // model by the built-in price table, never the first (the priciest on most
  // lists). A substitution is named on the ledger line. False when no listed model
  // has a known price (the owner and the head are told why).
  bool coerceSpawnModel(const PendingSpawn& p, const std::string& provider, std::string& model);
  // The provider's sub-agent default runs when the provider offers it: in its
  // model list, or accepted by the one validity check (a stored pick is never
  // refused - lane F2). Split out of coerceSpawnModel for the CCN ceiling.
  bool subDefaultIsRunnable(const std::string& provider, const std::string& sub,
                            const std::string& list) const;
  void refuseUnpricedModel(const PendingSpawn& p, const std::string& provider,
                           const std::string& asked, const std::string& sub,
                           const std::string& why);
  std::string routerOffer(const std::string& provider, const std::string& model) const;
  void refuseKeyless(const PendingSpawn& p, const std::string& provider, const std::string& model);
  std::string connectorHost(const PendingSpawn& p) const;
  void refuseConnector(const PendingSpawn& p, const std::string& provider, const std::string& host);
  bool hasKey(const std::string& provider) const {
    return d_.providerHasKey && d_.providerHasKey(provider);
  }
  std::string subModelOf(const std::string& provider) const {
    return d_.subModel ? d_.subModel(provider) : std::string();
  }
  void noteSpawnRefused(const std::string& chatId, const char* name, const std::string& task,
                        const std::string& reason, bool mayHaveRun = false);
  // The Done text exactly as every copy must carry it (CUM-536 marker included).
  std::string deliveredReply(const nimbus::orch::JobRecord& rec, bool& unverified);
  // One entry per chat with unread notes (at most kSpawnNoteChats); `text` is that
  // chat's lines, bounded to kSpawnNotesMax bytes plus one fold line.
  struct SpawnNotes {
    std::string chatId, text;
    bool folded = false;       // budget spent: nothing more is listed for this chat
    bool mayHaveRun = false;   // a timed-out dispatch is among the lines
  };
  std::vector<SpawnNotes> spawnNotes_;
  // The spawn ledger (lane L). PSRAM-backed (WorkingAllocator): it is RAM-only
  // and bounded, and the JobEngine lives in internal SRAM on the device.
  enum class LedgerState : uint8_t { Queued, Running, Done, Failed, Refused, Cancelled };
  struct LedgerEntry {
    std::string chatId, name, task, tag, provider, model, asked, reason;
    std::string swapped;   // why the model that runs is not the one asked for ("" = none)
    LedgerState state = LedgerState::Queued;
    uint32_t atMs = 0;
    bool notDurable = false;   // its journal record did not store: a restart loses it
  };
  std::vector<LedgerEntry, nimbus::orch::WorkingAllocator<LedgerEntry>> ledger_;
  void ledgerQueued(const PendingSpawn& p);
  void ledgerStarted(const PendingSpawn& p, const char* tag, const std::string& provider,
                     const std::string& model, bool durable = true);
  void ledgerSwapped(const PendingSpawn& p, const std::string& why);
  LedgerEntry* queuedEntry(const PendingSpawn& p);   // this spawn's Queued row, or nullptr
  void ledgerRefused(const std::string& chatId, const std::string& name, const std::string& task,
                     const std::string& reason);
  void ledgerFinished(const char* tag, LedgerState st, const std::string& reason = std::string());
  static std::string ledgerLine(const LedgerEntry& e, uint32_t now);
  // Found or added (bounded); nullptr when there is no room for a plain refusal.
  SpawnNotes* spawnNotesFor(const std::string& chatId, bool mayHaveRun);
  // Dispatch-timeout times, <= kAgentMaxJobs, none older than kMaybeRunningMs
  // (mutable: the const count drops expired entries too).
  mutable std::vector<uint32_t> timedOutAt_;
  void expireTimeouts() const;
  void noteDispatchTimeout();
  void deliver(const std::string& chatId, const std::string& text);
  void emitJobState(const char* tag, const char* backend, nimbus::orch::JobState st);
  void emitJobCleared(uint32_t key);
  std::string firstSubProvider() const;
  // ...and the park's words for the first keyed provider it skipped as closed.
  std::string firstSubProvider(std::string* closedWhy) const;
  bool subProviderRuns(const std::string& provider, std::string* closedWhy) const;
  uint32_t nowMs() const { return d_.platform.nowMs ? d_.platform.nowMs() : 0; }
  uint32_t freeHeap() const { return d_.platform.freeHeap ? d_.platform.freeHeap() : 0xFFFFFFFFu; }

  Deps   d_;
  Tuning t_;

  uint32_t tagSeq_  = 0;
  bool     tagSeeded_ = false;           // tagSeq_ moved past every tag the journal holds
  uint32_t notDurableNoteMs_ = 0;        // the last "will not survive a restart" line
  bool     notDurableNoted_ = false;
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

  // Lane QW4 (CUM-774, CUM-778; round 3, merge review 1-4): the background stop list (see
  // jobs.cpp). Saved fields: tag, jobId, backend, model, chatId, label, prepaid (the
  // session-estimate tokens already counted for it); the clocks are RAM.
  struct Stopping {
    std::string tag, jobId, backend, model, chatId, label;
    uint32_t prepaid = 0;
    uint8_t tries = 0;
    uint32_t cancelAtMs = 0, pollAtMs = 0, pollWaitMs = 0, firstErrMs = 0;
  };
  std::vector<Stopping> stopping_;
  bool stoppingLoaded_ = false;
  bool stopJob(const nimbus::orch::JobRecord& rec);   // true: confirmed or gone
  void markStopping(const nimbus::orch::JobRecord& rec);
  void finishStopped(const nimbus::orch::JobRecord& rec);
  void toStopList(const nimbus::orch::JobRecord& rec, uint32_t cancelAtMs, uint8_t tries, uint32_t prepaid);
  bool keepStopping(Stopping e);
  void keepUnseenRun(const char* tag, const PendingSpawn& p, const std::string& model);
  void loadStopList();
  void saveStopList();
  void serviceStopList();
  void askStop(size_t i);
  void pollStop(size_t i);
  void stopUnreachable(size_t i);
  void countFinal(const Stopping& e);
  void countEstimate(const std::string& backend, const std::string& model);
  void endStop(size_t i);
  void stopFrozenHelpers();
  std::vector<nimbus::orch::JobRecord> frozenHelpers(bool ceiling) const;
  uint32_t frozenCheckedAt_ = 0;
  bool frozenChecked_ = false;

  std::vector<PendingSpawn> pending_;
  uint32_t nextDispatchAt_ = 0;
  bool     dispatching_ = false;   // set around dispatchSpawn in pump (W6 arc gap)

  std::vector<FreshResult> fresh_;
  std::vector<LiveTag> liveTags_;   // capped (kLiveTagsMax), oldest dropped
  void noteLiveTag(const char* tag, bool live);
  bool liveFor(const char* tag) const;
  std::string freshChatId_;
  // The batch DRAINED (fresh results parked and no live/queued/dispatching
  // work) at drainedAtMs_. The stuck-synthesis clock runs from here, never from
  // the first result: a fan-out whose subs finish minutes apart is not "stuck"
  // just because its first result is old (CUM-480).
  bool     drained_ = false;
  uint32_t drainedAtMs_ = 0;
  // drained_ as the last take found it: restoreFresh puts it back with the results, so
  // a synthesis refused for memory on every pump still meets the stuck fallback (the
  // take re-arms the clock; without this the fallback never came).
  bool     drainedBeforeTake_ = false;
  void deliverOwnerFallback();           // the stuck-synthesis raw delivery

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
