#pragma once

// Panel-readback policy for shared-bus boards (CUM-392).
//
// On a board where a RESISTIVE touch controller (XPT2046) reports its coordinates
// on the SAME MISO line the display controller is read back on, reading a panel
// register (RDDST healthy()) or a pixel (RAMRD readPixel) drives the panel's SDO
// onto that shared line. A module whose SDO does not cleanly tri-state then
// contends the line the XPT2046 drives, and every touch ADC channel comes back
// pinned near mid-scale: touch reads a press everywhere and a position nowhere.
// That is the CUM-392 signature on the classic solide_s3 board, and it is
// UNIT-DEPENDENT: it appears only on modules whose ILI9341 does not release SDO,
// which is why a bench unit can read the panel all day and never show it.
//
// ⚠ Panel reads are NOT new in v4.5.x. v4.4.6 already read the panel: healthy() is
// evaluated inside renderAndPush's unchanged-frame branch (at most every ~5 s, and
// only while something is rendering), and panelConfigOk/panelContentOk read on
// demand under the register probe. Field touch worked on the affected units with
// those reads present, so the PER-PUSH read is field-proven safe on shared-MISO
// boards. What v4.5.0 ADDED is pollControllerLiveness - a panel RDDST read every
// ~2 s from loop() that runs INDEPENDENT of rendering, so the shared MISO is now
// driven at idle moments v4.4.6 never touched (the FIX-4 XPT2046 poll landed beside
// it). Those additions were only ever validated on a Freenove, whose capacitive
// touch shares nothing with the panel bus.
//
// So this gate removes ONLY the new render-independent liveness poll on a shared-MISO
// resistive board, LITERALLY restoring v4.4.6's panel-read set (the per-push
// healthy() read stays, and its result feeds the liveness verdict so the CUM-388
// wrong-variant guardrail still latches). It is provably inert on a capacitive /
// separate-bus board, where the poll never shared a line and keeps running.
//
// This is the PORTABLE decision. It is pure (no Arduino, host-tested) and takes
// board facts as primitives so it does not reach into the solide board struct -
// the device binding in include/nimbus_board_touch_bus.h supplies the facts from
// solide::board(). The whole fix hangs off ONE predicate so every read site shares
// it and a new board is covered by construction (see test/test_panel_read_policy).

namespace nimbus::display {

// True when the panel is read back on a MISO a resistive touch controller also
// drives. touchIsResistiveSpi: board().touchKind == ResistiveSpi. panelMiso:
// board().tft.miso (the panel's readback line; -1 = the panel is write-only).
// touchCs: board().tft.tcs (the XPT2046 chip select; -1 = no SPI touch fitted).
// A resistive board with no panel MISO cannot read the panel at all, and a board
// with no touch CS has no resistive controller on the line, so both are safe.
constexpr bool sharedMisoResistiveTouch(bool touchIsResistiveSpi, int panelMiso,
                                        int touchCs) {
  return touchIsResistiveSpi && panelMiso >= 0 && touchCs >= 0;
}

// Panel register/pixel readbacks are electrically safe only where they cannot
// contend a resistive touch MISO. Inert-true on a capacitive or separate-bus
// board, which is why the fix is provably a no-op on freenove_s3 / Lumi.
constexpr bool panelReadbackSafe(bool touchIsResistiveSpi, int panelMiso,
                                 int touchCs) {
  return !sharedMisoResistiveTouch(touchIsResistiveSpi, panelMiso, touchCs);
}

// Runtime override for the diagnostic A/B (PROBES, test envs only). Default keeps
// the capability gate; ForceOn re-enables the reads to REPRODUCE the fault on one
// image; ForceOff silences every read for a clean baseline. Production never sets
// anything but Default, so production behaviour is exactly panelReadbackSafe().
enum class PanelReadOverride : int { Default = -1, ForceOff = 0, ForceOn = 1 };

// The single decision every panel-read site consults: may a panel readback run?
constexpr bool panelReadAllowed(bool touchIsResistiveSpi, int panelMiso,
                                int touchCs, PanelReadOverride ov) {
  return ov == PanelReadOverride::ForceOn    ? true
         : ov == PanelReadOverride::ForceOff ? false
                                             : panelReadbackSafe(
                                                   touchIsResistiveSpi, panelMiso,
                                                   touchCs);
}

}  // namespace nimbus::display
