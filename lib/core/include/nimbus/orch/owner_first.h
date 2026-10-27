#pragma once
#include <cstdint>

// owner_first - the automatic fold and the staged system turn wait for the owner.
//
// Every turn, fold and staged job runs on one task (tg_poll), one at a time, and
// a Telegram message is only seen when that task polls between its jobs. Lumi on
// v4.6.0 (26 Oct 2026) showed the cost: the post-update awareness turn ran four
// minutes, the fold pump started a 74 s fold in the same pass, and the owner's
// message, sent during the turn, was fetched and answered 5.5 minutes later.
//
// The rule, decided here and wired by the orchestrator's pump (pollJobs):
//   * a due automatic fold starts after kQuietMs with no turn ending, and never
//     while an inbound page is held for the rest of an album. So that a brisk
//     conversation still gets its summary (the rows a history shift hid are in
//     neither the summary nor the window until a fold takes them), a fold that has
//     waited kFoldMaxDeferMs or kFoldMaxDeferTurns turns starts anyway, in the next
//     pump pass that follows a poll with no turn ending: the last reply has been
//     sent and nothing is running. At Lumi's window the history shifts about every
//     6-8 turns, so a 6-turn bound keeps at most about one window of rows hidden;
//     10 minutes covers a slower steady chat (one message every 90 s). A chat with
//     no gap at all (every poll returns a message) never has such a pass, so at
//     twice the bound the fold starts right after a turn: never inside one, but that
//     turn's reply goes out when the fold ends (the poll loop drains replies after
//     the pump);
//   * the staged system turn (the post-update awareness turn) starts only after
//     kQuietMs with no turn ending and kQuietMs after it was staged, so an owner who
//     writes right after the update notice is answered first.
// This decides only what STARTS: a message that arrives while a fold or the system
// turn is running waits for it (the turn task is single by design). Routines,
// sub-agent synthesis and the nightly upkeep are not gated, and a fold queued by a
// context overflow is not either (the next turn needs it). Owner work never waits
// on this gate. Arduino-free, host-tested (test_owner_first).
namespace nimbus {
namespace orch {

class OwnerFirstGate {
 public:
  static constexpr uint32_t kQuietMs = 120000;
  static constexpr uint32_t kFoldMaxDeferMs = 600000;
  static constexpr uint32_t kFoldMaxDeferTurns = 6;

  // Any turn ended (owner, local, scheduled or synthesis; failed ones too).
  void noteTurnEnd(uint32_t nowMs) {
    ++turnEnds_;
    lastTurnEndMs_ = nowMs;
    anyTurn_ = true;
  }
  // The pump pass ended: the next pass follows a channel poll and a reply drain.
  void notePumpEnd() { turnEndsAtPump_ = turnEnds_; }

  // A due automatic fold may start now. Called once per pump pass while a fold is
  // due: the first refusal starts the wait the bound counts from.
  bool foldMayStart(bool pageHeld, uint32_t nowMs) {
    if (pageHeld) return false;
    if (quiet(nowMs) || waitedOut(nowMs)) {
      deferring_ = false;
      return true;
    }
    if (!deferring_) {
      deferring_ = true;
      deferSinceMs_ = nowMs;
      deferSinceTurns_ = turnEnds_;
    }
    return false;
  }
  // No fold is due: a later one starts its own wait.
  void noteFoldNotDue() { deferring_ = false; }

  // The staged system turn (staged at `stagedAtMs`) may start now.
  bool systemTurnMayStart(bool pageHeld, uint32_t nowMs, uint32_t stagedAtMs) const {
    return !pageHeld && quiet(nowMs) && nowMs - stagedAtMs >= kQuietMs;
  }

 private:
  bool quiet(uint32_t nowMs) const { return !anyTurn_ || nowMs - lastTurnEndMs_ >= kQuietMs; }
  bool waitedOut(uint32_t nowMs) const {
    if (!deferring_) return false;
    const uint32_t tier = turnEnds_ == turnEndsAtPump_ ? 1 : 2;   // 2: no gap in the chat at all
    return nowMs - deferSinceMs_ >= tier * kFoldMaxDeferMs || turnEnds_ - deferSinceTurns_ >= tier * kFoldMaxDeferTurns;
  }

  uint32_t turnEnds_ = 0;
  uint32_t turnEndsAtPump_ = 0;
  uint32_t lastTurnEndMs_ = 0;
  uint32_t deferSinceMs_ = 0;
  uint32_t deferSinceTurns_ = 0;
  bool anyTurn_ = false;
  bool deferring_ = false;
};

}  // namespace orch
}  // namespace nimbus
