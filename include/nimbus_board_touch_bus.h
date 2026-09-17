#pragma once

// Device-side resolver for the shared-MISO touch/panel bus hazard (CUM-392).
// The portable policy lives in lib/core (nimbus/display/panel_read_policy.h,
// host-tested); this binds it to the compile-time board map so tft_out.cpp, the
// health payload and the test console share ONE source of truth for "may a panel
// readback run on this board" instead of each re-deriving it. Same pattern as
// nimbus_board_flip.h (CUM-189) and nimbus_board_batt.h (CUM-370).

#include <solide/board.h>

#include "nimbus/display/panel_read_policy.h"

namespace nimbus {

// True on a board where a resistive XPT2046 reports on the panel's readback MISO
// (solide_s3). A panel register/pixel read then contends that line with touch.
inline bool boardSharedMisoResistiveTouch() {
  const solide::Board& b = solide::board();
  return display::sharedMisoResistiveTouch(
      b.touchKind == solide::TouchKind::ResistiveSpi, b.tft.miso, b.tft.tcs);
}

// True when panel readbacks are electrically safe on THIS board (capacitive /
// separate-bus). The fix's inertness on freenove_s3 / Lumi rests on this reading
// true there, so the read sites behave exactly as before on those boards.
inline bool boardPanelReadbackSafe() {
  return !boardSharedMisoResistiveTouch();
}

}  // namespace nimbus
