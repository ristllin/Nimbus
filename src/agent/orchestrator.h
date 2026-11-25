#pragma once
#include "nimbus/orch/tool_registry.h"  // Principal (results-ring reads are ns-scoped)
#include <Arduino.h>

#include <vector>

#include "adapter.h"
#include "nimbus/attention.h"
#include "nimbus/orch/device_actions.h"  // ValidatedAction (DeviceSink payload)
#include "nimbus/orch/rbac.h"            // Role / TenantStore (v3.7.0)
#include "nimbus/orch/speak_outcome.h"    // SpeakOutcome (SpeakSink's answer)
#include "nimbus/orch/world.h"  // SessionInfo (session.list / running-sessions digest)
#include "nimbus/orch/token_usage.h"  // TokenUsage - Phase 0 cost seam (lastTurnUsage)
#include "nimbus/orch/loops.h"        // FireOutcome - Local Loops scheduled-turn result
#include "nimbus/orch/head_switch.h"  // HeadPause - what holds a provider off
#include "nimbus/orch/unattended_policy.h"  // TurnClass - the unattended policy (RB-Q2a)

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
//
// The glue is split (lane RB-SEAM1b, PRD section 10): this header declares the
// engine-neutral calls, all defined in orchestrator_common.cpp, and includes
// engine_api.h, the calls an engine seam defines (orchestrator_e1.cpp for the old
// engine). A caller includes this header only and never names an engine.

namespace agent {
namespace voicereply {
struct Outcome;   // voice_reply.h
}
struct DeviceApplyDeps;   // nimbus/harness/device_apply.h
namespace orchestrator {

// ---- caller-supplied sinks (installed by main.cpp) --------------------------

// Emit a job/voice/ask event onto the attention Router (SAME ring path as Notifier).
using EventSink = void (*)(const nimbus::attn::Event& e);

// Deliver text to the owner over Telegram (thread-safe queue on the device). Returns
// false if the message could NOT be enqueued (queue full) so the caller can log the
// drop instead of losing the owner's reply silently.
using SendSink = bool (*)(const String& chatId, const String& text);

// Voice a short text (the `tts` device action, a job-result announcement). `chatId` is the
// chat the turn is in ("" for a job announcement): the Telegram fallback goes there when it
// is a Telegram chat, else to the owner chat (telegram::ownerChat). Answers how it ended: played on the
// speaker, delivered to Telegram with a message id, or not voiced with the reason - the turn
// records exactly that, never the request. Nullable.
using SpeakSink = nimbus::orch::SpeakOutcome (*)(const String& chatId, const String& text);

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

// ---- the ring's ask -------------------------------------------------------------

// Resolve a pending "needs you" ask (the owner engaged). Call from the owner-input
// paths that don't route through handleMessage (e.g. the on-device voice reply).
void clearAsk();

// Hold-to-talk message lifecycle (CUM-456): how many on-device voice messages
// (chatId "voice") handleMessage() has FINISHED since boot - every exit path: a
// turn that answered or failed, an honest early refusal, a command - and whether
// the latest one's turn answered. handleMessage runs one message at a time in
// arrival order, so the panel knows its OWN message is done when the count reaches
// (count at send) + 1: not on the first delivered text (a mid-turn fallback notice,
// or a sub-agent result that happens to land), and not on a synthesis turn on the
// same chat (those never pass through handleMessage). Written on the turn task,
// read on the main loop (atomics; the reply is delivered BEFORE the count moves).
// Either out-param may be null.
void voiceMessages(uint32_t* handled, bool* lastOk);

// Is `cid` currently in the Telegram allowlist? (Local Loops fire-time re-check.)
bool isChatAllowed(const String& cid);
// The first allow-listed chat (owner default target for a loop with no chat_id).
String firstAllowedChat();
// Cancel a running job by tag or full jobId.
bool cancel(const char* tagOrJobId);

// ---- queries ----------------------------------------------------------------

int    activeJobCount();     // active (non-seen) journal records
int    pendingSpawnCount();  // queued spawns not yet dispatched (W11 self-state)
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
bool resultsGet(const std::string& tag, size_t& offset, size_t maxBytes, std::string& out,
                size_t& total, const nimbus::orch::Principal& who);
std::string resultsList(const nimbus::orch::Principal& who);

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
// An approval: the chat's row, written now (nimbus::orch::noteApproved). Best effort: with
// a full table, or a write that did not land, the allowlist fallback still answers User.
void tenantApproved(const std::string& chatId, bool telegramOwner);
bool tenantSetQuota(const std::string& chatId, const nimbus::orch::Quota& q,
                    std::string& err);
bool tenantRemove(const std::string& chatId, std::string& err);

#ifdef NIMBUS_TEST
// HIL seam: request a clean restart from the web task. The flag is drained by
// the MAIN loop (never reboot from AsyncTCP mid-response); persistence and
// migration proofs need a reboot that does not open serial, because that both
// resets the board and wedges the host CDC driver.
void stageTestReboot();
bool testRebootRequested();
#endif

// Output-channel helpers backing the reply.speak / reply.telegram tools (P6).
// TTS -> device speaker. True only once the sound task opened the speaker for the clip
// (a bounded wait); false with `why` when it is unavailable, faulted, too long, or the
// speaker would not start. capture=false when the caller already records the spoken
// text to history (the tts device action does).
bool   speakOnDevice(const String& text, bool capture = true, String* why = nullptr);
// "" chatId => current turn's chat. asVoice: synthesize and upload now as Telegram voice
// messages (voice_reply.h), never the speaker; `result` gets the tool's words (the
// receipt with Telegram's message ids, or why nothing arrived) and the return is true
// only when every note arrived. False with `result` untouched: the chat is not allowed.
// `voiceOut` (engine 2's send, lane E2-P-OUT1): the voice delivery's whole outcome.
bool   sendToChat(const String& chatId, const String& text, bool asVoice,
                  std::string* result = nullptr, voicereply::Outcome* voiceOut = nullptr);

// The device-action executors (lifted from engine 1's buildApplyDeps by lane E2-P-TOOLS):
// config knobs into the stores, led/lights/reboot to the main loop's DeviceSink, tts to the
// speak sink, orch_model checked against the model store and written to the head setting.
// Execution only; the policy is device_apply.cpp's. Both engines fill their deps from it.
void fillDeviceApplyDeps(DeviceApplyDeps& d);

}  // namespace orchestrator
}  // namespace agent

#include "engine_api.h"   // the calls each engine seam defines (lane RB-SEAM1b)
