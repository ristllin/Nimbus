// wifi_net_up - see nimbus/wifi/net_up.h. Pure, host-tested (test/test_net_up).
#include "nimbus/wifi/net_up.h"

#include <algorithm>

namespace nimbus {
namespace wifi {

NetEdges NetUpLatch::tick(bool staLinked, bool sawDisc) {
  NetEdges e;
  // A session that was live at the last tick has ended if the station is down now, or if
  // a disconnect landed in between (it may already have rejoined: then down AND up).
  e.down = up_ && (!staLinked || sawDisc);
  // A session begins whenever the station is up now and none is live (or it just ended).
  // No GOT_IP input on purpose: an up station is up, whoever saw the event.
  e.up = staLinked && (!up_ || e.down);
  up_ = staLinked;
  return e;
}

SntpKick::Why SntpKick::kick(uint32_t nowMs, Why why) {
  kicked_     = true;
  lastKickMs_ = nowMs;
  return why;
}

SntpKick::Why SntpKick::tick(uint32_t nowMs, bool netUp, bool upEdge, bool synced) {
  if (upEdge) owedUp_ = true;
  // No link, no kick. Owed kicks are dropped too: the next up edge re-arms one.
  if (!netUp) {
    owedUp_ = owedReq_ = false;
    return Why::None;
  }
  const uint32_t sinceKick = nowMs - lastKickMs_;   // wraps cleanly on uint32_t
  if (owedUp_ || owedReq_) {
    if (kicked_ && sinceKick < kMinGapMs) return Why::None;   // owed, honored later
    const Why why = owedUp_ ? Why::LinkUp : Why::Requested;
    owedUp_ = owedReq_ = false;
    retryMs_ = kFirstRetryMs;   // a fresh start restarts the backstop schedule
    return kick(nowMs, why);
  }
  if (synced) return Why::None;   // lwIP's own poll keeps a synced clock fresh
  // Up and unsynced with no kick at all (no edge was ever fed): kick at once. This is
  // the backstop for the CUM-504 class, so a missed edge can never mean "never".
  if (!kicked_) return kick(nowMs, Why::LinkUp);
  if (sinceKick < retryMs_) return Why::None;
  retryMs_ = std::min<uint32_t>(retryMs_ * 2, kMaxRetryMs);
  return kick(nowMs, Why::Retry);
}

const char* sntpWhyName(SntpKick::Why w) {
  switch (w) {
    case SntpKick::Why::LinkUp: return "link up";
    case SntpKick::Why::Retry:  return "retry";
    case SntpKick::Why::Requested: return "sync now";
    case SntpKick::Why::None:   break;
  }
  return "none";
}

}  // namespace wifi
}  // namespace nimbus
