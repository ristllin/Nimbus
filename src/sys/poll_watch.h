#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>

// poll_watch (device glue) - the Telegram poll task's liveness guard (the pure rules:
// lib/core nimbus/sys/poll_guard.h, host-tested in test_poll_guard).
//
//   boot()      setup(), before the crash notice is planned: reads the RTC streak
//               record, so the boot after a guard restart knows it was one.
//   arm(task)   telegram::begin() once the poll task exists; disarm() when it stops on
//               purpose (a quiesce, its own exit).
//   beat(step)  any task may call it; only the watched task's calls count. `step` is a
//               string literal naming where the task was ("provider read", "tool").
//   tick()      loop(): at 1 Hz, the check. A task silent past the limit is suspended
//               (so its saved frame is stable), its PC and a short backtrace are read,
//               the durable log gets one line, and the device restarts through a
//               deliberate panic whose reason carries the same facts.
//
// No task, no NVS, no allocation outside the two lines it writes once. The streak record
// lives in RTC no-init memory.
namespace nimbus::pollwatch {

void boot();
void arm(TaskHandle_t task);
void disarm();
void beat(const char* step);
void tick();

bool bootAfterStall();    // this boot follows a guard restart
uint8_t streakAtBoot();   // the streak that restart ended (1 = the first in a row)
uint32_t silentS();       // the poll task's current silence (0 when not armed)
const char* lastStep();   // where it last showed progress

}  // namespace nimbus::pollwatch
