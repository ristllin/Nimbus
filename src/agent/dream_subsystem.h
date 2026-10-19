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
// pending streak alert goes out here, when a turn of the owner's ends (never from
// the night itself).
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

}  // namespace dream
}  // namespace agent
