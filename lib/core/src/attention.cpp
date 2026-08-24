#include "nimbus/attention.h"

namespace nimbus::attn {

namespace {
using solide::ring::Status;

// Ambient path: render StatusIdle via the coalescer (attention=false).
ScreenIntent ambient() { return {true, ScreenId::StatusIdle, false}; }
// Attention path: immediate render, bypasses the coalescer.
ScreenIntent urgent(ScreenId s) { return {true, s, true}; }
}  // namespace

bool isAttentionStatus(solide::ring::Status s) {
  return s == Status::WaitingInput || s == Status::AwaitingApproval ||
         s == Status::Error;
}

Decision Router::route(const Event& e, uint32_t nowMs) {
  if (tap_) tap_(e);   // cross-cutting observer (SFX) - sees every event, both modes
  Decision d;
  switch (e.type) {
    case Event::Type::JobState: {
      // Drop out-of-range status bytes rather than minting a phantom slot - the
      // same garbage-tolerance nsn_proto applies at the wire. Idle..Offline (0..6).
      if (e.status > uint8_t(Status::Offline)) return d;  // no-op decision
      const Status st = Status(e.status);
      // Tombstone gate: a stale attention arc the backstop just force-expired must
      // not flap back from the broker's next full-snapshot re-send of the SAME
      // (key,status). Any DIFFERENT status (incl. Offline) clears the tombstone and
      // flows normally - a real state change always revives the job.
      if (st == Status::Offline || !isAttentionStatus(st)) {
        tombstoneClear(e.key);
      } else if (tombstoneBlocks(e.key, uint8_t(st), nowMs)) {
        return d;  // suppressed: identical stale re-add inside the tombstone TTL
      }
      // NOTE (P3): upsert() returns -1 when the ring is full of strictly
      // higher-priority jobs (e.g. 8 AwaitingApproval) and refuses this one.
      // The badge/notify below still fire once, but a refused attention job is
      // not persistently tracked and won't resurface in topAttention(). This
      // only bites with > RING_MAX_SEGMENTS simultaneous attention jobs; the
      // Notifier frame→event mapping (which can carry up to 16 segments) must
      // decide the overflow policy then. Tracked in docs/plan.md Known Limits.
      jobs_.upsert(e.key, st, nowMs);  // Offline frees the slot
      if (e.hasAccent) jobs_.setAccent(e.key, e.accentHue);  // 255 = white is valid
      d.ringDirty = true;
      if (isAttentionStatus(st)) {
        d.screen = urgent(ScreenId::StatusIdle);  // owner: no badge box - the job rows + ring say it
        d.notify = {true, Event::Type::JobState};
      } else {
        d.screen = ambient();
      }
      break;
    }
    case Event::Type::JobProgress:
      jobs_.setProgress(e.key, e.value);
      d.ringDirty = true;
      d.screen = ambient();
      break;
    case Event::Type::IncomingAsk:
      // The ask already reached the user's channel - no NotifyIntent.
      // Stamp askSince_ only when a NEW ask opens. A re-ask while one is ALREADY
      // pending must NOT reset the dwell clock, or forceExpireAttention's absolute
      // cap is defeated: a re-firing ask (a routine that asks each run, or the model
      // re-asking across turns the owner never answers) would breathe FOREVER,
      // because scheduled turns deliberately never call AskCleared. Keeping the
      // original stamp means the latch still ages out ~AttnHoldMs after the FIRST
      // unanswered ask.
      if (!askPending_) askSince_ = nowMs;
      askPending_ = true;
      d.ringDirty = true;
      d.screen = urgent(ScreenId::Ask);
      break;
    case Event::Type::AskCleared:
      askPending_ = false;
      d.ringDirty = true;
      d.screen = ambient();
      break;
    case Event::Type::Voice:
      // The glyph must not wait for the 30 s coalescer: any live stage takes
      // the attention path; returning to None settles back to ambient.
      voice_ = e.stage;
      d.ringDirty = true;
      d.screen = (e.stage != VoiceStage::None) ? urgent(ScreenId::VoiceGlyph)
                                            : ambient();
      break;
    case Event::Type::LowBattery:
      // T1 warning is a compact badge over the persistent status display (brief:
      // "force Battery Saver + warn (e-ink badge)"), not the full Battery
      // telemetry screen the user navigates to explicitly.
      lowBattery_ = true;
      d.ringDirty = true;
      d.screen = urgent(ScreenId::StatusIdle);  // owner: no badge box
      d.notify = {true, Event::Type::LowBattery};
      break;
    case Event::Type::BatteryOk:
      lowBattery_ = false;
      d.ringDirty = true;
      d.screen = ambient();
      break;
    case Event::Type::NetworkDegraded:
      netDegraded_ = true;  // shown on the status screen only - ring untouched
      d.screen = ambient();
      break;
    case Event::Type::NetworkOk:
      netDegraded_ = false;
      d.screen = ambient();
      break;
  }
  return d;
}

Router::Attention Router::topAttention() const {
  // Fixed source precedence: ask > attention jobs (by state priority) > T1.
  if (askPending_) return {true, Status::WaitingInput, 213, Attention::Src::Ask};

  solide::ring::Slot snap[RING_MAX_SEGMENTS];
  const int n = jobs_.snapshot(snap, RING_MAX_SEGMENTS);
  int best = -1;
  uint8_t bestPri = 0;
  for (int i = 0; i < n; ++i) {
    // Only attention states count - Running/Done may be the allocator's
    // highestPriority() but must never light the Dark/Calm attention LED.
    if (!isAttentionStatus(snap[i].status)) continue;
    const uint8_t p = solide::ring::priorityFor(snap[i].status);
    if (best < 0 || p > bestPri) { best = i; bestPri = p; }
  }
  if (best >= 0)
    return {true, snap[best].status,
            solide::ring::styleFor(snap[best].status).hue,
            Attention::Src::Job};

  if (lowBattery_) return {true, Status::Error, 0, Attention::Src::LowBattery};
  return {};
}

void Router::tombstoneSet(uint32_t key, uint8_t status, uint32_t nowMs) {
  // Reuse this key's slot if present, else the first free, else the OLDEST (a
  // bounded table can't refuse - the oldest is closest to its TTL anyway).
  int use = -1, oldest = 0;
  for (int i = 0; i < RING_MAX_SEGMENTS; ++i) {
    if (tombs_[i].used && tombs_[i].key == key) { use = i; break; }
    if (!tombs_[i].used && use < 0) use = i;
    if (tombs_[i].used && (int32_t)(tombs_[i].at - tombs_[oldest].at) < 0) oldest = i;
  }
  if (use < 0) use = oldest;
  tombs_[use] = {key, status, nowMs, true};
}

void Router::tombstoneClear(uint32_t key) {
  for (int i = 0; i < RING_MAX_SEGMENTS; ++i)
    if (tombs_[i].used && tombs_[i].key == key) tombs_[i].used = false;
}

bool Router::tombstoneBlocks(uint32_t key, uint8_t status, uint32_t nowMs) {
  for (int i = 0; i < RING_MAX_SEGMENTS; ++i) {
    if (!tombs_[i].used || tombs_[i].key != key) continue;
    if ((int32_t)(nowMs - tombs_[i].at) > (int32_t)kTombstoneTtlMs) {
      tombs_[i].used = false;   // TTL safety valve: never suppress forever
      return false;
    }
    return tombs_[i].status == status;  // identical stale re-add → suppress
  }
  return false;
}

bool Router::forceExpireAttention(uint32_t nowMs, uint32_t maxAgeMs) {
  bool cleared = false;
  // Attention-status jobs: expire any whose dwell in that status exceeds the cap.
  // enteredAt is stamped on the status CHANGE (upsert), so a stuck Error arc's
  // age is exact. Snapshot first, then upsert Offline - freeing the slot.
  // Each expiry leaves a TOMBSTONE so the broker's next full-snapshot re-send of
  // the same stale (key,status) can't flap the arc back (see attention.h).
  solide::ring::Slot snap[RING_MAX_SEGMENTS];
  const int n = jobs_.snapshot(snap, RING_MAX_SEGMENTS);
  for (int i = 0; i < n; ++i) {
    if (isAttentionStatus(snap[i].status) &&
        (int32_t)(nowMs - snap[i].enteredAt) > (int32_t)maxAgeMs) {
      tombstoneSet(snap[i].key, uint8_t(snap[i].status), nowMs);
      jobs_.upsert(snap[i].key, Status::Offline, nowMs);  // free the slot (collapse)
      cleared = true;
    }
  }
  // Ask latch: no per-key timestamp, so it carries its own askSince_.
  if (askPending_ && (int32_t)(nowMs - askSince_) > (int32_t)maxAgeMs) {
    askPending_ = false;
    cleared = true;
  }
  return cleared;
}

}  // namespace nimbus::attn
