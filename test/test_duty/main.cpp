#include <unity.h>

#include "nimbus/duty.h"

using namespace nimbus;

void setUp() {}
void tearDown() {}

// dutyPermille is the whole timing story of the low-battery cue: a warning that a
// battery is low must not itself be why the battery goes flat. Tested here rather
// than on hardware because it is pure arithmetic, and because a stuck-ON envelope
// is exactly the bug that would be invisible in a quick bench look (it just looks
// like the old continuous breathe).

static void test_lit_inside_the_window() {
  TEST_ASSERT_EQUAL_UINT32(1000, dutyPermille(0, 3000, 60000));
  TEST_ASSERT_EQUAL_UINT32(1000, dutyPermille(1, 3000, 60000));
  TEST_ASSERT_EQUAL_UINT32(1000, dutyPermille(2999, 3000, 60000));
}

static void test_dark_through_the_gap() {
  TEST_ASSERT_EQUAL_UINT32(0, dutyPermille(3000, 3000, 60000));   // boundary is EXCLUSIVE
  TEST_ASSERT_EQUAL_UINT32(0, dutyPermille(30000, 3000, 60000));
  TEST_ASSERT_EQUAL_UINT32(0, dutyPermille(59999, 3000, 60000));
}

static void test_repeats_every_period() {
  for (uint32_t cycle = 1; cycle < 5; ++cycle) {
    const uint32_t base = cycle * 60000u;
    TEST_ASSERT_EQUAL_UINT32(1000, dutyPermille(base, 3000, 60000));
    TEST_ASSERT_EQUAL_UINT32(1000, dutyPermille(base + 2999, 3000, 60000));
    TEST_ASSERT_EQUAL_UINT32(0, dutyPermille(base + 3000, 3000, 60000));
  }
}

// A zero period must mean "no gating at all", because that is the default on every
// existing animation. If it returned 0 instead, arming nothing would turn the whole
// ring off - every other cue in the firmware would go dark.
static void test_zero_period_never_gates() {
  TEST_ASSERT_EQUAL_UINT32(1000, dutyPermille(0, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(1000, dutyPermille(12345, 3000, 0));
  TEST_ASSERT_EQUAL_UINT32(1000, dutyPermille(0xFFFFFFFFu, 3000, 0));
}

// The device clock wraps every ~49.7 days. The only acceptable artifact is one
// shortened cycle; a stuck-on or stuck-off state would strand the cue.
static void test_survives_the_clock_wrap() {
  const uint32_t nearWrap = 0xFFFFFFFFu;
  const uint32_t lit = dutyPermille(nearWrap, 3000, 60000);
  TEST_ASSERT_TRUE(lit == 0 || lit == 1000);            // well-defined, never garbage
  // Straddle the wrap: both sides must still be one of the two valid states, and
  // the cue must resume lighting shortly after.
  bool sawLit = false, sawDark = false;
  for (uint32_t i = 0; i < 130000u; i += 250u) {
    const uint32_t t = nearWrap - 60000u + i;           // wraps mid-loop
    const uint32_t v = dutyPermille(t, 3000, 60000);
    TEST_ASSERT_TRUE(v == 0 || v == 1000);
    if (v) sawLit = true; else sawDark = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(sawLit, "cue never lights across the wrap");
  TEST_ASSERT_TRUE_MESSAGE(sawDark, "cue never rests across the wrap");
}

// The duty cycle is what makes the cue cheap: it must be ON for a small fraction
// of the time. Measured rather than asserted from the constants, so a future edit
// to either constant that makes the cue near-continuous fails here.
static void test_shipped_constants_are_a_brief_pulse() {
  uint32_t lit = 0, total = 0;
  for (uint32_t t = 0; t < kLowBattCuePeriodMs * 4u; t += 10u) {
    if (dutyPermille(t, kLowBattCueOnMs, kLowBattCuePeriodMs)) lit++;
    total++;
  }
  const uint32_t pct = lit * 100u / total;
  TEST_ASSERT_TRUE_MESSAGE(pct <= 10, "the low-battery cue is lit >10% of the time");
  TEST_ASSERT_TRUE_MESSAGE(pct >= 1, "the low-battery cue never lights");
}

// The backlight is the largest continuous draw on a colour panel, so the battery
// mode has to reach it. Asserted as an ORDERING plus bounds rather than exact
// numbers, so the levels can be re-tuned by eye without a test edit - but a mode
// that fails to dim, or one that dims to invisible, still fails.
static void test_backlight_follows_the_battery_mode() {
  const uint8_t dark = backlightPctFor(Posture::Dark);
  const uint8_t calm = backlightPctFor(Posture::Calm);
  const uint8_t full = backlightPctFor(Posture::Full);

  TEST_ASSERT_TRUE_MESSAGE(dark < calm, "Dark must be dimmer than Balanced");
  TEST_ASSERT_TRUE_MESSAGE(calm < full, "Balanced must be dimmer than Full");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(100, full, "Full is the desk display - full brightness");
  // Never zero: 0 is the screensaver's "off", and a battery mode that silently
  // blanked the panel would be indistinguishable from broken hardware.
  TEST_ASSERT_TRUE_MESSAGE(dark >= 10, "Dark must stay readable, not blank the panel");
  // The RESTING level must be dim but NEVER zero: a fully dark colour panel is
  // indistinguishable from a dead one, which is not hypothetical - a resting
  // screen was reported as a fault symptom during a blank-screen investigation.
  TEST_ASSERT_TRUE_MESSAGE(kBacklightRestPct > 0,
      "a resting screen must still glow, or it cannot be told from a broken one");
  TEST_ASSERT_TRUE_MESSAGE(kBacklightRestPct < dark,
      "resting must be dimmer than the dimmest active battery mode");
  TEST_ASSERT_TRUE_MESSAGE(full <= 100, "percent, not a raw duty value");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_lit_inside_the_window);
  RUN_TEST(test_dark_through_the_gap);
  RUN_TEST(test_repeats_every_period);
  RUN_TEST(test_zero_period_never_gates);
  RUN_TEST(test_survives_the_clock_wrap);
  RUN_TEST(test_shipped_constants_are_a_brief_pulse);
  RUN_TEST(test_backlight_follows_the_battery_mode);
  return UNITY_END();
}
