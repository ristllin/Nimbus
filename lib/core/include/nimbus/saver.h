#pragma once
#include <cstdint>

// nimbus::SaverTimer - the screensaver idle clock (portable, host-tested).
//
// The device shows ScreenId::Screensaver after `thresholdMin` minutes with no
// ACTIVITY, where activity is a deliberately narrow set of edges wired in
// main.cpp: input events, attention-router job EDGES (repeat frames and the
// broker's ~30 s snapshot heartbeats must NOT count, or a Notifier with a
// running broker would never sleep), and any non-ambient screen render (turns,
// asks, menu, QR...). Periodic ambient StatusIdle repaints do not count.
//
// All math is wrap-safe uint32 millis arithmetic. threshold 0 = disabled.
// noteActivity may be called from a non-main task (a turn finishing on
// tg_poll): it is a single aligned 32-bit store, torn reads are impossible on
// Xtensa/RISC-V and a lost race only nudges the idle clock by one event.

namespace nimbus {

// The three screensaver stages, ordered by how far the panel has powered down:
//   Active   - the owner's live screen, backlight at the battery mode's level.
//   Rest     - the screensaver logo, backlight at kBacklightRestPct (a faint
//              glow: "resting", not "broken").
//   DeepDim  - backlight fully OFF to save the panel's dominant draw; the touch
//              controller stays armed so any tap wakes it (the waking tap is
//              swallowed, never actuated). Reached only after Rest, only on
//              battery (see nimbus::deepDimExtraMs), and only where the backlight
//              is genuinely driveable.
enum class SaverStage : uint8_t { Active = 0, Rest = 1, DeepDim = 2 };

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
  // underflowed that to ~2^32 and fired the screensaver SECONDS after a gesture
  // interaction (repro: menu close -> logo flash; SAVERDBG now=19937
  // last=19938). A "future" lastMs means zero idle, not a 49-day wrap.
  bool due(uint32_t nowMs) const {
    if (thrMin_ == 0) return false;
    const uint32_t idle = idleMs(nowMs);
    if (idle > 0x80000000u) return false;   // lastMs ahead of nowMs: zero idle
    return idle >= uint32_t(thrMin_) * 60000u;
  }

  // True once the screensaver threshold PLUS `deepExtraMs` of further idle have
  // passed - the point at which the backlight goes fully off (deep-dim).
  //   deepExtraMs == 0 disables deep-dim (the external-power case): the panel
  //   holds at Rest and never goes fully dark.
  // Same wrap-safe / future-safe idle math as due(). The threshold is checked in
  // two steps (rest, then the extra) so the sum can never overflow uint32 for
  // large thresholds.
  bool deepDimDue(uint32_t nowMs, uint32_t deepExtraMs) const {
    if (thrMin_ == 0 || deepExtraMs == 0) return false;
    const uint32_t idle = idleMs(nowMs);
    if (idle > 0x80000000u) return false;   // lastMs ahead of nowMs: zero idle
    const uint32_t restMs = uint32_t(thrMin_) * 60000u;
    if (idle < restMs) return false;
    return (idle - restMs) >= deepExtraMs;
  }

  // The composite stage for `nowMs` given a per-mode deep-dim delay. The single
  // source of truth the device loop and its tests share for "which stage now".
  SaverStage stage(uint32_t nowMs, uint32_t deepExtraMs) const {
    if (deepDimDue(nowMs, deepExtraMs)) return SaverStage::DeepDim;
    if (due(nowMs)) return SaverStage::Rest;
    return SaverStage::Active;
  }

 private:
  uint32_t idleMs(uint32_t nowMs) const { return nowMs - lastMs_; }

  uint32_t lastMs_ = 0;
  uint16_t thrMin_ = 60;   // owner default: 1 h
};

// Clamp an owner-supplied screen-rest delay to the stored range: 0..1440 minutes,
// where 0 means the screen never rests (always on). A remote setter (POST
// /api/config) parses a signed value, so this must fold a negative back to 0
// rather than let it wrap through the uint16 store (where -1 would land at the
// 1440 ceiling, the opposite of what the owner asked for). Pure + host-tested; the
// web handler and the store's setSaverMin() share this one range.
constexpr int clampSaverMinutes(long v) {
  return v < 0 ? 0 : (v > 1440 ? 1440 : int(v));
}

// A touch that arrives while the panel is deep-dimmed (backlight fully off) is a
// WAKE ONLY: it must restore the screen and be SWALLOWED, never actuating what
// sits under the finger - a blind tap must not press a Danger-zone button. This
// predicate is the single source of truth the device touch path and its tests
// share (CUM-292); the device passes "was the backlight genuinely off".
constexpr bool wakeTapIsSwallowed(bool backlightWasOff) { return backlightWasOff; }

}  // namespace nimbus
