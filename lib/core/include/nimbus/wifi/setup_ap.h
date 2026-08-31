#pragma once
#include <cstdint>

// Setup access-point recovery policy - the PURE, host-tested decision for keeping the
// first-run setup network alive. NO Arduino, NO WiFi.h: decideSetupAp() takes a
// snapshot of the radio + onboarding state and returns ONE action for the device to
// run this tick.
//
// Why this exists (CUM-190). During first-time setup the softAP is the owner's only
// lifeline, yet several paths can take it down while onboarding is unfinished: a
// failed softAP() (reports 0.0.0.0), the white-screen beacon-drop after the STA
// joins, and - the nastiest - a wrong Wi-Fi password, which makes the core retry the
// association forever and starve the AP's beacons on the single shared 2.4 GHz radio
// (softAP() still reports success while the network cannot be seen). The device then
// showed "setup hotspot is down, restart the device", turning a recoverable state
// into a physical-restart wall on the flagship first-run flow. The firmware must
// recover every one of these without a reboot; this function decides how.

namespace nimbus {
namespace wifi {

// The one action to take with the setup AP this tick.
enum class SetupApAct : uint8_t {
  None = 0,   // leave the radio as it is
  RestoreAp,  // nothing reachable (STA down AND AP address gone) -> bring the AP back
  ProtectAp,  // a join is starving the AP during unfinished onboarding -> stop the STA
  DropAp,     // STA reachable over the LAN; shed the AP beacon (TFT white-screen risk)
};
const char* setupApActName(SetupApAct a);   // "none"|"restore"|"protect"|"drop"

struct SetupApInputs {
  bool     orchestrator = false;  // the radio only runs in Orchestrator mode (Notifier is BLE-only)
  bool     tftBoard     = false;  // the AP-beacon white-screen risk applies to the jumper-wired TFT only
  bool     staConnected = false;  // WL_CONNECTED && localIP() != 0
  bool     apAddressed  = false;  // softAPIP() != 0.0.0.0 (a failed AP reports 0.0.0.0)
  bool     onboarded    = true;   // the setup wizard has finished (store::onboarded())
  bool     handoffGrace = false;  // just-joined grace window before a white-screen drop
  uint32_t msSinceJoin  = 0;      // since the last credential test began; 0 = none pending
  uint32_t joinGraceMs  = 30000;  // allow a join this long before protecting the AP from it
  // The multi-network failover supervisor is actively cycling scan -> next saved network
  // (WifiPolicy in Scanning/Joining). "Trying the NEXT saved network" is PROGRESS, not
  // churn (CUM-207): while it is true the supervisor owns the radio, so this function
  // stands down and returns None. The setup/recovery AP only comes back once failover is
  // EXHAUSTED (the supervisor reaches Unreachable and clears this), never mid-cycle - a
  // bounded window, so a TFT board is never stranded with the AP down.
  bool     failoverActive = false;
};

// Decide the single setup-AP action for this tick. See the header note for the why.
SetupApAct decideSetupAp(const SetupApInputs& in);

}  // namespace wifi
}  // namespace nimbus
