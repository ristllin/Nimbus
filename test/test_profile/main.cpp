#include <unity.h>

#include <cstring>

#include "nimbus/profile.h"

using namespace nimbus;

void setUp() {}
void tearDown() {}

static void test_presets_match_plan_table() {
  TEST_ASSERT_EQUAL(int(Posture::Dark), presetValue(ProfileId::BatterySaver, Param::Posture));
  TEST_ASSERT_EQUAL(int(Posture::Calm), presetValue(ProfileId::Balanced, Param::Posture));
  TEST_ASSERT_EQUAL(int(Posture::Full), presetValue(ProfileId::Desk, Param::Posture));
  TEST_ASSERT_EQUAL(10, presetValue(ProfileId::BatterySaver, Param::RingBrightness));
  TEST_ASSERT_EQUAL(30, presetValue(ProfileId::Balanced, Param::RingBrightness));
  TEST_ASSERT_EQUAL(60, presetValue(ProfileId::Desk, Param::RingBrightness));
  TEST_ASSERT_EQUAL(60000, presetValue(ProfileId::BatterySaver, Param::CoalesceMs));
  TEST_ASSERT_EQUAL(30000, presetValue(ProfileId::Balanced, Param::CoalesceMs));
  TEST_ASSERT_EQUAL(15000, presetValue(ProfileId::Desk, Param::CoalesceMs));
  TEST_ASSERT_EQUAL(300, presetValue(ProfileId::Balanced, Param::DwellMs));
}

static void test_effective_uses_preset_then_override() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  TEST_ASSERT_EQUAL(30, c.effective(Param::RingBrightness));
  c.setOverride(Param::RingBrightness, 99);
  TEST_ASSERT_EQUAL(99, c.effective(Param::RingBrightness));
  c.clearOverride(Param::RingBrightness);
  TEST_ASSERT_EQUAL(30, c.effective(Param::RingBrightness));
}

// The core promise of the sparse-override model: switching profiles changes
// non-overridden values and preserves user overrides.
static void test_override_survives_profile_switch() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  c.setOverride(Param::AttnLedIndex, 22);
  c.setProfile(ProfileId::Desk);
  TEST_ASSERT_EQUAL(22, c.effective(Param::AttnLedIndex));
  TEST_ASSERT_EQUAL(60, c.effective(Param::RingBrightness));  // Desk preset
  TEST_ASSERT_EQUAL(int(Posture::Full), int(c.posture()));
}

static void test_clear_all_overrides() {
  Config c;
  c.setOverride(Param::RingFps, 5);
  c.setOverride(Param::DwellMs, 1000);
  TEST_ASSERT_TRUE(c.hasOverride(Param::RingFps));
  c.clearAllOverrides();
  TEST_ASSERT_FALSE(c.hasOverride(Param::RingFps));
  TEST_ASSERT_FALSE(c.hasOverride(Param::DwellMs));
}

static void test_selector_precedence_forced_vbus_user() {
  Selector s;
  s.setUser(ProfileId::Balanced);
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(s.resolve()));

  s.setVbus(true);
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(s.resolve()));

  s.setForced(true);  // battery T1 beats everything
  TEST_ASSERT_EQUAL(int(ProfileId::BatterySaver), int(s.resolve()));

  s.setForced(false);
  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(s.resolve()));
  s.setVbus(false);
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(s.resolve()));
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), int(s.user()));  // user intent kept
}

static void test_attn_hue_auto_sentinel() {
  Config c;
  for (int p = 0; p < kProfileCount; ++p) {
    c.setProfile(ProfileId(p));
    TEST_ASSERT_EQUAL(-1, c.effective(Param::AttnHue));
  }
}

// paramMeta/stepParam are the single source of truth for editable ranges; the
// settings menu leans on them. Guard the shapes the editor depends on.
static void test_param_meta_shapes() {
  // Every param has a non-empty, ordered range.
  for (int i = 0; i < kParamCount; ++i) {
    ParamMeta m = paramMeta(Param(i));
    TEST_ASSERT_TRUE(m.max >= m.min);
    if (m.kind == ParamKind::Int) TEST_ASSERT_TRUE(m.step > 0);
  }
  // Spot-check the sentinel/enum shapes.
  ParamMeta hue = paramMeta(Param::AttnHue);
  TEST_ASSERT_EQUAL(-1, hue.min);          // -1 = auto
  TEST_ASSERT_EQUAL(255, hue.max);
  ParamMeta anim = paramMeta(Param::AttnAnim);
  TEST_ASSERT_EQUAL(int(ParamKind::Enum), int(anim.kind));
  TEST_ASSERT_EQUAL(0, anim.min);
  TEST_ASSERT_EQUAL(5, anim.max);          // ring::Anim Off..Fade
}

// paramLabel + paramDescription feed the menu row and help pane. Panel
// constraints: labels are short (<= 26 chars, one row), descriptions non-empty
// and <= 108 chars (wrap to <= 3 lines at 48 cols), and every char is printable
// ASCII 32-126 (the 5x7 font renders anything else as '?').
static void test_param_labels_and_descriptions_fit_the_panel() {
  for (int i = 0; i < kParamCount; ++i) {
    const char* lbl = paramLabel(Param(i));
    TEST_ASSERT_NOT_NULL(lbl);
    const size_t ln = std::strlen(lbl);
    TEST_ASSERT_TRUE_MESSAGE(ln > 0 && ln <= 26, paramName(Param(i)));
    for (size_t j = 0; j < ln; ++j)
      TEST_ASSERT_TRUE_MESSAGE((unsigned char)lbl[j] >= 32 &&
                               (unsigned char)lbl[j] <= 126, paramName(Param(i)));

    const char* d = paramDescription(Param(i));
    TEST_ASSERT_NOT_NULL(d);
    const size_t n = std::strlen(d);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, paramName(Param(i)));
    TEST_ASSERT_TRUE_MESSAGE(n <= 108, paramName(Param(i)));
    for (size_t j = 0; j < n; ++j) {
      const unsigned char ch = (unsigned char)d[j];
      TEST_ASSERT_TRUE_MESSAGE(ch >= 32 && ch <= 126, paramName(Param(i)));
    }
  }
}

// The call-to-action hold (Param::AttnHoldMs) is posture-scaled since 2026-08-04:
// Full can afford to insist for 5 min; on Balanced/Dark the same hold read as
// "the ring is stuck" (owner report - a rainbow breathing for minutes on an idle
// desk). Still a tunable Int spanning 30 s .. 30 min in every mode.
static void test_attn_hold_default_and_range() {
  TEST_ASSERT_EQUAL_INT32(60000,  presetValue(ProfileId(0), Param::AttnHoldMs));
  TEST_ASSERT_EQUAL_INT32(120000, presetValue(ProfileId(1), Param::AttnHoldMs));
  TEST_ASSERT_EQUAL_INT32(300000, presetValue(ProfileId(2), Param::AttnHoldMs));
  ParamMeta m = paramMeta(Param::AttnHoldMs);
  TEST_ASSERT_EQUAL_INT32(30000, m.min);
  TEST_ASSERT_EQUAL_INT32(1800000, m.max);
  TEST_ASSERT_EQUAL_INT32(int(ParamKind::Int), int(m.kind));
}

static void test_step_param_clamp_and_wrap() {
  // Int clamps, no wrap.
  TEST_ASSERT_EQUAL(0, stepParam(Param::RingBrightness, 0, -1));       // at min
  TEST_ASSERT_EQUAL(255, stepParam(Param::RingBrightness, 255, +1));   // at max
  TEST_ASSERT_EQUAL(35, stepParam(Param::RingBrightness, 30, +1));     // +step 5
  // AttnHue sentinel boundary: 0 - step clamps down to the -1 auto min.
  TEST_ASSERT_EQUAL(-1, stepParam(Param::AttnHue, 0, -1));
  // Enum wraps at both ends.
  TEST_ASSERT_EQUAL(0, stepParam(Param::AttnAnim, 5, +1));             // 5 -> 0
  TEST_ASSERT_EQUAL(5, stepParam(Param::AttnAnim, 0, -1));             // 0 -> 5
  // Bool toggles.
  TEST_ASSERT_EQUAL(1, stepParam(Param::TgLowBattPing, 0, +1));
  TEST_ASSERT_EQUAL(0, stepParam(Param::TgLowBattPing, 1, +1));        // wrap
}

// CUM-187: the ring-only params (hidden on a ringless board, in both the device
// menu and the web UI) are exactly the seven LED-ring controls; the five non-ring
// params stay visible on every board. This predicate is the single source both
// surfaces read.
static void test_is_ring_param_classifies_the_led_controls() {
  const Param ring[] = {Param::Posture, Param::RingBrightness, Param::RingFps,
                        Param::AttnLedIndex, Param::AttnHue, Param::AttnAnim,
                        Param::AttnPeriodMs};
  const Param nonRing[] = {Param::CoalesceMs, Param::DwellMs, Param::TelemetryPeriodS,
                           Param::TgLowBattPing, Param::AttnHoldMs};
  for (Param p : ring) TEST_ASSERT_TRUE(isRingParam(p));
  for (Param p : nonRing) TEST_ASSERT_FALSE(isRingParam(p));
  int ringCount = 0;
  for (int i = 0; i < kParamCount; ++i)
    if (isRingParam(Param(i))) ++ringCount;
  TEST_ASSERT_EQUAL(7, ringCount);   // the two sets partition every param (7 + 5 == 12)
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_is_ring_param_classifies_the_led_controls);
  RUN_TEST(test_presets_match_plan_table);
  RUN_TEST(test_param_meta_shapes);
  RUN_TEST(test_attn_hold_default_and_range);
  RUN_TEST(test_param_labels_and_descriptions_fit_the_panel);
  RUN_TEST(test_step_param_clamp_and_wrap);
  RUN_TEST(test_effective_uses_preset_then_override);
  RUN_TEST(test_override_survives_profile_switch);
  RUN_TEST(test_clear_all_overrides);
  RUN_TEST(test_selector_precedence_forced_vbus_user);
  RUN_TEST(test_attn_hue_auto_sentinel);
  return UNITY_END();
}
