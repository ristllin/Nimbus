#pragma once
#include <cstdint>
#include <string>

#include "nimbus/wifi/policy.h"

// wifi_link - the device seam that finally wires the portable failover state machine
// (nimbus::wifi::WifiPolicy) to the live radio. This is "step 8" the store + policy were
// built for (see known_networks.h / wifi_portal.cpp): without it the device relies on the
// Arduino core's WiFi.setAutoReconnect(true), which retries ONE SSID forever and never
// fails over to another saved network - so a device carried to a second location hunts a
// network that no longer exists (CUM-207).
//
// DESIGN - additive, engage-on-failure (deliberately NOT a full ownership swap).
//   The supervisor stays PASSIVE and lets the existing, bench-verified path (core
//   auto-reconnect + decideSetupAp, CUM-190) run untouched, EXCEPT in one narrow window:
//   Orchestrator mode, onboarding finished, at least TWO saved networks, the link has
//   been down past a short grace, no manual join in flight, and nobody on the setup AP.
//   Only then does it ENGAGE - stop the core hammering the dead SSID, and drive
//   WifiPolicy to scan and try the next saved network by signal. The moment a network
//   comes online it DISENGAGES and hands steady-state reconnection back to the core.
//   So single-network and first-run flows are byte-identical to before this change.
//
// NO new task, NO concurrency (frozen invariant): tick() runs inline in loop(). The one
// WiFi.onEvent latch it registers runs on the existing Arduino WiFi event task, exactly
// like the SFX link-sound handler already does.

namespace nimbus::net::link {

// A flattened read of the supervisor for the screen copy layer + the setup-AP reconciler.
struct Status {
  nimbus::wifi::LinkState state = nimbus::wifi::LinkState::Idle;
  bool        engaged = false;         // the supervisor is actively driving the radio
  bool        failoverActive = false;  // engaged AND scanning/joining across saved nets
  int         candIndex = 0;           // 1-based position being tried now (0 = n/a)
  int         candCount = 0;           // reachable saved networks this cycle
  std::string joiningSsid;             // the network being joined, when Joining
};

// Register the WiFi event latch + configure the policy. Call once, after net::begin().
void begin();

// Re-seed the policy from the known-networks store. Call after any list change (add /
// forget / reorder / connect) so a failover cycle uses the current credentials + order.
void refreshKnown();

// Drive one tick from loop(). `orchMode` and `onboarded` are the live gates; the rest is
// read from the radio + store internally. Cheap and safe to call every iteration.
void tick(uint32_t nowMs, bool orchMode, bool onboarded);

// True while the supervisor is cycling scan -> next saved network. Fed to decideSetupAp
// so "trying the NEXT network" reads as progress, not churn - the setup AP only returns
// once this clears (failover exhausted). See setup_ap.h.
bool failoverActive();

// Snapshot for liveLinkView() so the on-screen status can read "Joining <ssid> 2/3...".
Status status();

// The owner explicitly published the setup AP (or resumed): while held, the supervisor
// stays out of the way so it can never scan the setup network off the air. wifi_portal's
// publishSetupNetwork()/cancelSetupHold() call these.
void setManualHold(bool held);

}  // namespace nimbus::net::link
