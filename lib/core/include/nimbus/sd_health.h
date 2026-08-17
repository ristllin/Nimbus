#pragma once
#include <cstdint>

// SdHealthTracker - the portable decision core for SD graceful degradation.
// The device feeds it the outcome of each SD IO attempt (real vector/episodic
// writes AND a periodic liveness probe); it debounces transient blips with a
// consecutive-streak threshold and emits a DEMOTE edge (the card went away - the
// memory subsystem must drop to no-card behaviour without a reboot) or a PROMOTE
// edge (the card is answering again - re-wire to the SD tier). Keeping this pure
// makes the sudden-loss/recovery contract host-testable, so future refactors
// can't silently regress it (the HIL test spec).
//
//   healthy --(demoteAfter consecutive failures)--> lost      [Event::Demote]
//   lost    --(promoteAfter consecutive successes)--> healthy [Event::Promote]
//
// A single opposite result resets the opposing streak, so a flapping card can't
// thrash the tier: N in a row are required each way.

namespace nimbus {

class SdHealthTracker {
 public:
  enum class Event : uint8_t { None = 0, Demote, Promote };

  explicit SdHealthTracker(uint8_t demoteAfter = 2, uint8_t promoteAfter = 2)
      : demoteAfter_(demoteAfter ? demoteAfter : 1),
        promoteAfter_(promoteAfter ? promoteAfter : 1) {}

  bool lost() const { return lost_; }

  // Feed one IO outcome. Returns the edge (if this result crossed a threshold).
  Event note(bool ioOk) {
    if (!lost_) {
      if (ioOk) { failStreak_ = 0; return Event::None; }
      if (++failStreak_ >= demoteAfter_) { lost_ = true; failStreak_ = okStreak_ = 0; return Event::Demote; }
      return Event::None;
    }
    if (!ioOk) { okStreak_ = 0; return Event::None; }
    if (++okStreak_ >= promoteAfter_) { lost_ = false; failStreak_ = okStreak_ = 0; return Event::Promote; }
    return Event::None;
  }

  // Explicit transitions (e.g. FAULT injection / the /api/sdprobe button). Return
  // the edge if the state actually changed, else None.
  Event forceDemote() { if (lost_) return Event::None; lost_ = true; failStreak_ = okStreak_ = 0; return Event::Demote; }
  Event forcePromote() { if (!lost_) return Event::None; lost_ = false; failStreak_ = okStreak_ = 0; return Event::Promote; }

  uint8_t failStreak() const { return failStreak_; }
  uint8_t okStreak() const { return okStreak_; }

 private:
  uint8_t demoteAfter_;
  uint8_t promoteAfter_;
  uint8_t failStreak_ = 0;
  uint8_t okStreak_ = 0;
  bool    lost_ = false;
};

}  // namespace nimbus
