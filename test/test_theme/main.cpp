#include <unity.h>

#include "nimbus/theme.h"

using namespace nimbus;

void setUp() {}
void tearDown() {}

static void test_known_themes_distinct() {
  ThemeColor ocean = themeAccent("ocean");
  ThemeColor ember = themeAccent("ember");
  ThemeColor forest = themeAccent("forest");
  // each palette resolves to a non-black, distinct colour
  TEST_ASSERT_TRUE(ocean.r || ocean.g || ocean.b);
  TEST_ASSERT_FALSE(ocean.r == ember.r && ocean.g == ember.g && ocean.b == ember.b);
  TEST_ASSERT_FALSE(ember.r == forest.r && ember.g == forest.g && ember.b == forest.b);
}

static void test_provider_tints() {
  ThemeColor oa = themeAccent("openai");
  ThemeColor an = themeAccent("anthropic");
  ThemeColor mi = themeAccent("mistral");
  TEST_ASSERT_TRUE(oa.g > oa.r);              // openai green-forward
  TEST_ASSERT_TRUE(mi.r > mi.b);              // mistral warm
  TEST_ASSERT_FALSE(oa.r == an.r && oa.g == an.g && oa.b == an.b);
}

static void test_unknown_falls_back_to_default_not_black() {
  ThemeColor def = themeAccent("teal");
  ThemeColor unknown = themeAccent("does-not-exist");
  ThemeColor empty = themeAccent("");
  // unknown + empty both resolve to the default (teal), never dark
  TEST_ASSERT_EQUAL_UINT8(def.r, unknown.r);
  TEST_ASSERT_EQUAL_UINT8(def.g, unknown.g);
  TEST_ASSERT_EQUAL_UINT8(def.b, unknown.b);
  TEST_ASSERT_EQUAL_UINT8(def.r, empty.r);
  TEST_ASSERT_TRUE(def.r || def.g || def.b);  // default is not black
}

static void test_theme_list_nonempty() {
  std::string list = themeList();
  TEST_ASSERT_TRUE(list.find("teal") != std::string::npos);
  TEST_ASSERT_TRUE(list.find("ember") != std::string::npos);
  TEST_ASSERT_TRUE(list.find("mistral") != std::string::npos);
}

static void test_theme_hue_distinct_and_plausible() {
  // Each palette maps to a distinct ring hue (0-254 HSV space).
  uint8_t ember = themeHue("ember");    // warm orange/red -> low hue
  uint8_t forest = themeHue("forest");  // green -> mid hue
  uint8_t ocean = themeHue("ocean");    // blue -> high-ish hue
  TEST_ASSERT_NOT_EQUAL(ember, forest);
  TEST_ASSERT_NOT_EQUAL(forest, ocean);
  TEST_ASSERT_TRUE(ember < forest);     // red-ish below green-ish
  TEST_ASSERT_TRUE(forest < ocean);     // green below blue
}

static void test_theme_palettes() {
  ThemeColor pal[kThemeMaxColors];
  // Rainbow is the full 6-colour ROYGBIV; ocean/ember are multi-colour.
  TEST_ASSERT_EQUAL(6, themePalette("rainbow", pal, kThemeMaxColors));
  TEST_ASSERT_TRUE(themePalette("ocean", pal, kThemeMaxColors) >= 2);
  // The primary (themeAccent) is palette[0].
  ThemeColor a = themeAccent("ember");
  themePalette("ember", pal, kThemeMaxColors);
  TEST_ASSERT_EQUAL_UINT8(a.r, pal[0].r);
  TEST_ASSERT_EQUAL_UINT8(a.g, pal[0].g);
  // maxN clamps the count.
  TEST_ASSERT_EQUAL(2, themePalette("rainbow", pal, 2));
  // Unknown -> the teal palette (non-empty, never dark).
  int n = themePalette("does-not-exist", pal, kThemeMaxColors);
  TEST_ASSERT_TRUE(n >= 1);
  TEST_ASSERT_TRUE(pal[0].r || pal[0].g || pal[0].b);
}


// ---- owner R3 guards (2026-07-13): anti-homogeneity + red-reserved ----------
static uint8_t circDist(uint8_t a, uint8_t b) {
  int d = (int)a - (int)b; if (d < 0) d = -d;
  return (uint8_t)(d > 127 ? 255 - d : d);   // hue space wraps
}
static void test_role_hues_distinct_per_theme() {
  const int n = nimbus::themeCount();
  for (int t = 0; t < n; t++) {
    const std::string name = nimbus::themeAt(t);
    uint8_t h[4];
    for (int r = 0; r < 4; r++) h[r] = nimbus::themeRoleHue(name, r);
    for (int a = 0; a < 4; a++)
      for (int b = a + 1; b < 4; b++) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s roles %d/%d too close (%u vs %u)",
                 name.c_str(), a, b, h[a], h[b]);
        TEST_ASSERT_TRUE_MESSAGE(circDist(h[a], h[b]) >= 14, msg);
      }
  }
}
static void test_red_reserved_for_alert_only() {
  const int n = nimbus::themeCount();
  for (int t = 0; t < n; t++) {
    const std::string name = nimbus::themeAt(t);
    for (int r = 0; r < 4; r++) {
      const uint8_t h = nimbus::themeRoleHue(name, r);
      char msg[64];
      snprintf(msg, sizeof msg, "%s role %d is in the RED band (hue %u)",
               name.c_str(), r, h);
      TEST_ASSERT_TRUE_MESSAGE(h > 12 && h < 243, msg);
    }
    TEST_ASSERT_TRUE(nimbus::themeAlertHue(name) <= 20);   // alert MAY be red - its job
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_theme_palettes);
  RUN_TEST(test_known_themes_distinct);
  RUN_TEST(test_theme_hue_distinct_and_plausible);
  RUN_TEST(test_provider_tints);
  RUN_TEST(test_unknown_falls_back_to_default_not_black);
  RUN_TEST(test_theme_list_nonempty);
  RUN_TEST(test_role_hues_distinct_per_theme);
  RUN_TEST(test_red_reserved_for_alert_only);
  return UNITY_END();
}
