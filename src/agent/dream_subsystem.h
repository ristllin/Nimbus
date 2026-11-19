#pragma once
#include <Arduino.h>

#include <string>

#include "nimbus/harness/dream.h"   // the pure core: gate, night, reserved identity
#include "nimbus/harness/hooks.h"   // DreamStartEv/DreamEndEv slots
#include "nimbus/orch/loops.h"      // LoopFireRequest / FireOutcome / LoopRecord

// dream_subsystem - the device glue around the pure agent::dream core
// (lib/harness/dream.h): the nightly memory upkeep. Runs entirely on tg_poll via the
// Local Loops scheduler: the reserved "dream" loop record (ensured by main.cpp with
// loops::ensureLoop) fires through here instead of the scheduled-turn path. The night
// itself is agent::dream::runNight (host-tested); this file binds its seams: the
// pending-memory drain, maintenance under memory::Lock (once per local day) and the
// dream state in /data/dream.json (LittleFS). It makes no model call.
namespace agent {
namespace dream {

// Wire the lifecycle hooks (onDreamStart/onDreamEnd; other slots ignored) and device
// tuning. Call once from orchestratorBegin (Orchestrator mode only).
void begin(const agent::Hooks& hooks);

// Feed the idle gate's quiet clock - called from the engine's onTurnEnd hook.
void noteTurnEnd(uint32_t nowMs);

// Local Loops pre-fire gate (loops::setFireGate): 0 => idle, fire; else defer
// seconds. Non-reserved loops always pass (0).
uint32_t gateDefer(const nimbus::orch::LoopRecord& l);

// The dream FireHook: the pending-memory drain, then the night (maintenance, row).
// Dispatched from main.cpp's loops fire lambda when the record is the reserved dream.
nimbus::orch::FireOutcome fire();

// The console DREAM drill (test builds). requestDrill only queues a night (any task);
// pumpDrill runs it on tg_poll between turns (orchestrator::pollJobs) and logs
// "DREAM fired ok=<0|1> detail=<...>". Maintenance runs at most once per local day, so
// a second drill that day writes a "skipped" row. requestDrill returns false when a
// drill is already waiting (the two coalesce).
bool requestDrill();
void pumpDrill();

// The last night of this boot, for /api/state and the health report (any task).
struct Status {
  bool known = false;          // a night has run since boot
  const char* last = "";       // outcome slug
  char cause[33] = {};         // a starved night: the gate's reason
  bool stateSaved = true;      // /data/dream.json landed
  char stateWhy[24] = {};      // why not ("storage not mounted", "storage write failed")
};
Status status();

}  // namespace dream
}  // namespace agent
