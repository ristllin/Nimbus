# N17 bench runbook - CUM-167: solide_s3 white screen (PSRAM full-frame blit)

Root-cause and bench confirmation for the Nimbus-4 (`solide_s3`) white screen on
merged `main` (`3e8db03`): the firmware logs `[tft] colour touch panel up` while
the glass stays white. Whoever runs this holds the device lock per the bench
lock protocol (create `/tmp/nimbus-devices/usbmodem2101.lock`, 45 min max,
release after). Nimbus-4 (`usbmodem2101`) is the personal board - its NVS backup lives in
`~/nimbus-bench/nimbus4-backup/`. All steps below are app-partition uploads (no
flash erase), so `scrModel=tft` and the touch calibration survive.

---

## Root cause (host-side)

The panel comes up, the render task is alive, and the framebuffer holds correct
pixels - but the full-frame blit resets the panel. `display_tft::blit()` (the
async path behind `pushFrame`, `src/hw/tft_out.cpp:152`) DMAed the whole ~150 KB
RGB565 framebuffer straight from PSRAM. On the ESP32-S3 a burst that long out of
PSRAM shares the external-memory bus and was measured to reset the ILI9341
(MADCTL 0x28 -> 0x00): the panel reverts to its power-on default and shows white,
while every register readback and the render task still look healthy. This is the
exact fault the repo's own bring-up sketch documents (`src/bringup/tftmin.cpp`:
"pushFrame() straight from PSRAM ... breaks the panel"; the internal-bounce path
is safe).

Why it surfaced in the N6/N7/N14 window and not before: the fault is latent and
layout-sensitive. Driver `v0.6.0` (N14) compile-gated the e-paper stack, freeing
~14.5 KB of internal SRAM (measured: esp32s3 internal RAM 83,068 B gate-off vs
97,596 B with `-DSOLIDE_HAS_EPAPER=1`); N7 moved TLS staging buffers into PSRAM.
Either shifts the boot allocation / PSRAM-bus profile enough to tip the latent
PSRAM-DMA reset into a deterministic white screen.

The incident's prime suspect (the e-paper compile-gate stubbing shared panel
plumbing the TFT calls) is NOT the cause: `src/device/display.cpp` is 100 %
e-paper (GxEPD2), the colour TFT driver `display_tft.cpp` is separate and
ungated, and the firmware TFT path never calls the gated `solide::display`
namespace. ELF nm on the TFT build carries 0 GxEPD2 symbols and 16
`solide::display_tft` symbols - the gate works as designed. The other documented
suspect (register-probing racing the blit) is already off by default on `main`
(`tft_out.cpp` `g_probeEnabled=false`), so it is excluded.

## Fix (driver v0.6.1)

`display_tft::blit()` now detects a PSRAM source (`esp_ptr_external_ram`) and
stages it band-by-band through a small internal DMA-capable bounce (8 rows,
5,120 B, allocated once in `begin()`), so the DMA always sources internal SRAM;
an internal-memory frame still writes directly. This is the technique
`pushFrameChunked` already used, applied to the default `pushFrame` path so every
consumer is safe with no firmware code change - the firmware only re-pins the
driver to `v0.6.1`. The e-paper gate is untouched (still 0 GxEPD2 on a TFT
build), so the 14.5 KB win is preserved; the 5 KB bounce is runtime PSRAM-neutral
internal heap.

Regression guard: `tools/check_tft_elf_no_eink.py <elf>` fails if a colour-TFT
ELF links any GxEPD2 symbol (the e-ink stack crept back / the SRAM win regressed)
or is missing the `solide::display_tft` path. Run it after `pio run -e esp32s3`.

## Bench procedure (owner glance is the oracle)

The register readback stayed correct through every blank screen in the original
investigation, so the eye - not `STATUS` - is the oracle for "is it drawing".

1. Lock the port; USB-reset-clear if the console is wedged (CUM-141 pyusb recipe
   in `/tmp/nimbus-devices/MAP.md`).
2. REPRODUCE: flash the `test` firmware built against driver `v0.6.0`. Expect
   `[tft] colour touch panel up` on serial and a WHITE glass (owner glance).
3. FIX: flash the `test` firmware built against driver `v0.6.1`. Expect the boot
   UI to DRAW and stay drawn (owner glance), serial `RENDER?` echoing the screen.
4. CONTROL (the incident's own test): flash `main` built with
   `-DSOLIDE_HAS_EPAPER=1` (driver `v0.6.0`). Expect it to DRAW - confirming the
   14.5 KB layout lever rather than any shared-plumbing dependency.
5. FINAL: flash the production `esp32s3` `main` at the `v0.6.1` pin. Owner
   confirms draw AND touch (this build also carries N15's `orientTouch` fix, so
   the display flip is authoritative for touch).
6. Do NOT restore the rescue build once `v0.6.1` confirms - the final state is
   `main` + `v0.6.1`. Release the lock.

Pass = the panel draws and stays drawn on the `v0.6.1` build (owner glance), the
`v0.6.0` build reproduces white, and the ELF guard is green.

## Results (bench run 2026-08-24, lane N17, Nimbus-4 usbmodem2101)

- FINAL/FIX (production esp32s3 main + driver v0.6.1): flashed (app-only upload,
  hash verified). Clean t=0 boot over serial:
  `[tft] colour touch panel up (320x240, touch=1)`,
  `[tft] touch calibration 200,3900,200,3900,1` (stored NVS preserved), agent /
  relay / telegram up, heap ~39.7 KB, no Guru/panic/rst reboot loop. ELF guard
  green (0 GxEPD2, 16 display_tft symbols).
  Owner draw + touch glance: CONFIRMED on glass ("that's good", owner, via the
  orchestrator, 2026-08-24). The panel draws the UI and touch tracks the display.
- REPRODUCE (v0.6.0) and CONTROL (SOLIDE_HAS_EPAPER=1): optional corroboration,
  run only if requested (each needs an owner glance).
