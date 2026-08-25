#pragma once
// relay_liveness - the PURE, host-tested steady-state liveness contract for the
// device's relay WebSocket (CUM-191). No Arduino, no TLS.
//
// Connect-time presence is hello-ack-gated and identity-bound (CUM-182) and stays
// exactly as is. This adds the STEADY-STATE guarantee the field was missing: the
// device sends application "ping" heartbeats and expects a matched "pong" ack back
// from the relay. A HALF-OPEN socket - the relay-side session is gone (a pod rolled,
// a silent drop) but the device's TCP never errored - keeps delivering nothing while
// the device stays latched "online" forever, so every tunneled request 504s at the
// hub timeout until a manual reboot. Requiring the pong ack closes that: after
// kMaxMissedHeartbeatAcks consecutive UNACKED pings the link is declared dead, and the
// caller drops presence (honest /api/state), tears the socket down, and redials with
// backoff. Any pong proves both directions live and resets the miss count.
//
// Why ack-gated and not "any inbound byte": the old loop reset its liveness deadline
// on any inbound traffic, so a half-open link that still dribbled bytes (or simply had
// not yet crossed the coarse silence window) stayed "online". Only a MATCHED pong
// proves the relay->device direction still works.
//
// Pure integer state + an injected monotonic clock, so test/test_relay_liveness can
// simulate the half-open case (pings sent, no pongs ever) and assert the device goes
// dead within N heartbeat periods with no socket, no TLS, no board.

#include <cstdint>

#include "nimbus/cloud/relay_codec.h"  // kDefaultHeartbeatMs

namespace nimbus {
namespace cloud {

// Consecutive unacked heartbeats that declare a half-open link dead. Two marks the
// device offline within ~2 heartbeat periods (the CUM-191 release-gate bound). One
// dropped app-level pong without a broken connection does not happen over TCP (frames
// are delivered in order or the connection breaks), so two is not falsely trippy.
constexpr uint8_t kMaxMissedHeartbeatAcks = 2;

// Steady-state heartbeat liveness tracker. All times are monotonic milliseconds
// (millis() on device, a synthetic clock in tests). The caller sends a ping when
// duePing() is true then calls notePingSent(); a matched pong calls notePong();
// dead() reports the half-open verdict. Trivially copyable POD - no allocation.
struct HeartbeatLiveness {
  uint32_t intervalMs = kDefaultHeartbeatMs;  // ping cadence (armed from the clamped hb)
  uint8_t maxMissedAcks = kMaxMissedHeartbeatAcks;
  uint32_t nextPingAt = 0;
  uint8_t outstanding = 0;  // pings sent since the last pong (0 == fully acked)

  // Arm at online start (call once the Welcome/hello-ack lands). The first ping falls
  // due one interval out. A zero argument falls back to a safe default rather than
  // scheduling every tick (interval) or never tripping (maxMissed).
  void reset(uint32_t now, uint32_t interval, uint8_t maxMissed = kMaxMissedHeartbeatAcks) {
    intervalMs = interval ? interval : kDefaultHeartbeatMs;
    maxMissedAcks = maxMissed ? maxMissed : kMaxMissedHeartbeatAcks;
    nextPingAt = now + intervalMs;
    outstanding = 0;
  }

  // True once it is time to send the next heartbeat ping. Wrap-safe (signed diff), so a
  // millis() rollover at ~49 days does not stall the heartbeat.
  bool duePing(uint32_t now) const { return (int32_t)(now - nextPingAt) >= 0; }

  // Record that a ping was just sent: schedule the next and count it outstanding.
  void notePingSent(uint32_t now) {
    if (outstanding < 0xFF) outstanding++;
    nextPingAt = now + intervalMs;
  }

  // A matched pong ack arrived - the link is proven live in both directions.
  void notePong() { outstanding = 0; }

  // The link is dead once maxMissedAcks pings stand unacked. Checked right after
  // notePingSent(), so with the default of 2 the device trips on the second
  // consecutive unacked heartbeat (~2 ping cadences after the link went half-open).
  bool dead() const { return outstanding >= maxMissedAcks; }
};

}  // namespace cloud
}  // namespace nimbus
