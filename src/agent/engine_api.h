#pragma once
// engine_api.h - the calls an ENGINE SEAM implements (lane RB-SEAM1b, no behavior change).
//
// The device glue is split three ways (PRD section 10):
//   orchestrator.h           the public surface for the device's other files. It includes this
//                            header, so a caller sees one API and never names an engine.
//   orchestrator_common.cpp  the engine-neutral half (job glue, ring arcs, voice, the tenant
//                            table, the session snapshot and staged terminates, the status
//                            getters, the reply sinks, the results ring): it defines what
//                            orchestrator.h itself declares.
//   orchestrator_e1.cpp      the old engine's seam over TurnEngine: it defines every call below
//                            and is the only device file that includes nimbus/harness/engine.h.
//                            The plain engine's seam (orchestrator_plain.cpp, RB-I0 onward)
//                            defines the same calls; a device image links exactly one seam.
// test/test_engine_api_seam pins the split: every call here is defined in the engine seam and
// never in the common half; every call orchestrator.h declares is defined in the common half.
// The callers' calls keep the signatures orchestrator.h always had; the last section is the
// seam's own (what the common half asks of the engine).
#include "orchestrator.h"   // Sinks, HeavyFabric and the orch value types (it includes this file)

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace agent {
namespace orchestrator {

// ---- lifecycle --------------------------------------------------------------

// Wire the fabric + sinks, load the journal + two-part memory. Call once when
// Orchestrator mode starts (after arbiter::begin() + fabricInit()).
void begin(HeavyFabric* fabric, const Sinks& sinks);

// ---- turn entry points (SYNCHRONOUS - call from the Telegram poll task) ------

// Handle an incoming Telegram message: run one orchestrator turn (reply vs spawn),
// fold any fresh sub-agent results in, apply memory/device/spawn/await/reply.
void handleMessage(const String& text, const String& fromName, const String& chatId);

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
                                              bool ownerReminder = false,
                                              bool selfWakeup = false);

// Advance background jobs: dispatch at most one queued spawn per cycle (gated by
// AGENT_MAX_ACTIVE_INFLIGHT + heap), poll at most one due job (round-robin), deliver
// terminal results, and fire the auto-synthesis turn when a batch completes.
// Returns the active job count (the Telegram loop uses it to pace its long-poll).
int pollJobs();

// OTA install hook - the owner-only Telegram `/update` command calls this to
// approve+install a pending update. Injected from main.cpp (which owns otaupd),
// so the orchestrator layer carries no OTA dependency. Returns the reply text.
void setOtaInstallHook(String (*fn)());

// Stage the fixed post-update notice (orch::updateNoticeText, RB-Q2a) from ANY task; on
// tg_poll at the top of pollJobs() it goes to the owner's chat and into its history at
// once, with no model call. One slot, latest stager wins.
void stageUpdateNotice(const String& text);

// ---- status for the web and the tools ---------------------------------------

bool   turnInFlight();       // true while a runTurn() is executing (drives the spinner)
uint32_t                 turnCount();      // non-empty turns since boot
// What holds paid requests to provider `p` off now on `seam` (the pause switch, a refusal
// park covering that seam, its monthly budget). The orch_model guard and [THIS TURN RUNS ON]
// read Chat; the catalog, the PDF route and the spawn advice read Spawn (CUM-821).
nimbus::orch::HeadPause providerPause(const std::string& p, nimbus::orch::PaidSeam seam);
bool   toolRidesLoop(const std::string& name);   // P7: is this registry tool loop-callable now?

// ---- last-turn data ---------------------------------------------------------

// Real provider token usage of the most recently completed turn (Phase 0 cost
// seam). Local Loops meters this against its cost caps.
nimbus::orch::TokenUsage lastTurnUsage();
nimbus::orch::TokenUsage sessionUsage();   // running sum of billed tokens since boot
// The last failed turn since boot as JSON ({"turn","src","agoS","cause"}), or "null"
// (device.status.lastFail; RAM only, lane T).
String lastFailJson();
// Last-turn introspection: the complete anatomy of the most recent turn (exact
// system prompt + per-turn input block + raw model output) as ONE plain-text blob.
// Returns a heap_caps PSRAM buffer (caller frees; stream it chunked - never copy
// into an internal String) or nullptr if no turn has run. Serves GET /api/lastturn.
char*  lastTurnDebugPs(size_t& outLen);
String lastInstructions();   // last composed World system prompt (for PROMPT? / diagnostics)

// ---- the turn in flight -----------------------------------------------------

// True while a scheduled-loop turn is executing - the device-action validator
// refuses `reboot` and `loop.create` in this window (prism fork-bomb/reset guard).
bool inScheduledTurn();
// Lane J: did the running turn read outside content (a folded-in media row or
// an external-tainted tool result)? The standing rules' taint rail reads it.
bool turnReadOutsideContent();
// CUM-523: whether text the device serves to the model may name the Telegram-only
// tools (reply.telegram, files.send): the running turn's one sample (the same one
// its tool list follows), else - outside a turn - whether any turn could deliver
// (the bot runs and a chat is allowlisted).
bool telegramForTurn();
// CUM-523: whether that text may name files.send: the turn's sample AND a chat it
// can deliver to (a public-mode guest turn may reply.telegram but not send a file).
bool fileSendForTurn();
// What the unattended policy reads about the turn in flight (a person's turn outside one,
// and on any task other than the turn task).
nimbus::orch::UnattendedFacts unattendedFacts();
String currentChat();                    // routing chatId of the in-flight turn ("" if none)
// CUM-826: the in-flight turn's model text reached the owner outside apply (a tool's
// send, speech that started playing). Call it only once the hand-over succeeded.
// Turn task only.
void   noteTurnSpoke();

// ---- fold status ------------------------------------------------------------

// A manual fold request (/compact, Summarize now, the console): staged from any task,
// drained into the fold driver's persisted queue on the next pump pass (lane F).
// False when it was not staged (no id, an id too long for a chat, or every slot holds
// another chat's request).
bool stageManualFold(const char* chatId);
// GET /api/fold/status's body for `chat` ("" = the default chat: the owner's Telegram
// chat, else the web chat), from the tg_poll-published copy; any task. False when the
// device knows no conversation by that name.
bool foldStatusJsonFor(const std::string& chat, std::string& out);
// v3.6.0 fold: the one-line CTX? diagnostic for the HIL harness.
String foldStatusText(const char* chatArg = nullptr);
#ifdef NIMBUS_TEST
// FOLDRESET (bench rows reset between rows): today's paid fold count restarts at 0.
void stageFoldDailyReset();
#endif

// ---- clear conversation and the staged maintenance (cross-task-safe) ----------
// memorySnapshot() reads a fixed char-buffer echo of the model memory (mirrored
// by the turn task), so the AsyncTCP task can never chase g_mem's std::string
// across a realloc. The mutators only STAGE flags; the turn task drains them
// (pollJobs/handleMessage) so OrchMemory stays single-task. All are safe
// no-ops before begin() (Notifier mode: snapshot is "", flags never drain).
String memorySnapshot();     // current model memory ("" if none / not running)
void   requestMemoryClear(); // stage a model-memory wipe (echo zeroed at once)
void   requestConvReset();   // stage a provider-conversation reset (drained on tg_poll,
                             // AFTER any in-flight turn's write-back - prism B)
void   requestConvClear();   // the web app's "Clear conversation": stage the web chat's
                             // /clear (fresh conversation + active task cleared, keeping
                             // long-term memory, files and the history itself - CUM-511)
void   noteConfigChanged();  // stage a directive (sysPrompt) reload from NVS

// ---- what the common half asks of the engine (orchestrator_common.cpp only) ----
bool engineRunning();                      // the engine exists (begin() has run)
bool engineReapStuckTurn(uint32_t nowMs);  // the engine's stuck-turn reaper fired
void engineSetHeadArc(bool on);            // light or clear the head's ring arc
// The reply sinks (speakOnDevice, sendToChat): a history row's tags with the id of the turn in
// flight appended ("turn:<row id>", so the chat groups a reply with its turn); the base
// alone outside a turn. Turn task.
String withTurnTag(const char* base);
// sendToChat answered the turn in flight's own chat mid-loop, so the turn appends no
// contentless "Done." (called only while engineRunning()).
void engineNoteToolReplied(const std::string& text);

}  // namespace orchestrator
}  // namespace agent
