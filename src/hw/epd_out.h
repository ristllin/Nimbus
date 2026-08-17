#pragma once
#include "nimbus/epd_render/fb.h"
#include "nimbus/epd_sched.h"

// epd_out - device glue from the portable 1-bit framebuffer to the panel.
// nimbus::epd::Fb packs 1=black, MSB-first, 37-byte stride - exactly what
// solide::display::requestBitmap feeds to GxEPD2's drawBitmap(GxEPD_BLACK), so
// the buffer passes straight through with no repacking.

namespace nimbus::hw {

// Push a rendered framebuffer to the panel. kind selects the refresh path:
// FastBW/Partial -> fast B/W (~2.2 s), Color -> 3-colour (~18.5 s, black plane
// only for now). Non-blocking: the solide render task owns the SPI/timing.
//
// Dirty-gated: returns TRUE if the frame was actually pushed to the panel, FALSE
// if it was suppressed because it is byte-identical to the last frame pushed (or
// the SCREEN capability is fault-injected). Callers MUST honour a FALSE return -
// do not start a panel-busy window, and release the e-ink scheduler latch - else
// the FSM wedges. The comparison is against ONE shared "last pushed" snapshot, so
// it correctly repaints a status screen after a menu (different last frame).
// The TRUE full-update (ghost-clearing) waveform is driven internally by the idle
// failsafe (content unchanged for setGhostClearMinutes), NOT by the caller: the
// fullClear param is retained for call-site compatibility but no longer forces it (a
// mid-interaction 18 s freeze was intolerable - owner 2026-07-15).
bool pushFramebuffer(const epd::Fb& fb, epd::Kind kind, bool fullClear = false);

// Set the idle ghost-clear interval (minutes of unchanged on-panel content before a
// full-update de-ghost). Wired from Param::FullRefreshEveryN. Clamped to [1, 120].
void setGhostClearMinutes(uint16_t minutes);

// Force the NEXT pushFramebuffer down the de-ghost (TRUE full-update / OTP waveform)
// path, bypassing the dirty-gate and the idle clock. Test seam (`DEGHOST` console
// cmd): the OTP waveform is the one refresh that composites the 3-colour panel's
// red plane, i.e. the exact path of the "screen occasionally turns red" bug - this
// makes that path exercisable in seconds instead of a 15-minute idle wait.
void forceGhostClearNextPush();

}  // namespace nimbus::hw
