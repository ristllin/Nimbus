#pragma once
// orchestrator_common.h - PRIVATE to the device seam: what orchestrator_common.cpp (the
// engine-neutral half: tenants, job glue, ring arcs, voice, status getters, reply sinks)
// and orchestrator.cpp (the old engine's half) share. Not for any other file: the public
// surface stays orchestrator.h, unchanged for its callers (lane RB-SEAM1a, no behaviour
// change). Both files include this header, so every shared object is declared once and
// type-checked against its one definition.
#include "orchestrator.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <string>
#include <vector>

#include "nimbus/harness/jobs.h"   // JobEngine, HeavyFabric
#include "nimbus/orch/journal.h"    // Journal
#include "nimbus/orch/rbac.h"   // TenantStore, Principal

namespace agent {
namespace orchestrator {

// ---- module state (orchestrator_common.cpp, except g_journal) -------------------
extern HeavyFabric* g_fabric;   // set by begin() (orchestrator.cpp)
extern Sinks        g_sinks;    // set by begin()
extern JobEngine*   g_jobs;     // constructed in begin()
// The sub-agent journal: defined in orchestrator.cpp, in PSRAM (psramObject; its
// declaration there is pinned by tests/hil/test_psram_statics_host.py).
extern nimbus::orch::Journal& g_journal;

// ---- job glue (orchestrator_common.cpp) -------------------------------------------
// A FreeRTOS mutex held for one scope and given back on every path out, an exception
// included (a turn catches std::bad_alloc, L24). A null mutex holds nothing.
struct MuxHold {
  SemaphoreHandle_t m;
  explicit MuxHold(SemaphoreHandle_t mux) : m(mux) {
    if (m) xSemaphoreTake(m, portMAX_DELAY);
  }
  ~MuxHold() {
    if (m) xSemaphoreGive(m);
  }
  MuxHold(const MuxHold&) = delete;
  MuxHold& operator=(const MuxHold&) = delete;
};
extern SemaphoreHandle_t g_sessMux;   // guards the session snapshot; created in begin()
// tg_poll only: mirror the journal for cross-task readers; apply web-staged terminates.
void refreshSessionSnapshot();
void drainTerminates();

// ---- ring arcs (orchestrator_common.cpp) -----------------------------------------
void emitAsk();                    // raise the "needs you" ask on the attention Router
extern uint32_t g_attnHoldMs;      // latched AttnHoldMs; begin() hands it to the job engine

// ---- voice (orchestrator_common.cpp) ----------------------------------------------
// Hold-to-talk message lifecycle (CUM-456): written by handleMessage and the turn end
// (orchestrator.cpp), read by voiceMessages().
extern std::atomic<uint32_t> s_voiceHandled;
extern std::atomic<bool>     s_voiceOk;
extern bool                  s_inVoiceMessage;   // tg_poll only

// ---- reply sinks (orchestrator_common.cpp) ----------------------------------------
bool chatAllowed(const String& cid);                    // in the persisted Telegram allowlist
bool telegramDeliverable(const std::string& chatId);    // HarnessConfig::telegramDeliverable

// ---- the engine seam: what the common half asks of the engine (orchestrator.cpp) ----
bool engineRunning();                      // the engine exists (begin() has run)
bool engineReapStuckTurn(uint32_t nowMs);  // the engine's stuck-turn reaper fired
void engineSetHeadArc(bool on);            // light or clear the head's ring arc

}  // namespace orchestrator
}  // namespace agent
