#pragma once
#include <cstdint>

// Panel-heal decision - the PURE core of the TFT watchdog's "should I re-arm and
// repaint?" policy (device glue: src/hw/tft_out.cpp). No Arduino, no SPI, so the
// white-screen regression class is pinned by a host test that drives the SAME
// function the device runs - not a mirror kept in lockstep by hand.
//
// Why this exists as its own module (CUM-167/CUM-231 white-screen class):
// the ILI9341 can silently lose its state (a brownout on its rail, ESD, a
// setup-AP beacon knocking it to SLPIN) and sit WHITE while every register still
// reads back correct. Two failure modes, and the register check (healthy()) sees
// only one:
//   (a) CONFIG loss - MADCTL reverts toward power-on; healthy() catches it.
//   (b) PIXEL/SLEEP loss - registers stay correct, glass is blank; INVISIBLE to
//       healthy(). Observed in the field: correct framebuffer, hal.display true,
//       glass white until a restart.
// The load-bearing invariant, and the exact thing that regressed before CUM-231:
// past the trust window the panel is ALWAYS re-armed. healthy() must NEVER gate
// the re-arm - it only upgrades the response from "repaint" to "reconfigure then
// repaint". An earlier build returned early when healthy() read true (to skip a
// "needless" blit); that made mode (b) unrecoverable. This module makes that
// early-return impossible to reintroduce without failing test_panel_heal.

namespace nimbus {
namespace panel {

// What the watchdog should do this cycle.
//   rearm   - re-send SLPOUT + DISPON + pixel-format + access-order (reset-free,
//             microseconds, a no-op on a live panel). Wakes a slept-but-configured
//             panel that healthy() reports as fine.
//   repaint - push the frame unconditionally, past the dirty gate.
struct HealAction {
  bool rearm = false;
  bool repaint = false;
};

// Dirty-gate branch: an UNCHANGED frame is trusted only for healWindowMs. Within
// the window nothing happens (the caller returns "unchanged" before it even reads
// the panel). Past it the panel is re-armed UNCONDITIONALLY; the config readback
// only decides whether to ALSO repaint (a confirmed config loss). configOk is
// meaningful only past the window - within it the device never pays for the read.
constexpr HealAction unchangedFrameAction(uint32_t sinceLastPushMs,
                                          uint32_t healWindowMs, bool configOk) {
  if (sinceLastPushMs < healWindowMs) return {false, false};
  return {/*rearm=*/true, /*repaint=*/!configOk};
}

// Periodic watchdog (tickHealth): once the window elapses the panel is ALWAYS
// re-armed AND repainted. The register/content probe classifies what happened for
// the counters; it never gates whether we act - that is the whole point of the
// unconditional repaint (it is the only cover for the undetectable pixel-loss
// mode). Within the window there is nothing to do.
constexpr HealAction tickHealthAction(uint32_t sinceLastHealthMs,
                                      uint32_t healWindowMs) {
  if (sinceLastHealthMs < healWindowMs) return {false, false};
  return {/*rearm=*/true, /*repaint=*/true};
}

}  // namespace panel
}  // namespace nimbus
