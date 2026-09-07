#pragma once

// board_power - the EXPLICIT per-board battery-policy defaults (CUM-202).
//
// Battery monitoring defaults per board:
//   Solide S3  (hand-built, ships WITH a 2S pack)          -> ON  (shipped)
//   Freenove CYD / all-in-one (battery is an optional add-on) -> OFF (opt-in)
//
// This REPLACES the old `solide::board().epd.sck >= 0` proxy: the e-ink stack is
// deprecated, so if those epd pins are ever cleaned out of the board table the
// proxy would silently flip the Freenove's default to ON and the floating ADC
// would read "empty" and deep-sleep a board with no pack fitted. An explicit
// field keyed on the board slug cannot drift like that, and a new board must make
// a deliberate choice (unknown -> OFF, the safe default: never auto-sample /
// auto-sleep a sense path we do not know). Portable + host-tested; the device
// binds it to the compile-time SOLIDE_BOARD slug (see include/nimbus_board_power.h).

namespace nimbus::power {

struct BoardBattMonDefault {
  const char* boardSlug;      // matches the -DSOLIDE_BOARD compile slug
  bool        battMonDefault; // battery monitoring enabled out of the box?
};

// One row per shipped board. Adding a board without a row here makes its default
// OFF (safe) - the host test pins the two shipped boards so this can't regress.
inline constexpr BoardBattMonDefault kBoardBattMonDefaults[] = {
    {"solide_s3", true},     // hand-built 2S, pack is part of the build
    {"freenove_s3", false},  // all-in-one, battery optional -> opt-in
};

constexpr bool boardSlugEq(const char* a, const char* b) {
  while (*a && *b) {
    if (*a != *b) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

// Explicit per-board default; unknown board -> false (OFF, the safe choice).
constexpr bool battMonDefaultForBoard(const char* slug) {
  if (!slug) return false;
  for (const auto& e : kBoardBattMonDefaults)
    if (boardSlugEq(slug, e.boardSlug)) return e.battMonDefault;
  return false;
}

// The battery divider the ADC actually applies (CUM-370). A fixed-divider board
// (an all-in-one carrier with an ONBOARD resistor divider) uses the board map's
// fixed value; a hand-built board's divider resistors vary per unit, so the
// owner-set NVS value is authoritative. Pure so the /api/state report and the ADC
// begin() share ONE selection and can never disagree about which divider is in
// use - the field is documented as self-describing for host-side correction, so a
// mismatch mis-scales voltages. The device binds boardHasFixedDivider to the board
// map (include/nimbus_board_batt.h).
constexpr uint16_t effectiveDividerX100(bool boardHasFixedDivider,
                                        uint16_t boardDividerX100,
                                        uint16_t storeDividerX100) {
  return boardHasFixedDivider ? boardDividerX100 : storeDividerX100;
}

// Clamp an owner battery cell-count override to what the board physically
// supports (CUM-371). A board's series-cell count is fixed by its pack wiring and
// sense divider; telling a 1S board it is 2S makes the effective cell count 2, and
// the ADC plausibility gate then reads a real 1S ~4200 mV pack as an implausible
// ~2100 mV/cell "2S" and rejects EVERY sample: monitoring on, yet every reading
// invalid.
//   requested  : 0 = board default, else the desired series-cell count.
//   boardCells : the board's physical series-cell count (its ceiling).
// Returns the override to persist: 0 (board default) or 1..boardCells. A request
// above the board ceiling is clamped down (a 1S board cannot be told it is 2S); a
// genuine 2S board still accepts 2.
constexpr uint8_t clampBattCellsOverride(int requested, int boardCells) {
  const int ceil = boardCells > 0 ? boardCells : 1;
  if (requested <= 0) return 0;
  return uint8_t(requested > ceil ? ceil : requested);
}

}  // namespace nimbus::power
