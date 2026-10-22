#include "loop_watch.h"

#include <Arduino.h>

#include "agent_log.h"

namespace nimbus::loopwatch {
namespace {
sys::LoopPassMonitor g_passes;   // loop task only: no lock
uint32_t g_lastEntryMs = 0;
}  // namespace

void notePass() {
  const uint32_t now = millis();
  if (g_lastEntryMs) {
    const uint32_t passMs = now - g_lastEntryMs;
    if (g_passes.note(passMs, now)) agent::alog(sys::slowPassLine(passMs, g_passes).c_str());
  }
  g_lastEntryMs = now;
}

const sys::LoopPassMonitor& passes() { return g_passes; }

}  // namespace nimbus::loopwatch
