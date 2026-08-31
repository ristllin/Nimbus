#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/wifi/known_networks.h"

// Wi-Fi link policy - the PURE, host-tested state machine that decides when the
// device scans, joins, or stops trying. NO Arduino, NO WiFi.h: `tick()` takes a
// snapshot of the world and returns ONE action for the device to execute.
//
// Why this exists. The entire reconnect policy used to be `WiFi.setAutoReconnect(true)`
// - no attempt cap, no backoff, no give-up. With a stored network that is out of
// range the ESP32 scans every channel forever. The S3 has ONE 2.4 GHz radio shared
// between the station and the access point, and the AP must sit on the station's
// channel, so that endless scan starves the setup AP's beacons: `softAP()` reports
// success while the network cannot be seen or joined. The device traps itself -
// the bad credentials block the only route to fixing the credentials.
//
// The fix is a real FAILURE STATE. `Unreachable` does not mean "give up"; it means
// "stop competing with the AP for the radio". It keeps retrying on a slow schedule
// (30s -> 5min ceiling, about 1% radio duty instead of ~100%), so a router that
// comes back is still picked up, while the AP always has airtime to beacon.
//
// See docs/wifi-resilience.md.

namespace nimbus {
namespace wifi {

enum class LinkState : uint8_t { Idle = 0, Scanning, Joining, Online, Unreachable };
const char* linkStateName(LinkState s);   // "idle"|"scanning"|"joining"|"online"|"unreachable"

// How a disconnect reason should be TREATED. Retrying a wrong password or an absent
// SSID is exactly the airtime waste this feature removes, so only Transient retries.
enum class JoinFail : uint8_t { None = 0, NotFound, AuthReject, Transient, SelfInitiated };
JoinFail    classifyReason(int idfReason);
const char* joinFailName(JoinFail f);

struct PolicyCfg {
  uint32_t tickMs             = 250;      // caller self-throttle (not used internally)
  uint32_t joinTimeoutMs      = 12000;    // scan ~3s + join 12s < the 25s HIL join budget
  uint32_t scanTimeoutMs      = 12000;    // wedge guard; the driver's own timeout is 60s
  uint32_t interAttemptMs     = 1500;     // beacon breathing room between assoc attempts
  uint32_t onlineGraceMs      = 5000;     // absorb one core-initiated reconnect
  uint32_t unreachableFirstMs = 30000;    // first slow retry
  uint32_t unreachableMaxMs   = 300000;   // ceiling: one ~3s scan / 300s ~= 1% duty
  uint32_t apHoldMs           = 900000;   // "publish setup network" quiet window (15 min)
  uint8_t  attemptsPerCandidate = 2;      // Transient only
  bool     suppressScanWithApClients = true;   // someone is configuring - do not scan
};

// A snapshot of the world, gathered by the device each tick.
struct Inputs {
  uint32_t nowMs        = 0;
  bool     staLinked    = false;   // WL_CONNECTED && localIP() != 0
  bool     apUp         = true;
  uint8_t  apStations   = 0;
  bool     scanBusy     = false;
  int      knownCount   = 0;
  bool     gotIp        = false;   // one-shot event flags, cleared by the device
  bool     disconnected = false;
  int      lastReason   = 0;
  bool     reqPublishAp = false;   // the on-device escape hatch
  bool     reqCancelHold = false;
  bool     reqScan      = false;   // explicit "scan now" (menu/web)
  int      reqJoinIndex = -1;      // direct join, index into the known list
};

enum class Act : uint8_t { None = 0, StartScan, Join, DropSta, EnterUnreachable, ReassertAp };

struct Action {
  Act         kind        = Act::None;
  int         knownIndex  = -1;
  std::string ssid, pass;                  // Join only - the device never re-indexes
  LinkState   state       = LinkState::Idle;   // state AFTER this action
  bool        stateChanged = false;            // log/repaint/sfx on the edge only
};

class WifiPolicy {
 public:
  void configure(const PolicyCfg& c) { cfg_ = c; }
  const PolicyCfg& cfg() const { return cfg_; }

  void setKnown(const std::vector<KnownNet>& nets);
  void noteScanResults(const std::vector<ScanHit>& hits, uint32_t nowMs);
  void noteScanFailed(uint32_t nowMs);

  Action tick(const Inputs& in);            // THE pure transition

  LinkState   state() const { return state_; }
  JoinFail    lastFail() const { return lastFail_; }
  int         lastReason() const { return lastReason_; }
  std::string joiningSsid() const { return joiningSsid_; }
  std::string onlineSsid() const { return onlineSsid_; }
  uint32_t    nextRetrySec(uint32_t nowMs) const;
  uint32_t    apHoldSecLeft(uint32_t nowMs) const;
  int         candidateCount() const { return (int)cands_.size(); }
  // 0-based cursor into the candidate list for THIS failover cycle - which saved
  // network is being tried right now. The device adds 1 to render "trying <ssid> 2/3"
  // (CUM-207). Only meaningful while Scanning/Joining; 0 otherwise.
  int         candidateIndex() const { return (int)candIdx_; }
  void        reset();

 private:
  Action enterUnreachable(uint32_t now, LinkState from);
  Action startScan(uint32_t now);
  Action joinCandidate(uint32_t now, size_t idx);
  bool   advanceCandidate(uint32_t now, Action& out);

  PolicyCfg              cfg_;
  LinkState              state_ = LinkState::Idle;
  std::vector<KnownNet>  known_;
  std::vector<ScanHit>   hits_;
  std::vector<Candidate> cands_;
  size_t                 candIdx_ = 0;
  uint8_t                attempts_ = 0;

  bool     scanPending_ = false;      // we asked; results not yet in
  uint32_t scanStartMs_ = 0;
  uint32_t joinStartMs_ = 0;
  uint32_t nextRetryAt_ = 0;
  uint32_t backoffMs_   = 0;
  uint32_t apHoldUntil_ = 0;          // 0 = no hold
  bool     apHoldArmed_ = false;
  uint32_t dropAtMs_    = 0;          // Online -> link lost, grace start
  bool     dropPending_ = false;

  int         lastReason_ = 0;
  JoinFail    lastFail_ = JoinFail::None;
  std::string joiningSsid_, onlineSsid_;
};

}  // namespace wifi
}  // namespace nimbus
