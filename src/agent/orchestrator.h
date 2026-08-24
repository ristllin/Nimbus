#pragma once
#include "nimbus/orch/tool_registry.h"  // Principal (results-ring reads are ns-scoped)
#include <Arduino.h>

#include <vector>

#include "adapter.h"
#include "nimbus/attention.h"
#include "nimbus/orch/device_actions.h"  // ValidatedAction (DeviceSink payload)
#include "nimbus/orch/rbac.h"            // Role / TenantStore (v3.7.0)
#include "nimbus/orch/world.h"  // SessionInfo (session.list / running-sessions digest)
#include "nimbus/orch/token_usage.h"  // TokenUsage - Phase 0 cost seam (lastTurnUsage)
#include "nimbus/orch/loops.h"        // FireOutcome - Local Loops scheduled-turn result

// orchestrator - the Head-Orchestrator-v2 turn loop for Nimbus, ported from
// Nuage-Solide src/agent/orchestrator.{h,cpp}. Single-task by design: handleMessage()
// and pollJobs() are SYNCHRONOUS and run inside the Telegram poll task between
// long-polls, so only one TLS session is ever open (the Telegram socket is closed
// first). On the S3 + PSRAM this constraint relaxes (own task behind a flag,
// plan §3.7); ported single-task first as the safe default.
//
// INTEGRATION (plan §3.6 - the load-bearing change vs Nuage): the orchestrator does
// NOT own the display/LED. Sub-session states flow OUT as nimbus::attn::Event via a
// caller-supplied sink, so they land on the SAME attention Router / ring path as
// Notifier jobs. main.cpp installs the sink (routing each Event into its
// attn::Router, recomposing the ring, feeding ScreenIntents to the scheduler) and the
// Telegram/TTS sinks. The orchestrator itself pulls in NO Router / renderer.
//
// The turn parse + validation, two-part memory, journal, and device-action
// classification are the PORTABLE core (lib/core nimbus::orch::*); this file is the
// device glue that drives adapters, TLS failover, and the sinks.

namespace agent {
namespace orchestrator {

// ---- caller-supplied sinks (installed by main.cpp) --------------------------

// Emit a job/voice/ask event onto the attention Router (SAME ring path as Notifier).
using EventSink = void (*)(const nimbus::attn::Event& e);

// Deliver text to the owner over Telegram (thread-safe queue on the device). Returns
// false if the message could NOT be enqueued (queue full) so the caller can log the
// drop instead of losing the owner's reply silently.
using SendSink = bool (*)(const String& chatId, const String& text);

// Speak a short result aloud (opt-in). LIVE-GATED + bench-broken: the audio path
// is unverified here - see the note in orchestrator.cpp. Nullable.
using SpeakSink = void (*)(const String& text);

// Execute a VALIDATED device action (led / lights / reboot - the ring/power side
// effects the orchestrator itself must not own, per the §3.6 split). Called on the
// poll task with an action validateAction() already allowed + clamped; the main.cpp
// implementation stages it thread-safely and the main loop applies it. Nullable
// (actions log as no-ops when unset - the pre-P5 behavior).
using DeviceSink = void (*)(const nimbus::orch::ValidatedAction& a);

struct Sinks {
  EventSink  event  = nullptr;  // -> attn::Router (required for ring/panel feedback)
  SendSink   send   = nullptr;  // -> telegram::send (required to answer the owner)
  SpeakSink  speak  = nullptr;  // -> audio TTS (optional; bench-broken here)
  DeviceSink device = nullptr;  // -> ring/power executor (led/lights/reboot)
};

// How long a FAILED sub-agent's red arc holds on the ring before it is reaped
// (mirrors the Notifier's call-to-action hold; main.cpp feeds Param::AttnHoldMs
// so the web/menu tunable governs both modes). Done arcs keep the short grace.
void setAttnHoldMs(uint32_t ms);

// ---- lifecycle --------------------------------------------------------------

// Wire the fabric + sinks, load the journal + two-part memory. Call once when
// Orchestrator mode starts (after arbiter::begin() + fabricInit()).
void begin(HeavyFabric* fabric, const Sinks& sinks);

// ---- turn entry points (SYNCHRONOUS - call from the Telegram poll task) ------

// Handle an incoming Telegram message: run one orchestrator turn (reply vs spawn),
// fold any fresh sub-agent results in, apply memory/device/spawn/await/reply.
void handleMessage(const String& text, const String& fromName, const String& chatId);

// Resolve a pending "needs you" ask (the owner engaged). Call from the owner-input
// paths that don't route through handleMessage (e.g. the on-device voice reply).
void clearAsk();

// Real provider token usage of the most recently completed turn (Phase 0 cost
// seam). Local Loops meters this against its cost caps.
nimbus::orch::TokenUsage lastTurnUsage();
nimbus::orch::TokenUsage sessionUsage();   // running sum of billed tokens since boot
uint32_t                 turnCount();      // non-empty turns since boot

// Run a Local Loops scheduled turn synchronously (mirrors the auto-synthesis
// turn). Fires on the tg_poll task; returns real usage + the delivered reply.
// loopId (optional, additive) feeds the ledger's spend-attribution tag
// "loop:<id>" so scheduled spend is auditable per loop. quietOk (additive; the
// DREAM loop): an all-empty turn delivers nothing (no bare-"Done." fallback).
nimbus::orch::FireOutcome injectScheduledTurn(const String& chatId, const String& prompt,
                                              const String& name,
                                              const String& loopId = String(),
                                              bool quietOk = false,
                                              bool once = false,
                                              bool ownerReminder = false);
// True while a scheduled-loop turn is executing - the device-action validator
// refuses `reboot` and `loop.create` in this window (prism fork-bomb/reset guard).
bool inScheduledTurn();
// Is `cid` currently in the Telegram allowlist? (Local Loops fire-time re-check.)
bool isChatAllowed(const String& cid);
// The first allow-listed chat (owner default target for a loop with no chat_id).
String firstAllowedChat();

// OTA install hook - the owner-only Telegram `/update` command calls this to
// approve+install a pending update. Injected from main.cpp (which owns otaupd),
// so the orchestrator layer carries no OTA dependency. Returns the reply text.
void setOtaInstallHook(String (*fn)());

// Stage an unattended SYSTEM turn (e.g. the post-firmware-update awareness turn)
// from ANY task; drained on tg_poll at the top of pollJobs() via
// injectScheduledTurn (quietOk - the model may stay silent). One slot, latest
// stager wins; targets the first allow-listed chat.
void stageSystemTurn(const String& prompt, const String& name);

// Advance background jobs: dispatch at most one queued spawn per cycle (gated by
// AGENT_MAX_ACTIVE_INFLIGHT + heap), poll at most one due job (round-robin), deliver
// terminal results, and fire the auto-synthesis turn when a batch completes.
// Returns the active job count (the Telegram loop uses it to pace its long-poll).
int pollJobs();

// Cancel a running job by tag or full jobId.
bool cancel(const char* tagOrJobId);

// ---- queries ----------------------------------------------------------------

int    activeJobCount();     // active (non-seen) journal records
int    pendingSpawnCount();  // queued spawns not yet dispatched (W11 self-state)
bool   turnInFlight();       // true while a runTurn() is executing (drives the spinner)
// Last-turn introspection: the complete anatomy of the most recent turn (exact
// system prompt + per-turn input block + raw model output) as ONE plain-text blob.
// Returns a heap_caps PSRAM buffer (caller frees; stream it chunked - never copy
// into an internal String) or nullptr if no turn has run. Serves GET /api/lastturn.
char*  lastTurnDebugPs(size_t& outLen);
// Router key of the synthetic "head" turn job (ring feedback only). The panel job
// list SKIPS it - its Running/Offline edges were flashing a full 2.2 s refresh at
// the start AND end of every turn (owner: "heavy flickering while processing").
uint32_t headJobKey();
bool   reapStuckTurn(uint32_t nowMs);  // free a dead turn's blue head arc (loop-budget+2min); true if fired
bool   reconcileHeadArc(uint32_t nowMs);  // W6: keep the head arc lit while children run; true if it changed the arc
uint32_t ringBackstopFires();  // CUM-11: times a belt-and-braces backstop cleared a stuck arc (soak target: 0)
void   noteRingBackstopFired();  // called by the other ring backstops (attention watchdog, working ceiling)
String jobsSummary();        // human-readable job list (for /jobs)
String sessionsJson();       // JSON session list for the web UI (reads the snapshot)
// Typed session list for the running-sessions digest (nimbus::orch::SessionInfo).
// Reads the journal DIRECTLY - call ONLY from the tg_poll/turn task (composeInputs).
std::vector<nimbus::orch::SessionInfo> sessionInfos();
// Cross-task-safe (AsyncTCP) session snapshot, rebuilt on tg_poll after each pump().
// The MCP session.list tool + sessionsJson() read THIS, never the journal directly,
// so an external /mcp request can't race the poll task's journal writes (prism F24).
std::vector<nimbus::orch::SessionInfo> sessionInfosSnapshot();
bool sessionKnown(const std::string& id);   // is `id` in the current snapshot?
// Stage a session.terminate from the AsyncTCP/MCP task; pollJobs drains it on the
// tg_poll task (the journal's single writer). Terminate is thus async + best-effort.
void stageTerminate(const std::string& id);

// Recent-results ring (results.get / results.list): FULL tool + sub-agent results
// kept PSRAM-resident so a clip is a VIEW, never a loss (Context Fabric Stage 1).
// Mutex-guarded - writers are the tg_poll turn path (clamp spill, fresh-result
// overflow), readers include the AsyncTCP /mcp task.
// ⚠ `ns` is the OWNING tenant (the spilling turn's Principal.ns). Reads are
// scoped to it - an empty ns is device-internal and admin-only.
std::string resultsPut(const char* kind, const std::string& name, const std::string& fullText,
                       const std::string& jobTag = std::string(),
                       const std::string& ns = std::string());
bool resultsGet(const std::string& tag, size_t offset, size_t maxBytes, std::string& out,
                size_t& total, const nimbus::orch::Principal& who);
std::string resultsList(const nimbus::orch::Principal& who);

// v3.6.0 fold: stage a manual compaction of `chatId` (spinlocked slot, drained by
// the tg_poll pump ~1 s later) + the one-line CTX? diagnostic for the HIL harness.
// v3.7.0 RBAC: the tenant table (roles + quotas), tg_poll-owned.
// The table has two real writers (the web surface and the assistant's tenant.*
// tools) and both want a synchronous answer, so it is mutex-guarded rather than
// staged. These accessors are the ONLY way in: each returns a value, never a
// reference into the table, so no caller can hold a pointer across a task
// switch and watch the vector reallocate under it.
void loadTenants();
void persistTenants();
nimbus::orch::Role roleOfChat(const String& chatId);
// Copy of the whole table (plus the admin count, if wanted) for listing.
std::vector<nimbus::orch::Tenant> tenantSnapshot(size_t* adminsOut = nullptr);
// This tenant's EXPLICIT quota (0 fields = inherit the role default). False if
// there is no row yet.
bool tenantQuotaOf(const std::string& chatId, nimbus::orch::Quota& out);
// Upsert / remove. All persist on success. `err` carries a human-readable
// reason on refusal - "that is the only admin" is the one that matters.
bool tenantSetRole(const std::string& chatId, nimbus::orch::Role r, std::string& err);
bool tenantSetQuota(const std::string& chatId, const nimbus::orch::Quota& q,
                    std::string& err);
bool tenantRemove(const std::string& chatId, std::string& err);

void stageManualFold(const char* chatId);

#ifdef NIMBUS_TEST
// HIL seam: request a clean restart from the web task. The flag is drained by
// the MAIN loop (never reboot from AsyncTCP mid-response); persistence and
// migration proofs need a reboot that does not open serial, because that both
// resets the board and wedges the host CDC driver.
void stageTestReboot();
bool testRebootRequested();
#endif
String foldStatusText(const char* chatArg = nullptr);

// ---- web-UI memory surface (cross-task-safe) ---------------------------------
// memorySnapshot() reads a fixed char-buffer echo of the model memory (mirrored
// by the turn task), so the AsyncTCP task can never chase g_mem's std::string
// across a realloc. The two mutators only STAGE flags; the turn task drains them
// (pollJobs/handleMessage) so OrchMemory stays single-task. All three are safe
// no-ops before begin() (Notifier mode: snapshot is "", flags never drain).
String memorySnapshot();     // current model memory ("" if none / not running)
String lastInstructions();   // last composed World system prompt (for PROMPT? / diagnostics)

bool   toolRidesLoop(const std::string& name);   // P7: is this registry tool loop-callable now?

// Output-channel helpers backing the reply.speak / reply.telegram tools (P6).
String currentChat();                    // routing chatId of the in-flight turn ("" if none)
bool   speakOnDevice(const String& text, bool capture = true);// TTS -> device speaker; false if
                                         // unavailable/faulted. capture=false when the caller already
                                         // records the spoken text to history (the tts device action does)
bool   sendToChat(const String& chatId, const String& text, bool asVoice);  // "" chatId => current turn's chat
void   requestMemoryClear(); // stage a model-memory wipe (echo zeroed at once)
void   requestConvReset();   // stage a provider-conversation reset (drained on tg_poll,
                             // AFTER any in-flight turn's write-back - prism B)
void   requestConvClear();   // /clear: stage a drop of conversation context + scratchpad
                             // active task, keeping long-term memory and files
void   noteConfigChanged();  // stage a directive (sysPrompt) reload from NVS

}  // namespace orchestrator
}  // namespace agent
