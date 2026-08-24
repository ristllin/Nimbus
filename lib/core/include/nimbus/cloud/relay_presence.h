#pragma once
// relay_presence - the device-side presence lifecycle for one relay session,
// as a small pure (no Arduino) state machine so the rule can be host-tested.
//
// The load-bearing invariant (CUM-182): the device is "online" ONLY after the
// relay acknowledges registration with a hello-ack (the Welcome frame) that is
// bound to THIS device's id. A successful TLS connect or WS upgrade, on its own,
// must NEVER make the device report online - otherwise the device can dial a
// stale/wrong endpoint (a Cloudflare-fronted host that completes a TLS+WS
// handshake) and latch a false "connected" while the real relay never registers
// it. Presence follows registration, not the socket.
//
// Mirrors the wire contract in relay_codec.h / cumulo-nimbus protocol.ts: the
// Welcome frame now echoes `deviceId`. A relay that predates that field echoes
// nothing; we accept that (legacy) but reject a Welcome that echoes a DIFFERENT
// id (wrong endpoint / spoof).
#include <cstdint>

namespace nimbus {
namespace cloud {

// The stages of one dial attempt. Presence is Online only in the Online stage,
// and the machine can only reach Online through onHelloAck().
enum class ConnStage : uint8_t {
  Idle,          // no socket
  TlsConnected,  // TLS handshake done (NOT online)
  Upgraded,      // WS 101 upgrade done (NOT online)
  HelloSent,     // our hello frame written (NOT online)
  Online,        // relay hello-ack accepted, bound to our id
  Closed,        // session ended
};

// The verdict of evaluating a received hello-ack against our identity.
enum class AckResult : uint8_t {
  Accept,          // welcome id matches ours, or the relay echoed no id (legacy)
  RejectMismatch,  // welcome echoed a DIFFERENT device id -> refuse presence
};

// Pure decision: does this hello-ack authorize presence for our device?
//   welcomeId : the id the relay echoed in Welcome (may be nullptr/"" on a
//               legacy relay that does not echo it).
//   ourId     : this device's cloud id (never empty in a real session).
// Accept when the relay echoed no id (backward compatible) or it matches ours;
// RejectMismatch when the relay echoed a non-empty id that differs from ours.
AckResult evaluateHelloAck(const char* welcomeId, const char* ourId);

// The presence state machine for one session. Not thread-safe by itself; the
// firmware owns it from the single relay task.
class RelayPresence {
 public:
  void reset() { stage_ = ConnStage::Idle; }
  void onTlsConnected() { stage_ = ConnStage::TlsConnected; }
  void onUpgraded() { stage_ = ConnStage::Upgraded; }
  void onHelloSent() { stage_ = ConnStage::HelloSent; }

  // Apply a received hello-ack. Transitions to Online ONLY when the hello was
  // already sent AND the ack is accepted for our id. Returns true iff now online.
  bool onHelloAck(const char* welcomeId, const char* ourId) {
    if (stage_ != ConnStage::HelloSent) return false;  // out-of-order ack: ignore
    if (evaluateHelloAck(welcomeId, ourId) != AckResult::Accept) return false;
    stage_ = ConnStage::Online;
    return true;
  }

  void onClosed() { stage_ = ConnStage::Closed; }

  bool online() const { return stage_ == ConnStage::Online; }
  ConnStage stage() const { return stage_; }

 private:
  ConnStage stage_ = ConnStage::Idle;
};

}  // namespace cloud
}  // namespace nimbus
