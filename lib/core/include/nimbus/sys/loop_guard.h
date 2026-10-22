#pragma once
#include <cstdint>
#include <string>

#include "nimbus/cloud/relay_timing.h"   // kTaskWdtTimeoutMs: the one watchdog constant

// loop_guard - the rules that keep the main loop inside its task watchdog.
//
// The loop task is the only task the 8 s task watchdog watches (src/main.cpp). It
// shares one lock with every task that touches the card and the memory engines
// (agent::memory::lock, a recursive mutex). The turn task holds that lock across a
// whole tool dispatch, which can include a provider round trip (an embedding) or a
// card scan, and the web task holds it across its memory routes. A loop that waits
// for the lock waits as long as the slowest of those: past 8 s the watchdog resets
// the device. On the owner's device every boot from 19 to 21 Oct 2026 ended inside a
// tool dispatch this way.
//
// So a watched task never waits for that lock. It tries for at most kLoopLockWaitMs
// and, when the lock is busy, skips or parks the guarded work for a later pass. A
// blocking acquire on a watched task is a bug: the device counts each one, and the
// host test test_loop_guard rejects one in the loop's storage calls at source level.
namespace nimbus::sys {

// The longest a watched task tries for the shared lock before it gives up.
inline constexpr uint32_t kLoopLockWaitMs = 20;
// Bounded tries one loop pass can make: the SD health tick (pending timeline rows and
// the probe share one try), the battery history row, a timeline event, the retention
// pass and the episodic index slice.
inline constexpr uint32_t kLoopLockTriesPerPass = 5;
// Even if every try of a pass times out, the pass spends at most a small share of the
// watchdog window on them.
static_assert(kLoopLockWaitMs * kLoopLockTriesPerPass * 20 <= cloud::kTaskWdtTimeoutMs,
              "the loop's bounded lock tries must stay a small share of the watchdog");

// How long a caller may wait for the shared lock: a watched task waits at most
// `requestedMs`, capped at kLoopLockWaitMs (a caller that retries every pass asks for
// 0), unless it already holds the lock (a recursive re-take returns at once). Any
// other task may block until the lock is free (UINT32_MAX).
uint32_t lockWaitMs(bool watchedTask, bool ownsLock, uint32_t requestedMs = kLoopLockWaitMs);

// A loop pass longer than this is evidence that something blocked the loop: it is
// logged, so a stall short of the watchdog still names itself in the field.
inline constexpr uint32_t kSlowPassMs = 2000;
// After the first slow pass, log again only for a new maximum this much higher, or
// once the quiet interval has passed (one slow pass per window, never a flood).
inline constexpr uint32_t kSlowPassRelogStepMs = 1000;
inline constexpr uint32_t kSlowPassRelogQuietMs = 10u * 60u * 1000u;

class LoopPassMonitor {
 public:
  // Record one pass of `passMs` that ended at `nowMs`. True when the caller should log
  // it (slowPassLine): the first slow pass, a new maximum kSlowPassRelogStepMs above
  // the last one logged, or a slow pass kSlowPassRelogQuietMs after the last log.
  bool note(uint32_t passMs, uint32_t nowMs);
  uint32_t maxPassMs() const { return maxMs_; }
  uint32_t maxAtMs() const { return maxAt_; }
  uint32_t slowPasses() const { return slow_; }

 private:
  uint32_t maxMs_ = 0;
  uint32_t maxAt_ = 0;
  uint32_t slow_ = 0;
  uint32_t loggedMs_ = 0;
  uint32_t loggedAt_ = 0;
  bool logged_ = false;
};

// "loop: one pass took 2400 ms (longest 2400 ms, 1 slow pass this boot)"
std::string slowPassLine(uint32_t passMs, const LoopPassMonitor& m);

// The holder side: a task that held the shared lock this long is named in the log,
// so a slow tool shows up even after the loop stopped waiting for it. The model picks
// the tools, so the line is rate limited like the slow-pass line: the first long hold,
// a new longest hold kSlowPassRelogStepMs above the last one logged, or a long hold
// kLongHoldRelogQuietMs after the last line (every hold is still counted).
inline constexpr uint32_t kLongHoldMs = 3000;
inline constexpr uint32_t kLongHoldRelogQuietMs = 60u * 1000u;

class LockHoldMonitor {
 public:
  // Record one hold of `holdMs` by `what` (a tool name) that ended at `nowMs`. True when
  // it should be logged (longHoldLine).
  bool note(const std::string& what, uint32_t holdMs, uint32_t nowMs);
  uint32_t longestMs() const { return longestMs_; }
  const std::string& longestWhat() const { return longestWhat_; }
  uint32_t longHolds() const { return long_; }

 private:
  uint32_t longestMs_ = 0;
  std::string longestWhat_;
  uint32_t long_ = 0;
  uint32_t loggedMs_ = 0;
  uint32_t loggedAt_ = 0;
  bool logged_ = false;
};

// "memory: tool memory.search held the card lock for 9200 ms"
std::string longHoldLine(const std::string& what, uint32_t holdMs);

}  // namespace nimbus::sys
