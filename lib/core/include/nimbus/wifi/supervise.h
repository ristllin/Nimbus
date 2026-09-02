#pragma once
#include <cstdint>

// Wi-Fi failover SUPERVISOR policy - the PURE, host-tested decision for the link-DOWN
// tick of the multi-network supervisor (src/net/wifi_link.cpp). NO Arduino, NO WiFi.h:
// decideSupervise() takes a snapshot of the radio + saved-list state and returns ONE
// action for the seam to run this tick, plus the next value of the owed-kick flag.
//
// Why this exists (CUM-294, promoted from the CUM-207 independent re-review). The
// supervisor hands a live retry back to the core with a single WiFi.begin() "re-begin
// kick" when it bows out of an engaged failover with the link still down. That kick fired
// only on the engaged->disengaged EDGE, so an edge BLOCKED by a transient owner of the
// radio (a manual credential test in flight, a client on the setup AP, or any
// ineligibility) was consumed forever: the STA then sat idle until a scan, a manual join,
// or a reboot. Two shapes were traced, one live-reproduced (2026-09-02): a manual join to
// a hotspot that then vanished stranded the device for ~27 min because the
// msSinceJoinAttempt blocker never released the kick.
//
// The fix, encoded here:
//   - kickOwed: when a re-begin kick is DUE but a blocker holds, REMEMBER it rather than
//     drop the edge, and fire it from the ineligible branch once the blockers clear with
//     the link still down.
//   - The manual-join hold EXPIRES once the joined network is gone past a bounded grace.
//     msSinceJoin is measured from the credential test and is cleared to 0 the moment the
//     station connects, so a value that is non-zero AND older than manualJoinHoldMs means
//     the joined network never came up (or is gone). The old bow-out kick required
//     msSinceJoin == 0, which never became true after a never-landed join, so the kick was
//     consumed forever; here the same bounded window that gates engaging also releases the
//     owed kick. The owner asked for that network, not for permanent radio silence.
//
// Single-network and first-run flows stay byte-identical: with fewer than two saved
// networks (or nothing engaged and no owed kick) the decision is exactly the pre-change
// bow-out with no kick. See docs/wifi-resilience.md.

namespace nimbus {
namespace wifi {

// The one action the supervisor takes on a link-DOWN tick.
enum class SuperviseAct : uint8_t {
  WaitGrace = 0,  // link down but inside the core's fast-reconnect grace - do nothing
  Engage,         // take the radio and start/continue driving the failover cycle
  Drive,          // already engaged - run one driven tick of the policy
  BowOut,         // ineligible - step aside (disengage); fireRebegin may hand a retry back
};
const char* superviseActName(SuperviseAct a);   // "wait"|"engage"|"drive"|"bowout"

// A snapshot of the world on a link-DOWN tick. The seam has already handled Notifier mode
// and the link-UP path before calling this, so the link is always down here and
// downSinceMs is the (non-zero) first-down timestamp.
struct SuperviseInputs {
  bool     onboarded   = false;   // the setup wizard has finished (store::onboarded())
  bool     engaged      = false;  // the supervisor currently owns the radio
  int      knownCount   = 0;      // saved networks in the store
  bool     manualHold   = false;  // the owner published the setup AP (escape hatch hold)
  uint32_t msSinceJoin  = 0;      // since the last manual credential test began; 0 = none
  uint32_t apStations   = 0;      // clients on the setup AP (someone is configuring)
  uint32_t nowMs        = 0;
  uint32_t downSinceMs  = 0;      // first tick the link was seen down (non-zero here)
  bool     kickOwed     = false;  // a re-begin kick was deferred by an earlier blocked edge
  // Tunables - the failover timing constants (the seam leaves these at their defaults).
  uint32_t coreGraceMs      = 8000;    // let the core try first before engaging
  uint32_t manualJoinHoldMs = 20000;   // a manual join suppresses failover this long, then
                                       // the hold (and any owed re-begin kick) releases
};

struct SuperviseDecision {
  SuperviseAct act         = SuperviseAct::WaitGrace;
  bool         fireRebegin = false;   // seam calls rebeginBootSlot() this tick
  bool         kickOwed    = false;   // the owed-kick flag's next value (seam stores it)
};

// Decide the single supervisor action for this link-down tick. See the header note.
SuperviseDecision decideSupervise(const SuperviseInputs& in);

}  // namespace wifi
}  // namespace nimbus
