// setup_ap - see nimbus/wifi/setup_ap.h. Pure, host-tested.
#include "nimbus/wifi/setup_ap.h"

namespace nimbus {
namespace wifi {

const char* setupApActName(SetupApAct a) {
  switch (a) {
    case SetupApAct::RestoreAp: return "restore";
    case SetupApAct::ProtectAp: return "protect";
    case SetupApAct::DropAp:    return "drop";
    case SetupApAct::None:      break;
  }
  return "none";
}

SetupApAct decideSetupAp(const SetupApInputs& in) {
  // Notifier mode keeps the radio off (BLE owns the internal SRAM), so there is no
  // softAP to reconcile - turning it back on would defeat the Wi-Fi-off design.
  if (!in.orchestrator) return SetupApAct::None;

  // Nothing is reachable: the station is down AND the AP never came up (or was torn
  // down). The setup/recovery network must come straight back so the owner is never
  // locked out. Board-independent - only the white-screen DROP below is TFT-specific.
  if (!in.staConnected && !in.apAddressed) return SetupApAct::RestoreAp;

  // The station is up and reachable over the LAN. On the jumper-wired TFT the AP's
  // continuous beacon TX can knock the panel white, so shed the AP once the join has
  // settled past the handoff grace. The web UI and LAN stay up on the STA.
  if (in.staConnected && in.apAddressed) {
    if (in.tftBoard && !in.handoffGrace) return SetupApAct::DropAp;
    return SetupApAct::None;
  }

  // Station down but the AP still reports an address. During UNFINISHED onboarding a
  // join that has run long past the grace is almost certainly a wrong password whose
  // endless retries are starving the AP's beacons (the AP looks up but cannot be
  // reached). Stop the station competing for the radio so the setup network is
  // reachable again - no reboot. Once onboarding is complete the slow-retry policy
  // owns this instead, so we do not fight it there.
  if (!in.staConnected && in.apAddressed && !in.onboarded &&
      in.msSinceJoin > 0 && in.msSinceJoin >= in.joinGraceMs)
    return SetupApAct::ProtectAp;

  return SetupApAct::None;
}

}  // namespace wifi
}  // namespace nimbus
