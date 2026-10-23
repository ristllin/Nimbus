#pragma once
#include <cstdint>

// SdRecovery - what the device does about the memory card around an outage (the
// SdHealthTracker's Demote and Promote edges): when to remount it, when to write what it
// refused meanwhile, and when to tell the owner.
//
//   remount  only while lost, only once internal memory has recovered (a remount needs
//            fresh DMA memory for the card's setup and fails, leaving the card unmounted,
//            when memory is short), and at most once per kRetryEveryMs: the first one
//            kRetryEveryMs after the loss (a card that answers again is promoted by the
//            probe well before that, with nothing torn down).
//   flush    right after the recovery, step after step while each step makes progress,
//            then (rows still held, the card refusing them) at most once per kRetryEveryMs.
//            Owed vectors are written once per recovery, never on the timer: a card that
//            refuses them (full, failing) costs one attempt, not one per minute.
//   notice   once per outage, after kNoticeGraceMs (a blip that heals itself within the
//            grace loses nothing: writes are held and flushed on recovery, and the device
//            screen shows it); a card that keeps dropping is told about at most once per
//            kNoticeEveryMs.
//
// All times are millis() and wrap-safe (elapsed = now - then). Portable and pure, so the
// host suite drives every edge.

namespace nimbus {

class SdRecovery {
 public:
  static constexpr uint32_t kRetryEveryMs = 60u * 1000u;
  static constexpr uint32_t kNoticeGraceMs = 20u * 1000u;
  static constexpr uint32_t kNoticeEveryMs = 6u * 3600u * 1000u;

  // The demote edge: an outage starts.
  void onLost(uint32_t nowMs) {
    lost_ = true;
    lostAtMs_ = nowMs;
    lastRemountMs_ = nowMs;   // the first remount waits a full interval
    toldThisOutage_ = false;
  }
  // The promote edge: the outage ends, and what was held is written next.
  void onRecovered() {
    lost_ = false;
    afterRecovery_ = true;
  }

  bool lost() const { return lost_; }

  bool remountDue(bool memoryOk, uint32_t nowMs) const {
    return lost_ && memoryOk && nowMs - lastRemountMs_ >= kRetryEveryMs;
  }
  void noteRemount(uint32_t nowMs) { lastRemountMs_ = nowMs; }

  // rowsHeld: history rows wait for the card; vectorsOwed: the vectors' last persist failed.
  bool flushDue(bool rowsHeld, bool vectorsOwed, uint32_t nowMs) const {
    if (lost_) return false;
    if (afterRecovery_) return rowsHeld || vectorsOwed;
    return rowsHeld && nowMs - lastFlushMs_ >= kRetryEveryMs;
  }
  // Is this flush the one right after a recovery (the only one that writes owed vectors)?
  bool flushAfterRecovery() const { return afterRecovery_; }
  // One flush step ran. moreToDo: rows are still held and this step wrote some, so the
  // next step follows at once; otherwise the recovery's flush is over and a held row
  // waits for the timer (or a new row, which moves held rows itself).
  void noteFlush(uint32_t nowMs, bool moreToDo) {
    lastFlushMs_ = nowMs;
    if (!moreToDo) afterRecovery_ = false;
  }

  bool noticeDue(uint32_t nowMs) const {
    if (!lost_ || toldThisOutage_) return false;
    if (nowMs - lostAtMs_ < kNoticeGraceMs) return false;
    return !toldEver_ || nowMs - lastToldMs_ >= kNoticeEveryMs;
  }
  void noteNoticeSent(uint32_t nowMs) {
    toldThisOutage_ = true;
    toldEver_ = true;
    lastToldMs_ = nowMs;
  }

 private:
  bool lost_ = false;
  uint32_t lostAtMs_ = 0;
  uint32_t lastRemountMs_ = 0;   // the loss, then each remount attempt
  bool afterRecovery_ = false;
  uint32_t lastFlushMs_ = 0;
  bool toldThisOutage_ = false;
  bool toldEver_ = false;
  uint32_t lastToldMs_ = 0;
};

// The owner's one notice about an outage (plain words; the device screen and the web
// page show the live state).
inline const char* sdOutageNotice() {
  return "The memory card stopped responding, so saved memories, history and files are out of "
         "reach for now. New messages and memories are held on the device and written to the "
         "card as soon as it answers again.";
}

}  // namespace nimbus
