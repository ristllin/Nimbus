#include "nimbus/epd_sched.h"

// Pure FSM: no clock, no panel access - the caller feeds nowMs and executes
// the returned commands. All timing is unsigned-subtraction interval math,
// safe across uint32 wrap for intervals < 2^31 ms.

namespace nimbus::epd {

// ms since the last detent, robust to a detent stamped slightly AFTER the tick's
// nowMs (encoder events are ISR-timestamped, then drained by a loop that may
// have sampled millis() earlier). A "future" detent reads as 0 elapsed - i.e.
// keep waiting - instead of underflowing to ~2^32 and firing instantly.
uint32_t Scheduler::dwellElapsed(uint32_t nowMs) const {
  const uint32_t d = nowMs - lastDetentMs_;
  return d < 0x80000000u ? d : 0u;
}

void Scheduler::configure(const SchedConfig& cfg) {
  // Live profile switch: parameters only. Pending intent, armed dwell and the
  // coalesce/ghosting bookkeeping deliberately survive - the new windows apply
  // to work already in flight.
  cfg_ = cfg;
}

void Scheduler::onDetent(uint8_t screenId, uint32_t nowMs) {
  // Detents never render; they only (re)arm the dwell timer. Re-arming during
  // panel-busy is the normal "keep scrolling through a 2.2 s refresh" path:
  // the render fires dwellMs after the LAST detent once the panel frees.
  dwellArmed_ = true;
  lastDetentMs_ = nowMs;
  dwellScreen_ = screenId;
}

void Scheduler::onIntent(uint8_t screenId, bool attention, uint32_t /*nowMs*/,
                         Kind kind) {
  if (!attention && hasPending_ && pendingAttention_) {
    // Ambient never clobbers a pending attention. Dropping (not queueing) it
    // is safe: ambient producers re-emit on every state delta, so the ambient
    // screen reaches the panel via a fresh intent in the next coalesce window.
    return;
  }
  // Latest wins the single slot; attention replaces anything.
  hasPending_ = true;
  pendingScreen_ = screenId;
  pendingKind_ = kind;
  pendingAttention_ = attention;
}

void Scheduler::onRenderDone(uint32_t nowMs) {
  if (!busy_) return;
  busy_ = false;
  // Only ambient completions advance the coalesce clock: attention and dwell
  // renders are user/event-initiated and must not delay the next ambient
  // flush. (lastIssuedCounts_ marks the in-flight command as ambient; the
  // ghost counter itself is resolved entirely at issue time in tick().)
  if (lastIssuedCounts_) {
    lastAmbientDoneMs_ = nowMs;
    ambientEverDone_ = true;
    lastIssuedCounts_ = false;
  }
}

bool Scheduler::clearPendingAmbient() {
  if (hasPending_ && !pendingAttention_) { hasPending_ = false; return true; }
  return false;
}

RenderCommand Scheduler::tick(uint32_t nowMs) {
  RenderCommand cmd;
  if (busy_) return cmd;

  // Priority: attention > settled dwell > coalesced ambient. An armed but
  // not-yet-settled dwell does not block an ambient flush.
  if (hasPending_ && pendingAttention_) {
    cmd = {true, pendingScreen_, pendingKind_, false};
    hasPending_ = false;
    lastIssuedCounts_ = false;
  } else if (dwellArmed_ && dwellElapsed(nowMs) >= cfg_.dwellMs) {
    cmd = {true, dwellScreen_, Kind::FastBW, false};
    dwellArmed_ = false;
    lastIssuedCounts_ = false;
  } else if (hasPending_ && (!ambientEverDone_ ||
                             nowMs - lastAmbientDoneMs_ >= cfg_.coalesceMs)) {
    cmd = {true, pendingScreen_, pendingKind_, false};
    hasPending_ = false;
    lastIssuedCounts_ = true;
  }
  if (!cmd.render) return cmd;

  busy_ = true;
  // Ghosting: every fullEveryN-th issued fast/partial command is upgraded to
  // a full clear cycle and resets the counter; color renders neither count
  // nor upgrade. fullEveryN == 0 disables the upgrade (counter freezes).
  if (cmd.kind != Kind::Color && cfg_.fullEveryN != 0) {
    if (uint8_t(sinceFullClear_ + 1) >= cfg_.fullEveryN) {
      cmd.fullClear = true;
      sinceFullClear_ = 0;
    } else {
      ++sinceFullClear_;
    }
  }
  return cmd;
}

}  // namespace nimbus::epd
