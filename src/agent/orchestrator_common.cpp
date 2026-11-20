#include "orchestrator_common.h"

#include <ArduinoJson.h>

#include "../sys/agent_log.h"
#include "orch_persist.h"   // LittleFsTenantStoreIO
#include "store.h"
#include "nimbus/orch/loop_delivery.h"  // telegramOwnerOf (roleOfChat's rule, host-tested)

// orchestrator_common - the engine-neutral half of the device seam (lane RB-SEAM1a, no
// behaviour change): tenants, job glue, ring arcs, voice, status getters and reply sinks,
// moved here from orchestrator.cpp so a second engine can be bound beside the old one.
// orchestrator.cpp keeps the old engine's half; what the two share is declared in
// orchestrator_common.h. The public surface is orchestrator.h, unchanged.

namespace agent {
namespace orchestrator {

namespace orch = nimbus::orch;

// ---- module state shared with orchestrator.cpp ---------------------------------
HeavyFabric* g_fabric = nullptr;
Sinks        g_sinks;
// The portable sub-agent job machinery (spawn queue / dispatch gate / round-robin
// poll / fresh results / reap scheduler) - constructed in begin(), Stage F lift.
JobEngine*   g_jobs = nullptr;


// ---- sub-agent journal: cross-task snapshot + staged terminate (prism F24) ---
// The sub-agent journal (g_journal) is SINGLE-WRITER on the tg_poll task - pump()
// update/markSeen/gc it every cycle. External MCP clients reach session.list and
// session.terminate on the AsyncTCP web task, which must NEVER touch the journal
// concurrently (a poll firing mid-request could tear a record / corrupt job state).
// So, mirroring loops:: web-staging and the g_memEcho model:
//   * READS  (session.list, sessionsJson) go through a snapshot rebuilt on tg_poll
//     after each pump(), copied out under a short mutex - never the journal;
//   * WRITES (session.terminate) are STAGED from the web task and drained on
//     tg_poll (terminate becomes async - the remote cancel is best-effort anyway).
// The turn's OWN session_ops[] terminate stays synchronous: it runs on tg_poll via
// d.cancelSession -> cancel(), already the journal's writer task, so it's untouched.
SemaphoreHandle_t        g_sessMux = nullptr;   // guards g_sessSnapshot
// MuxHold is in orchestrator_common.h (the results ring in orchestrator.cpp holds it too).
static std::vector<nimbus::orch::SessionInfo> g_sessSnapshot;
static portMUX_TYPE             g_termSpin = portMUX_INITIALIZER_UNLOCKED;
static std::vector<std::string> g_termQueue;           // guarded by g_termSpin

// Rebuild the AsyncTCP-readable snapshot from the journal. Called ONLY on tg_poll
// (same task as pump), so the sessionInfos() journal read is race-free; the vector
// is built outside the lock and swapped in (O(1), no alloc while the mutex is held).
void refreshSessionSnapshot() {
  std::vector<nimbus::orch::SessionInfo> fresh =
      g_jobs ? g_jobs->sessionInfos() : std::vector<nimbus::orch::SessionInfo>{};
  MuxHold hold(g_sessMux);
  g_sessSnapshot.swap(fresh);
}

// Apply web-staged terminates on tg_poll (the journal's single writer). Swap the
// queue out under the spinlock (O(1)), then cancel() each outside it. A bad/stale
// id is a harmless no-op (the job already finished between staging and draining).
void drainTerminates() {
  std::vector<std::string> work;
  portENTER_CRITICAL(&g_termSpin);
  work.swap(g_termQueue);
  portEXIT_CRITICAL(&g_termSpin);
  for (const std::string& id : work) {
    bool ok = g_jobs && g_jobs->cancel(id.c_str());
    alogf("orchestrator: staged terminate %s -> %s", id.c_str(), ok ? "cancelled" : "not found");
  }
}


bool cancel(const char* tagOrJobId) {
  return g_jobs && g_jobs->cancel(tagOrJobId);
}

int  activeJobCount() { return g_journal.count(); }
int  pendingSpawnCount() { return g_jobs ? g_jobs->pendingCount() : 0; }   // queued, not yet dispatched

static const char* stateStr(orch::JobState s) {
  switch (s) {
    case orch::JobState::Queued:     return "queued";
    case orch::JobState::Running:    return "running";
    case orch::JobState::NeedsInput: return "needs_input";
    case orch::JobState::Done:       return "done";
    case orch::JobState::Error:      return "error";
    case orch::JobState::Cancelled:  return "cancelled";
    default:                         return "unknown";
  }
}

String jobsSummary() {
  int n = g_journal.count();
  if (n == 0) return "No active jobs.";
  String s;
  for (int i = 0; i < n; i++) {
    orch::JobRecord rec;
    if (!g_journal.get(i, rec)) continue;
    s += String(rec.tag) + " (" + rec.backend + "/" + rec.model + ") " +
         rec.category + " [" + stateStr(rec.state) + "]\n";
  }
  return s;
}


// tg_poll ONLY (the journal's writer task) - used by composeInputs on the turn.
// AsyncTCP/MCP callers must use sessionInfosSnapshot() instead.
std::vector<nimbus::orch::SessionInfo> sessionInfos() {
  return g_jobs ? g_jobs->sessionInfos() : std::vector<nimbus::orch::SessionInfo>{};
}

// Cross-task-safe read of the session snapshot (see refreshSessionSnapshot).
std::vector<nimbus::orch::SessionInfo> sessionInfosSnapshot() {
  std::vector<nimbus::orch::SessionInfo> out;
  MuxHold hold(g_sessMux);   // the copy allocates: given back on every path out (L24)
  out = g_sessSnapshot;
  return out;
}

bool sessionKnown(const std::string& id) {
  MuxHold hold(g_sessMux);
  for (const auto& s : g_sessSnapshot)
    if (s.id == id) return true;
  return false;
}


void stageTerminate(const std::string& id) {
  portENTER_CRITICAL(&g_termSpin);
  g_termQueue.push_back(id);   // bounded (kAgentMaxJobs); drained on tg_poll
  portEXIT_CRITICAL(&g_termSpin);
}

}  // namespace orchestrator
}  // namespace agent
