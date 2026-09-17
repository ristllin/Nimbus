// test_panel_read_policy - the shared-MISO panel-read gate that fixes the classic
// (resistive) touch regression (CUM-392).
//
// The regression: v4.5.0 ADDED a render-independent panel RDDST poll
// (pollControllerLiveness, ~2 s) on top of the per-push healthy() read v4.4.6
// already made. On solide_s3 the ILI9341 and the XPT2046 share MISO, so a panel
// read drives the panel SDO onto the line touch reports on; on a module that does
// not release SDO every touch channel pins mid-scale (X ~2100, Y ~2100, z ~4237).
// freenove_s3 is capacitive on a separate I2C bus, so the poll is inert there -
// which is why it only ever got validated on a Freenove.
//
// The fix (per the orchestrator's binding ruling) restores v4.4.6's read SET: it
// gates OFF only the new render-independent poll on a shared-MISO resistive board
// and KEEPS the per-push read, so it is v4.4.6 parity (not zero reads) and CUM-388
// still latches. panelReadAllowed() here is that gate decision for the poll.
//
// This suite guards the CLASS, not the instance (AGENTS.md "test the class"):
//   1. It iterates EVERY compiled board definition and asserts that ANY board with
//      a resistive controller on a shared MISO has the independent poll gated OFF by
//      DEFAULT. A future board added with that hazard and no guard makes the class
//      case below FAIL - it is a synthetic board the gate must already cover by
//      construction, so a name-based or per-board special-case fix would not pass.
//   2. It pins the two real boards' verdicts (solide_s3 gated, freenove_s3 not).
//   3. It pins the PROBES override semantics used for the on-bench A/B.
//
// This is NOT the tautological pin the earlier lane shipped: it does not restate
// byte-identical header values, it asserts the read-gate DECISION FUNCTION the
// firmware consults (tft_out.cpp panelReadsAllowed -> boardPanelReadAllowed ->
// panelReadAllowed), anchored to concrete per-board expectations, so a predicate
// narrowed to a name-match or a board map mis-marked safe fails here. What it
// CANNOT check on the host is the wiring - that pollControllerLiveness actually
// consults panelReadsAllowed() and that the per-push read still feeds the verdict;
// tft_out is device code (not in the native build), so that half is covered by the
// device build plus the on-bench PROBES A/B.
#include <unity.h>

#include "nimbus/display/panel_read_policy.h"
#include "solide/boards/board_freenove_s3.h"
#include "solide/boards/board_solide_s3.h"

using nimbus::display::PanelReadOverride;
using nimbus::display::panelReadAllowed;
using nimbus::display::panelReadbackSafe;
using nimbus::display::sharedMisoResistiveTouch;

void setUp() {}
void tearDown() {}

// Extract the three facts the policy needs from a board map, exactly as the device
// binding (include/nimbus_board_touch_bus.h) does from solide::board().
struct Facts {
  bool resistive;
  int  miso;
  int  tcs;
};
static Facts factsOf(const solide::Board& b) {
  return Facts{b.touchKind == solide::TouchKind::ResistiveSpi, b.tft.miso, b.tft.tcs};
}
static bool gatedByDefault(const Facts& f) {
  // The firmware forbids the read when panelReadAllowed(..., Default) is false.
  return !panelReadAllowed(f.resistive, f.miso, f.tcs, PanelReadOverride::Default);
}

// ---- 1. The class rule over every compiled board -------------------------------
//
// The registry lists every board definition. A NEW board header MUST be added here
// (the same convention test_fresh_device uses), and the moment it is, this test
// asserts the class invariant on it automatically.
struct BoardCase { const char* name; const solide::Board* board; };
static const BoardCase kBoards[] = {
    {"solide_s3", &solide::kBoardSolideS3},
    {"freenove_s3", &solide::kBoardFreenoveS3},
};

// The two real boards, pinned to CONCRETE verdicts (not the predicate restated
// against itself). solide_s3 MUST be gated and freenove_s3 MUST NOT be - so a board
// map mis-defined as safe (e.g. touchKind flipped, or miso cleared) fails HERE, and
// the invariant below (gated IFF hazard) is checked against absolute expectations
// rather than the same function on both sides.
static void test_real_boards_pinned() {
  const Facts solide = factsOf(solide::kBoardSolideS3);
  TEST_ASSERT_TRUE_MESSAGE(sharedMisoResistiveTouch(solide.resistive, solide.miso,
                                                    solide.tcs),
                           "solide_s3 must carry the shared-MISO hazard");
  TEST_ASSERT_TRUE_MESSAGE(gatedByDefault(solide), "solide_s3 must be gated OFF");

  const Facts free = factsOf(solide::kBoardFreenoveS3);
  TEST_ASSERT_FALSE_MESSAGE(sharedMisoResistiveTouch(free.resistive, free.miso,
                                                     free.tcs),
                            "freenove_s3 must NOT carry the hazard");
  TEST_ASSERT_FALSE_MESSAGE(gatedByDefault(free), "freenove_s3 must NOT be gated");
}

// The class rule over every compiled board: gated OFF by default IFF it carries the
// shared-MISO resistive hazard. A new board header added to kBoards is covered the
// moment it is listed. (This is the class check; test_real_boards_pinned above is
// what makes it non-tautological by anchoring the two real maps to absolutes.)
static void test_every_board_obeys_the_class_rule() {
  for (const auto& bc : kBoards) {
    const Facts f = factsOf(*bc.board);
    const bool hazard = sharedMisoResistiveTouch(f.resistive, f.miso, f.tcs);
    TEST_ASSERT_EQUAL_MESSAGE(hazard, gatedByDefault(f), bc.name);
    TEST_ASSERT_EQUAL_MESSAGE(!hazard, panelReadbackSafe(f.resistive, f.miso, f.tcs),
                              bc.name);
  }
}

// A future resistive board on a shared MISO (a hypothetical resistive CYD) MUST be
// gated off by construction - no code change, no allowlist entry. If the predicate
// were ever narrowed to name-match solide_s3, this synthetic board would slip
// through and this test would fail. That is the class guard the ticket demands.
static void test_future_resistive_shared_miso_board_is_gated() {
  solide::Board future = solide::kBoardSolideS3;   // start from a real map...
  future.name = "future-resistive-cyd";
  future.tft.miso = 7;                              // ...a different but valid MISO
  future.tft.tcs = 9;                               // ...a different but valid touch CS
  future.touchKind = solide::TouchKind::ResistiveSpi;
  const Facts f = factsOf(future);
  TEST_ASSERT_TRUE(sharedMisoResistiveTouch(f.resistive, f.miso, f.tcs));
  TEST_ASSERT_TRUE(gatedByDefault(f));   // the read path must refuse it
}

// A capacitive board (or any board whose panel is write-only, miso < 0, or which
// has no SPI touch, tcs < 0) is NEVER gated: the fix must be provably inert there.
static void test_capacitive_and_separate_bus_never_gated() {
  const Facts cap = factsOf(solide::kBoardFreenoveS3);
  TEST_ASSERT_FALSE(sharedMisoResistiveTouch(cap.resistive, cap.miso, cap.tcs));
  TEST_ASSERT_FALSE(gatedByDefault(cap));

  // Resistive controller but the panel is write-only (no readback line): a panel
  // read cannot happen, so there is nothing to gate.
  TEST_ASSERT_FALSE(sharedMisoResistiveTouch(/*resistive=*/true, /*miso=*/-1, /*tcs=*/48));
  // No SPI touch fitted (tcs < 0): no resistive controller on the line.
  TEST_ASSERT_FALSE(sharedMisoResistiveTouch(/*resistive=*/true, /*miso=*/1, /*tcs=*/-1));
}

// ---- 3. The PROBES override used for the on-glass A/B ---------------------------
//
// ForceOn re-enables the reads on the hazardous board (to REPRODUCE the fault on
// one image); ForceOff silences them everywhere (clean baseline); Default is the
// capability gate. The safe board is unaffected by ForceOn because it never had a
// contention problem, and is silenced by ForceOff like any other.
static void test_override_semantics() {
  const Facts hazard = factsOf(solide::kBoardSolideS3);
  const Facts safe = factsOf(solide::kBoardFreenoveS3);

  // Default = capability gate.
  TEST_ASSERT_FALSE(panelReadAllowed(hazard.resistive, hazard.miso, hazard.tcs,
                                     PanelReadOverride::Default));
  TEST_ASSERT_TRUE(panelReadAllowed(safe.resistive, safe.miso, safe.tcs,
                                    PanelReadOverride::Default));
  // ForceOn = reproduce: reads run even on the hazardous board.
  TEST_ASSERT_TRUE(panelReadAllowed(hazard.resistive, hazard.miso, hazard.tcs,
                                    PanelReadOverride::ForceOn));
  // ForceOff = clean baseline: no reads anywhere.
  TEST_ASSERT_FALSE(panelReadAllowed(hazard.resistive, hazard.miso, hazard.tcs,
                                     PanelReadOverride::ForceOff));
  TEST_ASSERT_FALSE(panelReadAllowed(safe.resistive, safe.miso, safe.tcs,
                                     PanelReadOverride::ForceOff));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_real_boards_pinned);
  RUN_TEST(test_every_board_obeys_the_class_rule);
  RUN_TEST(test_future_resistive_shared_miso_board_is_gated);
  RUN_TEST(test_capacitive_and_separate_bus_never_gated);
  RUN_TEST(test_override_semantics);
  return UNITY_END();
}
