#include "nimbus/ring_plan.h"

#include "nimbus_config.h"

namespace nimbus::ring {

namespace {
// Voice stages paint the single LED blue (== Running's comet hue) so the
// vocabulary stays consistent with the segment renderer.
constexpr uint8_t kVoiceHue = 170;
// Calm-level session-event blink colour (white); the "working" breathe uses the
// caller-supplied theme hue instead of a fixed colour.
constexpr uint8_t kActivityHue = 255;

// Full-level idle heartbeat: with no live jobs, Desk keeps ONE faint white
// breathe so the ring reads as "alive at your desk" instead of dead-dark. Reuses
// the Idle status vocabulary (styleFor(Idle) = white breathe). The key is a
// reserved sentinel that can't collide with a real job/segment index, so the
// device Animator gives it the same born/collapse lifecycle as any segment.
// kIdleGlowKey now lives in ring_plan.h (shared with ring_out for the heartbeat's
// full-brightness render).
// Faint desk-idle glow GLOBAL cap. Kept below the profile brightness so a reveal
// (which uses the full profile brightness) is unmistakably brighter. The heartbeat
// SEGMENT is rendered at full per-status brightness in ring_out (NOT the 20% Idle
// dim, which is for idle SESSIONS) so the final pixel is ~24/255 - a faint but
// clearly-alive white, not the ~5/255 double-dimmed near-black the audit caught.
constexpr uint8_t  kIdleGlowBrightness = 24;

uint8_t clampByte(int32_t v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

uint8_t attnIndex(const Config& cfg) {
  int32_t i = cfg.effective(Param::AttnLedIndex);
  if (i < 0) i = 0;
  if (i > NIMBUS_RING_LEDS - 1) i = NIMBUS_RING_LEDS - 1;
  return uint8_t(i);
}

uint8_t voiceAnim(attn::VoiceStage s) {
  switch (s) {
    case attn::VoiceStage::Recording:  return uint8_t(solide::ring::Anim::Breathe);
    case attn::VoiceStage::Processing: return uint8_t(solide::ring::Anim::Comet);
    case attn::VoiceStage::Speaking:   return uint8_t(solide::ring::Anim::Solid);
    case attn::VoiceStage::None:       break;
  }
  return uint8_t(solide::ring::Anim::Off);
}

// True when the router holds at least one live session that is still working -
// any live job that is not finished (Done). Calm lights a soft breathe for it so a
// Notifier's Balanced ring DIMS but never blanks while a session runs (owner
// 2026-08-31: "balanced should still show lights, just less, not none"). Attention
// (CTA) statuses already own the single LED earlier in compose(), and an all-Done
// table keeps its own green glance cue, so neither is the reason this returns true.
bool anyWorkingJob(const attn::Router& router) {
  solide::ring::Slot snap[RING_MAX_SEGMENTS];
  const int n = router.jobs().snapshot(snap, RING_MAX_SEGMENTS);
  for (int i = 0; i < n; ++i)
    if (snap[i].status != solide::ring::Status::Done) return true;
  return false;
}
}  // namespace

void Cursor::onDetent(int dir, int ledCount, uint32_t nowMs) {
  if (ledCount <= 0) return;
  pos_ = ((pos_ + dir) % ledCount + ledCount) % ledCount;  // negative-safe wrap
  lastMoveMs_ = nowMs;
  moved_ = true;
}

void Cursor::setIndex(int idx, int ledCount, uint32_t nowMs) {
  if (ledCount <= 0) return;
  // Clamp rather than wrap: a tap names a target that is already on screen, so
  // an out-of-range index is a caller bug, and wrapping would silently focus
  // some unrelated session instead of the one under the finger.
  if (idx < 0) idx = 0;
  if (idx >= ledCount) idx = ledCount - 1;
  pos_ = idx;
  lastMoveMs_ = nowMs;
  moved_ = true;
}

void Cursor::reset() { moved_ = false; }

bool Cursor::activeAt(uint32_t nowMs, uint32_t decayMs) const {
  return moved_ && (nowMs - lastMoveMs_) < decayMs;  // unsigned: wrap-safe
}

Plan compose(const attn::Router& router, const Config& cfg, const Cursor& cursor,
             bool panelBusy, uint32_t nowMs, const ComposeOpts& opts) {
  // Local aliases keep the body diff-free vs the old positional signature.
  const uint32_t cursorDecayMs    = opts.cursorDecayMs;
  const bool     orchWorking      = opts.orchWorking;
  const uint32_t lastActivityMs   = opts.lastActivityMs;
  const uint32_t activityWindowMs = opts.activityWindowMs;
  const uint8_t  themeHue         = opts.themeHue;
  const bool     reveal           = opts.reveal;
  const uint8_t  attnThemedHue    = opts.attnThemedHue;
  const bool     lowBattCueOn     = opts.lowBattCue;
  Plan plan;
  plan.posture = cfg.posture();
  plan.brightness = clampByte(cfg.effective(Param::RingBrightness));
  plan.voice = router.voiceStage();

  // "Wake the ring" (single-click reveal): promote to the full segment treatment
  // at full brightness for the reveal window, so live status is glanceable from
  // any posture - the dimmed Desk idle heartbeat, or Dark/Calm's single LED.
  //
  // Fan-out split (Orchestrator only, opts.fanoutSegments): while sub-sessions
  // are LIVE in the job table, Balanced promotes to the same segment treatment -
  // the ring splits into one arc per sub-session, notifier-style - at Balanced's
  // own profile brightness. It self-clears on the existing lifecycle: Done arcs
  // ember out and the reap Offlines them (ambientHoldFor(Calm)), emptying the
  // table and dropping the posture back to the single-LED grammar. Dark is
  // deliberately untouched (idle-dark is the owner's Dark contract; a single
  // click still reveals). The Notifier never sets the flag, so its Calm stays
  // single-LED.
  bool fanoutSplit = false;
  if (opts.fanoutSegments && plan.posture == Posture::Calm) {
    solide::ring::Slot probe[RING_MAX_SEGMENTS];
    fanoutSplit = router.jobs().snapshot(probe, RING_MAX_SEGMENTS) > 0;
  }
  const Posture effPosture = (reveal || fanoutSplit) ? Posture::Full : plan.posture;
  plan.posture = effPosture;

  if (effPosture == Posture::Full) {
    // Full segment treatment straight from the allocator; `single` stays unlit.
    plan.segCount = router.jobs().snapshot(plan.segs, RING_MAX_SEGMENTS);
    // Desk idle heartbeat: no jobs and no voice stage -> a single faint white
    // breathe rather than a dark ring. Dark/Calm stay dark when idle; only Full
    // (the always-on desk display) glows. Dimmed so it never reads as a job -
    // but a reveal shows it at full brightness so the wake is unmistakable.
    if (plan.segCount == 0 && plan.voice == attn::VoiceStage::None) {
      const attn::Router::Attention att = router.topAttention();
      // The low-battery warning is OWNER-OPT-IN and off by default: a ring that
      // breathes red all night is the brightest thing the device does, held
      // exactly when there is least power to spend on it. Suppressed here rather
      // than at the source so the badge, the Telegram ping and the T2 deep sleep
      // all keep firing - only the ambient LIGHT is silenced.
      const bool lowBattMuted =
          att.src == attn::Router::Attention::Src::LowBattery && !lowBattCueOn;
      if (att.active && !lowBattMuted) {
        // An ask / low-battery with no job of its own: show it as a full-brightness
        // arc so a reveal (Dark/Calm -> Full) doesn't render LESS than Calm did.
        // A call-to-action ALWAYS lights, even though idle is dark.
        solide::ring::Slot& s = plan.segs[0];
        s.used = true;
        s.enteredAt = nowMs;
        s.progress = 0;
        s.key = kAttnGlowKey;
        s.status = att.status;                 // WaitingInput (ask) / Error (low batt)
        s.hasAccent = true;                    // carry the themed/wire attention hue
        s.accentHue = (attnThemedHue != 255) ? attnThemedHue : att.hue;
        plan.segCount = 1;
        // Full brightness - it needs you; do NOT dim it. ⚠ That still governs the
        // ASK, which shares kAttnGlowKey; only the low-battery cue is recessive,
        // so the dim is keyed off the SOURCE, never off the shared key.
        if (att.src == attn::Router::Attention::Src::LowBattery) {
          plan.lowBattCue = true;
          plan.cueBrightPct = kLowBattCueBrightPct;
        }
      } else if (reveal) {
        // Wake feedback: a single-click reveal briefly lights the idle ring (full
        // profile brightness) so the click visibly registers; it fades back to dark
        // when the reveal window ends.
        solide::ring::Slot& s = plan.segs[0];
        s.used = true;
        s.enteredAt = nowMs;
        s.progress = 0;
        s.key = kIdleGlowKey;
        s.status = solide::ring::Status::Idle;   // styleFor(Idle) = white
        s.hasAccent = false;                     // white; no provider accent
        s.accentHue = 0;
        plan.segCount = 1;
      } else {
        // Owner (2026-07-15): Full idle goes FULLY DARK. An all-day desk ring
        // glowing white with nothing happening reads as "stuck / on for no reason"
        // - worse, a bright steady white was the field symptom. No heartbeat
        // segment -> empty Full plan -> the Animator renders black -> ring off.
        // (Supersedes the earlier "faint white heartbeat so it doesn't look dead".)
        plan.segCount = 0;
      }
    }
  } else {
    // Dark AND Calm: ring dark, at most the one attention/activity LED.
    plan.segCount = 0;
    attn::Router::Attention att = router.topAttention();
    // Dark is a DISENGAGE-the-LEDs posture (owner 2026-07-16: "dark should show
    // nothing at all ever, maybe just on error show red, that's it"). So in Dark
    // the ambient status cue is suppressed for every status EXCEPT Error - a job
    // in trouble still surfaces a calm red breathe so a failure can't silently
    // vanish on a dark desk. Calm keeps the single themed cue for all statuses.
    // (The job table still RETAINS non-error CTAs - a single-click reveal or a
    // switch to Calm/Full shows them; only the Dark ambient VISUAL is silenced.
    // The ring simulator mirrors this exactly: Dark → off unless Error.)
    const bool darkSuppress =
        plan.posture == Posture::Dark && att.status != solide::ring::Status::Error;
    // Same owner opt-in as the Full branch. Dark's Error exemption above would
    // otherwise let the low-battery red through in the one mode that promises
    // "no lights ever except an error" - and this is the path a real battery
    // device normally takes, because T1 forces the Dark battery mode.
    const bool lowBattMuted =
        att.src == attn::Router::Attention::Src::LowBattery && !lowBattCueOn;
    if (att.active && !darkSuppress && !lowBattMuted) {
      int32_t hue = cfg.effective(Param::AttnHue);
      plan.single.lit = true;
      plan.single.index = attnIndex(cfg);
      // AUTO hue: prefer the caller's THEME-resolved attention hue (statusStyle x
      // palette) so Dark/Calm follow the theme; the raw wire hue is only the
      // fallback for callers that don't supply one (owner R3 - the last
      // wire-hue-passthrough on the ring).
      plan.single.hue = hue == NIMBUS_ATTN_HUE_AUTO
                            ? (attnThemedHue != 255 ? attnThemedHue : att.hue)
                            : uint8_t(hue);
      plan.single.anim = uint8_t(cfg.effective(Param::AttnAnim));
      plan.single.periodMs = uint16_t(cfg.effective(Param::AttnPeriodMs));
      // cueBrightPct stays 100 here on purpose: Dark/Calm already run at 10/255
      // and 30/255 and the driver's breathe floors at 0.15, so a further x0.25
      // would land at ~1/255 - indistinguishable from off. The mode's own
      // brightness IS the dimming; the duty cycle does the rest.
      if (att.src == attn::Router::Attention::Src::LowBattery) plan.lowBattCue = true;
    }
    if (plan.voice != attn::VoiceStage::None) {
      // Voice owns the LED while a stage is live; configured index/period keep.
      plan.single.lit = true;
      plan.single.index = attnIndex(cfg);
      plan.single.hue = kVoiceHue;
      plan.single.anim = voiceAnim(plan.voice);
      plan.single.periodMs = uint16_t(cfg.effective(Param::AttnPeriodMs));
      // Voice owns the LED now, so the periodic envelope must NOT be armed -
      // otherwise hold-to-talk would strobe on the cue's 3 s / 60 s cycle.
      plan.lowBattCue = false;
    }
    // Calm level ONLY: a soft orchestrator-activity cue when nothing more urgent
    // (attention / voice) already owns the LED - a brief blink when a sub-agent just
    // started or finished, else a slow "working" breathe while a turn is running.
    // Dark stays silent; Full shows activity through its segment arcs instead.
    if (plan.posture == Posture::Calm && !plan.single.lit) {
      const bool pulsing = lastActivityMs != 0 &&
                           (uint32_t)(nowMs - lastActivityMs) < activityWindowMs;
      if (pulsing) {
        plan.single.lit = true;
        plan.single.index = attnIndex(cfg);
        plan.single.hue = themeHue;   // activity cue follows the theme too (R3)
        // Soft quick breathe, NOT a 4 Hz hard blink (ambient grammar, owner
        // 2026-07-16: no strobes, ever - a 1.5 s window of gentle swell still
        // reads "something just happened" without startling).
        plan.single.anim = uint8_t(solide::ring::Anim::Breathe);
        plan.single.periodMs = 800;
      } else if (orchWorking || anyWorkingJob(router)) {
        // orchWorking = an Orchestrator turn is running (no job in the table yet).
        // anyWorkingJob = a live session in the table is still working - this is the
        // Notifier's path: its Calm was going FULLY DARK for a plain Running session
        // (owner 2026-08-31, CUM-253), because only the Orchestrator flag lit here.
        // Balanced must DIM, never blank; the single-LED grammar is kept (segCount
        // stays 0), only Dark is lights-out. (The Orchestrator never reaches this
        // with live jobs - fanoutSplit already promoted Calm to Full arcs above.)
        plan.single.lit = true;
        plan.single.index = attnIndex(cfg);
        plan.single.hue = themeHue;   // "thinking" breathe in the user's theme colour
        plan.single.anim = uint8_t(solide::ring::Anim::Breathe);
        plan.single.periodMs = uint16_t(cfg.effective(Param::AttnPeriodMs));
      }
    }
    // Calm level ONLY, LOWEST precedence: surface a finished (Done) session so a
    // completed job is GLANCEABLE in Calm - the owner's "on balanced I can't see
    // what finished". Placed AFTER the activity glow so it never suppresses the
    // "working" breathe / sub-agent blink; CTAs (attention) and voice already own
    // the LED above, so a job that needs you still wins. Bounded by the EXISTING
    // ambient table retention - Mapper::timeout Offlines a Done job at
    // ambientHoldFor(Calm)=30 s once the broker goes quiet - so the LED self-clears
    // on schedule with NO new timer and no stuck-on beyond a Full-posture Done arc.
    // Dark stays silent ("only when a job needs you").
    if (plan.posture == Posture::Calm && !plan.single.lit) {
      solide::ring::Slot snap[RING_MAX_SEGMENTS];
      const int n = router.jobs().snapshot(snap, RING_MAX_SEGMENTS);
      for (int i = 0; i < n; ++i) {
        if (snap[i].status != solide::ring::Status::Done) continue;
        const solide::ring::Style ds = solide::ring::styleFor(solide::ring::Status::Done);
        plan.single.lit = true;
        plan.single.index = attnIndex(cfg);
        plan.single.hue = ds.hue;                 // 85 (Done green) - honest Done hue
        plan.single.anim = uint8_t(ds.anim);      // Anim::Fade
        plan.single.periodMs = uint16_t(cfg.effective(Param::AttnPeriodMs));
        break;
      }
    }
  }

  // Cursor glow, both postures. A busy panel holds the glow past decay (the
  // "panel syncing" shimmer) - but only for the busy window the interaction
  // itself produced (§3.4: the dwell render starts <= dwellMs after the last
  // detent and costs at most a full-clear cycle). A panel busy with unrelated
  // work - an ambient coalesce flush or an attention screen minutes later -
  // must never resurrect a stale glow: Dark/Calm stay dark between attentions.
  plan.cursor.active =
      cursor.activeAt(nowMs, cursorDecayMs) ||
      (panelBusy && cursor.activeAt(nowMs, NIMBUS_CURSOR_SYNC_HOLD_MS));
  plan.cursor.index = cursor.index();
  plan.cursor.syncing = panelBusy;
  // Session-cursor hue: the accent of the segment the cursor points at (the ring
  // arc for that sub-session). Clamped - the session/segment list shrinks when a
  // job completes (on another task) independently of detents, so an index left
  // past the end must not read out of bounds. 255 (white) when out of range /
  // no segments (Passive / idle).
  plan.cursor.hue = (plan.segCount > 0 && cursor.index() < plan.segCount)
                        ? plan.segs[cursor.index()].accentHue
                        : 255;
  return plan;
}

}  // namespace nimbus::ring
