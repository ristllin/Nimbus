#pragma once
#include <Arduino.h>

#include <functional>
#include <string>

#include "nimbus/harness/dream.h"   // the pure core: gate/prompt/reserved identity
#include "nimbus/harness/hooks.h"   // DreamStartEv/DreamEndEv slots
#include "nimbus/orch/loops.h"      // LoopFireRequest / FireOutcome / LoopRecord

// dream_subsystem - DREAMING's device glue around the pure agent::dream core
// (lib/harness/dream.h). Runs entirely on tg_poll via the Local Loops
// scheduler: the reserved "dream" loop record (ensured by main.cpp with
// loops::ensureLoop) fires through here instead of the plain scheduled-turn
// path. The night itself is agent::dream::runNight (host-tested); this file only
// binds its seams: maintenance under memory::Lock (once per local day), the
// owner's day from the episodic store, the scratchpad, the memory write counters,
// one quiet, memory-only injectScheduledTurn, and the dream state in
// /data/dream.json (LittleFS; the legacy NVS baseline is read once, never written).
namespace agent {
namespace dream {

// The owner message for a failing streak (main.cpp: the first allowed Telegram chat).
using AlertSink = std::function<void(const std::string& text)>;

// Wire the lifecycle hooks (onDreamStart/onDreamEnd; other slots ignored), the
// streak alert sink and device tuning. Call once from orchestratorBegin
// (Orchestrator mode only).
void begin(const agent::Hooks& hooks, AlertSink alert);

// Feed the idle gate's quiet clock - called from the engine's onTurnEnd hook. A
// pending streak alert, and the note the reflection held for the owner, go out
// here as one message when a turn ends (never from the night itself).
void noteTurnEnd(uint32_t nowMs);

// Local Loops pre-fire gate (loops::setFireGate): 0 => idle, fire; else defer
// seconds. Non-reserved loops always pass (0).
uint32_t gateDefer(const nimbus::orch::LoopRecord& l);

// The dream FireHook: maintenance, digest, reflection turn. Dispatched from
// main.cpp's loops fire lambda when r.id is the reserved dream id.
// force = the console DREAM drill: run the paid reflection even on a provably
// quiet night (the drill exists to exercise the full path deterministically).
nimbus::orch::FireOutcome fire(const nimbus::orch::LoopFireRequest& r,
                               bool force = false);

// The console DREAM drill (test builds). requestDrill only queues a forced night (any
// task); pumpDrill runs it on tg_poll between turns (orchestrator::pollJobs) and logs
// "DREAM fired ok=<0|1> detail=<...>". A night is a full turn, and the loop task that
// reads the console has an 8 KB stack: the drill used to run there and panic the
// device (stack canary), so it never runs on the caller's task.
// requestDrill returns false when a drill is already waiting (the two coalesce).
bool requestDrill(const std::string& chatId);
void pumpDrill();

// The boot reconcile (L19-31): a marker with no outcome in /data/dream.json names
// a reflection turn the restart cut short. Call on every pollJobs pass (tg_poll,
// before the loops tick): once internal storage is mounted, the clock is synced and
// the timeline and loop records are up, it turns that marker into the night's
// verdict - fail, one timeline row, the streak and the loop's consecFails exactly
// once - and schedules the same-night retry while the window is open. A torn file
// or a marker a later fire superseded is never counted (dream::BootReconcile).
void pumpBootReconcile();

// The last night of this boot, for /api/state and the health report (any task).
struct Status {
  bool known = false;          // a night has run since boot
  const char* last = "";       // outcome slug
  char cause[33] = {};
  char err[64] = {};           // the failure's own text ("" when ok)
  bool stateSaved = true;      // /data/dream.json landed
  char stateWhy[24] = {};      // why not ("storage not mounted", "storage write failed")
  uint8_t attempts = 0;        // reflections tonight
  uint8_t streak = 0;          // failed or degraded nights in a row
};
Status status();

}  // namespace dream
}  // namespace agent
