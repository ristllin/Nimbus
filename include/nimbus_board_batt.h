#pragma once

// Device-side battery-hardware resolvers bound to the compiled board map.
// The portable selection logic lives in lib/core (nimbus/power/board_power.h,
// host-tested); this binds it to solide::board() so main.cpp (the ADC begin) and
// webui.cpp (/api/state) share ONE source of truth for the divider actually used
// by the ADC (CUM-370). Same pattern as nimbus_board_power.h (CUM-202).

#include <solide/board.h>
#include "nimbus/power/board_power.h"

namespace nimbus {

// A fixed-divider board carries an ONBOARD resistor divider (an all-in-one
// carrier with no separate panel option), so its board-map dividerX100 is
// authoritative; a hand-built board's divider resistors vary per unit, so the
// owner-set NVS value wins. Mirrors the panel-fixed predicate the ADC path
// already used (board().epd.sck < 0): a board wired for one fixed onboard panel is
// the same class that carries one fixed onboard divider.
inline bool boardHasFixedDivider() { return solide::board().epd.sck < 0; }

// The battery divider actually fed to the ADC: the board's fixed value on a
// fixed-divider board, else the owner-set NVS divider. main.cpp and webui.cpp both
// call this so the reported divider is always the one in use.
inline uint16_t effectiveBattDivX100(uint16_t storeDividerX100) {
  return power::effectiveDividerX100(boardHasFixedDivider(),
                                     solide::board().batt.dividerX100,
                                     storeDividerX100);
}

}  // namespace nimbus
