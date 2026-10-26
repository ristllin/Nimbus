#pragma once
#include <cstdint>

// owner_first - background work on the turn task waits for the owner.
//
// Every turn, fold and staged job runs on one task (tg_poll), one at a time, and
// a Telegram message is only seen when that task polls between its jobs. Lumi on
// v4.6.0 (26 Oct 2026) showed the cost: the post-update awareness turn ran four
// minutes, the fold pump started a 74 s fold in the same pass, and the owner's
// message, sent during the turn, was fetched and answered 5.5 minutes later.
//
// The rule, decided here and wired by the orchestrator's pump (pollJobs):
//   * background work (an automatic fold) starts only in a pump pass where no
//     turn ended since the previous pass ended - so the channel poll that ran in
//     between found nobody waiting - and no inbound page is being held for the
//     rest of an album;
//   * a staged system turn (the post-update awareness turn) also waits until the
//     owner has been quiet for kSystemTurnQuietMs since it was staged and since
//     the last turn ended, so an owner who writes right after the update notice
//     is answered first.
// Owner work never waits on this gate. Arduino-free, host-tested (test_owner_first).
namespace nimbus {
namespace orch {

class OwnerFirstGate {
 public:
  static constexpr uint32_t kSystemTurnQuietMs = 120000;

  // Any turn ended (owner, local, scheduled or synthesis; failed ones too).
  void noteTurnEnd(uint32_t nowMs) {
    ++turnEnds_;
    lastTurnEndMs_ = nowMs;
    anyTurn_ = true;
  }
  // The pump pass ended: the next pass follows a channel poll.
  void notePumpEnd() { turnEndsAtPump_ = turnEnds_; }

  // An automatic fold (or other background work) may start in this pass.
  bool backgroundMayStart(bool pageHeld) const { return !pageHeld && turnEnds_ == turnEndsAtPump_; }

  // The staged system turn may run in this pass (staged at `stagedAtMs`).
  bool systemTurnMayStart(bool pageHeld, uint32_t nowMs, uint32_t stagedAtMs) const {
    if (!backgroundMayStart(pageHeld)) return false;
    if (nowMs - stagedAtMs < kSystemTurnQuietMs) return false;
    return !anyTurn_ || nowMs - lastTurnEndMs_ >= kSystemTurnQuietMs;
  }

 private:
  uint32_t turnEnds_ = 0;
  uint32_t turnEndsAtPump_ = 0;
  uint32_t lastTurnEndMs_ = 0;
  bool anyTurn_ = false;
};

}  // namespace orch
}  // namespace nimbus
