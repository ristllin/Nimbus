# `docs/hardware/`

Hardware documentation for the Nimbus device (Solide S3 board). Start at the
[hardware reference](../hardware.md) - it covers everything common to both boards and
links into the two configurations.

Nimbus ships in **one of two display/input configurations** (same firmware, selected by
the NVS `screenModel` setting):

| File | Configuration |
|---|---|
| [`eink-knob.md`](eink-knob.md) | **E-paper + knob** (`screenModel=eink`) - the default, shipped build |
| [`touch-tft.md`](touch-tft.md) | **Touch TFT** (`screenModel=tft`) - color ILI9341 + XPT2046 touch |

Building one from parts? Start with the parts list ([`bom.md`](bom.md)) and
the step-by-step build guides:
[`build-eink.md`](build-eink.md) (e-paper + knob) · [`build-tft.md`](build-tft.md) (touch TFT).

## Folder layout

| Path | Contents |
|---|---|
| `../hardware.md` | Top-level reference: board configurations + everything shared (first flash, peripherals, power, battery) |
| `eink-knob.md`, `touch-tft.md` | The two per-configuration pages (pinout, config-specific pins, notes) |
| [`bom.md`](bom.md) | Consolidated bill of materials (both configurations, prices, sourcing links) |
| `build-eink.md`, `build-tft.md` | The two step-by-step build guides |
| [`diagrams/`](diagrams/) | Pinout SVGs (`pinout-eink.svg`, `pinout-tft.svg`), block-level wiring SVGs (`wiring-eink.svg`, `wiring-tft.svg`), the ring status-language drawing (`ring-status-language.svg`, embedded by `../led-ux.md`), and the touch-UI screenshots (`tft-*.png`) |
| [`hardware/fab/`](https://github.com/ristllin/Nimbus/tree/main/hardware/fab) | PCB manufacturing outputs (Gerbers, ODB++), now at the repo top level - archival, not needed to build or use the firmware |
