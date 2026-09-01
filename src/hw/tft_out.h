#pragma once
#include <cstdint>

#include "nimbus/attention.h"
#include "nimbus/render_context.h"
#include "nimbus/tft_render/screens.h"

// tft_out - device glue from the portable RGB565 renderer to the ILI9341.
//
// The colour counterpart of epd_out. Two differences, both from the hardware:
//   - the framebuffer is 150 KB, so it lives in PSRAM (never the ~300 KB
//     internal SRAM - the same lesson as the earlier buffers and the SFX queue);
//   - the panel refreshes in ~31 ms rather than 2.2 s, so there is no dwell or
//     ghosting machinery. A dirty-gate is still worth it: it saves the SPI
//     traffic and, more importantly, keeps the shared bus free for touch.

namespace nimbus::hw::tft {

// Outcome of a render+push. ⚠ THREE states, not two: "the panel already shows
// this frame" is a SUCCESS, not a drop. Collapsing them into one bool made
// callers retain their needs-paint flag forever on an identical frame, which
// turned the dirty-gate into a hot loop (a full 240x320 compose plus a 150 KB
// memcmp every ~3 ms until an unrelated pixel moved).
enum class Push : uint8_t {
  Pushed,     // pixels went to the panel
  Unchanged,  // identical to what is already there - nothing to do, panel correct
  Dropped,    // NOT painted (driver down, busy, or FAULT screen) - retry later
};

// Render `screen` from `ctx` and push it. The tap regions from the render are
// retained on Pushed - hitTest() below queries them, so the input path never
// recomputes layout. On Unchanged the existing regions already describe this
// frame; on Dropped they still describe the older frame that IS on the panel.
Push renderAndPush(nimbus::attn::ScreenId screen, const nimbus::render::ScreenCtx& ctx);

// Repaint ONLY the on-screen ring rectangle (ringless-board Notifier screen) from
// the given StatusIdle context, at animation cadence. Composes the frame in RAM
// and region-pushes just the ring square via display_tft::pushRegion, so the ring
// animates smoothly without the ~31 ms full-frame blit. Returns false if not ready,
// busy, or the frame has no ring. MUST be called on the main task (see ring_out).
bool repaintRingRegion(const nimbus::render::ScreenCtx& ctx);

// The bytes of the frame currently ON THE PANEL (big-endian RGB565, kFbBytes),
// or null before the first push. This is the dirty-gate snapshot, i.e. what the
// glass is actually showing - not a fresh re-render, so a screenshot taken from
// it cannot disagree with the panel.
// Times the dirty gate expired and forced a repaint (the panel watchdog).
// Non-zero is NORMAL on an idle device - it is not an error count.
uint32_t healCount();
// Unconditional watchdog repaints. Distinct from healCount(): a heal means the
// panel's REGISTERS were wrong, a repaint happens every window regardless and is
// what covers the pixel-loss mode that no register reveals.
uint32_t repaintCount();
// Times the panel's OWN pixels were found to disagree with the frame we pushed.
// The one health signal that observes content rather than configuration - a
// panel can hold a perfect register set and still have lost its image.
uint32_t contentLostCount();
bool panelContentOk(int samples);
// Panel register probing, OFF by default. It reads the panel from the main task
// while the render task blits, and those reads were measured to return noise
// during blits - so it may be disturbing the panel rather than observing it.
void setProbeEnabled(bool on);
bool probeEnabled();
bool panelConfigOk();

// Periodic panel watchdog - MUST be called from loop() on a TFT board.
//
// ⚠ Without it the panel can never recover. The health check used to live only
// inside renderAndPush(), which an IDLE device never calls: with nothing to
// redraw, a panel that silently reset stayed blank indefinitely. Measured with
// the TFTBREAK drill: healthy=0 for 12 s and heals=0, i.e. the recovery path was
// unreachable exactly when it was needed.
//
// Returns true when the caller should repaint the current screen.
bool tickHealth(uint32_t now);

// Drop the identical-frame cache so the NEXT renderAndPush always blits, even if
// the composed pixels are byte-identical to the last frame. Used after a 180
// degree flip: setFlip re-maps MADCTL but the on-screen GRAM only rotates when a
// fresh frame lands, and a same-content frame would otherwise be skipped.
void forceRepaint();

const uint8_t* lastFrame();
size_t lastFrameBytes();

// The tap regions of the frame currently ON THE PANEL. Null when nothing has
// been rendered yet.
const nimbus::tft::Rendered* current();

// Resolve a panel coordinate to an action on the current frame.
const nimbus::tft::TapRegion* hitTest(int x, int y);

// Backlight, 0-100. On a TFT the backlight IS the idle draw, so the idle path
// blanks it instead of drawing a screensaver (that would cost MORE power).
void setBacklight(uint8_t pct);
uint8_t backlight();
// True when the backlight is genuinely driveable (a real PWM pin that attached).
// backlight() alone reports the REQUESTED level, which a panel with the backlight
// tied to 3V3 still echoes back - so deep-dim (backlight fully off) only arms
// where this is true, or a wake-tap could be swallowed on a still-lit screen.
bool backlightAttached();

bool begin();     // bring up the panel + touch; false if either fails
bool ready();

}  // namespace nimbus::hw::tft
