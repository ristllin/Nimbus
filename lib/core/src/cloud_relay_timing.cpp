#include "nimbus/cloud/relay_timing.h"

namespace nimbus {
namespace cloud {

WaitOutcome driveStagedWait(uint32_t totalMs, uint32_t stepMs,
                            const std::function<uint32_t()>& nowMs,
                            const std::function<bool()>& pollDone,
                            const std::function<void(uint32_t)>& stepWait,
                            const std::function<void()>& feed) {
  const uint32_t start = nowMs();
  for (;;) {
    if (pollDone()) return WaitOutcome::Done;
    const uint32_t elapsed = nowMs() - start;
    if (elapsed >= totalMs) return WaitOutcome::TimedOut;
    uint32_t remaining = totalMs - elapsed;
    uint32_t step = remaining < stepMs ? remaining : stepMs;
    if (step == 0) step = 1;   // always make forward progress
    stepWait(step);
    feed();
  }
}

}  // namespace cloud
}  // namespace nimbus
