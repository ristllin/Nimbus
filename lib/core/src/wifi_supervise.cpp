// wifi_supervise - see nimbus/wifi/supervise.h. Pure, host-tested.
#include "nimbus/wifi/supervise.h"

namespace nimbus {
namespace wifi {

const char* superviseActName(SuperviseAct a) {
  switch (a) {
    case SuperviseAct::Engage: return "engage";
    case SuperviseAct::Drive:  return "drive";
    case SuperviseAct::BowOut: return "bowout";
    case SuperviseAct::WaitGrace: break;
  }
  return "wait";
}

SuperviseDecision decideSupervise(const SuperviseInputs& in) {
  SuperviseDecision d;

  // A manual credential test (the user hit Connect, or the escape-hatch wizard) suppresses
  // failover so the supervisor never scans the network the owner is asking for off the air.
  // That hold is BOUNDED: msSinceJoin is measured from the credential test and is cleared
  // to 0 the instant the station connects, so a value that is non-zero AND aged past the
  // window means the joined network never came up (or is gone). The owner asked for that
  // network, not for permanent radio silence - so the hold, and the owed re-begin kick it
  // gated, both release once the window passes. The old bow-out kick required
  // msSinceJoin == 0, which a never-landed join never reached, consuming the kick forever
  // (CUM-294 live repro). Keyed on the join clock, not on down-time, so a manual join
  // issued while the link is already down still gets its full window instead of none.
  const bool manualActive = in.msSinceJoin != 0 && in.msSinceJoin < in.manualJoinHoldMs;

  // Someone else owns the radio, or the owner wants the setup AP up: the supervisor must
  // stand aside, and a boot-slot re-begin would fight them.
  const bool blocked = in.manualHold || manualActive || in.apStations != 0;

  // Take over only when there is somewhere to fail over TO and nobody else is driving.
  const bool eligible = in.onboarded && in.knownCount >= 2 && !blocked;

  if (!eligible) {
    d.act = SuperviseAct::BowOut;
    // A boot-slot re-begin is DUE this tick when the supervisor is bowing out of an engaged
    // failover (the classic engaged->disengaged edge) OR a kick from an earlier edge is
    // still owed - either way with the link down and at least one saved network to hand
    // back to the core. Without this hand-back the core sits idle after our disconnect
    // (it short-circuits auto-reconnect past our ASSOC_LEAVE) until a reboot (CUM-207 F2).
    const bool due = (in.engaged || in.kickOwed) && in.knownCount >= 1;
    if (due) {
      if (blocked) {
        // Skip the kick now (it would fight whoever owns the radio) but REMEMBER it -
        // firing it once the blocker clears is exactly the CUM-294 fix: a blocked edge is
        // no longer consumed forever.
        d.kickOwed = true;
      } else {
        d.fireRebegin = true;
        d.kickOwed = false;
      }
    } else {
      // Nothing to hand back yet (no saved network); keep owing until there is.
      d.kickOwed = in.kickOwed;
    }
    return d;
  }

  // Eligible to run the failover supervisor ourselves - any owed kick is moot, we are
  // taking the radio.
  d.kickOwed = false;
  if (!in.engaged) {
    // Let the core have its usual fast-reconnect window before we take over.
    if ((uint32_t)(in.nowMs - in.downSinceMs) < in.coreGraceMs) {
      d.act = SuperviseAct::WaitGrace;
      return d;
    }
    d.act = SuperviseAct::Engage;
    return d;
  }
  d.act = SuperviseAct::Drive;
  return d;
}

}  // namespace wifi
}  // namespace nimbus
