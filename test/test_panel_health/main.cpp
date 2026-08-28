#include <unity.h>

#include <cstdint>

// CUM-188 / F6 find: the panel health probe thrashed at flip=1.
//
// solide::display_tft::healthy() reads the ILI9341 RDDST status register (0x09)
// and compares its top byte against the MADCTL we wrote (`madctlFor(g_flip)`, with
// `madctlFor(flip) = flip ? 0xE8 : 0x28`). The bug: RDDST's status byte mirrors
// MADCTL's FIXED bits (BGR / MV / refresh order) but NOT the MY/MX 180-flip bits
// (0xC0), which stay at their power-on value regardless of the flip we write
// (measured on hardware). So the old full-byte compare made a flipped panel
// (madctlFor(1)=0xE8) read unhealthy FOREVER against the invariant RDDST 0x28,
// thrashing the panel watchdog.
//
// The driver fix masks the flip bits out of the compare, keeping madctlFor(g_flip)
// as the single source of truth for orientation. This host suite pins that compare
// contract so the mask cannot silently regress (the device fn itself is SPI-bound
// and not host-linkable; this mirrors its exact arithmetic, kept in lockstep).

namespace {

uint8_t madctlFor(bool flip) { return flip ? 0xE8 : 0x28; }

// The FIXED compare (post-fix): drop bit0 (the scan-direction flag that toggles
// during refresh) AND the MY/MX flip bits (0xC0) that RDDST does not reflect.
constexpr uint8_t kHealthMask = uint8_t(0xFE & ~0xC0);  // 0x3E

bool healthyCompare(uint8_t rddstTop, bool flip) {
  return (rddstTop & kHealthMask) == (madctlFor(flip) & kHealthMask);
}

// The OLD compare (pre-fix), kept only to prove it flapped at flip=1.
bool healthyCompareOld(uint8_t rddstTop, bool flip) {
  return (rddstTop & 0xFE) == (madctlFor(flip) & 0xFE);
}

}  // namespace

void setUp() {}
void tearDown() {}

// The measured healthy readback (0x28) must read HEALTHY in both orientations.
static void test_healthy_readback_ok_both_flips() {
  TEST_ASSERT_TRUE(healthyCompare(0x28, false));
  TEST_ASSERT_TRUE(healthyCompare(0x28, true));   // the bug read this false
}

// bit0 is the scan-direction flag and toggles during refresh; it must not matter.
static void test_scan_toggle_bit_is_ignored() {
  TEST_ASSERT_TRUE(healthyCompare(0x29, false));
  TEST_ASSERT_TRUE(healthyCompare(0x29, true));
}

// A panel that lost its configuration reverts these bits toward the power-on
// default; that must still read UNHEALTHY in both orientations (the mask must not
// weaken reset detection).
static void test_reset_readback_is_unhealthy_both_flips() {
  TEST_ASSERT_FALSE(healthyCompare(0x00, false));
  TEST_ASSERT_FALSE(healthyCompare(0x00, true));
  TEST_ASSERT_FALSE(healthyCompare(0xC0, false));   // only MY/MX set: still a loss of the 0x28 bits
}

// Regression witness: the OLD full-byte compare flapped at flip=1 (this is the bug).
static void test_old_compare_flapped_at_flip_one() {
  TEST_ASSERT_TRUE(healthyCompareOld(0x28, false));   // fine unflipped
  TEST_ASSERT_FALSE(healthyCompareOld(0x28, true));   // BROKEN: healthy panel read unhealthy
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_healthy_readback_ok_both_flips);
  RUN_TEST(test_scan_toggle_bit_is_ignored);
  RUN_TEST(test_reset_readback_is_unhealthy_both_flips);
  RUN_TEST(test_old_compare_flapped_at_flip_one);
  return UNITY_END();
}
