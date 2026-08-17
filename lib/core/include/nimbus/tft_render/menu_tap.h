#pragma once
#include "nimbus/settings_menu.h"
#include "nimbus/tft_render/screens.h"

// ============================================================================
// tft_render/menu_tap - turn a tap on the settings screen into menu-FSM calls.
//
// This lives in lib/core, not in main.cpp, for one reason: it is the single
// riskiest piece of the touch feature (with no knob, an off-by-one here means
// the owner activates a setting they did not touch) and it operates purely on
// SettingsMenu, which is already portable. Keeping it device-side would leave
// it testable ONLY by hardware - and the hardware tests are exactly the ones
// that cannot run yet.
//
// ⚠ The device must CALL this, never re-implement it. A test that rebuilds the
// production mapping proves only that the test agrees with itself (see the RBAC
// inert-rails lesson: eight green tests against a rail production never ran).
// ============================================================================

namespace nimbus::tft {

// Apply one tap to `menu`. Returns true if the FSM was touched (the caller then
// needs a repaint); false when the tap was a no-op, which includes a stale
// region naming a row the menu can no longer reach.
//
// The mapping mirrors the encoder exactly, so every downstream path - the
// dirty()/persist block, the request-flag drains - runs unchanged:
//   MenuRow    -> step the cursor onto that row, then onClick()
//   ValueUp    -> onRotate(+1)      ValueDown  -> onRotate(-1)
//   Commit     -> onClick()
//   Back/Home  -> onLongPress()     (one level up; from Main it closes)
//   ScrollUp   -> onRotate(-1)      ScrollDown -> onRotate(+1)
bool applyMenuTap(SettingsMenu& menu, const TapRegion& tap);

}  // namespace nimbus::tft
