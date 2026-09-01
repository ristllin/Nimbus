#pragma once

// Device-side resolver for the compiled board's display-flip base. The explicit
// per-board policy lives in lib/core (nimbus/display/board_flip.h, host-tested);
// this binds it to the compile-time SOLIDE_BOARD slug so main.cpp, webui.cpp and
// the test console share ONE source of truth instead of each re-deriving it.
//
// The owner's stored tftFlip is a plain "flip 180 for an upside-down mount" delta;
// this composes it with the board base so a fresh unit is upright per board (a
// Freenove panel is mounted 180 from the Solide - see board_flip.h, CUM-189).

#include "nimbus/display/board_flip.h"

#ifndef SOLIDE_BOARD
#define SOLIDE_BOARD solide_s3
#endif
#define NIMBUS_BOARDFLIP_STR2(x) #x
#define NIMBUS_BOARDFLIP_STR(x) NIMBUS_BOARDFLIP_STR2(x)

namespace nimbus {

// The display-flip base for THIS build's board (SOLIDE_BOARD).
inline bool displayFlipBaseForThisBoard() {
  return display::baseFlipForBoard(NIMBUS_BOARDFLIP_STR(SOLIDE_BOARD));
}

// The flip to hand solide::display_tft::setFlip(): the per-board base composed with
// the owner's stored tftFlip preference. Every setFlip call site uses this so the
// composition can never be applied at one site and missed at another.
inline bool effectiveTftFlip(bool userFlip) {
  return display::effectiveFlipForBoard(NIMBUS_BOARDFLIP_STR(SOLIDE_BOARD), userFlip);
}

}  // namespace nimbus
