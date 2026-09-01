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

// CUM-189/CUM-203: the per-board-model default is a single source both the fresh-boot
// path and the web "clear" fallback read, so they can never restore different
// orientations. Both shipping boards swap the axes (portrait-native under landscape)
// but differ on the invert - the per-kind split that stops a resistive Solide from
// mirroring one axis out of the box. Neither ever sets invertX (the 180 for an
// upside-down mount is orientTouch's job, applied on top, never folded in here).
static void test_board_default_per_kind_orientation() {
  // Capacitive (Freenove FT6336U): swap + invertY (bench-verified, CUM-189).
  const Cal cap = nimbus::touch::boardDefaultCal(nimbus::touch::TouchKind::Capacitive);
  TEST_ASSERT_TRUE(cap.swapXY);
  TEST_ASSERT_FALSE(cap.invertX);
  TEST_ASSERT_TRUE(cap.invertY);
  // Resistive (Solide S3 XPT2046): swap ONLY - the CUM-203 fix. A regression pin so
  // the `(void)kind` shared default (which mirrored one axis) can never come back.
  const Cal res = nimbus::touch::boardDefaultCal(nimbus::touch::TouchKind::Resistive);
  TEST_ASSERT_TRUE(res.swapXY);
  TEST_ASSERT_FALSE(res.invertX);
  TEST_ASSERT_FALSE(res.invertY);
  // The two kinds must not be identical - that would mean the seam never split.
  TEST_ASSERT_FALSE(cap == res);
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

// CUM-189: the first-run calibration POLICY, as a class over EVERY TouchKind. The
// deferred acceptance piece: a fresh resistive panel gets the guided step (its raw
// ADC span drifts per unit); a capacitive panel skips it entirely (it reports pixels,
// nothing per-unit to measure). This is the "calibration flow state machine" guard -
// a new TouchKind added without a first-run policy must FAIL here, host-side.
using nimbus::touch::FirstRunCal;
using nimbus::touch::firstRunCalPending;
using nimbus::touch::firstRunCalPolicy;
using nimbus::touch::TouchKind;

// The TEST-SIDE source of truth the policy must agree with. No default branch (Count
// falls through to a failing assert), so a newly added kind is a visible gap here as
// it is in firstRunCalPolicy - the CUM-228 "new kind ships without a policy" chokepoint.
static FirstRunCal expectedFirstRunFor(TouchKind kind) {
  switch (kind) {
    case TouchKind::Resistive:  return FirstRunCal::Calibrate;  // XPT2046 drifts per unit
    case TouchKind::Capacitive: return FirstRunCal::Skip;       // FT6336U reports pixels
    case TouchKind::Count:      break;                           // not a kind
  }
  TEST_FAIL_MESSAGE("TouchKind has no first-run calibration policy - a new touch class "
                    "must add one here AND in firstRunCalPolicy (CUM-189)");
  return FirstRunCal::Skip;
}

// Compile-time cardinality guard: adding a controller class breaks the build until the
// policy (firstRunCalPolicy) and this expectation both learn it. Mirrors the
// TouchKind::Count guard in test/test_fresh_device.
static_assert(static_cast<int>(TouchKind::Count) == 2,
              "A TouchKind was added/removed: extend firstRunCalPolicy() and "
              "expectedFirstRunFor() with its first-run calibration policy, then update "
              "this count (CUM-189 first-run step).");

// Property over EVERY kind: the policy matches, a FRESH board (no stored cal) pends iff
// the policy says Calibrate, and a board that ALREADY has a stored cal never re-offers.
static void test_first_run_policy_over_every_touchkind() {
  for (int i = 0; i < static_cast<int>(TouchKind::Count); ++i) {
    const TouchKind k = static_cast<TouchKind>(i);
    const FirstRunCal want = expectedFirstRunFor(k);
    TEST_ASSERT_EQUAL(int(want), int(firstRunCalPolicy(k)));
    // Fresh board (empty tchCal): pending exactly when the class calls for the step.
    TEST_ASSERT_EQUAL(want == FirstRunCal::Calibrate, firstRunCalPending(k, /*stored=*/false));
    // Already calibrated: NEVER re-offer, whatever the class - a one-time event.
    TEST_ASSERT_FALSE(firstRunCalPending(k, /*stored=*/true));
  }
}

// The two named kinds, pinned explicitly so the DoD reads off the test: capacitive
// SKIPS the step entirely; resistive gets it on a fresh board.
static void test_capacitive_skips_resistive_offers() {
  TEST_ASSERT_EQUAL(int(FirstRunCal::Skip), int(firstRunCalPolicy(TouchKind::Capacitive)));
  TEST_ASSERT_FALSE(firstRunCalPending(TouchKind::Capacitive, false));  // skipped even fresh

  TEST_ASSERT_EQUAL(int(FirstRunCal::Calibrate), int(firstRunCalPolicy(TouchKind::Resistive)));
  TEST_ASSERT_TRUE(firstRunCalPending(TouchKind::Resistive, false));    // offered when fresh
}

// "Resistive completes and persists": a fresh resistive board pends the step, the
// wizard drives to completion on realistic per-unit resistive raw readings, and the
// solved cal survives the round trip through the wire format tchCal stores - so the
// first-run flow leaves a persisted, re-parseable calibration (after which the board
// no longer pends). This is the end-to-end flow the seam runs, at the pure layer.
static void test_resistive_first_run_completes_and_persists() {
  TEST_ASSERT_TRUE(firstRunCalPending(TouchKind::Resistive, /*stored=*/false));

  CalWizard w;
  w.begin(320, 240, 24);
  // A real per-unit resistive panel: swapped axes (portrait-native mount), a drifted
  // span that is NOT the nominal default - exactly why the step exists.
  const nimbus::touch::RawSample raw[4] = {
      {310, 3720}, {310, 360}, {3650, 3720}, {3650, 360}};
  TEST_ASSERT_FALSE(w.done());
  for (int i = 0; i < 4; ++i) w.recordRaw(raw[i].x, raw[i].y);
  TEST_ASSERT_TRUE(w.done());

  Cal solved;
  TEST_ASSERT_TRUE(w.solve(solved));
  TEST_ASSERT_TRUE(solved.swapXY);   // this mount swaps
  // The persisted representation the seam writes to tchCal must re-parse identically.
  const std::string persisted = formatCal(solved);
  TEST_ASSERT_TRUE(persisted.size() > 0);
  Cal back;
  TEST_ASSERT_TRUE(parseCal(persisted, back));
  TEST_ASSERT_TRUE(solved == back);
  // Once persisted, the board is calibrated: first-run must not fire again.
  TEST_ASSERT_FALSE(firstRunCalPending(TouchKind::Resistive, /*stored=*/true));
}

// ---- CUM-245: the first-run GATE, the deliberate skip, and the pressure floor ----
//
// The gate is the fix for the fresh-resistive self-navigation bug: while a fresh
// resistive panel is uncalibrated, touch is routed to the calibration flow ONLY, never
// to UI navigation, and the gate opens the instant a cal is stored. These pin that
// routing as a class over EVERY TouchKind, plus the deliberate-skip recognizer and the
// uncalibrated pressure floor - the pure seams the device seam is built on.
using nimbus::touch::belowUncalFloor;
using nimbus::touch::kUncalPressureFloor;
using nimbus::touch::routeTouch;
using nimbus::touch::SkipHold;
using nimbus::touch::TouchRoute;

// The class rule: touch is GATED (never reaches navigation) exactly when a fresh board's
// class needs the guided step and no cal is stored yet - i.e. exactly firstRunCalPending.
// Asserted over EVERY kind, so a new touch class cannot ship reaching navigation while
// uncalibrated (nor gating a class that reports pixels and needs no cal).
static void test_route_gates_uncalibrated_resistive_only() {
  for (int i = 0; i < static_cast<int>(TouchKind::Count); ++i) {
    const TouchKind k = static_cast<TouchKind>(i);
    // Fresh (no stored cal): gated iff the class calls for the guided step.
    const bool wantGate = (expectedFirstRunFor(k) == FirstRunCal::Calibrate);
    TEST_ASSERT_EQUAL(wantGate ? int(TouchRoute::Gate) : int(TouchRoute::Navigate),
                      int(routeTouch(k, /*hasStoredCal=*/false)));
    // A stored cal ALWAYS opens the gate, whatever the class - navigation resumes.
    TEST_ASSERT_EQUAL(int(TouchRoute::Navigate), int(routeTouch(k, /*hasStoredCal=*/true)));
  }
}

// The two named kinds, read off explicitly: a fresh resistive panel is gated and a
// completed cal unlocks it; a capacitive panel is NEVER gated (fresh or not).
static void test_route_named_kinds() {
  TEST_ASSERT_EQUAL(int(TouchRoute::Gate),
                    int(routeTouch(TouchKind::Resistive, false)));   // fresh resistive: gated
  TEST_ASSERT_EQUAL(int(TouchRoute::Navigate),
                    int(routeTouch(TouchKind::Resistive, true)));    // calibrated: unlocked
  TEST_ASSERT_EQUAL(int(TouchRoute::Navigate),
                    int(routeTouch(TouchKind::Capacitive, false)));  // fresh capacitive: never gated
  TEST_ASSERT_EQUAL(int(TouchRoute::Navigate),
                    int(routeTouch(TouchKind::Capacitive, true)));
}

// The deliberate skip is a long, UNBROKEN press-and-hold. A tap (press then release) can
// never accumulate one, no matter how many taps.
static void test_skip_requires_unbroken_hold() {
  SkipHold s;
  // A held press that has not reached the threshold does not fire.
  TEST_ASSERT_FALSE(s.update(true, 1000));            // press begins
  TEST_ASSERT_FALSE(s.update(true, 1000 + SkipHold::kHoldMs - 1));  // 1 ms short
  TEST_ASSERT_TRUE(s.holding());
  // Cross the threshold on a continuous hold: confirmed.
  TEST_ASSERT_TRUE(s.update(true, 1000 + SkipHold::kHoldMs));
}

static void test_skip_release_resets_no_skip() {
  SkipHold s;
  // A stream of quick taps, each far longer apart than the hold window, never skips
  // because each release resets the accumulator.
  for (uint32_t t = 0; t < 10; ++t) {
    TEST_ASSERT_FALSE(s.update(true, t * 10000));         // press
    TEST_ASSERT_FALSE(s.update(false, t * 10000 + 50));   // release 50 ms later
    TEST_ASSERT_FALSE(s.holding());
  }
  // Even a long hold that is interrupted just before the threshold must not fire: the
  // release zeroes it and the next press starts a fresh window.
  TEST_ASSERT_FALSE(s.update(true, 100000));
  TEST_ASSERT_FALSE(s.update(true, 100000 + SkipHold::kHoldMs - 1));
  TEST_ASSERT_FALSE(s.update(false, 100000 + SkipHold::kHoldMs));   // released: reset
  TEST_ASSERT_FALSE(s.update(true, 200000));                        // new press
  TEST_ASSERT_FALSE(s.update(true, 200000 + 10));                   // only 10 ms in
}

// The countdown the on-screen label reads off: full when idle, counting down under a
// hold, and never showing 0 while still waiting.
static void test_skip_countdown() {
  SkipHold s;
  const int full = int(SkipHold::kHoldMs / 1000);
  TEST_ASSERT_EQUAL_UINT32(0, s.heldMs(12345));          // idle: nothing held
  TEST_ASSERT_EQUAL(full, s.secondsLeft(12345));         // idle: full countdown
  s.update(true, 1000);                                  // hold begins
  TEST_ASSERT_EQUAL_UINT32(0, s.heldMs(1000));
  TEST_ASSERT_EQUAL(full, s.secondsLeft(1000));          // 3 s left at t0
  s.update(true, 1000 + 1500);
  TEST_ASSERT_EQUAL_UINT32(1500, s.heldMs(1000 + 1500)); // half way
  TEST_ASSERT_EQUAL(2, s.secondsLeft(1000 + 1500));      // ceil(1.5) = 2 left
  // heldMs never exceeds the window even if the finger stays down past it.
  s.update(true, 1000 + SkipHold::kHoldMs + 5000);
  TEST_ASSERT_EQUAL_UINT32(SkipHold::kHoldMs, s.heldMs(1000 + SkipHold::kHoldMs + 5000));
}

// The uncalibrated pressure floor: it rejects a light/drifting read ONLY while
// uncalibrated AND resistive; a calibrated panel and a capacitive one are never floored.
static void test_uncal_pressure_floor() {
  // Below the floor while fresh + resistive: rejected (a floating panel cannot stream).
  TEST_ASSERT_TRUE(belowUncalFloor(true, true, kUncalPressureFloor - 1));
  TEST_ASSERT_TRUE(belowUncalFloor(true, true, 0));
  // At or above the floor: a deliberate press passes.
  TEST_ASSERT_FALSE(belowUncalFloor(true, true, kUncalPressureFloor));
  TEST_ASSERT_FALSE(belowUncalFloor(true, true, kUncalPressureFloor + 500));
  // Calibrated resistive: never floored, even a light touch (driver's own threshold owns).
  TEST_ASSERT_FALSE(belowUncalFloor(false, true, 0));
  // Capacitive: never floored (it reports a fixed pressure flag, not a resistive Z).
  TEST_ASSERT_FALSE(belowUncalFloor(true, false, 0));
  // The floor must be strictly firmer than the driver's own low noise-floor threshold
  // (raw Z ~350), or it would add nothing over the driver.
  TEST_ASSERT_TRUE(kUncalPressureFloor > 350);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_route_gates_uncalibrated_resistive_only);
  RUN_TEST(test_route_named_kinds);
  RUN_TEST(test_skip_requires_unbroken_hold);
  RUN_TEST(test_skip_release_resets_no_skip);
  RUN_TEST(test_skip_countdown);
  RUN_TEST(test_uncal_pressure_floor);
  RUN_TEST(test_first_run_policy_over_every_touchkind);
  RUN_TEST(test_capacitive_skips_resistive_offers);
  RUN_TEST(test_resistive_first_run_completes_and_persists);
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
  RUN_TEST(test_board_default_per_kind_orientation);
  RUN_TEST(test_board_default_round_trips_through_parse);
  return UNITY_END();
}
