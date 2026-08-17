#pragma once
#include <Arduino.h>

#include "nimbus/harness/dream.h"   // the pure core: gate/prompt/reserved identity
#include "nimbus/harness/hooks.h"   // DreamStartEv/DreamEndEv slots
#include "nimbus/orch/loops.h"      // LoopFireRequest / FireOutcome / LoopRecord

// dream_subsystem - DREAMING's device glue around the pure agent::dream core
// (lib/harness/dream.h). Runs entirely on tg_poll via the Local Loops
// scheduler: the reserved "dream" loop record (ensured by main.cpp with
// loops::ensureLoop) fires through here instead of the plain scheduled-turn
// path. Phase 1 (always): non-LLM memory maintenance under memory::Lock -
// vector decay / TTL-prune / dedup + persist. Phase 2: one ordinary
// injectScheduledTurn whose inputs are buildDreamInputs(yesterday's episodic
// digest + scratchpad + memory stats) - the existing turn contract
// (mem_write/mem_query/scratchpad ops) is the dream API, so every
// scheduled-turn refusal rail and LoopCap applies unchanged.
namespace agent {
namespace dream {

// Wire the lifecycle hooks (onDreamStart/onDreamEnd; other slots ignored) and
// device tuning. Call once from orchestratorBegin (Orchestrator mode only).
void begin(const agent::Hooks& hooks);

// Feed the idle gate's quiet clock - called from the engine's onTurnEnd hook.
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
