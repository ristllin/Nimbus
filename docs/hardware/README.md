# `docs/hardware/`

Hardware documentation for the Nimbus device. Start at the
[hardware reference](../hardware.md) - it covers everything common across the boards
and links into the configurations.

Nimbus runs in **one of two display configurations**:

| File | Configuration |
|---|---|
| [`touch-tft.md`](touch-tft.md) | **Touch TFT** (`screenModel=tft`) - color ILI9341 + XPT2046 resistive touch, hand-built on the Solide S3 board |
| [`all-in-one-cyd.md`](all-in-one-cyd.md) | **All-in-one (Freenove CYD)** (`SOLIDE_BOARD=freenove_s3`, `screenModel=tft`) - one off-the-shelf module, capacitive touch, lowest effort |

The first is hand-built on the Solide S3 board; the second is a separate
off-the-shelf board with its pinout fixed at flash time.

Building one from parts? Start with the parts list ([`bom.md`](bom.md)) and
the step-by-step build guide: [`build-tft.md`](build-tft.md) (touch TFT).
The all-in-one needs no build guide - see its [pinout & happy-path page](all-in-one-cyd.md).

## Folder layout

| Path | Contents |
|---|---|
| `../hardware.md` | Top-level reference: board configurations + everything shared (first flash, peripherals, power, battery) |
| `touch-tft.md`, `all-in-one-cyd.md` | The per-configuration pages (pinout, config-specific pins, notes) |
| [`bom.md`](bom.md) | Consolidated bill of materials (all configurations, prices, sourcing links) |
| `build-tft.md` | The step-by-step build guide |
| [`diagrams/`](diagrams/) | Pinout SVG (`pinout-tft.svg`), block-level wiring SVG (`wiring-tft.svg`), the ring status-language drawing (`ring-status-language.svg`, embedded by `../led-ux.md`), and the touch-UI screenshots (`tft-*.png`) |
| [`hardware/fab/`](https://github.com/ristllin/Nimbus/tree/main/hardware/fab) | PCB manufacturing outputs (Gerbers, ODB++), now at the repo top level - archival, not needed to build or use the firmware |
