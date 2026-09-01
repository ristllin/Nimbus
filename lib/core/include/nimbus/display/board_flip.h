#pragma once

// board_flip - the EXPLICIT per-board display-orientation base (CUM-189).
//
// The colour panel driver is board-agnostic: solide::display_tft::setFlip(false) is
// its "default landscape", and which end is physically up "cannot be detected in
// software" (solide/display_tft.h) - it is the caller's to supply. The two shipping
// boards mount the panel 180 apart:
//
//   Solide S3                  - upright at the driver's default        -> base false
//   Freenove CYD / all-in-one  - panel mounted 180 from that            -> base true
//
// So a fresh Freenove rendered 180 UPSIDE-DOWN while a fresh Solide was upright, both
// at the same stored tftFlip=0 (CUM-189). This is the per-board BASE the firmware
// composes with the owner's tftFlip - the "flip 180 for an upside-down mount" delta,
// default 0. Effective flip handed to the driver = base XOR tftFlip. The owner's
// menu/web toggle stays a plain 0/1 delta from "correct as mounted"; the base makes
// "correct" actually upright per board.
//
// Keyed on the compile-time SOLIDE_BOARD slug (NOT the epd/pin table, which the e-ink
// cleanup may yet touch) so it cannot drift the way the old `epd.sck >= 0` proxies
// did (see board_power.h, the sibling battery-default that made the same choice).
// Portable + host-tested; the device binds it in include/nimbus_board_flip.h.

namespace nimbus::display {

struct BoardFlipBase {
  const char* boardSlug;   // matches the -DSOLIDE_BOARD compile slug
  bool        baseFlip;    // driver flip for an UPRIGHT panel at tftFlip=0
};

// One row per shipped board. Adding a board without a row makes its base false (the
// driver's default landscape); the host test pins the two shipped boards so this
// can't regress, and a new board makes a deliberate choice by adding its measured row.
inline constexpr BoardFlipBase kBoardFlipBases[] = {
    {"solide_s3", false},    // hand-built TFT variant, panel upright at default
    {"freenove_s3", true},   // all-in-one panel mounted 180 from the Solide
};

constexpr bool flipSlugEq(const char* a, const char* b) {
  while (*a && *b) {
    if (*a != *b) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

// Explicit per-board base; unknown board -> false (the driver's default landscape).
constexpr bool baseFlipForBoard(const char* slug) {
  if (!slug) return false;
  for (const auto& e : kBoardFlipBases)
    if (flipSlugEq(slug, e.boardSlug)) return e.baseFlip;
  return false;
}

// The flip to hand the driver's setFlip(): the per-board base composed with the
// owner's stored "flip 180" preference. Kept here (not the device header) so the
// composition itself is host-tested, not just the base.
constexpr bool effectiveFlipForBoard(const char* slug, bool userFlip) {
  return baseFlipForBoard(slug) ^ userFlip;
}

}  // namespace nimbus::display
