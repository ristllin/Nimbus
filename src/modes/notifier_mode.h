#pragma once
#include <Arduino.h>

#include "nimbus/attention.h"
#include "nimbus/notifier_map.h"
#include "nimbus/nsn_proto.h"

// NotifierMode - the device loop for Notifier mode. Owns the incremental nsn
// decoder and the portable frame→event Mapper; applies complete frames to the
// attention Router and expires the ring if the link goes quiet. All UX logic
// (posture, ring plan, render scheduling) lives downstream in the portable
// core - this class is only the transport edge.
//
// BLE-only (decided 2026-07; USB serial is power + flashing only, never a
// status transport - see AGENTS.md). A future WiFi/TCP transport would follow
// feedBle()'s pattern: its own decoder, same shared Mapper/Router/job table.

namespace nimbus {

class NotifierMode {
 public:
  // linkTimeoutMs: how long an AMBIENT ring (running/done) lingers after the
  // broker goes quiet. attnHoldMs: how long a CALL-TO-ACTION (needs-input /
  // needs-approval / error) holds before it too expires - much longer, because a
  // job waiting on you shouldn't vanish just because the broker stopped talking.
  NotifierMode(attn::Router& router, uint32_t linkTimeoutMs = 5000,
               uint32_t attnHoldMs = 300000 /* 5 min */)
      : router_(router), timeoutMs_(linkTimeoutMs), attnHoldMs_(attnHoldMs) {}

  // Run just the link-timeout check (no I/O) - call once per main loop
  // iteration so a quiet BLE link still clears a stale ring (a central that
  // disconnects without an explicit Offline frame shouldn't leave the last
  // job frozen on-screen forever). Returns true if it cleared the ring.
  //
  // ambientMs is the AMBIENT hold (running/done) for the CURRENT ring level -
  // callers pass notifier::ambientHoldFor(posture) so Full lingers (desk display)
  // and Dark clears fast. attnMs is the CALL-TO-ACTION hold (input/approval/error),
  // posture-independent but user-tunable (Param::AttnHoldMs) - callers pass the
  // effective value. The shorter overloads fall back to the constructor defaults.
  bool tick(uint32_t nowMs) { return tick(nowMs, timeoutMs_, attnHoldMs_); }
  bool tick(uint32_t nowMs, uint32_t ambientMs) { return tick(nowMs, ambientMs, attnHoldMs_); }
  bool tick(uint32_t nowMs, uint32_t ambientMs, uint32_t attnMs);

  // feedBle(): PRODUCTION BLE-transport entry point - net::ble::drain() calls
  // it per queued byte on the main task. Mirrors poll()'s inner loop but owns
  // a DEDICATED decoder: the serial and BLE byte streams interleave, and one
  // shared decoder would tear frames across transports. Both paths land in the
  // same mapper_/router_/last_, so the link-timeout in poll() naturally means
  // "quiet on ALL transports" and poll() stays untouched. Returns true when
  // Router state changed (caller drives the ring / panel, same as poll()).
  bool feedBle(uint8_t byte, uint32_t nowMs) {
    nsn::Frame frame;
    if (bleDecoder_.feed(byte, frame)) {
      ++bleFrames_;
      bleLastSeq_ = frame.seq;
      last_ = mapper_.apply(frame, router_, nowMs);
      return last_.ringDirty || last_.screen.render;
    }
    return false;
  }
  // Count of complete frames applied via feedBle() + the newest wire seq -
  // read by net::ble::drain() to emit the [0x02, seq] STATUS echo from the
  // main task (a frame can apply without changing Router state, so the bool
  // return above can't signal "frame done").
  uint32_t bleFrames() const { return bleFrames_; }
  uint8_t  bleLastSeq() const { return bleLastSeq_; }

  const notifier::FrameResult& last() const { return last_; }
  const notifier::Mapper& mapper() const { return mapper_; }

 private:
  attn::Router& router_;
  nsn::Decoder bleDecoder_;   // BLE stream (feedBle()) - the only frame input
  notifier::Mapper mapper_;
  notifier::FrameResult last_;
  uint32_t timeoutMs_;
  uint32_t attnHoldMs_;
  uint32_t bleFrames_ = 0;    // frames applied via feedBle()
  uint8_t  bleLastSeq_ = 0;   // wire seq of the newest of those
};

}  // namespace nimbus
