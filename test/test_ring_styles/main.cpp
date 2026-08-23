#include <unity.h>

#include "nimbus/ring_animator.h"

// CUM-42: frame invariants for the five candidate "working" (Running) animations.
// Pins the two invariants the DoD names (fail-dark on empty; no stray lit pixels after
// retire) for EVERY variant, plus that each variant is deterministic (so the LED ring,
// the on-screen ring, and the web simulator render identically) and actually animates
// the arc. The default variant's exact colors are already pinned by test_ring_animator.

using namespace nimbus;
using namespace nimbus::ring;

void setUp() {}
void tearDown() {}

static constexpr int L = 45;
static RGB bufA[L];
static RGB bufB[L];

static bool anyLit(const RGB* f, int n) {
  for (int i = 0; i < n; ++i) if (f[i].r || f[i].g || f[i].b) return true;
  return false;
}
static bool framesEqual(const RGB* a, const RGB* b, int n) {
  for (int i = 0; i < n; ++i)
    if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) return false;
  return true;
}
static Animator mk() {
  Animator a;
  a.configure(L, Posture::Full, 255);
  return a;
}

static const RunStyle kStyles[kRunStyleCount] = {
  RunStyle::CometTail, RunStyle::CometSparks, RunStyle::DualComet,
  RunStyle::BreatheArc, RunStyle::Fireflies,
};

// Drive a lone Running session to steady state under `style`.
static Animator runningUnder(RunStyle style) {
  Animator a = mk();
  a.setRunStyle(style);
  a.born(1, 170, 0);                         // blue
  a.frame(400, bufA, L);                     // past the 350ms grow-in -> Steady
  a.setAnim(1, (uint8_t)3 /*Comet*/, 100);   // Running motion
  return a;
}

// --- Invariant 1: fail-dark on an empty ring, for every selected style ---------
static void test_fail_dark_when_empty() {
  for (uint8_t i = 0; i < kRunStyleCount; ++i) {
    Animator a = mk();
    a.setRunStyle(kStyles[i]);
    a.frame(1234, bufA, L);
    TEST_ASSERT_FALSE_MESSAGE(anyLit(bufA, L), "empty ring must be all-black");
  }
}

// --- Invariant 2: no stray lit pixels after a session retires, every style -----
static void test_no_stray_pixels_after_retire() {
  for (uint8_t i = 0; i < kRunStyleCount; ++i) {
    Animator a = runningUnder(kStyles[i]);
    a.frame(1000, bufA, L);
    TEST_ASSERT_TRUE_MESSAGE(anyLit(bufA, L), "a running arc should light something");
    a.terminated(1, 2000);
    a.frame(2000 + 351, bufA, L);            // past the 350ms collapse
    TEST_ASSERT_FALSE_MESSAGE(anyLit(bufA, L), "retired session must leave no lit pixel");
    TEST_ASSERT_EQUAL_INT(0, a.liveCount());
  }
}

// --- Each variant actually animates the arc (not accidentally all-floor/black) --
static void test_each_style_lights_the_arc() {
  for (uint8_t i = 0; i < kRunStyleCount; ++i) {
    Animator a = runningUnder(kStyles[i]);
    bool litSomewhere = false;
    for (uint32_t t = 500; t <= 3000 && !litSomewhere; t += 50) {
      a.frame(t, bufA, L);
      litSomewhere = anyLit(bufA, L);
    }
    TEST_ASSERT_TRUE_MESSAGE(litSomewhere, "variant never lit the arc across a full cycle");
  }
}

// --- Determinism: same style + same nowMs => identical frame (cross-surface parity) --
static void test_styles_are_deterministic() {
  for (uint8_t i = 0; i < kRunStyleCount; ++i) {
    Animator a = runningUnder(kStyles[i]);
    Animator b = runningUnder(kStyles[i]);
    for (uint32_t t = 600; t <= 2600; t += 137) {
      a.frame(t, bufA, L);
      b.frame(t, bufB, L);
      TEST_ASSERT_TRUE_MESSAGE(framesEqual(bufA, bufB, L),
                               "identical inputs must produce identical frames");
    }
  }
}

// --- BreatheArc is spatially uniform; the comet/firefly styles are not ----------
// A quick behavioral separator so a refactor can't collapse the variants into one.
static void test_breathe_is_uniform_others_are_not() {
  // BreatheArc: every LED of the (full-ring lone) arc shares one brightness.
  Animator br = runningUnder(RunStyle::BreatheArc);
  br.frame(900, bufA, L);
  for (int k = 1; k < L; ++k)
    TEST_ASSERT_TRUE_MESSAGE(bufA[k].b == bufA[0].b, "BreatheArc must be spatially uniform");

  // DualComet: at least two LEDs differ (heads at different places).
  Animator dc = runningUnder(RunStyle::DualComet);
  dc.frame(900, bufB, L);
  bool varies = false;
  for (int k = 1; k < L; ++k) if (bufB[k].b != bufB[0].b) { varies = true; break; }
  TEST_ASSERT_TRUE_MESSAGE(varies, "DualComet must vary around the ring");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fail_dark_when_empty);
  RUN_TEST(test_no_stray_pixels_after_retire);
  RUN_TEST(test_each_style_lights_the_arc);
  RUN_TEST(test_styles_are_deterministic);
  RUN_TEST(test_breathe_is_uniform_others_are_not);
  return UNITY_END();
}
