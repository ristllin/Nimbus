#include <unity.h>

#include <cstdio>
#include <string>

#include "nimbus/settings_menu.h"
#include "nimbus/tft_render/fb565.h"
#include "nimbus/tft_render/menu_tap.h"
#include "nimbus/tft_render/screens.h"

// End-to-end touch navigation, on the host: RENDER the real menu -> HIT-TEST a
// real coordinate -> apply it to a REAL SettingsMenu -> assert the FSM moved.
//
// This is the interaction the whole feature rests on, and until now it was
// covered only by hardware tests that have never been able to run. Nothing here
// is a stand-in: it drives nimbus::tft::renderScreen, Rendered::hit() and
// nimbus::tft::applyMenuTap - the same three functions the device calls. A test
// that rebuilt the mapping would prove only that the test agrees with itself.

using nimbus::Config;
using nimbus::SettingsMenu;
using nimbus::attn::ScreenId;
using nimbus::render::ScreenCtx;
using nimbus::tft::applyMenuTap;
using nimbus::tft::Fb565;
using nimbus::tft::Rendered;
using nimbus::tft::renderScreen;
using nimbus::tft::TapRegion;

void setUp() {}
void tearDown() {}

namespace {

// Render whatever the menu currently shows, exactly as the device does.
Rendered renderMenuNow(Fb565& fb, const SettingsMenu& menu) {
  ScreenCtx c;
  c.deviceName = "Nimbus-4";
  const auto v = menu.view();
  c.menuTitle = v.title;
  c.menuItems = v.items;
  c.menuSelected = v.selected;
  return renderScreen(fb, ScreenId::Menu, c);
}

// Tap the centre of the Nth MenuRow region currently on screen.
const TapRegion* rowRegion(const Rendered& r, int nth) {
  int seen = 0;
  for (const auto& t : r.taps) {
    if (t.action != TapRegion::Action::MenuRow) continue;
    if (seen++ == nth) return &t;
  }
  return nullptr;
}

}  // namespace

// A tap must land on the SAME state the knob would reach: rotate onto the row,
// then click. This is the real invariant of the whole design ("taps dispatch
// into the existing FSM"), and it is far stronger than checking the cursor -
// onClick() descends, which resets the cursor in the new state, so the tapped
// index is deliberately NOT what you compare afterwards.
static bool sameState(const SettingsMenu& a, const SettingsMenu& b) {
  const auto va = a.view(), vb = b.view();
  return a.isOpen() == b.isOpen() && va.title == vb.title &&
         va.items == vb.items && va.selected == vb.selected;
}

static void test_tap_matches_the_knob() {
  Config cfg;
  SettingsMenu menu(cfg);
  menu.open();
  Fb565 fb;

  const Rendered r = renderMenuNow(fb, menu);
  // Row index 1, not 2: the landscape panel is 240 tall and fits about three
  // rows, so a third row is not guaranteed to be drawn. Index 1 still differs
  // from the default selection (0), which is what makes the assertion below real.
  const TapRegion* row = rowRegion(r, 1);        // second visible row
  TEST_ASSERT_NOT_NULL_MESSAGE(row, "menu rendered no tappable rows");
  const int target = row->index;
  TEST_ASSERT_TRUE_MESSAGE(target != menu.selected(),
                           "fixture is weak: the target row is already selected");

  // Resolve through the real hit-test, at the real centre of the drawn region.
  const TapRegion* hit = r.hit(row->x + row->w / 2, row->y + row->h / 2);
  TEST_ASSERT_NOT_NULL(hit);
  TEST_ASSERT_EQUAL_INT(int(TapRegion::Action::MenuRow), int(hit->action));
  TEST_ASSERT_EQUAL_INT(target, hit->index);
  TEST_ASSERT_TRUE(applyMenuTap(menu, *hit));

  // The knob doing the equivalent thing, from the same starting point.
  Config cfg2;
  SettingsMenu knob(cfg2);
  knob.open();
  while (knob.selected() != target) knob.onRotate(knob.selected() < target ? +1 : -1);
  knob.onClick();

  TEST_ASSERT_TRUE_MESSAGE(sameState(menu, knob),
                           "a tap on a row did not reach the same state the knob does");
}

// Every visible row must be reachable and land where the knob lands - the
// cursor wraps, so a naive stepper can converge from the wrong side.
static void test_every_visible_row_matches_the_knob() {
  Config cfg;
  for (int nth = 0; nth < 4; nth++) {
    SettingsMenu menu(cfg);
    menu.open();
    Fb565 fb;
    const Rendered r = renderMenuNow(fb, menu);
    const TapRegion* row = rowRegion(r, nth);
    if (!row) continue;
    const int target = row->index;
    TEST_ASSERT_TRUE(applyMenuTap(menu, *row));

    Config cfg2;
    SettingsMenu knob(cfg2);
    knob.open();
    while (knob.selected() != target) knob.onRotate(knob.selected() < target ? +1 : -1);
    knob.onClick();

    char msg[112];
    std::snprintf(msg, sizeof msg,
                  "row %d (index %d): tap and knob reached different states", nth, target);
    TEST_ASSERT_TRUE_MESSAGE(sameState(menu, knob), msg);
  }
}

// Back must actually leave - with no knob, a menu you cannot exit is a trap.
static void test_back_leaves_the_menu() {
  Config cfg;
  SettingsMenu menu(cfg);
  menu.open();
  TEST_ASSERT_TRUE(menu.isOpen());

  TapRegion back;
  back.action = TapRegion::Action::Back;
  TEST_ASSERT_TRUE(applyMenuTap(menu, back));
  TEST_ASSERT_FALSE_MESSAGE(menu.isOpen(),
                            "Back from the top level did not close the menu");
}

// The steppers must move a captured VALUE and Commit must leave the editor.
//
// The previous version of this test called menu.open() and fired ValueUp in
// State::Main, where onRotate is a plain cursor move - so it asserted list
// navigation that another test already covers and never touched an adjusting
// state at all. Mutation proof it was hollow: dropping onClick() from the
// Commit case left the whole suite green, because nothing ever passed
// Action::Commit to applyMenuTap.
static void test_stepper_adjusts_a_captured_value_and_commits() {
  Config cfg;
  SettingsMenu menu(cfg);
  menu.open();
  menu.setSfxVolume(50);

  // Descend into Sound, then capture Volume.
  auto descendInto = [&](const char* needle) {
    const auto v = menu.view();
    for (size_t i = 0; i < v.items.size(); i++) {
      if (v.items[i].find(needle) != std::string::npos) {
        while (menu.selected() != int(i))
          menu.onRotate(menu.selected() < int(i) ? +1 : -1);
        menu.onClick();
        return true;
      }
    }
    return false;
  };
  if (!descendInto("Sound") || !descendInto("Volume") || !menu.valueAdjusting()) {
    TEST_IGNORE_MESSAGE("could not reach a captured value; fixture needs updating");
    return;
  }

  const int v0 = menu.sfxVolume();
  TapRegion up;   up.action   = TapRegion::Action::ValueUp;
  TapRegion down; down.action = TapRegion::Action::ValueDown;

  TEST_ASSERT_TRUE(applyMenuTap(menu, up));
  TEST_ASSERT_NOT_EQUAL_MESSAGE(v0, menu.sfxVolume(), "ValueUp did not move the value");
  TEST_ASSERT_TRUE(applyMenuTap(menu, down));
  TEST_ASSERT_EQUAL_INT_MESSAGE(v0, menu.sfxVolume(), "ValueDown did not undo ValueUp");

  // Commit must actually leave the editor - this is the assertion whose absence
  // let a no-op Commit case pass.
  TapRegion commit; commit.action = TapRegion::Action::Commit;
  TEST_ASSERT_TRUE(applyMenuTap(menu, commit));
  TEST_ASSERT_FALSE_MESSAGE(menu.valueAdjusting(),
                            "Commit did not leave the adjusting state");
}

// A stale region - one naming a row the menu can no longer reach - must be
// IGNORED, never coerced into activating whatever is under the cursor.
static void test_out_of_range_row_is_ignored() {
  Config cfg;
  SettingsMenu menu(cfg);
  menu.open();
  const int before = menu.selected();

  TapRegion stale;
  stale.action = TapRegion::Action::MenuRow;
  stale.index = 9999;
  TEST_ASSERT_FALSE(applyMenuTap(menu, stale));
  TEST_ASSERT_EQUAL_INT_MESSAGE(before, menu.selected(),
                                "an out-of-range tap moved the cursor");

  stale.index = -1;
  TEST_ASSERT_FALSE(applyMenuTap(menu, stale));
  TEST_ASSERT_EQUAL_INT(before, menu.selected());
}

// Non-menu actions must be no-ops here: the device handles them with the menu
// closed, and silently consuming one would swallow the gesture.
static void test_non_menu_actions_are_no_ops() {
  Config cfg;
  SettingsMenu menu(cfg);
  menu.open();
  const int before = menu.selected();

  for (auto a : {TapRegion::Action::None, TapRegion::Action::OpenMenu,
                 TapRegion::Action::Mic, TapRegion::Action::SessionCard}) {
    TapRegion t;
    t.action = a;
    TEST_ASSERT_FALSE(applyMenuTap(menu, t));
    TEST_ASSERT_EQUAL_INT(before, menu.selected());
  }
}

// MUTATION CHECK: if the mapping were deleted, these tests must go red. Assert
// the positive path really does depend on applyMenuTap doing work, so this file
// cannot pass against an inert implementation (the failure mode prism found in
// the touch inject seam).
static void test_mapping_is_not_inert() {
  Config cfg;
  SettingsMenu menu(cfg);
  menu.open();
  Fb565 fb;
  const Rendered r = renderMenuNow(fb, menu);
  const TapRegion* row = rowRegion(r, 1);
  TEST_ASSERT_NOT_NULL(row);

  const int before = menu.selected();
  const bool acted = applyMenuTap(menu, *row);
  TEST_ASSERT_TRUE_MESSAGE(acted, "applyMenuTap reported no action on a real row");
  TEST_ASSERT_TRUE_MESSAGE(menu.selected() != before || row->index == before,
                           "applyMenuTap claimed to act but changed nothing");
}

// REGRESSION: a stray row tap while a value is captured must NOT step that
// value. onRotate steps the VALUE (not the cursor) in the adjusting states, so
// the naive stepping loop moved the owner's setting once per iteration and then
// reported "no action" - the change was real, silent, and unpainted.
// Measured before the fix: Volume 50 -> 85 from a single stray tap.
static void test_row_tap_while_adjusting_does_not_move_the_value() {
  Config cfg;
  SettingsMenu menu(cfg);
  menu.open();

  // Reach Sound > Volume and capture it.
  const int vol0 = 50;
  menu.setSfxVolume(vol0);
  while (menu.view().title.find("Sound") == std::string::npos) {
    // walk the Main list to the Sound row and descend
    bool found = false;
    const auto v = menu.view();
    for (size_t i = 0; i < v.items.size(); i++) {
      if (v.items[i].find("Sound") != std::string::npos) {
        while (menu.selected() != int(i))
          menu.onRotate(menu.selected() < int(i) ? +1 : -1);
        menu.onClick();
        found = true;
        break;
      }
    }
    if (!found) break;
  }
  // Find and capture the Volume row.
  {
    const auto v = menu.view();
    for (size_t i = 0; i < v.items.size(); i++) {
      if (v.items[i].find("Volume") != std::string::npos) {
        while (menu.selected() != int(i))
          menu.onRotate(menu.selected() < int(i) ? +1 : -1);
        menu.onClick();
        break;
      }
    }
  }
  if (!menu.valueAdjusting()) {
    TEST_IGNORE_MESSAGE("could not reach a captured-value state; fixture needs updating");
    return;
  }

  // Assert on the VOLUME itself, not adjustValuePct(): the fix commits and
  // leaves the adjusting state, after which adjustValuePct() is meaningless.
  // What must never change is the owner's actual setting.
  const int before = menu.sfxVolume();
  TapRegion stray;
  stray.action = TapRegion::Action::MenuRow;
  stray.index = 5;                       // some other row on screen
  applyMenuTap(menu, stray);
  TEST_ASSERT_EQUAL_INT_MESSAGE(before, menu.sfxVolume(),
                                "a stray row tap silently changed the volume");
  TEST_ASSERT_FALSE_MESSAGE(menu.valueAdjusting(),
                            "a tap while adjusting should commit, not stay captured");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_tap_matches_the_knob);
  RUN_TEST(test_every_visible_row_matches_the_knob);
  RUN_TEST(test_back_leaves_the_menu);
  RUN_TEST(test_stepper_adjusts_a_captured_value_and_commits);
  RUN_TEST(test_out_of_range_row_is_ignored);
  RUN_TEST(test_non_menu_actions_are_no_ops);
  RUN_TEST(test_mapping_is_not_inert);
  RUN_TEST(test_row_tap_while_adjusting_does_not_move_the_value);
  return UNITY_END();
}
