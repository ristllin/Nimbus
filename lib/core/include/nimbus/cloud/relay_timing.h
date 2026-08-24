#pragma once
#include <cstdint>
#include <functional>

// relay_timing - the PURE, host-tested timing contract for the cloud relay
// (CUM-160). No Arduino, no TLS.
//
// The device serializes every outbound TLS through a SINGLE work slot
// (src/sys/tls_arbiter.cpp - a frozen invariant: one slot, no on-device
// concurrency). A real relay/Cloudflare connect legitimately takes many seconds
// (12s+ observed) - that is latency, NOT a hang, and aborting it early just prevents
// the device from ever reconnecting (a field regression). The relay task is NOT
// watchdog-subscribed, so its blocking connect does not itself trip the 8s watchdog.
//
// The safe shape for the relay's own poll/read waits is therefore NOT an early abort
// but a WATCHDOG-FED, STEPPED wait: run up to a generous total, but never block a
// single step longer than `relayStepMs`, feeding the watchdog between steps so a
// watchdog-fed task can never starve. driveStagedWait() encodes exactly that, purely,
// so the control flow (feeds happen, no early abort, bounded steps) is host-tested.

namespace nimbus {
namespace cloud {

// The system task-watchdog timeout. SINGLE SOURCE for both the watchdog config
// (src/main.cpp esp_task_wdt_config) and the step budget below, so the two can never
// silently drift. Changing the on-device watchdog means changing this one constant.
constexpr uint32_t kTaskWdtTimeoutMs = 8000;

// The longest a single staged step may block. A quarter of the watchdog: even a
// watchdog-fed task blocked for one step still has 3/4 of the window to spare.
constexpr uint32_t relayStepMs = kTaskWdtTimeoutMs / 4;   // 2000 ms

enum class WaitOutcome : uint8_t { Done, TimedOut };

// Drive a long wait as bounded, watchdog-fed steps. Runs until pollDone() is true
// (WaitOutcome::Done) or `totalMs` elapses (WaitOutcome::TimedOut) - it never aborts
// before the real total, so a slow-but-legal operation completes. Each iteration:
// poll, check the total, then block for at most `stepMs` and feed. The caller injects
// everything so the flow is host-testable against a synthetic slow operation:
//   nowMs()      -> monotonic milliseconds
//   pollDone()   -> true once the operation has completed
//   stepWait(ms) -> block up to `ms` (device: one socket read/poll step)
//   feed()       -> feed the watchdog / yield between steps
WaitOutcome driveStagedWait(uint32_t totalMs, uint32_t stepMs,
                            const std::function<uint32_t()>& nowMs,
                            const std::function<bool()>& pollDone,
                            const std::function<void(uint32_t)>& stepWait,
                            const std::function<void()>& feed);

}  // namespace cloud
}  // namespace nimbus
