#pragma once
#include <cstdint>

// nimbus::SaverTimer - the screensaver idle clock (portable, host-tested).
//
// The device shows ScreenId::Screensaver after `thresholdMin` minutes with no
// ACTIVITY, where activity is a deliberately narrow set of edges wired in
// main.cpp: encoder events, attention-router job EDGES (repeat frames and the
// broker's ~30 s snapshot heartbeats must NOT count, or a Notifier with a
// running broker would never sleep), and any non-ambient screen render (turns,
// asks, menu, QR...). Periodic ambient StatusIdle repaints do not count.
//
// All math is wrap-safe uint32 millis arithmetic. threshold 0 = disabled.
// noteActivity may be called from a non-main task (a turn finishing on
// tg_poll): it is a single aligned 32-bit store, torn reads are impossible on
// Xtensa/RISC-V and a lost race only nudges the idle clock by one event.

namespace nimbus {

class SaverTimer {
 public:
  void setThresholdMin(uint16_t m) { thrMin_ = m; }
  uint16_t thresholdMin() const { return thrMin_; }

  void noteActivity(uint32_t nowMs) { lastMs_ = nowMs; }
  uint32_t lastActivityMs() const { return lastMs_; }

  // True once `thresholdMin` minutes have passed since the last activity.
  // (The device additionally gates entry on "ambient screen showing".)
  //
  // FUTURE-SAFE (field bug 2026-07-18, Board 1): the caller captures `nowMs` at
  // its loop top, but noteActivity() runs mid-iteration with a FRESHER millis()
  // - so lastMs_ can be a few ms "ahead" of nowMs. The old bare subtraction
  // underflowed that to ~2^32 and fired the screensaver SECONDS after a knob
  // interaction (repro: menu close -> logo flash; SAVERDBG now=19937
  // last=19938). A "future" lastMs means zero idle, not a 49-day wrap.
  bool due(uint32_t nowMs) const {
    if (thrMin_ == 0) return false;
    const uint32_t idle = nowMs - lastMs_;
    if (idle > 0x80000000u) return false;   // lastMs ahead of nowMs: zero idle
    return idle >= uint32_t(thrMin_) * 60000u;
  }

 private:
  uint32_t lastMs_ = 0;
  uint16_t thrMin_ = 60;   // owner default: 1 h
};

}  // namespace nimbus
