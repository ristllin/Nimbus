#pragma once
// write_pacing - settings-storage (NVS) writes that must not repeat on every tick.
//
// Three markers rewrote NVS far more often than their meaning changes, and none of them
// looked at whether the write landed (host-tested in test/test_ota_poll_guard):
//
//   - the "update available" notice marker (otaNotif): when its write failed, every
//     check re-sent the notice (two minutes after each boot, daily, every manual check);
//   - the low-battery ping epoch (lbPingEp): its saved copy moved whether or not the
//     write landed, so a refused write was never retried, and the five-minute wake
//     boots of the low-battery sleep pinged again;
//   - the battery model (battModel): rewritten every time the full-charge anchor moved
//     by a millivolt. On the owner's desk-powered device it spent about 1.2 KB of free
//     settings storage in three hours overnight (22 Oct 2026). Each rewrite writes the
//     new value before the old one is erased, so a churning value eats entries until
//     the partition compacts.
//
// No Arduino: the device seam (src/main.cpp, src/sys/ota_update.cpp) owns the clock,
// the store calls and the RTC copy.

#include <cstdint>
#include <string>

namespace nimbus {
namespace store {

// A per-boot RAM latch for "the owner was told about X": claim() is true at most once
// per value per boot, whatever the stored marker's write does.
class NoticeLatch {
 public:
  // True when `value` should be announced now: it is not the stored marker and was not
  // announced this boot. The value is latched either way.
  bool claim(const std::string& value, const std::string& stored) {
    if (value.empty() || value == stored || value == latched_) return false;
    latched_ = value;
    return true;
  }
  const std::string& latched() const { return latched_; }

 private:
  std::string latched_;
};

// A persisted copy of a RAM value that moves only when its write lands. A write that
// did not land is retried, but at most every retryMs (a refused write costs nothing
// on flash, yet one failure line per tick is noise).
class SavedMirror {
 public:
  explicit SavedMirror(uint32_t saved = 0, uint32_t retryMs = 10u * 60u * 1000u)
      : saved_(saved), retryMs_(retryMs) {}
  bool due(uint32_t value, uint32_t nowMs) const {
    if (value == saved_) return false;
    return !failed_ || (uint32_t)(nowMs - failedMs_) >= retryMs_;
  }
  void noteWrite(uint32_t value, bool landed, uint32_t nowMs) {
    failed_ = !landed;
    failedMs_ = nowMs;
    if (landed) saved_ = value;
  }
  uint32_t saved() const { return saved_; }

 private:
  uint32_t saved_;
  uint32_t retryMs_;
  uint32_t failedMs_ = 0;
  bool     failed_ = false;
};

// BATTMODEL-CHURN: when the battery model may be saved. An owner action (calibrate,
// reset) saves at once. Otherwise a save needs a change worth keeping (a completed
// discharge segment, or the full-charge anchor moved by at least kAnchorStepMv per
// cell from the saved value) and at least kMinIntervalMs since the last attempt (the
// first attempt of a boot may go at once). A write that did not land keeps the saved
// values, so the change is retried at the next allowed slot.
class BattModelSavePacer {
 public:
  static constexpr uint32_t kMinIntervalMs = 60u * 60u * 1000u;   // at most hourly
  static constexpr uint16_t kAnchorStepMv = 20;                   // per cell

  void seed(uint16_t savedSegments, uint16_t savedAnchorMv) {
    savedSegments_ = savedSegments;
    savedAnchor_ = savedAnchorMv;
  }
  bool changed(uint16_t segments, uint16_t anchorMv) const {
    const uint16_t step = anchorMv > savedAnchor_ ? anchorMv - savedAnchor_ : savedAnchor_ - anchorMv;
    return segments != savedSegments_ || step >= kAnchorStepMv;
  }
  bool due(uint16_t segments, uint16_t anchorMv, uint32_t nowMs, bool ownerAction) const {
    if (ownerAction) return true;
    if (!changed(segments, anchorMv)) return false;
    return !attempted_ || (uint32_t)(nowMs - lastAttemptMs_) >= kMinIntervalMs;
  }
  void noteSave(uint16_t segments, uint16_t anchorMv, bool landed, uint32_t nowMs) {
    attempted_ = true;
    lastAttemptMs_ = nowMs;
    if (!landed) return;
    savedSegments_ = segments;
    savedAnchor_ = anchorMv;
  }
  uint16_t savedSegments() const { return savedSegments_; }
  uint16_t savedAnchorMv() const { return savedAnchor_; }

 private:
  uint16_t savedSegments_ = 0xFFFF;   // nothing saved yet: the first real value differs
  uint16_t savedAnchor_ = 0xFFFF;
  uint32_t lastAttemptMs_ = 0;
  bool     attempted_ = false;
};

}  // namespace store
}  // namespace nimbus
