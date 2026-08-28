#pragma once

// Device-side resolver for the compiled board's battery-monitoring default.
// The explicit per-board policy lives in lib/core (nimbus/power/board_power.h,
// host-tested); this binds it to the compile-time SOLIDE_BOARD slug so main.cpp
// and webui.cpp share ONE source of truth instead of each re-deriving it.

#include "nimbus/power/board_power.h"

#ifndef SOLIDE_BOARD
#define SOLIDE_BOARD solide_s3
#endif
#define NIMBUS_BOARDPWR_STR2(x) #x
#define NIMBUS_BOARDPWR_STR(x) NIMBUS_BOARDPWR_STR2(x)

namespace nimbus {

// battMon default for THIS build's board (SOLIDE_BOARD). Passed to
// agent::store::battMon(def) as the board-derived default.
inline bool battMonDefaultForThisBoard() {
  return power::battMonDefaultForBoard(NIMBUS_BOARDPWR_STR(SOLIDE_BOARD));
}

}  // namespace nimbus
