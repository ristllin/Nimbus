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

}  // namespace nimbus::power
