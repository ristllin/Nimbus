#include "nimbus/ring_animator.h"

namespace nimbus::ring {

namespace {
// Transition durations (ms) - docs/led-ux.md.
constexpr uint32_t kGrowMs    = 350;   // birth: 1 LED -> full arc
constexpr uint32_t kDieMs     = 350;   // termination: collapse to a point
constexpr uint32_t kCrossMs   = 250;   // hue crossfade
constexpr uint32_t kRippleMs  = 500;   // done: success sweep
// Per-status steady-state motion (mirrors solide leds.cpp so the two paths match).
constexpr uint16_t kBreatheMs   = 2600;  // the needs-you breathe period (input/approval/error)
constexpr uint16_t kBlinkMs     = 300;   // legacy Blink period - NOTHING maps to Blink by
                                         // default anymore (ambient grammar: persistent states
                                         // never strobe); kept for wire/enum compat only
constexpr uint16_t kCometStepMs = 55;    // Running comet head speed
constexpr int      kCometTail   = 5;     // comet tail length (LEDs)
constexpr uint8_t  kFloorDim    = 10;    // faint floor so the arc never fully vanishes

// Linear interpolate a byte a->b at t in [0,1000] (fixed-point permille).
uint8_t lerp8(uint8_t a, uint8_t b, uint32_t permille) {
  if (permille >= 1000) return b;
  int d = int(b) - int(a);
  return uint8_t(int(a) + (d * int(permille)) / 1000);
}
RGB lerpRGB(RGB a, RGB b, uint32_t permille) {
  return {lerp8(a.r, b.r, permille), lerp8(a.g, b.g, permille),
          lerp8(a.b, b.b, permille)};
}
// Elapsed fraction of a phase as permille (0..1000), clamped.
uint32_t frac(uint32_t now, uint32_t start, uint32_t dur) {
  if (dur == 0 || now <= start) return 0;
  uint32_t e = now - start;
  return e >= dur ? 1000 : (e * 1000) / dur;
}

const RGB kBlack{0, 0, 0};
RGB hue(uint8_t h) {
  if (h == 255) return {255, 255, 255};   // 255 = WHITE sentinel (styleFor(Idle)) - audit F1:
                                          // without this, idle arcs + the desk heartbeat rendered
                                          // near-pure RED (the reserved alert colour).
  return solide::ring::hsv(uint16_t(h) * 257, 255, 255);   // 0-254 -> full wheel
}
// Rainbow theme: a full hue wheel laid around the ring, rotating with time -
// nowMs*11 wraps the 16-bit hue every ~6 s (smooth motion, not a strobe; the
// ambient grammar's no-strobe rule is about flashing, and the owner explicitly
// asked for the cycling look on this one opt-in theme).
RGB rainbowWheel(uint32_t nowMs, int pos, int ledCount) {
  const uint16_t hp = uint16_t(uint32_t(pos) * 65535u / uint32_t(ledCount > 0 ? ledCount : 1) +
                               nowMs * 11u);
  return solide::ring::hsv(hp, 255, 255);
}

// --- CUM-42: the five candidate "working" (Running) animations ---------------
// Each returns a per-LED brightness in permille (0..1000) for LED index k of a
// `len`-LED arc at `nowMs`. PURE + DETERMINISTIC (no rand/millis capture) so the LED
// ring, the on-screen ring, and the web simulator render byte-identically and the
// host tests can pin exact frames. The hue/brightness scaling is applied by the
// caller's put(); this only shapes the motion.
constexpr uint16_t kSparkLingerMs = 900;    // ember fade time behind the CometSparks head
constexpr uint16_t kFireflyMs     = 2200;   // per-LED Fireflies breathe period
constexpr uint8_t  kFireflyThresh = 150;    // below this a firefly reads as dark(ish)

// Small integer hash for the stable per-LED randomness (spark seeds, firefly phases).
uint32_t h32(uint32_t x) { x *= 2654435761u; x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; return x; }

uint32_t lvl8(uint8_t v) { return uint32_t(v) * 1000u / 255u; }   // 0..255 -> permille
uint8_t withFloor(uint8_t f) { return f < kFloorDim ? kFloorDim : f; }   // arc never fully dark

// Each variant is its own small function so the dispatcher stays flat (keeps the
// per-function complexity low) - all return a per-LED level in permille (0..1000).
uint32_t cometTailLevel(int k, int len, uint32_t nowMs) {
  const int tail = (len - 1 < kCometTail) ? (len - 1) : kCometTail;
  const int dist = (solide::ring::cometHead(nowMs, len, kCometStepMs) - k + len) % len;
  return lvl8(withFloor(solide::ring::cometFalloff(dist, tail)));
}
uint32_t cometSparksLevel(int k, int len, uint32_t nowMs) {
  const int tail = 2 < len - 1 ? 2 : (len - 1 > 0 ? len - 1 : 0);
  const int dist = (solide::ring::cometHead(nowMs, len, kCometStepMs) - k + len) % len;
  uint8_t f = solide::ring::cometFalloff(dist, tail);
  if (dist > tail && (h32((uint32_t)k) & 3u) == 0u) {   // ~1 in 4 LEDs seed an ember
    const uint8_t e = solide::ring::fadeLevel((uint32_t)(dist - tail) * kCometStepMs, kSparkLingerMs);
    if (e > f) f = e;                                    // ember lingers, then dies
  }
  return lvl8(withFloor(f));
}
uint32_t dualCometLevel(int k, int len, uint32_t nowMs) {
  const int tail = 3 < len - 1 ? 3 : (len - 1 > 0 ? len - 1 : 0);
  const int head = solide::ring::cometHead(nowMs, len, kCometStepMs);
  const uint8_t f1 = solide::ring::cometFalloff((head - k + len) % len, tail);
  const uint8_t f2 = solide::ring::cometFalloff(((head + len / 2) - k + len) % len, tail);
  return lvl8(withFloor(f1 > f2 ? f1 : f2));
}
uint32_t firefliesLevel(int k, uint32_t nowMs) {
  const uint16_t phase = (uint16_t)(h32((uint32_t)k + 0x9e37u) % kFireflyMs);
  const uint8_t b = solide::ring::breatheLevel(nowMs + phase, kFireflyMs);
  if (b <= kFireflyThresh) return lvl8(kFloorDim);   // dim between twinkles
  // Map (thresh..255] -> (floor..1000] so lit fireflies pop above the floor.
  return lvl8(kFloorDim) + uint32_t(b - kFireflyThresh) * (1000u - lvl8(kFloorDim)) / (255u - kFireflyThresh);
}

uint32_t runStyleLevel(RunStyle style, int k, int len, uint32_t nowMs) {
  if (len <= 0) return 0;
  switch (style) {
    case RunStyle::CometSparks: return cometSparksLevel(k, len, nowMs);
    case RunStyle::DualComet:   return dualCometLevel(k, len, nowMs);
    case RunStyle::BreatheArc:  return lvl8(solide::ring::breatheLevel(nowMs, kBreatheMs));
    case RunStyle::Fireflies:   return firefliesLevel(k, nowMs);
    case RunStyle::CometTail:
    default:                    return cometTailLevel(k, len, nowMs);
  }
}
}  // namespace

void Animator::configure(int ledCount, Posture posture, uint8_t brightness) {
  leds_ = ledCount > 0 ? ledCount : 45;
  posture_ = posture;
  bright_ = brightness;
}

RGB Animator::scaled(RGB c) const {
  // Brightness is the DRIVER's single global cap (leds::setBrightness() on the raw
  // frame, applied once in ring_out). The Animator emits normalized pixels; the
  // per-status relative brightness is applied separately in put(). Baking the
  // global cap HERE too squared it - Full posture ran at (b/255)^2, ~1.4% at the
  // Balanced default, crushing dim states to black (audit F2). Pass-through now;
  // bright_ retained only so configure()'s signature stays stable.
  (void)bright_;
  return c;
}

int Animator::find(uint32_t key) const {
  for (int i = 0; i < ANIM_MAX_SEGMENTS; ++i)
    if (segs_[i].used && segs_[i].key == key) return i;
  return -1;
}
int Animator::alloc(uint32_t key) {
  int i = find(key);
  if (i >= 0) return i;
  for (int j = 0; j < ANIM_MAX_SEGMENTS; ++j)
    if (!segs_[j].used) { segs_[j] = Seg{}; segs_[j].used = true; segs_[j].key = key; return j; }
  return -1;
}

void Animator::born(uint32_t key, uint8_t h, uint32_t nowMs) {
  int i = alloc(key);
  if (i < 0) return;
  Seg& s = segs_[i];
  s.phase = Phase::Grow;
  s.phaseStart = nowMs;
  s.hue = h;
  s.fromHue = h;
  s.hueStart = 0;
}
void Animator::setHue(uint32_t key, uint8_t h, uint32_t nowMs) {
  int i = find(key);
  if (i < 0) return;
  if (segs_[i].hue == h) return;
  segs_[i].fromHue = segs_[i].hue;
  segs_[i].hue = h;
  segs_[i].hueStart = nowMs ? nowMs : 1;  // 0 reserved for "inactive"
}
void Animator::setAnim(uint32_t key, uint8_t anim, uint8_t brightPct) {
  int i = find(key);
  if (i < 0) return;
  segs_[i].anim = anim;
  segs_[i].bright = brightPct > 100 ? 100 : brightPct;
}
void Animator::done(uint32_t key, uint32_t nowMs) {
  int i = find(key);
  if (i < 0) return;
  segs_[i].phase = Phase::Ripple;
  segs_[i].phaseStart = nowMs;
}
void Animator::terminated(uint32_t key, uint32_t nowMs) {
  int i = find(key);
  if (i < 0) return;
  segs_[i].phase = Phase::Dying;
  segs_[i].phaseStart = nowMs;
}
void Animator::revive(uint32_t key) {
  // A segment that finished (Ripple) and is running AGAIN must drop the done
  // sweep immediately - Orchestrator sub-agents flip Done->Running rapidly, and
  // the lingering ~500 ms ripple read as stray flicker amid the working comet
  // (owner P2.4). Steady renders whatever live anim/hue the plan re-pushes.
  int i = find(key);
  if (i >= 0 && segs_[i].phase == Phase::Ripple) segs_[i].phase = Phase::Steady;
}
void Animator::clear() {
  for (auto& s : segs_) s.used = false;   // no collapse animation - this runs while
                                          // the ring is Dark/Calm and not rendering
  lastPlaced_ = 0;
}
void Animator::setSingleCue(bool lit, uint8_t hue, uint8_t anim, uint16_t periodMs) {
  single_ = {lit, hue, anim, uint16_t(periodMs ? periodMs : 2600)};
}
void Animator::setRainbow(bool on, uint8_t alertHue) {
  rainbow_ = on;
  rainbowAlert_ = alertHue;
}

int Animator::liveCount() const {
  int n = 0;
  for (int i = 0; i < ANIM_MAX_SEGMENTS; ++i) if (segs_[i].used) ++n;
  return n;
}

void Animator::frame(uint32_t nowMs, RGB* out, int n) {
  if (n <= 0) return;
  const int L = leds_ < n ? leds_ : n;
  for (int i = 0; i < n; ++i) out[i] = kBlack;

  // Duty envelope (low-battery cue): during the gap the frame stays all-black.
  // Returning a BLACK FRAME rather than skipping the render is what keeps the
  // device's animation tick alive - ring_out stops ticking the moment a plan goes
  // unlit, so expressing this as "not lit" would extinguish the cue for good
  // instead of pulsing it. It also keeps the driver's raw-frame staleness
  // watchdog fed.
  if (envPeriodMs_ && dutyPermille(nowMs, envOnMs_, envPeriodMs_) == 0) return;

  // ---- Dark/Calm: ONE dim breathing cue over the whole ring ----
  // The "one renderer, all postures" refactor: Dark/Calm render through THIS same
  // raw-frame path as Full (posture is a MODE, not a fork to the driver's Pattern
  // engine). So the colour, gamma and animation match Full exactly - the cue
  // honours the plan's hue/anim/period (Error breathes RED here now; AttnPeriodMs
  // is live), where the old driver-Pattern::Pulse fork was hardcoded blue @ 2000 ms
  // and gamma-corrected while Full was linear (the audit's P12 gamma split).
  if (posture_ != Posture::Full) {
    if (!single_.lit) return;   // nothing needs you -> ring stays off (already cleared)
    const RGB base = hue(single_.hue);   // hue() maps 255 -> white
    const auto A = solide::ring::Anim(single_.anim);
    uint32_t lvl;
    if (A == solide::ring::Anim::Solid || A == solide::ring::Anim::Fade)
      lvl = 1000;                        // steady (a Done glance)
    else                                 // Breathe (and any legacy Blink - strobes banned)
      lvl = uint32_t(solide::ring::breatheLevel(nowMs, single_.periodMs)) * 1000 / 255;
    // Rainbow theme: the dim cue breathes as a rotating gradient - except an
    // ALERT cue (Error keeps its fixed red-family hue) and white/255.
    const bool rb = rainbow_ && single_.hue != rainbowAlert_ && single_.hue != 255;
    for (int i = 0; i < L; ++i)
      out[i] = scaled(lerpRGB(kBlack, rb ? rainbowWheel(nowMs, i, L) : base, lvl));
    return;
  }

  // ---- Full: composite segments with their lifecycle transitions ----
  // Order segments by slot; lay them out around the ring with a 1-LED gap.
  int order[ANIM_MAX_SEGMENTS];
  int count = 0;
  for (int i = 0; i < ANIM_MAX_SEGMENTS; ++i) if (segs_[i].used) order[count++] = i;
  if (count == 0) return;

  solide::ring::Span spans[ANIM_MAX_SEGMENTS];
  // A lone session gets a WIDE gap (~1/4 ring) so it reads as a clear arc, not a
  // full circle; 2+ sessions use a tight 1-LED gap so they pack around the ring.
  // Owner rule (2026-07-13): a lone session fills the FULL ring - the earlier
  // ~3/4-arc gap read as 'broken LEDs'. Semantics stay safe: status is carried
  // by COLOR + PATTERN (theme roles; red = errors only), not by arc length.
  const int gap = (count == 1) ? 0 : 1;
  int placed = solide::ring::layout(L, count, gap, spans, ANIM_MAX_SEGMENTS);
  lastPlaced_ = placed;
  lastN_ = L > 0 ? L : 1;
  for (int s2 = 0; s2 < placed; ++s2) lastSpans_[s2] = spans[s2];

  // SEPARATORS (owner 2026-07-13): with 2+ sessions the 1-LED gaps used to stay
  // DARK - invisible next to bright arcs, so you couldn't count the agents. Paint
  // every gap LED a dim STATIC white divider instead (static per the owner's
  // motion rule): count the dividers and you've counted your agents. A lone
  // session has gap=0 -> one unbroken ring, no divider.
  //
  // CONTRAST-AWARE (owner 2026-08-09): a QUEUED sub-session's arc is the white
  // sentinel (Idle style, hue 255) - and a white arc beside a white divider is an
  // invisible split ("if a session is static white it's impossible to see the
  // splits"). A divider BORDERING a white arc therefore renders DARK (a notch -
  // clearly visible against white), while every other divider keeps the
  // owner-approved dim white. layout() is sequential, so the gap after spans[s]
  // separates seg order[s] from seg order[(s+1) % placed].
  if (placed >= 2) {
    auto whiteish = [&](int s) {
      const Seg& sg = segs_[order[s]];
      return sg.hue == 255 || (sg.hueStart != 0 && sg.fromHue == 255);
    };
    for (int s = 0; s < placed; ++s) {
      const int nxt = (s + 1) % placed;
      const bool dark = whiteish(s) || whiteish(nxt);
      const int gapStart = (spans[s].start + spans[s].len) % L;
      const int gapLen = ((spans[nxt].start - gapStart) % L + L) % L;
      for (int k = 0; k < gapLen; ++k) {
        if (!dark)
          out[(gapStart + k) % L] = scaled(RGB{46, 46, 46});  // dim neutral divider
                                                              // (audit F4: was unscaled
                                                              // -> outshone the arcs)
        // dark: the LED stays black - the notch IS the divider.
      }
    }
  }

  for (int s = 0; s < placed; ++s) {
    Seg& seg = segs_[order[s]];
    const solide::ring::Span sp = spans[s];

    // Current base color: crossfade fromHue->hue over kCrossMs.
    RGB base = hue(seg.hue);
    if (seg.hueStart) {
      uint32_t cf = frac(nowMs, seg.hueStart, kCrossMs);
      base = lerpRGB(hue(seg.fromHue), hue(seg.hue), cf);
      if (cf >= 1000) seg.hueStart = 0;  // crossfade done
    }
    // Rainbow theme: this arc renders a rotating full-wheel gradient instead of
    // its static role hue - EXCEPT alert (Error keeps the reserved fixed hue) and
    // white/255 (idle stays deliberately dull). baseAt(k) is the per-LED base
    // every phase below paints with; k is the offset within the arc.
    const bool segRainbow = rainbow_ && seg.hue != rainbowAlert_ && seg.hue != 255;
    auto baseAt = [&](int k) -> RGB {
      return segRainbow ? rainbowWheel(nowMs, sp.start + k, L) : base;
    };

    switch (seg.phase) {
      case Phase::Grow: {
        // Arc grows from 1 LED to full, easing in from dim to full in its OWN
        // hue. (Was a fast rainbow shimmer - full hue wheel every ~32 ms - a
        // strobe by any name. Ambient grammar, owner 2026-07-16: even one-shot
        // cues don't flash; arrival reads as motion + brightening, not fireworks.)
        uint32_t g = frac(nowMs, seg.phaseStart, kGrowMs);
        int width = 1 + int((g * (sp.len - 1)) / 1000);
        const uint32_t lvl = 300 + (700 * g) / 1000;   // 30% -> 100% ease-in
        for (int k = 0; k < width; ++k) {
          int idx = (sp.start + k) % L;
          out[idx] = scaled(lerpRGB(kBlack, baseAt(k), lvl));
        }
        if (g >= 1000) seg.phase = Phase::Steady;
        break;
      }
      case Phase::Steady: {
        // Per-status MOTION (owner: "apply different patterns not just colours").
        // Each pattern modulates `base` over the arc; a per-status brightness scale
        // lets ambient states sit back. `dim(level)` = base at level/255, then the
        // segment's own brightness, then the global cap via scaled().
        auto put = [&](int k, uint32_t lvlPermille) {
          RGB c = lerpRGB(kBlack, baseAt(k), lvlPermille);
          if (seg.bright < 100)
            c = {uint8_t((int(c.r) * seg.bright) / 100), uint8_t((int(c.g) * seg.bright) / 100),
                 uint8_t((int(c.b) * seg.bright) / 100)};
          out[(sp.start + k) % L] = scaled(c);
        };
        const auto A = solide::ring::Anim(seg.anim);
        if (A == solide::ring::Anim::Comet) {           // the "working" slide - one of the
          // five CUM-42 candidate variants, selected by runStyle_ (default = the shipped
          // comet). Pure per-LED shaping keeps frame() itself simple.
          for (int k = 0; k < sp.len; ++k) put(k, runStyleLevel(runStyle_, k, sp.len, nowMs));
        } else if (A == solide::ring::Anim::Breathe) {  // whole arc breathes
          const uint32_t lp = uint32_t(solide::ring::breatheLevel(nowMs, kBreatheMs)) * 1000 / 255;
          for (int k = 0; k < sp.len; ++k) put(k, lp);
        } else if (A == solide::ring::Anim::Blink) {    // whole arc blinks (attention)
          const uint32_t lp = solide::ring::blinkOn(nowMs, kBlinkMs)
                                  ? 1000u : uint32_t(kFloorDim) * 1000 / 255;
          for (int k = 0; k < sp.len; ++k) put(k, lp);
        } else if (A == solide::ring::Anim::Fade) {     // settle toward a dim ember
          const uint32_t lp = uint32_t(solide::ring::fadeLevel(nowMs - seg.phaseStart, kRippleMs)) * 1000 / 255;
          // Done settles to 38% (was 10% - read as a BLANK arc next to bright
          // neighbors while holding its slot for minutes; owner 2026-07-14).
          for (int k = 0; k < sp.len; ++k) put(k, lp < 380 ? 380 : lp);
        } else {                                        // Solid / Off / default
          for (int k = 0; k < sp.len; ++k) put(k, 1000);
        }
        break;
      }
      case Phase::Ripple: {
        // Success sweep across the arc in the segment's OWN (themed) hue, then a
        // dim ember. Was hardcoded green (hue 85) - stray GREEN flashes on every
        // theme whenever anything finished (owner P2.4: "green LEDs flickering
        // while the white is spinning"); the plan already delivers the theme's
        // Done role hue via setHue, so `base` is the correct sweep colour.
        auto emberAt = [&](int k) -> RGB {
          const RGB b = baseAt(k);
          return {uint8_t(b.r / 4), uint8_t(b.g / 4), uint8_t(b.b / 4)};
        };
        const uint32_t r = frac(nowMs, seg.phaseStart, kRippleMs);
        if (r >= 1000) {  // ripple done -> HAND OFF to the Steady Fade ember. NOT a
                          // terminal latch (audit F3): a session that finishes and
                          // then runs again used to stay a green ember forever;
                          // Steady renders whatever live anim/hue the plan re-pushes.
          for (int k = 0; k < sp.len; ++k) out[(sp.start + k) % L] = scaled(emberAt(k));
          seg.phase = Phase::Steady;
          seg.phaseStart = nowMs;
        } else {
          const int headK = int((r * sp.len) / 1000);
          for (int k = 0; k < sp.len; ++k) {
            RGB c = (k <= headK && headK - k < 3) ? baseAt(k) : emberAt(k);
            out[(sp.start + k) % L] = scaled(c);
          }
        }
        break;
      }
      case Phase::Dying: {
        // Collapse from both ends toward the center, FADING as it goes (was a
        // hard full-brightness shrink-cut - a sudden change in the corner of the
        // eye; ambient grammar, owner 2026-07-16: exits dim out, they don't snap).
        uint32_t d = frac(nowMs, seg.phaseStart, kDieMs);
        int remain = int(((1000 - d) * sp.len) / 1000);
        int off = (sp.len - remain) / 2;
        for (int k = 0; k < remain; ++k)   // full -> dark across the collapse
          out[(sp.start + off + k) % L] = scaled(lerpRGB(baseAt(off + k), kBlack, d));
        if (d >= 1000) seg.used = false;  // gone
        break;
      }
    }
  }
}

}  // namespace nimbus::ring
