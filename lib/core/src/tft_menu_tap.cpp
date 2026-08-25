#include "nimbus/tft_render/menu_tap.h"

namespace nimbus::tft {

bool applyMenuTap(SettingsMenu& menu, const TapRegion& tap) {
  using Action = TapRegion::Action;

  switch (tap.action) {
    case Action::MenuRow: {
      if (tap.index < 0) return false;
      // ⚠ While a value is captured, onRotate steps the VALUE and never moves
      // the cursor - so the stepping loop below would never converge, and each
      // of its iterations would silently change the owner's setting. Measured:
      // tapping a row with Volume captured moved it 50 -> 85 and then reported
      // "no action", so nothing repainted and the damage was invisible.
      // A tap while adjusting can only mean "commit this value".
      if (menu.valueAdjusting()) { menu.onClick(); return true; }
      // A tap says "this one" - a cursor move and a click in one gesture.
      //
      // rowCount()/selected() rather than view(): view() returns a
      // vector<string> BY VALUE, and the device calls this while holding a
      // portENTER_CRITICAL spinlock, where allocating is forbidden.
      //
      // The cursor wraps, but stepping toward the target numerically always
      // converges without wrapping, so rowCount() steps is a sufficient bound.
      const int rows = menu.rowCount();
      if (rows <= 0 || tap.index >= rows) return false;
      int guard = 0;
      while (menu.selected() != tap.index && guard++ <= rows)
        menu.onRotate(menu.selected() < tap.index ? +1 : -1);
      // If it did NOT land on the target (a stale region against a menu that
      // changed state under the tap), do nothing: activating whatever row the
      // cursor happens to sit on is worse than ignoring the tap.
      if (menu.selected() != tap.index) return false;
      menu.onClick();
      return true;
    }
    case Action::ValueUp:    menu.onRotate(+1);   return true;
    case Action::ValueDown:  menu.onRotate(-1);   return true;
    case Action::Commit:     menu.onClick();      return true;
    case Action::Back:
    case Action::Home:       menu.onLongPress();  return true;
    case Action::ScrollUp:   menu.onRotate(-1);   return true;
    case Action::ScrollDown: menu.onRotate(+1);   return true;
    // "Show code" on the Sign-in QR -> the full device sign-in code (TokenDetail).
    case Action::ShowCode:   menu.showCode();     return true;

    // Not menu gestures - the device handles these with the menu closed.
    case Action::OpenMenu:
    case Action::Mic:
    case Action::SessionCard:
    case Action::None:
    default:                 return false;
  }
}

}  // namespace nimbus::tft
