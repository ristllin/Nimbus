// test_ring_contract - golden-frame CONTRACT tests for the LED pipeline.
//
// Born from the 2026-07 UX audit: the render pipeline has golden-image tests and
// produced almost no layer-drift bugs; the LED pipeline had none and produced
// all of them (idle rendered RED via the missed 255-white sentinel, brightness
// applied twice, the Done ripple latching terminal, unscaled separators). These
// tests pin the INTENDED contract of Animator::frame() so a disagreeing layer
// fails CI instead of shipping. Each test names the audit finding it guards.
//
// Contract principles (audit round, owner-approved direction):
//   C1  hue byte 255 is the WHITE sentinel on every path (red is reserved).
//   C2  the Animator does NOT bake global brightness into pixels - the driver's
//       strip-level setBrightness() is the ONE owner of the global cap (raw
//       frames are strip-scaled; baking it too squares the brightness).
//   C3  no lifecycle phase is terminal except Dying: a session that finishes
//       (ripple) and then works again must render its live status, not a fossil.
//   C4  decoration (separators) never outshines content (arcs).

#include <unity.h>

#include <initializer_list>

#include "nimbus/ring_animator.h"

using namespace nimbus::ring;
using solide::ring::RGB;

void setUp() {}
void tearDown() {}

static constexpr int N = 45;

static Animator makeAnim(uint8_t brightness = 30) {
  Animator a;
  a.configure(N, nimbus::Posture::Full, brightness);
  return a;
}
static void render(Animator& a, uint32_t t, RGB* out) {
  for (int i = 0; i < N; ++i) out[i] = {0, 0, 0};
  a.frame(t, out, N);
}
static RGB peak(const RGB* f) {  // channel-wise max across the frame
  RGB m{0, 0, 0};
  for (int i = 0; i < N; ++i) {
    if (f[i].r > m.r) m.r = f[i].r;
    if (f[i].g > m.g) m.g = f[i].g;
    if (f[i].b > m.b) m.b = f[i].b;
  }
  return m;
}

// C1 / audit F1: hue 255 = WHITE (styleFor(Idle) uses it) - the idle heartbeat
// and Idle arcs must be neutral, never red-dominant (red is the alert colour).
static void test_idle_hue255_renders_white_not_red() {
  Animator a = makeAnim();
  a.born(1, 255, 0);
  RGB f[N];
  render(a, 1000, f);  // grow (350 ms) long over - steady
  RGB p = peak(f);
  TEST_ASSERT_TRUE_MESSAGE(p.r > 0, "idle segment should be lit");
  // White-ish: green/blue must ride WITH red, not vanish (today: {255,0,15}*b).
  TEST_ASSERT_TRUE_MESSAGE(p.g >= p.r / 2, "idle renders red-dominant: 255 sentinel missed");
  TEST_ASSERT_TRUE_MESSAGE(p.b >= p.r / 2, "idle renders red-dominant: 255 sentinel missed");
}

// C2 / audit F2: the frame leaves the Animator UNSCALED - the driver's
// setBrightness() is the single owner of the global cap. (Baking it here too
// renders Full posture at (b/255)^2 - 1.4% at the Balanced default.)
static void test_animator_does_not_bake_global_brightness() {
  Animator a = makeAnim(/*brightness=*/30);
  a.born(1, 85, 0);                       // green
  a.setAnim(1, 1 /*Solid*/, 100);
  RGB f[N];
  render(a, 1000, f);
  RGB p = peak(f);
  TEST_ASSERT_TRUE_MESSAGE(p.g >= 250,
      "pixels left the Animator pre-dimmed: global brightness applied twice");
}

// C3 / audit F3: the Done ripple must NOT be terminal. After the ripple, a
// status change back to a live anim (the multi-turn daily flow: Done -> user
// replies -> Running) must render the live status, not a permanent green ember.
static void test_ripple_releases_when_session_runs_again() {
  Animator a = makeAnim();
  a.born(1, 170, 0);                      // blue running arc
  a.setAnim(1, 4 /*Comet*/, 100);
  RGB f[N];
  render(a, 500, f);                      // steady
  a.done(1, 1000);                        // -> ripple
  render(a, 1300, f);                     // mid-ripple
  render(a, 2000, f);                     // ripple long finished -> ember
  // Session goes back to work: the plan re-pushes hue + live anim.
  a.setHue(1, 170, 3000);
  a.setAnim(1, 4 /*Comet*/, 100);
  render(a, 4000, f);
  RGB p = peak(f);
  TEST_ASSERT_TRUE_MESSAGE(p.b > p.g,
      "arc still shows the green Done ember: Phase::Ripple latched terminal");
}

// C4 / audit F6: separators are decoration - they must render DIMMER than the
// arcs they separate (they currently skip the brightness scale entirely and
// outshine content at the default cap).
static void test_separator_dimmer_than_arcs() {
  Animator a = makeAnim(/*brightness=*/30);
  a.born(1, 85, 0);
  a.born(2, 170, 0);
  a.setAnim(1, 1 /*Solid*/, 100);
  a.setAnim(2, 1 /*Solid*/, 100);
  RGB f[N];
  render(a, 1000, f);
  //

  // Divider pixels are neutral grey (r==g==b>0); arc pixels are hue-dominant.
  int arcPeak = 0, divPeak = 0;
  for (int i = 0; i < N; ++i) {
    const RGB& c = f[i];
    const bool grey = c.r == c.g && c.g == c.b && c.r > 0;
    int mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    if (grey) { if (mx > divPeak) divPeak = mx; }
    else      { if (mx > arcPeak) arcPeak = mx; }
  }
  TEST_ASSERT_TRUE_MESSAGE(divPeak > 0, "expected a separator between two arcs");
  TEST_ASSERT_TRUE_MESSAGE(divPeak <= arcPeak / 2,
      "separator outshines the arcs: divider skips the brightness scale");
}

// C5 / "one renderer, all postures" refactor: Dark/Calm render the single attention
// cue through the SAME raw-frame path as Full. A red Error cue must breathe RED (the
// old driver-Pattern fork hardcoded blue and discarded the hue), whole-ring + dim.
static void test_calm_cue_breathes_in_its_hue() {
  Animator a;
  a.configure(N, nimbus::Posture::Calm, 30);
  a.setSingleCue(true, /*hue=*/0 /*red*/, uint8_t(solide::ring::Anim::Breathe), 2600);
  RGB f[N];
  render(a, 650, f);
  RGB p = peak(f);
  TEST_ASSERT_TRUE_MESSAGE(p.r > 0, "calm cue should be lit");
  TEST_ASSERT_TRUE_MESSAGE(p.r > p.b && p.r > p.g,
      "a red Error cue must render RED in Dark/Calm (was hardcoded blue Pulse)");
  int lit = 0; for (int i = 0; i < N; ++i) if (f[i].r) ++lit;
  TEST_ASSERT_EQUAL_MESSAGE(N, lit, "whole-ring dim cue (owner's Dark/Calm choice)");
  // Breathe varies: sample the trough (t=0) vs the peak (t=period/2), not two
  // phase-symmetric points, and assert a real amplitude swing.
  RGB lo[N], hi[N];
  render(a, 0, lo);
  render(a, 1300, hi);
  int slo = 0, shi = 0; for (int i = 0; i < N; ++i) { slo += lo[i].r; shi += hi[i].r; }
  TEST_ASSERT_TRUE_MESSAGE(shi > slo, "cue must breathe (peak brighter than trough), never hold-strobe");
}

static void test_calm_cue_unlit_is_off() {
  Animator a;
  a.configure(N, nimbus::Posture::Calm, 30);
  a.setSingleCue(false, 0, uint8_t(solide::ring::Anim::Breathe), 2600);
  RGB f[N]; render(a, 500, f);
  for (int i = 0; i < N; ++i)
    TEST_ASSERT_TRUE_MESSAGE(!f[i].r && !f[i].g && !f[i].b, "unlit calm cue = ring off");
}


// ---- low-battery duty envelope ------------------------------------------------
// The cue must be a brief pulse, not a continuous breathe. Asserted at the pixel
// level because "the plan says lowBattCue" proves nothing about what the ring does:
// the gap has to be genuinely BLACK, and the lit window has to genuinely light.
static void test_cue_envelope_pulses_then_goes_black() {
  Animator a;
  a.configure(N, nimbus::Posture::Calm, 30);
  a.setSingleCue(true, /*hue=*/0 /*red*/, uint8_t(solide::ring::Anim::Breathe), 2600);
  a.setCueEnvelope(nimbus::kLowBattCueOnMs, nimbus::kLowBattCuePeriodMs);

  RGB f[N];
  // Inside the window: lit. Sampled at 1.3 s, away from the breathe trough at 0.
  render(a, 1300, f);
  TEST_ASSERT_TRUE_MESSAGE(peak(f).r > 0, "low-battery cue never lights inside its window");

  // Inside the gap: EVERY led must be exactly black. A leaky gap would just look
  // like the old continuous breathe, which is the whole complaint.
  for (uint32_t t : {4000u, 12000u, 30000u, 59500u}) {
    render(a, t, f);
    for (int i = 0; i < N; ++i)
      TEST_ASSERT_TRUE_MESSAGE(!f[i].r && !f[i].g && !f[i].b,
                               "low-battery cue is lit during the rest gap");
  }
  // And it comes back on the next period - a one-shot would be a silent failure.
  render(a, nimbus::kLowBattCuePeriodMs + 1300u, f);
  TEST_ASSERT_TRUE_MESSAGE(peak(f).r > 0, "low-battery cue did not repeat");
}

// The envelope is opt-in at the renderer too: with no envelope armed (the default
// everywhere) rendering must be byte-identical to today, or every other cue in the
// firmware would start blinking.
static void test_no_envelope_renders_identically() {
  Animator a, b;
  for (Animator* x : {&a, &b}) {
    x->configure(N, nimbus::Posture::Calm, 30);
    x->setSingleCue(true, 0, uint8_t(solide::ring::Anim::Breathe), 2600);
  }
  b.setCueEnvelope(3000, 0);           // onMs set but period 0 == no gating
  RGB fa[N], fb[N];
  for (uint32_t t : {0u, 700u, 4000u, 30000u}) {
    render(a, t, fa);
    render(b, t, fb);
    for (int i = 0; i < N; ++i) {
      TEST_ASSERT_EQUAL_UINT8(fa[i].r, fb[i].r);
      TEST_ASSERT_EQUAL_UINT8(fa[i].g, fb[i].g);
      TEST_ASSERT_EQUAL_UINT8(fa[i].b, fb[i].b);
    }
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_idle_hue255_renders_white_not_red);
  RUN_TEST(test_animator_does_not_bake_global_brightness);
  RUN_TEST(test_ripple_releases_when_session_runs_again);
  RUN_TEST(test_separator_dimmer_than_arcs);
  RUN_TEST(test_calm_cue_breathes_in_its_hue);
  RUN_TEST(test_calm_cue_unlit_is_off);
  RUN_TEST(test_cue_envelope_pulses_then_goes_black);
  RUN_TEST(test_no_envelope_renders_identically);
  return UNITY_END();
}
