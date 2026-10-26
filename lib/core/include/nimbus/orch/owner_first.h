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
// The rule, decided here and wired by the orchestrator's pump (pollJobs): an
// automatic fold and the staged system turn (the post-update awareness turn)
// start only after kQuietMs with no turn ending, and never while an inbound page
// is being held for the rest of an album; the system turn also waits kQuietMs
// after it was staged. An owner in a conversation (a message within two minutes
// of the last reply) is therefore never queued behind either, on any channel and
// whatever order the poll loop runs its steps in.
// This decides only what STARTS: a message that arrives while a fold or the system
// turn is already running waits for it to finish (the turn task is single by
// design). Routines, sub-agent synthesis and the nightly upkeep are not gated, and
// a fold queued by a context overflow is not either (the next turn needs it).
// Owner work never waits on this gate. Arduino-free, host-tested (test_owner_first).
namespace nimbus {
namespace orch {

class OwnerFirstGate {
 public:
  static constexpr uint32_t kQuietMs = 120000;

  // Any turn ended (owner, local, scheduled or synthesis; failed ones too).
  void noteTurnEnd(uint32_t nowMs) {
    lastTurnEndMs_ = nowMs;
    anyTurn_ = true;
  }

  // An automatic fold may start now.
  bool backgroundMayStart(bool pageHeld, uint32_t nowMs) const {
    return !pageHeld && (!anyTurn_ || nowMs - lastTurnEndMs_ >= kQuietMs);
  }

  // The staged system turn (staged at `stagedAtMs`) may start now.
  bool systemTurnMayStart(bool pageHeld, uint32_t nowMs, uint32_t stagedAtMs) const {
    return backgroundMayStart(pageHeld, nowMs) && nowMs - stagedAtMs >= kQuietMs;
  }

 private:
  uint32_t lastTurnEndMs_ = 0;
  bool anyTurn_ = false;
};

}  // namespace orch
}  // namespace nimbus
