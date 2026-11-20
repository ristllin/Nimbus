#pragma once
// orchestrator_common.h - PRIVATE to the device seam: what orchestrator_common.cpp (the
// engine-neutral half: job glue, ring arcs, voice, job status getters, reply sinks)
// and orchestrator.cpp (the old engine's half) share. Not for any other file: the public
// surface stays orchestrator.h, unchanged for its callers (lane RB-SEAM1a, no behavior
// change). Both files include this header, so every shared object is declared once and
// type-checked against its one definition.
#include "orchestrator.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>   // SemaphoreHandle_t (the session snapshot)

#include <atomic>
#include <string>

#include "nimbus/harness/jobs.h"   // JobEngine, HeavyFabric
#include "nimbus/orch/journal.h"    // Journal
#include "nimbus/orch/moderation.h"   // ModGate, ModAction

namespace agent {
namespace orchestrator {

// ---- module state (orchestrator_common.cpp) -------------------------------------
extern HeavyFabric* g_fabric;   // set by begin() (orchestrator.cpp)
extern Sinks        g_sinks;    // set by begin()
extern JobEngine*   g_jobs;     // constructed in begin()
// The sub-agent journal, in PSRAM (psramObject; its declaration in orchestrator_common.cpp
// is pinned by tests/hil/test_psram_statics_host.py).
extern nimbus::orch::Journal& g_journal;

// ---- ring arcs (orchestrator_common.cpp) -----------------------------------------
void emitAsk();                    // raise the "needs you" ask on the attention Router
extern uint32_t g_attnHoldMs;      // latched AttnHoldMs; begin() hands it to the job engine

// ---- voice (orchestrator_common.cpp) ----------------------------------------------
// Hold-to-talk message lifecycle (CUM-456): written by handleMessage and the turn end
// (orchestrator.cpp), read by voiceMessages().
extern std::atomic<uint32_t> s_voiceHandled;
extern std::atomic<bool>     s_voiceOk;
extern bool                  s_inVoiceMessage;   // tg_poll only

// ---- the session snapshot and staged terminates (orchestrator_common.cpp) ---------
extern SemaphoreHandle_t g_sessMux;   // guards the snapshot; begin() creates it
void refreshSessionSnapshot();        // tg_poll only: after begin() and each pump()
void drainTerminates();               // tg_poll only: the staged session.terminate calls

// ---- reply sinks (orchestrator_common.cpp) ----------------------------------------
bool chatAllowed(const String& cid);                    // in the persisted Telegram allowlist
bool telegramDeliverable(const std::string& chatId);    // HarnessConfig::telegramDeliverable
// The moderation gate (CUM-69): role and switch gate, then classify and decide.
nimbus::orch::ModAction moderateGate(nimbus::orch::ModGate gate, const String& chatId,
                                     const String& text);

}  // namespace orchestrator
}  // namespace agent
