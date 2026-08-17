#include <unity.h>

#include "nimbus/status_style.h"
#include "nimbus/theme.h"

using namespace nimbus;
using solide::ring::Anim;
using solide::ring::Status;

void setUp() {}
void tearDown() {}

// AMBIENT GRAMMAR (owner 2026-07-16, supersedes "different patterns per status"):
// nothing that PERSISTS may strobe on an all-day device. Every needs-you state
// (input/approval/error) is the same smooth Breathe - HUE carries the meaning
// (role1 cool / role3 amber / reserved alert red). Blink maps to NO status.
static void test_status_anims_follow_ambient_grammar() {
  TEST_ASSERT_EQUAL(int(Anim::Comet),   int(statusStyle(Status::Running).anim));
  TEST_ASSERT_EQUAL(int(Anim::Breathe), int(statusStyle(Status::WaitingInput).anim));
  TEST_ASSERT_EQUAL(int(Anim::Breathe), int(statusStyle(Status::AwaitingApproval).anim));
  TEST_ASSERT_EQUAL(int(Anim::Fade),    int(statusStyle(Status::Done).anim));
  // Error BREATHES red - was a 300 ms hard square blink (3.3 flashes/s, held up
  // to 5 min): an alarm, not signage. The reserved alert hue does the shouting.
  TEST_ASSERT_EQUAL(int(Anim::Breathe), int(statusStyle(Status::Error).anim));
  TEST_ASSERT_EQUAL(int(Anim::Solid),   int(statusStyle(Status::Idle).anim));   // idle NEVER moves
  TEST_ASSERT_EQUAL(int(Anim::Off),     int(statusStyle(Status::Offline).anim));
  // The strobe ban, stated directly: no status may ever map to Blink.
  for (int s = 0; s <= int(Status::Offline); ++s)
    TEST_ASSERT_NOT_EQUAL(int(Anim::Blink), int(statusStyle(Status(s)).anim));
}

// Only Error uses the alert hue; ambient states sit back on brightness.
static void test_alert_and_brightness() {
  TEST_ASSERT_TRUE(statusStyle(Status::Error).alert);
  TEST_ASSERT_FALSE(statusStyle(Status::Running).alert);
  TEST_ASSERT_FALSE(statusStyle(Status::WaitingInput).alert);
  TEST_ASSERT_TRUE(statusStyle(Status::Idle).brightPct < statusStyle(Status::Running).brightPct);
  TEST_ASSERT_EQUAL_UINT8(100, statusStyle(Status::Running).brightPct);
}

// Different roles pull DIFFERENT hues from a theme's family (so statuses are
// visually separable within one theme), and role index clamps into the palette.
static void test_theme_role_hues_differ() {
  const uint8_t primary = themeRoleHue("ocean", 0);
  const uint8_t accent  = themeRoleHue("ocean", 3);
  TEST_ASSERT_NOT_EQUAL(primary, accent);
  // out-of-range clamps to the last palette entry, never a garbage/zero hue.
  TEST_ASSERT_EQUAL_UINT8(themeRoleHue("ocean", 3), themeRoleHue("ocean", 99));
  // unknown theme falls back to the default palette (teal), not a dark ring.
  TEST_ASSERT_EQUAL_UINT8(themeRoleHue("teal", 0), themeRoleHue("does-not-exist", 0));
}

// The theme actually drives the ring: two different themes yield different
// primary-role hues for the same status (this is the "all green" fix).
static void test_theme_changes_the_hue() {
  TEST_ASSERT_NOT_EQUAL(themeRoleHue("ocean", 0), themeRoleHue("ember", 0));
  TEST_ASSERT_NOT_EQUAL(themeRoleHue("mistral", 0), themeRoleHue("forest", 0));
}

// Error is theme-appropriate: a cool theme (ocean) gets a warmer/less-pure-red
// alert than a warm theme (ember), but both read as "alert" (low warm hue).
static void test_alert_hue_is_in_family() {
  const uint8_t ocean = themeAlertHue("ocean");
  const uint8_t ember = themeAlertHue("ember");
  TEST_ASSERT_TRUE(ocean > ember);          // ocean's alert shifts toward amber
  TEST_ASSERT_TRUE(ember <= 4);             // warm theme -> essentially red
  TEST_ASSERT_TRUE(ocean <= 24);            // still a warm alert, not a cool hue
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_status_anims_follow_ambient_grammar);
  RUN_TEST(test_alert_and_brightness);
  RUN_TEST(test_theme_role_hues_differ);
  RUN_TEST(test_theme_changes_the_hue);
  RUN_TEST(test_alert_hue_is_in_family);
  return UNITY_END();
}
