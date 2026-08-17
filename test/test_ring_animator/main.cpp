#include <unity.h>

#include "nimbus/ring_animator.h"

using namespace nimbus;
using namespace nimbus::ring;

void setUp() {}
void tearDown() {}

static constexpr int L = 45;
static RGB buf[L];

static bool anyLit(const RGB* f, int n) {
  for (int i = 0; i < n; ++i) if (f[i].r || f[i].g || f[i].b) return true;
  return false;
}
static int litCount(const RGB* f, int n) {
  int c = 0;
  for (int i = 0; i < n; ++i) if (f[i].r || f[i].g || f[i].b) ++c;
  return c;
}

static Animator mk() {
  Animator a;
  a.configure(L, Posture::Full, 255);  // full brightness so lerps are exact
  return a;
}

// ---- segment lifecycle ------------------------------------------------------
// (The old system-overlay tests - booting/connecting/connected/setup - were
//  removed with the dead SysState overlay: setSystem() was never called by any
//  production code; boot/connect LED feedback is driven from main.cpp via the
//  driver's self-animating Pattern, not through the Animator.)

// ---- rainbow theme (owner 2026-07-16, "purely for looks") -------------------
// With setRainbow(on) a non-alert arc renders a rotating gradient: LEDs differ
// from each other at one instant AND one LED changes over time. An ALERT-hue arc
// and a white/255 (idle) arc keep their fixed colour - red stays reserved, idle
// stays dull.
static void test_rainbow_cycles_hues_except_alert_and_white() {
  constexpr uint8_t kAlert = 0;   // pretend the theme's alert hue is red/0
  {
    Animator a = mk();
    a.setRainbow(true, kAlert);
    a.born(1, 100, 0);            // any non-alert, non-white hue
    a.setAnim(1, 1 /*Solid*/, 100);
    a.frame(400, buf, L);         // settled, full ring
    bool differAcross = false;    // gradient: not all LEDs the same colour
    for (int i = 1; i < L; ++i)
      if (buf[i].r != buf[0].r || buf[i].g != buf[0].g || buf[i].b != buf[0].b)
        { differAcross = true; break; }
    TEST_ASSERT_TRUE_MESSAGE(differAcross, "rainbow arc is a gradient, not one hue");
    RGB at400 = buf[0];
    a.frame(3400, buf, L);        // 3 s later: the wheel has rotated
    TEST_ASSERT_TRUE_MESSAGE(
        at400.r != buf[0].r || at400.g != buf[0].g || at400.b != buf[0].b,
        "rainbow hue rotates over time");
  }
  {
    Animator a = mk();            // alert arc: fixed hue even in rainbow mode
    a.setRainbow(true, kAlert);
    a.born(1, kAlert, 0);
    a.setAnim(1, 1 /*Solid*/, 100);
    a.frame(400, buf, L);
    for (int i = 1; i < L; ++i) {
      TEST_ASSERT_EQUAL_MESSAGE(buf[0].r, buf[i].r, "alert arc stays one fixed hue");
      TEST_ASSERT_EQUAL_MESSAGE(buf[0].g, buf[i].g, "alert arc stays one fixed hue");
    }
    TEST_ASSERT_TRUE_MESSAGE(buf[0].r > buf[0].g && buf[0].r > buf[0].b,
                             "alert arc renders the red-family alert hue");
  }
  {
    Animator a = mk();            // white/idle arc: stays white
    a.setRainbow(true, kAlert);
    a.born(1, 255, 0);
    a.setAnim(1, 1 /*Solid*/, 100);
    a.frame(400, buf, L);
    TEST_ASSERT_TRUE_MESSAGE(buf[0].r == buf[0].g && buf[0].g == buf[0].b,
                             "white sentinel stays neutral in rainbow mode");
  }
}

static void test_birth_grows_arc_then_settles() {
  Animator a = mk();
  a.born(1, 170, 0);  // blue segment born at t=0

  a.frame(10, buf, L);            // just born: ~1 LED
  int early = litCount(buf, L);
  a.frame(175, buf, L);           // mid-grow (~50%)
  int mid = litCount(buf, L);
  a.frame(400, buf, L);           // fully grown (>350ms)
  int full = litCount(buf, L);

  TEST_ASSERT_TRUE_MESSAGE(early < mid, "arc should widen during grow-in");
  TEST_ASSERT_TRUE_MESSAGE(mid <= full, "arc reaches full width");
  // Settled: a lone session reads as a clear ARC with a wide gap (not a full-ring
  // global state - owner: "full circle makes no sense"). ~3/4 ring, gap visible.
  // Owner rule (2026-07-13, inverting the earlier arc-gap design): one session
  // fills the WHOLE ring - status reads from color+pattern, not arc length.
  TEST_ASSERT_EQUAL_MESSAGE(L, full, "single session fills the FULL ring");
  // And it's blue-dominant once settled.
  int blue = 0; for (int i = 0; i < L; ++i) if (buf[i].b >= buf[i].r) ++blue;
  TEST_ASSERT_TRUE(blue > L / 2);
}

static int brightestIdx(const RGB* f, int n) {
  int best = -1, bi = 0;
  for (int i = 0; i < n; ++i) { int s = f[i].r + f[i].g + f[i].b; if (s > best) { best = s; bi = i; } }
  return bi;
}
static long totalBright(const RGB* f, int n) {
  long t = 0; for (int i = 0; i < n; ++i) t += f[i].r + f[i].g + f[i].b; return t;
}

// Per-status MOTION (P1): Comet's bright head SLIDES, Breathe/Blink levels VARY over
// time. The Animator renders each Steady segment in its status's animation pattern.
static void test_status_patterns_animate() {
  Animator a = mk();
  a.born(1, 170, 0);
  a.frame(400, buf, L);  // settle to Steady

  a.setAnim(1, uint8_t(solide::ring::Anim::Comet), 100);
  a.frame(1000, buf, L);            int h1 = brightestIdx(buf, L);
  a.frame(1000 + 55 * 8, buf, L);   int h2 = brightestIdx(buf, L);
  TEST_ASSERT_TRUE_MESSAGE(h1 != h2, "comet head should slide over time");

  a.setAnim(1, uint8_t(solide::ring::Anim::Breathe), 100);
  a.frame(2000, buf, L);            long b1 = totalBright(buf, L);
  a.frame(2000 + 1300, buf, L);     long b2 = totalBright(buf, L);   // ~half period
  TEST_ASSERT_TRUE_MESSAGE(b1 != b2, "breathe level should vary over its period");

  a.setAnim(1, uint8_t(solide::ring::Anim::Blink), 100);
  a.frame(3000, buf, L);            long k1 = totalBright(buf, L);
  a.frame(3000 + 150, buf, L);      long k2 = totalBright(buf, L);   // half blink period
  TEST_ASSERT_TRUE_MESSAGE(k1 != k2, "blink should toggle on/off");
}

static void test_termination_collapses_then_frees_slot() {
  Animator a = mk();
  a.born(1, 170, 0);
  a.frame(400, buf, L);  // settle
  TEST_ASSERT_EQUAL(1, a.liveCount());

  a.terminated(1, 1000);
  a.frame(1010, buf, L);  int start = litCount(buf, L);
  a.frame(1200, buf, L);  int shrinking = litCount(buf, L);
  TEST_ASSERT_TRUE_MESSAGE(shrinking < start, "arc should contract while dying");

  a.frame(1400, buf, L);  // past kDieMs -> gone
  TEST_ASSERT_FALSE(anyLit(buf, L));
  TEST_ASSERT_EQUAL(0, a.liveCount());  // slot freed
}

static void test_hue_crossfade_is_gradual() {
  Animator a = mk();
  a.born(1, 0, 0);       // red
  a.frame(400, buf, L);  // settled red
  RGB red = buf[L / 2];
  TEST_ASSERT_TRUE(red.r > red.g);

  a.setHue(1, 85, 1000); // crossfade red -> green
  a.frame(1125, buf, L); // mid crossfade (~50%): both channels present
  RGB midv = buf[L / 2];
  a.frame(1300, buf, L); // done
  RGB green = buf[L / 2];
  TEST_ASSERT_TRUE_MESSAGE(green.g > green.r, "ends green");
  // Midpoint is between: greener than pure red, redder than pure green.
  TEST_ASSERT_TRUE(midv.g > red.g);
}

static void test_done_ripple_then_ember() {
  Animator a = mk();
  a.born(1, 170, 0);      // blue segment
  a.frame(400, buf, L);
  a.done(1, 1000);
  // Mid ripple: a bright head in the SEGMENT'S OWN hue (themed, P2.4 - was a
  // hardcoded green that flashed on every theme whenever anything finished).
  a.frame(1100, buf, L);
  int brightOwnHue = 0;
  for (int i = 0; i < L; ++i) if (buf[i].b > 150 && buf[i].b > buf[i].g) ++brightOwnHue;
  TEST_ASSERT_TRUE_MESSAGE(brightOwnHue > 0, "ripple head sweeps in the segment's own hue");
  a.frame(1600, buf, L);  // settled ember: dim, nothing bright
  TEST_ASSERT_TRUE(anyLit(buf, L));
  for (int i = 0; i < L; ++i) TEST_ASSERT_TRUE(buf[i].b <= 80);  // dim ember
}

// P2.4: a segment that finished (mid-ripple) and is pushed LIVE again drops the
// done sweep immediately - no stray finished-flash amid the working comet.
static void test_revive_cancels_midflight_ripple() {
  Animator a = mk();
  a.born(1, 170, 0);
  a.frame(400, buf, L);
  a.done(1, 1000);
  a.frame(1100, buf, L);            // mid ripple
  a.revive(1);                      // Done -> Running again (rapid sub-agent flip)
  a.setAnim(1, uint8_t(solide::ring::Anim::Comet), 100);
  a.frame(1150, buf, L);            // would still be rippling for ~350ms without revive
  // Steady+Comet now: the frame must contain the comet's bright blue head, and no
  // full-arc ember wash (the ripple's signature) - check a dim-uniform arc is gone.
  int bright = 0;
  for (int i = 0; i < L; ++i) if (buf[i].b > 150) ++bright;
  TEST_ASSERT_TRUE_MESSAGE(bright > 0, "comet resumes immediately after revive");
}

// (Removed test_brightness_cap_scales - the Animator no longer caps brightness:
//  that is now the driver's single global cap (leds::setBrightness), fixing the
//  double-scale that crushed Full posture to ~1.4%. The replacement contract -
//  "the Animator does NOT bake global brightness" - lives in test_ring_contract.)


// ---- contrast-aware dividers (owner 2026-08-09) -----------------------------
// A QUEUED sub-session's arc is the white sentinel (hue 255, Idle style). The
// dividers are dim WHITE, so a white arc used to sit between white dividers -
// an invisible split. A divider BORDERING a white arc must render DARK (a
// notch); dividers between two colored arcs keep the white treatment.
//
// Geometry (replicating solide::ring::layout for L=45, n=3, gap=1): three
// 14-LED arcs at 0-13 / 15-28 / 30-43, gap LEDs at 14, 29, 44. Gap 14 sits
// between segs A,B; gap 29 between B,C; gap 44 between C,A (wrap).
static void test_divider_next_to_a_white_arc_goes_dark() {
  Animator a = mk();
  a.born(1, 100, 0);   // A: colored (green-ish)
  a.born(2, 255, 0);   // B: WHITE - a queued sub-session
  a.born(3, 170, 0);   // C: colored (blue)
  for (int id = 1; id <= 3; ++id) a.setAnim(id, 1 /*Solid*/, 100 /*brightPct*/);
  a.frame(1000, buf, L);   // fully settled

  auto lit = [&](int i) { return buf[i].r || buf[i].g || buf[i].b; };
  // Gap 14 (A|B) and gap 29 (B|C) border the white arc -> DARK notches.
  TEST_ASSERT_FALSE_MESSAGE(lit(14), "divider before the white arc must be dark");
  TEST_ASSERT_FALSE_MESSAGE(lit(29), "divider after the white arc must be dark");
  // Gap 44 (C|A) is between two colored arcs -> stays the dim white divider.
  TEST_ASSERT_TRUE_MESSAGE(lit(44), "colored-colored divider must stay lit");
  TEST_ASSERT_TRUE_MESSAGE(buf[44].r == buf[44].g && buf[44].g == buf[44].b,
                           "divider is neutral (r==g==b)");
  // And the white arc itself is still lit (the notch, not the arc, is dark).
  TEST_ASSERT_TRUE_MESSAGE(lit(21), "the white arc's own LEDs stay lit");
}

// No white arc anywhere -> every divider keeps the owner-approved white
// treatment (the 2026-07-13 "count the dividers" contract is unchanged).
static void test_all_colored_arcs_keep_white_dividers() {
  Animator a = mk();
  a.born(1, 100, 0);
  a.born(2, 23, 0);
  a.born(3, 170, 0);
  for (int id = 1; id <= 3; ++id) a.setAnim(id, 1 /*Solid*/, 100 /*brightPct*/);
  a.frame(1000, buf, L);
  const int gaps[] = {14, 29, 44};
  for (int i : gaps) {
    TEST_ASSERT_TRUE_MESSAGE(buf[i].r || buf[i].g || buf[i].b, "divider must be lit");
    TEST_ASSERT_TRUE_MESSAGE(buf[i].r == buf[i].g && buf[i].g == buf[i].b,
                             "divider is neutral (r==g==b)");
  }
}


// The crossfade clause of whiteish(): a segment mid-crossfade FROM white must
// keep its bordering notches dark for the fade - deleting the fromHue==255
// clause turns this red (prism: it was unpinned).
static void test_divider_stays_dark_while_crossfading_from_white() {
  Animator a = mk();
  a.born(1, 100, 0);
  a.born(2, 255, 0);            // white (queued)
  a.born(3, 170, 0);
  for (int id = 1; id <= 3; ++id) a.setAnim(id, 1 /*Solid*/, 100 /*brightPct*/);
  a.frame(1000, buf, L);        // settled: gaps 14/29 dark
  a.setHue(2, 23, 1000);        // the queued sub starts: white -> amber crossfade
  a.frame(1050, buf, L);        // mid-crossfade (kCrossMs window)
  TEST_ASSERT_FALSE_MESSAGE(buf[14].r || buf[14].g || buf[14].b,
                            "notch must stay dark while fading FROM white");
  TEST_ASSERT_FALSE_MESSAGE(buf[29].r || buf[29].g || buf[29].b,
                            "notch must stay dark while fading FROM white");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_rainbow_cycles_hues_except_alert_and_white);
  RUN_TEST(test_birth_grows_arc_then_settles);
  RUN_TEST(test_status_patterns_animate);
  RUN_TEST(test_termination_collapses_then_frees_slot);
  RUN_TEST(test_hue_crossfade_is_gradual);
  RUN_TEST(test_done_ripple_then_ember);
  RUN_TEST(test_revive_cancels_midflight_ripple);
  RUN_TEST(test_divider_next_to_a_white_arc_goes_dark);
  RUN_TEST(test_all_colored_arcs_keep_white_dividers);
  RUN_TEST(test_divider_stays_dark_while_crossfading_from_white);
  return UNITY_END();
}
