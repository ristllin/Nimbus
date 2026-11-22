#pragma once
#include <functional>
#include <string>

#include "nimbus/orch/schedule_tool.h"

// schedule_device - the device binding of engine 2's schedule tool (lane RB-U1b; PRD sections 4
// and 6): the portable tool (nimbus/orch/schedule_tool.h) over the loops subsystem's table
// (/data/loops.json, loops_subsystem.cpp) and its sidecar /data/loops2.json on internal flash,
// both written with agent::writeFileAtomic. Engine 2 only (tools/engine/engine2_only.txt). Every
// call runs under the PSRAM-first scope; a refused allocation is an error result, never an abort.
// No model text here: every result and error line is the core's (orch_schedule_tool.cpp).
namespace agent::scheduledev {

// The chat the running turn answers in (where an entry it makes delivers); "" = the owner default.
using ChatFn = std::function<std::string()>;

// Register `schedule` (the manifest's description and schema, plain::toolDecl) on the device's
// registry. `origin` reads the running turn (RB-I2's engine binding); null reads as unattended: a
// call may list and set a reminder, nothing else, until the engine binds it. `chat` null: "".
void registerSchedule(nimbus::orch::ToolRegistry& reg, nimbus::orch::notes::OriginFn origin, ChatFn chat);

// The loops subsystem's fire hook on engine 2 (RB-I1 installs it in loops::begin): a reminder
// delivers its text through `b.owner` with no model call; a routine runs through `b.run`
// (plain::runRoutine) with its state at /mem/routines/<id>.state on `b.files`.
nimbus::orch::FireOutcome fire(const nimbus::orch::LoopFireRequest& req, const nimbus::orch::schedule::FireBinding& b);

// The table and its two files as the core sees them (for RB-I1's due hook and the web routes).
nimbus::orch::schedule::Host& host();

}  // namespace agent::scheduledev
