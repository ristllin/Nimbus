#pragma once
#include <string>
#include <vector>

#include "nimbus/attention.h"
#include "nimbus/epd_render/screens.h"   // ScreenCtx - ONE shared data model
#include "nimbus/tft_render/fb565.h"

// ============================================================================
// tft_render/screens - the colour touch UI.
//
// It reads the SAME nimbus::epd::ScreenCtx the e-ink renderer reads. That is
// the whole design: one data model, two presentations. The e-ink renderer is
// frozen (its goldens must stay byte-identical), so this is a parallel
// renderer, not a refactor of it - a screen added to the device populates one
// context and both surfaces can draw it.
//
// Two differences from the e-ink side, both forced by the hardware:
//   - PORTRAIT 240x320 with colour, versus landscape 296x128 1-bit. The layouts
//     are therefore genuinely different, not scaled.
//   - There is NO KNOB (the panel consumes the encoder pins), so every screen
//     also emits TAP REGIONS. The renderer owns them because it owns the
//     layout - "where is that button" must not be computed twice.
// ============================================================================

namespace nimbus::tft {

// A rendered screen: pixels + what can be tapped on them.
struct Rendered {
  std::vector<TapRegion> taps;

  // On-screen ring bounding box on the panel (ringless-board Notifier screen), so
  // the device loop can push ONLY that rectangle at animation cadence instead of
  // the whole 320x240 frame. w == 0 means this screen has no on-screen ring.
  int16_t ringX = 0, ringY = 0, ringW = 0, ringH = 0;

  const TapRegion* hit(int x, int y) const {
    // Last match wins: regions are emitted back-to-front, so a control drawn
    // over a card claims the tap rather than the card underneath it.
    const TapRegion* found = nullptr;
    for (const auto& r : taps)
      if (r.contains(x, y)) found = &r;
    return found;
  }
};

// Draw `screen` into `fb` (cleared first) and return its tap regions.
// Every attn::ScreenId is handled; unknown ids fall back to the status screen
// so the panel is never left blank.
Rendered renderScreen(Fb565& fb, attn::ScreenId screen, const epd::ScreenCtx& ctx);

// Map a wire status (solide::ring::Status) to the screen's status tone. Kept
// here so the card tint and the ring's statusStyle() role stay in agreement.
StatusTone toneFor(uint8_t status);

}  // namespace nimbus::tft
