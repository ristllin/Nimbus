#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#include <string>

#include "nimbus/orch/degraded.h"

// degraded_device - the device side of "the device says when it runs degraded" (lane
// DEGRADE, nimbus/orch/degraded.h). It collects the facts the portable rules read, folds
// them through the debounce, and hands the one shown set to every surface: the home
// screen's strip, the details card, /api/state "degraded" and the web banner.
//
// Cadence and threading: tick() runs on the main loop every kTickMs and never waits (the
// card lock is tried with LoopLock(0); a busy lock keeps the last answer). noteTurn() runs
// on the turn task at the end of every turn. shown(), generation() and into() are read
// from any task (the AsyncTCP task serves /api/state); the shown set is a copy under a
// small mutex. The state is allocated by begin(), so it costs a pointer of static RAM, not the state.
namespace agent {
namespace degraded {

// Allocates the state and locks and unlocks each of its mutexes once. Called once from setup(),
// while memory is plentiful: a std::mutex creates its FreeRTOS semaphore at its first lock,
// and a failed allocation there aborts, so the first lock must not be a runtime one. Until
// then everything answers "nothing degraded".
void begin();

// A turn ended: `requested` is the head it started on, `served` the provider that answered,
// `path` the providers it ran on in order ("a -> b"), `billed` whether any provider billed.
void noteTurn(const std::string& requested, const std::string& served, const std::string& path, bool billed);

// Main loop. Collects the facts and folds them in (at most every kTickMs); true when the
// SHOWN set changed, so the caller repaints the home screen. `orchestrator`: false in Notifier
// mode (none of these subsystems) and on a device still in first-run setup; nothing shows then.
// `wifiUp`: the station link is up.
bool tick(uint32_t nowMs, bool orchestrator, bool wifiUp);

nimbus::orch::DegradedSet shown();   // a copy, in display order
uint32_t generation();               // changes whenever the shown set does
// /api/state "degraded": count, generation and one {id, line, why} per condition.
void into(JsonObject out);

}  // namespace degraded
}  // namespace agent
