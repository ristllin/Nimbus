#include "nimbus/wifi/policy.h"

namespace nimbus {
namespace wifi {

namespace {

// Wrap-safe deadline test - the codebase idiom. millis() rolls over every ~49 days
// and a naive `now >= deadline` stalls the machine for weeks at the wrap.
inline bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

// Elapsed with the F23 guard: a stamp in the FUTURE (clock stepped, or a stale mark
// captured after a wrap) yields 0, never a 49-day underflow that would make every
// timeout fire instantly.
inline uint32_t elapsed(uint32_t now, uint32_t since) {
  const int32_t d = (int32_t)(now - since);
  return d < 0 ? 0u : (uint32_t)d;
}

}  // namespace

const char* linkStateName(LinkState s) {
  switch (s) {
    case LinkState::Idle:        return "idle";
    case LinkState::Scanning:    return "scanning";
    case LinkState::Joining:     return "joining";
    case LinkState::Online:      return "online";
    case LinkState::Unreachable: return "unreachable";
  }
  return "idle";
}

const char* joinFailName(JoinFail f) {
  switch (f) {
    case JoinFail::None:          return "none";
    case JoinFail::NotFound:      return "not-found";
    case JoinFail::AuthReject:    return "auth";
    case JoinFail::Transient:     return "transient";
    case JoinFail::SelfInitiated: return "self";
  }
  return "none";
}

JoinFail classifyReason(int r) {
  switch (r) {
    // The trap case: the stored network simply is not here. Retrying it is what
    // burned the radio and starved the AP - move to the next candidate at once.
    case 201: return JoinFail::NotFound;          // NO_AP_FOUND
    // Credentials are wrong. Retrying cannot fix that.
    case 2:                                        // AUTH_EXPIRE
    case 15:                                       // 4WAY_HANDSHAKE_TIMEOUT (wrong PSK)
    case 202:                                      // AUTH_FAIL
    case 204:                                      // HANDSHAKE_TIMEOUT
    case 205: return JoinFail::AuthReject;         // CONNECTION_FAIL
    // Our own disconnectAsync(). Never counted as a failure or the machine would
    // consume an attempt every time it deliberately drops the link.
    case 8:   return JoinFail::SelfInitiated;      // ASSOC_LEAVE
    default:  return JoinFail::Transient;          // 200/203/4/... - worth one retry
  }
}

void WifiPolicy::reset() {
  state_ = LinkState::Idle;
  hits_.clear();
  cands_.clear();
  candIdx_ = 0;
  attempts_ = 0;
  scanPending_ = false;
  scanStartMs_ = joinStartMs_ = nextRetryAt_ = 0;
  backoffMs_ = 0;
  apHoldUntil_ = 0;
  apHoldArmed_ = false;
  dropAtMs_ = 0;
  dropPending_ = false;
  lastReason_ = 0;
  lastFail_ = JoinFail::None;
  joiningSsid_.clear();
  onlineSsid_.clear();
}

void WifiPolicy::setKnown(const std::vector<KnownNet>& nets) {
  known_ = nets;
  // The candidate cursor indexes the OLD list; keeping it would join the wrong
  // network after an add/forget. Re-derive on the next scan.
  cands_.clear();
  candIdx_ = 0;
}

void WifiPolicy::noteScanResults(const std::vector<ScanHit>& hits, uint32_t nowMs) {
  hits_ = hits;
  scanPending_ = false;
  scanStartMs_ = nowMs;
  cands_ = rankCandidates(known_, hits_);
  candIdx_ = 0;
  attempts_ = 0;
}

void WifiPolicy::noteScanFailed(uint32_t nowMs) {
  hits_.clear();
  cands_.clear();
  candIdx_ = 0;
  scanPending_ = false;
  scanStartMs_ = nowMs;
}

uint32_t WifiPolicy::nextRetrySec(uint32_t nowMs) const {
  if (state_ != LinkState::Unreachable || nextRetryAt_ == 0) return 0;
  if (due(nowMs, nextRetryAt_)) return 0;
  return (uint32_t)((nextRetryAt_ - nowMs) / 1000u) + 1u;
}

uint32_t WifiPolicy::apHoldSecLeft(uint32_t nowMs) const {
  if (!apHoldArmed_) return 0;
  if (due(nowMs, apHoldUntil_)) return 0;
  return (uint32_t)((apHoldUntil_ - nowMs) / 1000u) + 1u;
}

Action WifiPolicy::enterUnreachable(uint32_t now, LinkState from) {
  // Backoff doubles per consecutive failure, capped. Re-entering from Online (a
  // network that was working just vanished) restarts at the short delay.
  if (backoffMs_ == 0 || from == LinkState::Online)
    backoffMs_ = cfg_.unreachableFirstMs;
  else
    backoffMs_ = backoffMs_ * 2 > cfg_.unreachableMaxMs ? cfg_.unreachableMaxMs
                                                        : backoffMs_ * 2;
  nextRetryAt_ = now + backoffMs_;
  state_ = LinkState::Unreachable;
  joiningSsid_.clear();
  Action a;
  a.kind = Act::EnterUnreachable;
  return a;
}

Action WifiPolicy::startScan(uint32_t now) {
  scanPending_ = true;
  scanStartMs_ = now;
  state_ = LinkState::Scanning;
  Action a;
  a.kind = Act::StartScan;
  return a;
}

Action WifiPolicy::joinCandidate(uint32_t now, size_t idx) {
  candIdx_ = idx;
  const int ki = cands_[idx].knownIndex;
  joinStartMs_ = now;
  state_ = LinkState::Joining;
  joiningSsid_ = known_[(size_t)ki].ssid;
  Action a;
  a.kind = Act::Join;
  a.knownIndex = ki;
  a.ssid = known_[(size_t)ki].ssid;
  a.pass = known_[(size_t)ki].pass;
  return a;
}

// Move to the next candidate, or fall to Unreachable when they are exhausted.
bool WifiPolicy::advanceCandidate(uint32_t now, Action& out) {
  if (candIdx_ + 1 < cands_.size()) {
    attempts_ = 0;
    out = joinCandidate(now, candIdx_ + 1);
    return true;
  }
  out = enterUnreachable(now, LinkState::Joining);
  return true;
}

Action WifiPolicy::tick(const Inputs& in) {
  const LinkState before = state_;
  const uint32_t now = in.nowMs;
  Action a;

  // --- AP hold bookkeeping -------------------------------------------------
  // While someone is associated to the setup AP they are mid-configuration; keep
  // the quiet window alive so a scheduled scan can never kick them off.
  if (apHoldArmed_ && in.apStations > 0) apHoldUntil_ = now + cfg_.apHoldMs;
  if (apHoldArmed_ && due(now, apHoldUntil_)) apHoldArmed_ = false;
  if (in.reqCancelHold) apHoldArmed_ = false;

  // --- explicit requests win over everything -------------------------------
  if (in.reqPublishAp) {
    // The escape hatch: drop the station so the radio stops hopping, and hold the
    // AP quiet. This is what makes the setup network reliably joinable.
    apHoldArmed_ = true;
    apHoldUntil_ = now + cfg_.apHoldMs;
    scanPending_ = false;
    attempts_ = 0;
    backoffMs_ = cfg_.unreachableFirstMs;
    nextRetryAt_ = now + cfg_.apHoldMs;   // no automatic scan before the hold ends
    state_ = LinkState::Unreachable;
    joiningSsid_.clear();
    a.kind = Act::DropSta;
    a.state = state_;
    a.stateChanged = (before != state_);
    return a;
  }

  if (in.reqJoinIndex >= 0 && in.reqJoinIndex < (int)known_.size()) {
    // The user named the network, so skip the scan entirely - it would add ~3s and
    // tell us nothing. Also clears the hold: an explicit join outranks the quiet window.
    apHoldArmed_ = false;
    attempts_ = 0;
    backoffMs_ = 0;
    scanPending_ = false;
    const size_t ki = (size_t)in.reqJoinIndex;
    cands_.clear();
    Candidate c;
    c.knownIndex = (int)ki;
    cands_.push_back(c);
    a = joinCandidate(now, 0);
    a.state = state_;
    a.stateChanged = (before != state_);
    return a;
  }

  // --- events --------------------------------------------------------------
  // Accept an UNSOLICITED got-IP as a valid transition to Online: the Arduino core
  // latches `first_connect` and forces one uncommanded reconnect that we did not ask
  // for, and refusing it would leave the machine out of step with the real radio.
  if (in.gotIp || (in.staLinked && state_ != LinkState::Online)) {
    state_ = LinkState::Online;
    onlineSsid_ = !joiningSsid_.empty() ? joiningSsid_ : onlineSsid_;
    joiningSsid_.clear();
    attempts_ = 0;
    backoffMs_ = 0;
    nextRetryAt_ = 0;
    dropPending_ = false;
    lastFail_ = JoinFail::None;
    a.state = state_;
    a.stateChanged = (before != state_);
    return a;
  }

  if (in.disconnected) {
    lastReason_ = in.lastReason;
    const JoinFail cls = classifyReason(in.lastReason);
    if (cls != JoinFail::SelfInitiated) lastFail_ = cls;

    if (state_ == LinkState::Joining && cls != JoinFail::SelfInitiated) {
      // candIdx_ < cands_.size() guards the "retry the same candidate" path: a list edit
      // (setKnown) mid-join clears cands_ while state stays Joining, so retrying would
      // index an empty/shrunk vector (OOB). advanceCandidate() falls safely to Unreachable
      // when the cursor is out of range, which is exactly what a vanished candidate wants.
      if (cls == JoinFail::Transient && attempts_ + 1 < cfg_.attemptsPerCandidate &&
          candIdx_ < cands_.size()) {
        attempts_++;
        a = joinCandidate(now, candIdx_);   // same candidate, one more go
      } else {
        advanceCandidate(now, a);           // wrong password, absent, or stale cursor: move on
      }
      a.state = state_;
      a.stateChanged = (before != state_);
      return a;
    }
    if (state_ == LinkState::Online) {
      dropPending_ = true;                  // start the grace window
      dropAtMs_ = now;
    }
    // Unreachable/Scanning: a disconnect here is noise (often the core's own
    // uncommanded retry). It must not consume an attempt.
  }

  // --- per-state work ------------------------------------------------------
  switch (state_) {
    case LinkState::Idle:
      // An unprovisioned device must NEVER scan - matching the historical "no
      // credentials, so WiFi.begin() is never called" behaviour instead of
      // regressing it into a permanent scan.
      if (in.knownCount == 0) { a = enterUnreachable(now, LinkState::Idle); break; }
      a = startScan(now);
      break;

    case LinkState::Scanning:
      if (!scanPending_) {
        if (!cands_.empty()) a = joinCandidate(now, 0);
        else                 a = enterUnreachable(now, LinkState::Scanning);
      } else if (elapsed(now, scanStartMs_) >= cfg_.scanTimeoutMs) {
        scanPending_ = false;
        a = enterUnreachable(now, LinkState::Scanning);
      }
      break;

    case LinkState::Joining:
      if (elapsed(now, joinStartMs_) >= cfg_.joinTimeoutMs) {
        // Same guard as the disconnect path: never retry a candidate cursor that a
        // mid-join list edit left dangling past the end of cands_ (F1).
        if (attempts_ + 1 < cfg_.attemptsPerCandidate && candIdx_ < cands_.size()) {
          attempts_++;
          a = joinCandidate(now, candIdx_);
        } else {
          advanceCandidate(now, a);
        }
      }
      break;

    case LinkState::Online:
      if (dropPending_ && !in.staLinked && elapsed(now, dropAtMs_) >= cfg_.onlineGraceMs) {
        // A router blip re-scans rather than giving up: this is what keeps a
        // flap-and-recover cycle working.
        dropPending_ = false;
        a = in.knownCount == 0 ? enterUnreachable(now, LinkState::Online) : startScan(now);
      } else if (in.staLinked) {
        dropPending_ = false;
      }
      break;

    case LinkState::Unreachable:
      if (in.knownCount == 0) break;                 // nothing to look for, ever
      if (apHoldArmed_) break;                       // someone is configuring
      if (cfg_.suppressScanWithApClients && in.apStations > 0) break;
      if (in.scanBusy) break;
      if (nextRetryAt_ != 0 && due(now, nextRetryAt_)) a = startScan(now);
      break;
  }

  // An explicit scan request is honoured from any non-joining state.
  if (a.kind == Act::None && in.reqScan && state_ != LinkState::Joining && !in.scanBusy) {
    a = startScan(now);
  }

  // The AP is the guaranteed fallback: if it is down, ask the device to re-assert it.
  if (a.kind == Act::None && !in.apUp) a.kind = Act::ReassertAp;

  a.state = state_;
  a.stateChanged = (before != state_);
  return a;
}

}  // namespace wifi
}  // namespace nimbus
