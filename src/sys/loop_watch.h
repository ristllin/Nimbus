#pragma once
#include <cstdint>

#include "nimbus/sys/loop_guard.h"

// loop_watch (device glue) - times every pass of the main loop against the task
// watchdog (the pure rules: lib/core nimbus/sys/loop_guard.h, host-tested).
//
//   notePass()   first thing in loop(): the time since the previous loop() entry is
//                one pass; a pass of kSlowPassMs or more is logged (rate limited), so
//                a stall that stays under the watchdog still names itself.
//   passes()     the monitor (test console LOOPGUARD?, the HIL stall leg).
//
// No task, no NVS, no allocation outside the log line.
namespace nimbus::loopwatch {

void notePass();
const sys::LoopPassMonitor& passes();

}  // namespace nimbus::loopwatch
