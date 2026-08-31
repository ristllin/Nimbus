// wifi_link - see wifi_link.h. The device seam for the multi-network failover machine.
#include "wifi_link.h"

#include <WiFi.h>
#include <time.h>

#include <vector>

#include "wifi_portal.h"   // msSinceJoinAttempt() - is a manual credential test in flight
#include "wifi_store.h"    // the known-networks list (count / snapshot / note a join)

namespace nimbus::net::link {

namespace {

using nimbus::wifi::Act;
using nimbus::wifi::Action;
using nimbus::wifi::Inputs;
using nimbus::wifi::KnownNet;
using nimbus::wifi::LinkState;
using nimbus::wifi::ScanHit;
using nimbus::wifi::WifiPolicy;

WifiPolicy s_policy;

bool     s_engaged      = false;
bool     s_manualHold   = false;
// Set from any task (the web server re-seeds after a list edit); consumed on the main
// loop so s_policy is only ever mutated there. volatile is enough: one flag, set-only
// off-task, cleared on-task, and a missed edit is re-picked the next tick.
volatile bool s_knownDirty = false;
uint32_t s_downSinceMs  = 0;      // first tick the link was seen down; 0 = up / unknown
bool     s_scanInFlight = false;
uint32_t s_scanStartMs  = 0;

// Event latch. GOT_IP / DISCONNECTED fire on the Arduino WiFi event task; the tick on the
// main task reads and clears them. Plain bools are fine here (same one-writer/one-reader
// pattern as the SFX link-sound handler), and a missed edge only delays a decision one
// tick - the tick also reads the live radio state, so nothing is stranded on a lost event.
volatile bool s_evGotIp    = false;
volatile bool s_evDisc     = false;
volatile int  s_evReason   = 0;
bool          s_eventsBound = false;

// Take over the radio from the core (which retries the dead SSID forever): stop it, seed
// the policy from the CURRENT list, and start a clean failover cycle.
void engage(uint32_t now) {
  refreshKnown();
  s_policy.reset();
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
  s_scanInFlight = false;
  s_engaged = true;
}

// Hand steady-state reconnection back to the core. Never disconnects - if we are
// disengaging because a network came online, that connection must survive.
void disengage() {
  if (!s_engaged) return;
  if (s_scanInFlight) { WiFi.scanDelete(); s_scanInFlight = false; }
  WiFi.setAutoReconnect(true);
  s_engaged = false;
}

// Start an async scan for the policy. Never competes with an already-running scan (the
// web UI owns one too); a busy radio is reported to the policy as a failed scan, which it
// absorbs into its slow-retry schedule rather than starving the AP.
void startScan(uint32_t now) {
  if (s_scanInFlight || WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
  const int r = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
  if (r == WIFI_SCAN_FAILED) { s_policy.noteScanFailed(now); return; }
  s_scanInFlight = true;
  s_scanStartMs  = now;
}

// Deliver a finished scan to the policy as ranked ScanHits, or a failure on timeout.
void pollScan(uint32_t now) {
  if (!s_scanInFlight) return;
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if ((uint32_t)(now - s_scanStartMs) > 15000u) {   // wedge guard
      WiFi.scanDelete();
      s_scanInFlight = false;
      s_policy.noteScanFailed(now);
    }
    return;
  }
  if (n == WIFI_SCAN_FAILED) {
    s_scanInFlight = false;
    s_policy.noteScanFailed(now);
    return;
  }
  std::vector<ScanHit> hits;
  hits.reserve((size_t)n);
  for (int i = 0; i < n; i++) {
    ScanHit h;
    h.ssid   = std::string(WiFi.SSID(i).c_str());
    h.rssi   = (int8_t)WiFi.RSSI(i);
    h.locked = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    hits.push_back(h);
  }
  WiFi.scanDelete();
  s_scanInFlight = false;
  s_policy.noteScanResults(hits, now);
}

void execute(const Action& a, uint32_t now);   // defined below; driveEngaged uses it

// We drove this join to success: record it so the list promotes the network that
// actually worked, then hand steady-state reconnection back to the core.
void recordJoinAndStepAside() {
  const std::string joined = s_policy.joiningSsid().empty() ? s_policy.onlineSsid()
                                                            : s_policy.joiningSsid();
  if (!joined.empty()) {
    const time_t t = time(nullptr);
    const uint32_t day = t > 0 ? (uint32_t)(t / 86400) : 0u;
    nimbus::net::wifistore::noteJoined(String(joined.c_str()), day);
  }
  disengage();
}

// True when the "carried to a new location / link dropped, nobody is touching it" case
// holds and the supervisor may take over from the core. A manual join or setup-AP client
// always wins, and a lone saved network has nothing to fail over TO.
bool eligible(bool onboarded, int known) {
  const uint32_t mj = nimbus::net::msSinceJoinAttempt();
  const bool manualPending = mj != 0 && mj < 20000u;   // a bounded window for a manual try
  return onboarded && known >= 2 && !s_manualHold && !manualPending &&
         WiFi.softAPgetStationNum() == 0;
}

// One driven tick while engaged: deliver any scan, snapshot the world, run the policy.
void driveEngaged(uint32_t now, int known, bool gotIp, bool disc, int discReas) {
  pollScan(now);
  Inputs in;
  in.nowMs        = now;
  in.staLinked    = false;
  in.apUp         = (uint32_t)WiFi.softAPIP() != 0u;
  in.apStations   = WiFi.softAPgetStationNum();
  in.scanBusy     = s_scanInFlight;
  in.knownCount   = known;
  in.gotIp        = gotIp;
  in.disconnected = disc;
  in.lastReason   = discReas;
  execute(s_policy.tick(in), now);
}

void execute(const Action& a, uint32_t now) {
  switch (a.kind) {
    case Act::StartScan:
      startScan(now);
      break;
    case Act::Join:
      // Own the retries: drop any half-open association, then try this candidate. The
      // policy already picked the strongest reachable saved network and carries its
      // stored password, so the device never re-indexes.
      WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
      WiFi.begin(a.ssid.c_str(), a.pass.c_str());
      break;
    case Act::DropSta:
      WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
      break;
    case Act::EnterUnreachable:
    case Act::ReassertAp:
      // The setup/recovery AP has ONE authority: decideSetupAp() in loop() (CUM-190). It
      // sees failoverActive() go false the moment the policy reaches Unreachable and
      // brings the AP back then. Acting here too would fight it.
      break;
    case Act::None:
      break;
  }
}

}  // namespace

void markKnownDirty() { s_knownDirty = true; }

void refreshKnown() {
  std::vector<KnownNet> nets;
  nimbus::net::wifistore::all(nets);
  s_policy.setKnown(nets);
}

void begin() {
  if (!s_eventsBound) {
    WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
      if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        s_evGotIp = true;
      } else if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        s_evDisc   = true;
        s_evReason = (int)info.wifi_sta_disconnected.reason;
      }
    });
    s_eventsBound = true;
  }
  refreshKnown();
}

void setManualHold(bool held) {
  s_manualHold = held;
  if (held) disengage();   // the owner wants the setup AP; get off the radio at once
}

bool failoverActive() {
  return s_engaged && (s_policy.state() == LinkState::Scanning ||
                       s_policy.state() == LinkState::Joining);
}

Status status() {
  Status s;
  s.state          = s_policy.state();
  s.engaged        = s_engaged;
  s.failoverActive = failoverActive();
  s.candIndex      = s_policy.candidateIndex() + 1;   // 1-based for "2/3"
  s.candCount      = s_policy.candidateCount();
  s.joiningSsid    = s_policy.joiningSsid();
  return s;
}

void tick(uint32_t nowMs, bool orchMode, bool onboarded) {
  const uint32_t now = nowMs ? nowMs : 1;   // 0 is the "link up / unknown" sentinel below

  // Notifier keeps the radio off; nothing to supervise.
  if (!orchMode) { disengage(); s_downSinceMs = 0; return; }

  // Re-seed the policy on the MAIN task if the list changed off-task. Doing it here (not
  // in the web handler) keeps s_policy single-task, honoring the no-concurrency rule.
  if (s_knownDirty) { s_knownDirty = false; refreshKnown(); }

  // Drain the latched radio events for this tick.
  const bool gotIp    = s_evGotIp;    s_evGotIp = false;
  const bool disc     = s_evDisc;     s_evDisc  = false;
  const int  discReas = s_evReason;

  const bool staLinked = WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0u;
  if (staLinked) {
    // Connected: steady state belongs to the core. If we drove this join, record it.
    if (s_engaged) recordJoinAndStepAside();
    s_downSinceMs = 0;
    return;
  }

  // Link is down. Remember since when, so the core gets its usual fast reconnect window
  // before we take over.
  if (s_downSinceMs == 0) s_downSinceMs = now;

  const int known = nimbus::net::wifistore::count();
  if (!eligible(onboarded, known)) { disengage(); return; }

  if (!s_engaged) {
    if ((uint32_t)(now - s_downSinceMs) < 8000u) return;   // let the core try first
    engage(now);
  }
  driveEngaged(now, known, gotIp, disc, discReas);
}

}  // namespace nimbus::net::link
