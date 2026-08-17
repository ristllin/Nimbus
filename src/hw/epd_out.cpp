#include "hw/epd_out.h"

#include <Arduino.h>          // millis()
#include <esp_heap_caps.h>    // PSRAM-backed last-frame snapshot
#include <solide/display.h>

#include <cstring>            // memcmp / memcpy

#include "nimbus/fault.h"    // resilience: simulated e-ink screen fault
#include "../sys/agent_log.h"   // de-ghost observability (the red-plane fix's HIL seam)

namespace nimbus::hw {

// Idle ghost-clear: once the on-panel CONTENT has been unchanged for this long, the
// next push runs a real full-update waveform to wipe accumulated SSD1680 ghosting.
// Runtime-tunable via the "Ghost-clear interval" knob (Param::FullRefreshEveryN, now
// in MINUTES) -> setGhostClearMinutes(); default 15 min. This is the SOLE de-ghost
// trigger - owner decision 2026-07-15: the true waveform is an ~18 s freeze, tolerable
// only when idle, never mid-interaction (so the scheduler's render-count fullClear no
// longer drives it).
static uint32_t g_idleRefreshMs = 15u * 60u * 1000u;   // 15 min default
static bool     g_forceGhostClear = false;             // DEGHOST test seam

void setGhostClearMinutes(uint16_t minutes) {
  if (minutes < 1) minutes = 1;
  if (minutes > 120) minutes = 120;
  g_idleRefreshMs = uint32_t(minutes) * 60u * 1000u;
}

void forceGhostClearNextPush() { g_forceGhostClear = true; }

bool pushFramebuffer(const epd::Fb& fb, epd::Kind kind, bool fullClear) {
  // Resilience: FAULT screen suppresses the e-ink update - the compose/scheduler
  // pipeline keeps producing framebuffers, only the panel push is dropped, so the
  // device is exercised as if the display had failed.
  if (nimbus::fault::active(nimbus::fault::SCREEN)) return false;

  // Dirty-gate: the ~2.2 s panel flash has a finite lifetime, yet several producers
  // re-emit an IDENTICAL status frame - the battery-telemetry repaint tick (every
  // 60-300 s), the broker's ~30 s snapshot heartbeat, and header-glyph recompute.
  // Compare against the last frame ACTUALLY pushed and skip a redundant refresh.
  // One shared snapshot covers both the status g_fb and the menu g_menuFb, so a
  // menu-close repaint of an unchanged status still pushes (its last-pushed frame
  // was the menu). Color frames (rare, 3-plane) are never gated.
  // last frame ACTUALLY pushed - in PSRAM (another ~4.7 KB off the internal heap), so
  // the whole e-ink snapshot set lives in the idle 8 MB PSRAM (see main.cpp g_fb).
  static uint8_t* s_last = []() -> uint8_t* {
    uint8_t* p = static_cast<uint8_t*>(heap_caps_malloc(epd::kFbBytes, MALLOC_CAP_SPIRAM));
    if (!p) p = static_cast<uint8_t*>(heap_caps_malloc(epd::kFbBytes, MALLOC_CAP_INTERNAL));
    return p;
  }();
  static bool     s_have  = false;
  static uint32_t s_lastChangeMs = 0;      // when the on-panel CONTENT last changed
  const uint32_t now   = millis();
  const bool     color = (kind == epd::Kind::Color);
  const bool     identical =
      s_have && s_last && !color && std::memcmp(fb.data(), s_last, epd::kFbBytes) == 0;
  // stale = the SAME content has been on the panel for the idle interval -> due a
  // ghost-clear. Measured from the last CONTENT change (s_lastChangeMs), NOT the last
  // push: an identical re-emit (broker ~30 s heartbeat, battery tick) must not reset
  // this clock, or the failsafe would never mature (review-confirmed regression).
  const bool stale = s_have && uint32_t(now - s_lastChangeMs) >= g_idleRefreshMs;

  // Dirty-gate: skip a byte-identical frame UNLESS the idle failsafe is now due (that
  // one push runs the de-ghost). The scheduler's periodic fullClear does NOT force a
  // re-push here - a forced flash of identical content is both pointless (the fast LUT
  // does not clear ghosts) and, by reaching the clock update below, used to keep
  // resetting staleness so the true clear never fired.
  if (identical && !stale && !g_forceGhostClear) return false;
  (void)fullClear;   // scheduler's render-count clear no longer drives the panel;
                     // de-ghost is time-based (g_idleRefreshMs / setGhostClearMinutes).

  const bool fast = !color;
  // TRUE full-update waveform (the only thing that wipes SSD1680 ghosting), gated to
  // `stale && identical` - i.e. an IDLE re-emit of unchanged content (the battery-
  // telemetry repaint / broker heartbeat) after the interval. So the ~18 s freeze lands
  // on a frame nobody is waiting for (content is identical) and NEVER on a content
  // change: waking the device after a long idle gets a fast, already-de-ghosted panel,
  // not an 18 s crawl on the first interaction (review caveat, owner 2026-07-15).
  // The DEGHOST test seam forces one push down this exact path on demand.
  const bool doFullClear = fast && ((stale && identical) || g_forceGhostClear);
  g_forceGhostClear = false;   // one-shot, consumed (or dropped on a colour push)
  // Log every de-ghost: it is the ONE refresh that runs the OTP (red-compositing)
  // waveform, i.e. exactly the path of the "screen occasionally turns red" bug -
  // this line lets HIL/owners correlate a red incident with a de-ghost timestamp
  // and verify the driver-side red-plane blank (solide-drivers) actually ran.
  if (doFullClear)
    agent::alogf("epd: de-ghost full-update (idle %u min)",
                 (unsigned)(g_idleRefreshMs / 60000u));
  // Kind::Partial -> the SSD1680 differential mode: no invert flash (the menu's
  // flicker-free path). Chosen by the caller for churn-prone ambient screens
  // (StatusIdle during processing - owner: "heavy flickering"); the driver bounds
  // ghost accumulation with an internal every-10th-partial full frame, plus the
  // idle ghost-clear above. A de-ghost push always runs the true full waveform.
  const bool partial = fast && kind == epd::Kind::Partial && !doFullClear;
  // black plane = the framebuffer; no separate red plane (Nimbus screens are
  // 1-bit). Buffer is static/long-lived at the call sites, as GxEPD2 requires.
  solide::display::requestBitmap(fb.data(), nullptr, epd::kW, epd::kH, fast, doFullClear, partial);
  // Reached only on a real content change or the de-ghost push. Advancing the clock
  // here (not on skipped re-emits) makes `stale` measure "time since content changed",
  // and re-arms the 15-min timer right after a de-ghost.
  if (s_last) { std::memcpy(s_last, fb.data(), epd::kFbBytes); s_have = true; }
  s_lastChangeMs = now;
  return true;
}

}  // namespace nimbus::hw
