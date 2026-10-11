#pragma once
#include <cstdint>

// Network-up detection + the SNTP kick schedule (CUM-504). PURE, host-tested: NO
// Arduino, NO WiFi.h. The device seam is src/net/wifi_link.cpp (the latch, ticked from
// loop() on the main task) and src/agent/loops_subsystem.cpp (sntpTick).
//
// Why this exists. The only boot-time SNTP start used to be a WiFi.onEvent(GOT_IP)
// handler that main.cpp registered about 10 s into setup(), long after net::begin() had
// already started the station on its stored credentials. On a strong network the join
// and DHCP finished first, GOT_IP fired while nobody was listening (the Arduino core
// dispatches an event only to the callbacks registered at that moment, with no replay),
// and nothing ever started SNTP again. The clock stayed at 1970 for the whole uptime: no
// daily or weekly routine fired, the dream upkeep never ran, and every prompt said the
// date-time was UNKNOWN. The link sounds and the setup-AP drop hung off the same missed
// edge.
//
// The rule encoded here: an event is an edge you can miss, the link state is a level you
// can always read. Network-up is derived from the station's LIVE state, polled every
// tick, never from having observed GOT_IP. The only event input is a latched disconnect,
// which adds the one thing two polls cannot see: a drop and rejoin that both land between
// ticks, which must still count as a new session (SNTP is re-kicked on every reconnect).

namespace nimbus {
namespace wifi {

// What changed since the last tick. When both are set the session ended and a new one
// began between two ticks: deliver `down` first, then `up`.
struct NetEdges {
  bool down = false;   // an IP session ended
  bool up   = false;   // an IP session began
};

// The network-up latch. Starts DOWN, so a station that is already up on the very first
// tick (it joined before anything was listening: the fast stored-credential boot) reports
// an up edge right away instead of waiting for an event that already fired.
class NetUpLatch {
 public:
  // staLinked: radio truth read THIS tick (associated AND holding an IP). sawDisc: a
  // STA_DISCONNECTED event was latched since the last tick. The seam must drain the event
  // latch BEFORE reading the radio, so a drop that lands in between is seen next tick.
  NetEdges tick(bool staLinked, bool sawDisc);
  bool up() const { return up_; }

 private:
  bool up_ = false;
};

// When to (re)start SNTP. One kick on every up edge, synced or not (a reconnect re-syncs
// whatever drifted while offline). An owner request (Sync now) is a kick too. While the
// link is up and the clock is still unsynced, re-kick on a slow doubling backoff as a
// backstop: lwIP already retries and rotates servers on its own (15 s receive timeout),
// so the backstop starts late enough not to restart it mid-rotation. Never while the link
// is down. Never more often than kMinGapMs: a flapping link defers its kick (it is owed,
// not dropped) instead of restarting the client on every rejoin. This class is the ONLY
// thing that restarts the client (one producer, the main task).
class SntpKick {
 public:
  enum class Why : uint8_t {
    None = 0,    // nothing to do this tick
    LinkUp,      // the link came up (or was found up with no kick yet)
    Retry,       // still unsynced on a live link: backstop re-kick
    Requested,   // the owner asked (Sync now)
  };

  static constexpr uint32_t kMinGapMs     = 10000;     // floor between any two kicks
  static constexpr uint32_t kFirstRetryMs = 180000;    // first backstop after a kick (3 min)
  static constexpr uint32_t kMaxRetryMs   = 1800000;   // backoff ceiling (30 min)

  // The owner asked for a sync. Honored at the next tick with a live link (gap rules
  // apply); dropped if the link is down then (the next up edge kicks anyway).
  void request() { owedReq_ = true; }

  // nowMs: a free-running millisecond clock (wraps). netUp: the latch verdict this tick.
  // upEdge: the latch reported an up edge this tick. synced: the wall clock is real.
  Why tick(uint32_t nowMs, bool netUp, bool upEdge, bool synced);

 private:
  Why kick(uint32_t nowMs, Why why);

  bool     kicked_     = false;   // at least one kick has happened
  bool     owedUp_     = false;   // an up edge is waiting out kMinGapMs
  bool     owedReq_    = false;   // an owner request is waiting for a live link / the gap
  uint32_t lastKickMs_ = 0;
  uint32_t retryMs_    = kFirstRetryMs;
};

const char* sntpWhyName(SntpKick::Why w);   // "none" | "link up" | "retry" | "sync now"

}  // namespace wifi
}  // namespace nimbus
