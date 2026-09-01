#pragma once

// screen_model - resolving the stored scrModel value to what the owner sees.
//
// The colour panel is the ONLY supported display (e-ink was removed in v4.4). The
// scrModel NVS key is frozen with "tft" as the supported value. Two rules, pinned
// host-side so a fresh / NVS-erased device can never regress into a false alarm
// (CUM-189):
//
//   - ABSENT / erased key -> the colour panel, SILENTLY. A factory-fresh or wiped
//     unit set nothing, so there is nothing to warn about. This is the exact state
//     the owner QAs on first boot, and it must NOT surface a misconfiguration page.
//   - an EXPLICIT legacy value ("eink", or any other non-"tft" string) -> the colour
//     panel PLUS the honest "unsupported display setting" migration notice, for a
//     real e-ink unit whose owner is moving onto the colour panel.
//
// The distinction is ABSENT vs EXPLICIT, which the plain screenModel() getter loses
// (it collapses absent to a default). The boot seam (src/main.cpp) therefore reads
// the RAW stored value and calls this, so the device and the host test share one
// class rule instead of each re-deriving it. Portable, no Arduino, host-tested.

namespace nimbus::display {

// Byte-equality without <cstring> so this stays usable in a constexpr context.
constexpr bool scrModelEq(const char* a, const char* b) {
  while (*a && *b) {
    if (*a != *b) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

// The supported, colour-panel value of the frozen scrModel key.
inline constexpr const char* kScreenModelTft = "tft";

// Show the "unsupported display setting" notice? Only for an EXPLICIT, non-"tft"
// stored value. An absent / empty value is a fresh device and stays silent.
constexpr bool showsUnsupportedNotice(const char* storedScrModel) {
  if (!storedScrModel || !*storedScrModel) return false;   // absent -> silent
  return !scrModelEq(storedScrModel, kScreenModelTft);      // explicit legacy -> notice
}

}  // namespace nimbus::display
