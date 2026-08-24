#pragma once
#include <cstdint>

#include "nimbus/attention.h"
#include "nimbus/nsn_proto.h"
#include "nimbus/profile.h"  // Posture (for ambientHoldFor)

// notifier_map - the portable heart of Notifier mode. Translates decoded nsn
// wire frames (nsn_proto.h) into attention-Router events so that Notifier jobs
// and Orchestrator sub-sessions share ONE ring/panel code path (plan §3.6).
//
// Model: each nsn frame is a COMPLETE snapshot of desired ring state, and the
// broker packs segments densely in ring order, so the segment INDEX is the
// stable job key. Applying a frame upserts JobState for every present segment
// and Offlines the tail that shrank away since the previous frame. The wire hue
// is the authoritative provider accent (255 = white / unknown provider, which
// the Router now represents faithfully via Event::hasAccent).
//
// Not preserved: the wire's per-segment `anim`/`span` overrides - Nimbus derives
// animation from status (solide::ring::styleFor), which matches the broker's
// STATE_STYLE defaults in the common case. Documented in docs/plan.md.
//
// Pure and host-tested. The device serial loop (src/modes/notifier_mode) owns
// the nsn::Decoder + transport and feeds decoded frames in here.

namespace nimbus::notifier {

// Posture-scaled AMBIENT hold: how long a Running/Done ring lingers after the
// broker goes quiet, per ring level. Full = a desk display that stays lit (5 min),
// Calm = a glance (30 s), Dark = battery-frugal (5 s). Calls-to-action
// (WaitingInput/AwaitingApproval/Error) always hold far longer regardless of
// posture - see Mapper::timeout()'s attnMs. NotifierMode::tick() feeds the result
// as the ambientMs argument each loop, so a posture change takes effect live.
uint32_t ambientHoldFor(Posture p);

struct FrameResult {
  int  eventsRouted = 0;
  bool ringDirty = false;      // OR of every routed Decision's ringDirty
  bool anyAttention = false;   // any segment is an attention status this frame
  // One aggregate screen intent for the whole frame: immediate Badge when any
  // segment needs attention, else a coalesced StatusIdle refresh.
  attn::ScreenIntent screen;
};

class Mapper {
 public:
  // Route a decoded frame into `router` at nowMs. Stores brightness and the
  // frame time (for timeout()).
  FrameResult apply(const nsn::Frame& f, attn::Router& router, uint32_t nowMs);

  // Call periodically. When the link goes quiet (no frame), expire stale
  // segments so the ring doesn't linger - but on TWO clocks: AMBIENT segments
  // (Running/Done/Idle) clear after `ambientMs` (a dead session shouldn't leave a
  // stale ring); ATTENTION segments (WaitingInput/AwaitingApproval/Error) are
  // CALLS-TO-ACTION and hold until `attnMs` (>> ambientMs) so a job that needs you
  // doesn't vanish the moment the broker stops re-sending. Returns a result with
  // ringDirty when it actually expired something, else an empty (no-op) result.
  FrameResult timeout(attn::Router& router, uint32_t nowMs, uint32_t ambientMs,
                      uint32_t attnMs);

  uint8_t brightness() const { return brightness_; }
  bool    seenFrame() const { return seen_; }
  uint8_t liveCount() const { return liveCount_; }

  // v2 per-segment session metadata (protocol v2), keyed by segment index == job
  // key. harness = nsn::kHarness* (0 = unknown / v1 broker); title = short name
  // (cwd basename / task, "" if none). The device UI (buildCtx -> JobInfo) reads
  // these to NAME a session on the panel instead of a bare "job N".
  uint8_t     harnessOf(uint32_t key) const { return key < nsn::kMaxSegs ? harness_[key] : 0; }
  const char* titleOf(uint32_t key) const { return key < nsn::kMaxSegs ? title_[key] : ""; }

  void reset();

 private:
  uint8_t  liveCount_ = 0;    // segments live from the last applied frame
  uint8_t  segStatus_[nsn::kMaxSegs] = {};  // per-segment wire status (for timeout())
  uint8_t  harness_[nsn::kMaxSegs] = {};    // v2 per-segment harness tag
  char     title_[nsn::kMaxSegs][nsn::kMaxTitle + 1] = {};  // v2 per-segment title
  uint8_t  brightness_ = 0;
  uint32_t lastFrameMs_ = 0;
  bool     seen_ = false;
};

}  // namespace nimbus::notifier
