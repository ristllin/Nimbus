#include <unity.h>

#include <cstdint>

// Panel-health compare contract (CUM-188 thrash fix + CUM-231 mask-miss fix).
//
// solide::display_tft::healthy() reads the ILI9341 RDDST status register (0x09)
// and compares its top byte against the MADCTL we wrote (`madctlFor(g_flip)`, with
// `madctlFor(flip) = flip ? 0xE8 : 0x28`). Measured on hardware: RDDST's top byte
// mirrors MADCTL's FIXED bits (BGR / MV / refresh order) but reports the MY/MX
// 180-flip bits (0xC0) as their power-on 0 regardless of the flip we write; a
// healthy panel reads 0x28 in BOTH orientations, and a reset reverts it to 0x00.
//
// Two regressions this suite pins, so neither can silently come back:
//   * v0.7.0 (full-byte compare, 0xFE): a flipped panel (0xE8 expected) read
//     UNHEALTHY forever against the invariant 0x28 readback and thrashed the
//     watchdog (CUM-188).
//   * v0.7.1 (mask 0xC0 out of BOTH sides, 0x3E): stopped the thrash but dropped
//     fault detection - a partial state loss that raises MY/MX in RDDST (got=0xE8)
//     while the fixed bits stay correct read HEALTHY (CUM-231 white-screen leg).
//
// The v0.7.2 contract compares the full byte minus the refresh scan-toggle bit0
// against madctlFor(g_flip) with the flip bits CLEARED (the expected RDDST
// readback, 0x28 for both flips) - flip-aware, not a mask. It keeps the no-thrash
// property AND catches got=0xE8. The device fn is SPI-bound and not host-linkable;
// this mirrors its exact arithmetic, kept in lockstep with the driver.

namespace {

uint8_t madctlFor(bool flip) { return flip ? 0xE8 : 0x28; }

// v0.7.2 CONTRACT: expected readback = madctlFor(flip) with MY/MX cleared (RDDST
// reports those as 0), compared against got minus the scan-toggle bit0.
bool healthyCompare(uint8_t rddstTop, bool flip) {
  const uint8_t expected = uint8_t(madctlFor(flip) & ~0xC0);   // 0x28 for both flips
  return (rddstTop & 0xFE) == expected;
}

// v0.7.1 mask compare (dropped 0xC0 from both sides). Kept to prove it MISSES the
// MY/MX partial-loss signature.
constexpr uint8_t kOldMask = uint8_t(0xFE & ~0xC0);   // 0x3E
bool healthyCompareMask(uint8_t rddstTop, bool flip) {
  return (rddstTop & kOldMask) == (madctlFor(flip) & kOldMask);
}

// v0.7.0 full-byte compare. Kept to prove it FLAPPED at flip=1.
bool healthyCompareOld(uint8_t rddstTop, bool flip) {
  return (rddstTop & 0xFE) == (madctlFor(flip) & 0xFE);
}

}  // namespace

void setUp() {}
void tearDown() {}

// The measured healthy readback (0x28) must read HEALTHY in both orientations.
static void test_healthy_readback_ok_both_flips() {
  TEST_ASSERT_TRUE(healthyCompare(0x28, false));
  TEST_ASSERT_TRUE(healthyCompare(0x28, true));   // v0.7.0 read this false (thrash)
}

// bit0 is the scan-direction flag and toggles during refresh; it must not matter.
static void test_scan_toggle_bit_is_ignored() {
  TEST_ASSERT_TRUE(healthyCompare(0x29, false));
  TEST_ASSERT_TRUE(healthyCompare(0x29, true));
}

// A panel that lost its configuration reverts toward the power-on default; that
// must read UNHEALTHY in both orientations (the fix must not weaken reset
// detection).
static void test_reset_readback_is_unhealthy_both_flips() {
  TEST_ASSERT_FALSE(healthyCompare(0x00, false));
  TEST_ASSERT_FALSE(healthyCompare(0x00, true));
  TEST_ASSERT_FALSE(healthyCompare(0xC0, false));   // only MY/MX set: fixed bits lost
  TEST_ASSERT_FALSE(healthyCompare(0x08, false));   // lost MV (landscape rotation)
}

// CUM-231 core: a partial state loss that leaves the fixed bits correct but raises
// MY/MX in RDDST (got=0xE8) must read UNHEALTHY so the watchdog re-inits. This is
// the fault the v0.7.1 mask waved through as healthy.
static void test_partial_loss_raising_my_mx_is_unhealthy() {
  TEST_ASSERT_FALSE(healthyCompare(0xE8, false));   // MY|MX raised, fixed bits ok
  TEST_ASSERT_FALSE(healthyCompare(0xE8, true));
  TEST_ASSERT_FALSE(healthyCompare(0x68, false));   // only MX raised
  TEST_ASSERT_FALSE(healthyCompare(0xA8, false));   // only MY raised
}

// Regression witness A: the OLD full-byte compare flapped at flip=1 (CUM-188 bug).
static void test_old_compare_flapped_at_flip_one() {
  TEST_ASSERT_TRUE(healthyCompareOld(0x28, false));   // fine unflipped
  TEST_ASSERT_FALSE(healthyCompareOld(0x28, true));   // BROKEN: healthy read unhealthy
}

// Regression witness B: the v0.7.1 mask MISSED the MY/MX partial loss (CUM-231 bug)
// - and the v0.7.2 contract catches it. Also proves the mask fixed the thrash, so
// the fix is strictly better on both axes.
static void test_mask_missed_partial_loss_and_contract_catches_it() {
  TEST_ASSERT_TRUE(healthyCompareMask(0x28, true));    // mask fixed the thrash
  TEST_ASSERT_TRUE(healthyCompareMask(0xE8, false));   // BUG: partial loss read healthy
  TEST_ASSERT_FALSE(healthyCompare(0xE8, false));      // FIX: contract reads it unhealthy
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_healthy_readback_ok_both_flips);
  RUN_TEST(test_scan_toggle_bit_is_ignored);
  RUN_TEST(test_reset_readback_is_unhealthy_both_flips);
  RUN_TEST(test_partial_loss_raising_my_mx_is_unhealthy);
  RUN_TEST(test_old_compare_flapped_at_flip_one);
  RUN_TEST(test_mask_missed_partial_loss_and_contract_catches_it);
  return UNITY_END();
}
