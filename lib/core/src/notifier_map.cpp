#include "nimbus/notifier_map.h"

namespace nimbus::notifier {

using attn::Event;
using solide::ring::Status;

uint32_t ambientHoldFor(Posture p) {
  switch (p) {
    case Posture::Full: return 300000;  // 5 min - desk display stays lit
    case Posture::Calm: return 30000;   // 30 s - a glance, then clear
    case Posture::Dark: return 5000;    // 5 s - battery-frugal quick clear
  }
  return 5000;
}

namespace {
Event jobEvent(uint32_t key, uint8_t status, bool hasAccent, uint8_t hue) {
  Event e;
  e.type = Event::Type::JobState;
  e.key = key;
  e.status = status;
  e.hasAccent = hasAccent;
  e.accentHue = hue;
  return e;
}
}  // namespace

FrameResult Mapper::apply(const nsn::Frame& f, attn::Router& router,
                          uint32_t nowMs) {
  FrameResult r;
  const uint8_t n = f.count > nsn::kMaxSegs ? uint8_t(nsn::kMaxSegs) : f.count;

  // Present segments: index == stable job key. The wire hue is the accent.
  for (uint8_t i = 0; i < n; ++i) {
    segStatus_[i] = f.segs[i].state;   // remembered so timeout() can tell CTAs apart
    harness_[i] = f.segs[i].harness;   // v2: session metadata for the panel
    size_t k = 0;
    for (; k < size_t(nsn::kMaxTitle) && f.segs[i].title[k]; ++k) title_[i][k] = f.segs[i].title[k];
    title_[i][k] = 0;
    attn::Decision d =
        router.route(jobEvent(i, f.segs[i].state, /*hasAccent=*/true,
                              f.segs[i].hue),
                     nowMs);
    r.eventsRouted++;
    r.ringDirty |= d.ringDirty;
    if (d.screen.attention) r.anyAttention = true;
  }

  // Tail that shrank away since the last frame: free those slots.
  for (uint8_t i = n; i < liveCount_; ++i) {
    harness_[i] = 0; title_[i][0] = 0;   // drop stale v2 metadata for freed slots
    attn::Decision d = router.route(jobEvent(i, uint8_t(Status::Offline),
                                             /*hasAccent=*/false, 0),
                                    nowMs);
    r.eventsRouted++;
    r.ringDirty |= d.ringDirty;
  }

  liveCount_ = n;
  brightness_ = f.brightness;
  lastFrameMs_ = nowMs;
  seen_ = true;

  // One aggregate intent for the whole snapshot.
  r.screen = r.anyAttention ? attn::ScreenIntent{true, attn::ScreenId::StatusIdle, true}
                         : attn::ScreenIntent{true, attn::ScreenId::StatusIdle, false};
  return r;
}

FrameResult Mapper::timeout(attn::Router& router, uint32_t nowMs,
                            uint32_t ambientMs, uint32_t attnMs) {
  FrameResult r;
  if (!seen_ || liveCount_ == 0) return r;         // nothing to expire
  // ⚠ SIGNED delta, like every other age check in the codebase (see
  // Router::forceExpireAttention). A frame stamped a few ms in the FUTURE
  // relative to nowMs is normal: loop() caches now=millis() at the top of the
  // iteration, so anything that applies a frame later in that same iteration
  // stamps a slightly larger value. Unsigned, that tiny negative wrapped to
  // ~4.29e9 ms and read as "quiet for 49 days" - every job, calls-to-action
  // included, was expired on the very next tick. Measured on hardware: a
  // 3-segment frame went 3 -> 0 jobs within one loop iteration.
  const int32_t quiet = int32_t(nowMs - lastFrameMs_);
  if (quiet < int32_t(ambientMs)) return r;        // still fresh (or from the future)

  bool anyLive = false;
  for (uint8_t i = 0; i < liveCount_; ++i) {
    // Attention (needs-you / error) segments are calls-to-action: hold them until
    // attnMs so a job waiting on you doesn't vanish when the broker goes quiet.
    // Ambient segments expire at ambientMs. Re-Offlining an already-gone segment
    // is idempotent (Router returns not-dirty), so this stays edge-triggered.
    if (attn::isAttentionStatus(Status(segStatus_[i])) && quiet < int32_t(attnMs)) {
      anyLive = true;
      continue;
    }
    attn::Decision d = router.route(jobEvent(i, uint8_t(Status::Offline),
                                             /*hasAccent=*/false, 0),
                                    nowMs);
    r.eventsRouted++;
    r.ringDirty |= d.ringDirty;
  }
  if (!anyLive) liveCount_ = 0;                     // everything expired
  if (r.ringDirty)
    r.screen = attn::ScreenIntent{true, attn::ScreenId::StatusIdle, false};
  return r;
}

void Mapper::reset() {
  liveCount_ = 0;
  brightness_ = 0;
  seen_ = false;
}

}  // namespace nimbus::notifier
