#include <unity.h>

#include "nimbus/touch_cal.h"

// A resistive panel's calibration is MEASURED per unit and then stored, so the
// parse sits between a human typing corner values and the driver that maps
// every tap. A silently-wrong parse here is indistinguishable from broken
// hardware - hence the emphasis on rejecting rather than coercing.

using nimbus::touch::Cal;
using nimbus::touch::formatCal;
using nimbus::touch::orientTouch;
using nimbus::touch::parseCal;
using nimbus::touch::Point;

void setUp() {}
void tearDown() {}

static void test_parses_four_fields() {
  Cal c;
  TEST_ASSERT_TRUE(parseCal("200,3900,240,3850", c));
  TEST_ASSERT_EQUAL_UINT16(200, c.minX);
  TEST_ASSERT_EQUAL_UINT16(3900, c.maxX);
  TEST_ASSERT_EQUAL_UINT16(240, c.minY);
  TEST_ASSERT_EQUAL_UINT16(3850, c.maxY);
  TEST_ASSERT_FALSE(c.swapXY);
  TEST_ASSERT_FALSE(c.invertX);
  TEST_ASSERT_FALSE(c.invertY);
}

static void test_parses_flags() {
  Cal c;
  TEST_ASSERT_TRUE(parseCal("200,3900,240,3850,7", c));
  TEST_ASSERT_TRUE(c.swapXY);
  TEST_ASSERT_TRUE(c.invertX);
  TEST_ASSERT_TRUE(c.invertY);

  TEST_ASSERT_TRUE(parseCal("200,3900,240,3850,4", c));
  TEST_ASSERT_FALSE(c.swapXY);
  TEST_ASSERT_FALSE(c.invertX);
  TEST_ASSERT_TRUE(c.invertY);
}

static void test_round_trips() {
  Cal a;
  a.minX = 310; a.maxX = 3700; a.minY = 280; a.maxY = 3820;
  a.swapXY = true; a.invertY = true;
  Cal b;
  TEST_ASSERT_TRUE(parseCal(formatCal(a), b));
  TEST_ASSERT_TRUE(a == b);
}

// A partially-applied calibration is WORSE than the default, because it looks
// deliberate. Every malformed input must leave the caller's value untouched.
static void test_rejects_malformed_without_mutating() {
  const Cal original;                      // defaults
  const char* bad[] = {
      "",                       // empty
      "200,3900,240",           // too few
      "200,3900,240,3850,1,2",  // too many
      "200,,240,3850",          // empty field
      "200,3900,240,abc",       // non-numeric
      "200,3900,240,-50",       // negative
      "-1,3900,240,3850",       // negative
      "200,3900,240,9999",      // beyond the 12-bit ADC
      "3900,200,240,3850",      // min > max on X
      "200,3900,3850,240",      // min > max on Y
      "200,200,240,3850",       // zero span (would divide by zero)
      "200,3900,240,3850,8",    // flags out of range
  };
  for (const char* s : bad) {
    Cal c = original;
    TEST_ASSERT_FALSE_MESSAGE(parseCal(s, c), s);
    TEST_ASSERT_TRUE_MESSAGE(c == original, s);   // untouched
  }
}

static void test_accepts_boundaries() {
  Cal c;
  TEST_ASSERT_TRUE(parseCal("0,4095,0,4095,0", c));
  TEST_ASSERT_EQUAL_UINT16(0, c.minX);
  TEST_ASSERT_EQUAL_UINT16(4095, c.maxX);
}

static void test_tolerates_spaces() {
  Cal c;
  TEST_ASSERT_TRUE(parseCal(" 200 , 3900 , 240 , 3850 ", c));
  TEST_ASSERT_EQUAL_UINT16(3900, c.maxX);
}

// ---- orientTouch: the single-source 180 reconciliation (CUM-160) ------------
static const int16_t W = 320, H = 240;

static void test_unflipped_is_identity() {
  Point p = orientTouch(Point{10, 20, true}, /*displayFlipped=*/false, W, H);
  TEST_ASSERT_EQUAL_INT16(10, p.x);
  TEST_ASSERT_EQUAL_INT16(20, p.y);
  TEST_ASSERT_TRUE(p.down);
}

static void test_flipped_mirrors_both_axes() {
  Point p = orientTouch(Point{10, 20, true}, /*displayFlipped=*/true, W, H);
  TEST_ASSERT_EQUAL_INT16(W - 1 - 10, p.x);   // 309
  TEST_ASSERT_EQUAL_INT16(H - 1 - 20, p.y);   // 219
}

static void test_flip_is_involutive_applied_once() {
  // Applying the flip twice returns the original: proof the transform is the 180
  // and nothing else, so a double-source (cal + this) would land 180 out.
  Point once = orientTouch(Point{47, 96, true}, true, W, H);
  Point twice = orientTouch(once, true, W, H);
  TEST_ASSERT_EQUAL_INT16(47, twice.x);
  TEST_ASSERT_EQUAL_INT16(96, twice.y);
}

static void test_corners_map_to_opposite_corners_when_flipped() {
  Point tl = orientTouch(Point{0, 0, true}, true, W, H);
  TEST_ASSERT_EQUAL_INT16(W - 1, tl.x);
  TEST_ASSERT_EQUAL_INT16(H - 1, tl.y);
  Point br = orientTouch(Point{W - 1, H - 1, true}, true, W, H);
  TEST_ASSERT_EQUAL_INT16(0, br.x);
  TEST_ASSERT_EQUAL_INT16(0, br.y);
}

static void test_not_down_passes_through_even_when_flipped() {
  // No live coordinate to mirror; leave the sentinel intact.
  Point p = orientTouch(Point{-1, -1, false}, true, W, H);
  TEST_ASSERT_EQUAL_INT16(-1, p.x);
  TEST_ASSERT_EQUAL_INT16(-1, p.y);
  TEST_ASSERT_FALSE(p.down);
}

// CUM-189: the per-board-model default is a single source both the fresh-boot path
// and the web "clear" fallback read, so they can never restore different
// orientations. Both shipping boards mount portrait-native under landscape.
static void test_board_default_is_swap_and_invert_y() {
  for (nimbus::touch::TouchKind k :
       {nimbus::touch::TouchKind::Resistive, nimbus::touch::TouchKind::Capacitive}) {
    const Cal d = nimbus::touch::boardDefaultCal(k);
    TEST_ASSERT_TRUE(d.swapXY);
    TEST_ASSERT_FALSE(d.invertX);
    TEST_ASSERT_TRUE(d.invertY);
  }
}

// The default must be a valid, self-consistent calibration (a non-empty span that
// parseCal would accept), so pushing it never divides by zero on a resistive panel.
static void test_board_default_round_trips_through_parse() {
  const Cal d = nimbus::touch::boardDefaultCal(nimbus::touch::TouchKind::Resistive);
  TEST_ASSERT_TRUE(d.minX < d.maxX);
  TEST_ASSERT_TRUE(d.minY < d.maxY);
  Cal back;
  TEST_ASSERT_TRUE(parseCal(formatCal(d), back));
  TEST_ASSERT_TRUE(d == back);
}

// CUM-189: solveCornerCal derives the axis mapping from four corner presses. Order
// is [tl, tr, bl, br]. These vectors cover the three panel orientations the wizard
// (tools/tcal_wizard.py derive) handles, plus the degenerate reject.
using nimbus::touch::RawSample;
using nimbus::touch::solveCornerCal;

static void test_solve_identity_panel() {
  // rawX == screenX, rawY == screenY: no swap, no invert.
  const RawSample c[4] = {{200, 200}, {3900, 200}, {200, 3900}, {3900, 3900}};
  Cal r;
  TEST_ASSERT_TRUE(solveCornerCal(c, r));
  TEST_ASSERT_FALSE(r.swapXY);
  TEST_ASSERT_FALSE(r.invertX);
  TEST_ASSERT_FALSE(r.invertY);
  TEST_ASSERT_EQUAL_UINT16(200, r.minX);
  TEST_ASSERT_EQUAL_UINT16(3900, r.maxX);
}

static void test_solve_swapped_panel() {
  // screen X rides raw Y, screen Y rides raw X (a 90-degree mount).
  const RawSample c[4] = {{200, 200}, {200, 3900}, {3900, 200}, {3900, 3900}};
  Cal r;
  TEST_ASSERT_TRUE(solveCornerCal(c, r));
  TEST_ASSERT_TRUE(r.swapXY);
  TEST_ASSERT_FALSE(r.invertX);
  TEST_ASSERT_FALSE(r.invertY);
}

static void test_solve_inverted_x() {
  // rawX DECREASES as screen X grows -> invertX.
  const RawSample c[4] = {{3900, 200}, {200, 200}, {3900, 3900}, {200, 3900}};
  Cal r;
  TEST_ASSERT_TRUE(solveCornerCal(c, r));
  TEST_ASSERT_FALSE(r.swapXY);
  TEST_ASSERT_TRUE(r.invertX);
  TEST_ASSERT_FALSE(r.invertY);
  TEST_ASSERT_EQUAL_UINT16(200, r.minX);   // min/max stay ordered even when inverted
  TEST_ASSERT_EQUAL_UINT16(3900, r.maxX);
}

// A solved calibration must survive a round trip through the wire format the NVS
// key and the web field use.
static void test_solved_cal_round_trips() {
  const RawSample c[4] = {{200, 200}, {3900, 200}, {200, 3900}, {3900, 3900}};
  Cal r;
  TEST_ASSERT_TRUE(solveCornerCal(c, r));
  Cal back;
  TEST_ASSERT_TRUE(parseCal(formatCal(r), back));
  TEST_ASSERT_TRUE(r == back);
}

// Presses all landing in one spot (or a shorted line) must be REJECTED, leaving the
// previous calibration untouched - a half-applied cal looks deliberate and is worse.
static void test_solve_rejects_degenerate_presses() {
  const RawSample c[4] = {{2000, 2000}, {2010, 1995}, {1995, 2008}, {2005, 2002}};
  Cal r;
  r.minX = 111;   // sentinel; must be left untouched on reject
  TEST_ASSERT_FALSE(solveCornerCal(c, r));
  TEST_ASSERT_EQUAL_UINT16(111, r.minX);
}

// CUM-189: the on-device wizard sequences four corner targets and hands the raw
// presses to the solver. This drives it exactly as the device loop would.
using nimbus::touch::CalWizard;

static void test_wizard_targets_are_the_four_corners_inset() {
  CalWizard w;
  w.begin(320, 240, 24);
  TEST_ASSERT_EQUAL(4, w.count());
  TEST_ASSERT_FALSE(w.done());
  // [tl, tr, bl, br]
  TEST_ASSERT_EQUAL_INT16(24, w.targetX(0));   TEST_ASSERT_EQUAL_INT16(24, w.targetY(0));
  TEST_ASSERT_EQUAL_INT16(295, w.targetX(1));  TEST_ASSERT_EQUAL_INT16(24, w.targetY(1));
  TEST_ASSERT_EQUAL_INT16(24, w.targetX(2));   TEST_ASSERT_EQUAL_INT16(215, w.targetY(2));
  TEST_ASSERT_EQUAL_INT16(295, w.targetX(3));  TEST_ASSERT_EQUAL_INT16(215, w.targetY(3));
}

static void test_wizard_advances_and_solves() {
  CalWizard w;
  w.begin(320, 240, 24);
  // Feed raw readings for an identity panel; each recordRaw advances the step.
  const nimbus::touch::RawSample raw[4] = {
      {200, 200}, {3900, 200}, {200, 3900}, {3900, 3900}};
  for (int i = 0; i < 4; ++i) {
    TEST_ASSERT_EQUAL(i, w.step());
    const bool last = w.recordRaw(raw[i].x, raw[i].y);
    TEST_ASSERT_EQUAL(i == 3, last);
  }
  TEST_ASSERT_TRUE(w.done());
  Cal c;
  TEST_ASSERT_TRUE(w.solve(c));
  TEST_ASSERT_FALSE(c.swapXY);
  TEST_ASSERT_EQUAL_UINT16(200, c.minX);
  TEST_ASSERT_EQUAL_UINT16(3900, c.maxX);
}

static void test_wizard_solve_before_done_fails() {
  CalWizard w;
  w.begin(320, 240);
  w.recordRaw(200, 200);   // only one corner
  Cal c;
  TEST_ASSERT_FALSE(w.solve(c));   // not done yet
}

static void test_wizard_reset_restarts() {
  CalWizard w;
  w.begin(320, 240);
  w.recordRaw(200, 200);
  w.recordRaw(3900, 200);
  TEST_ASSERT_EQUAL(2, w.step());
  w.reset();
  TEST_ASSERT_EQUAL(0, w.step());
  TEST_ASSERT_FALSE(w.done());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_wizard_targets_are_the_four_corners_inset);
  RUN_TEST(test_wizard_advances_and_solves);
  RUN_TEST(test_wizard_solve_before_done_fails);
  RUN_TEST(test_wizard_reset_restarts);
  RUN_TEST(test_solve_identity_panel);
  RUN_TEST(test_solve_swapped_panel);
  RUN_TEST(test_solve_inverted_x);
  RUN_TEST(test_solved_cal_round_trips);
  RUN_TEST(test_solve_rejects_degenerate_presses);
  RUN_TEST(test_parses_four_fields);
  RUN_TEST(test_parses_flags);
  RUN_TEST(test_round_trips);
  RUN_TEST(test_rejects_malformed_without_mutating);
  RUN_TEST(test_accepts_boundaries);
  RUN_TEST(test_tolerates_spaces);
  RUN_TEST(test_unflipped_is_identity);
  RUN_TEST(test_flipped_mirrors_both_axes);
  RUN_TEST(test_flip_is_involutive_applied_once);
  RUN_TEST(test_corners_map_to_opposite_corners_when_flipped);
  RUN_TEST(test_not_down_passes_through_even_when_flipped);
  RUN_TEST(test_board_default_is_swap_and_invert_y);
  RUN_TEST(test_board_default_round_trips_through_parse);
  return UNITY_END();
}
