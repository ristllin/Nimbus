# `docs/hardware/`

Hardware documentation for the Nimbus device. Start at the
[hardware reference](../hardware.md) - it covers everything common across the boards
and links into the configurations.

Nimbus runs in **one of three display/input configurations**:

| File | Configuration |
|---|---|
| [`eink-knob.md`](eink-knob.md) | **E-paper + knob** (`screenModel=eink`) - the default, shipped build |
| [`touch-tft.md`](touch-tft.md) | **Touch TFT** (`screenModel=tft`) - color ILI9341 + XPT2046 resistive touch |
| [`all-in-one-cyd.md`](all-in-one-cyd.md) | **All-in-one (Freenove CYD)** (`SOLIDE_BOARD=freenove_s3`, `screenModel=tft`) - one off-the-shelf module, capacitive touch, lowest effort |

The first two are hand-built on the Solide S3 board and picked by the NVS
`screenModel` setting; the third is a separate off-the-shelf board with its pinout
fixed at flash time.

Building one from parts? Start with the parts list ([`bom.md`](bom.md)) and
the step-by-step build guides:
[`build-eink.md`](build-eink.md) (e-paper + knob) · [`build-tft.md`](build-tft.md) (touch TFT).
The all-in-one needs no build guide - see its [pinout & happy-path page](all-in-one-cyd.md).

## Folder layout

| Path | Contents |
|---|---|
| `../hardware.md` | Top-level reference: board configurations + everything shared (first flash, peripherals, power, battery) |
| `eink-knob.md`, `touch-tft.md`, `all-in-one-cyd.md` | The per-configuration pages (pinout, config-specific pins, notes) |
| [`bom.md`](bom.md) | Consolidated bill of materials (all configurations, prices, sourcing links) |
| `build-eink.md`, `build-tft.md` | The two step-by-step build guides |
| [`diagrams/`](diagrams/) | Pinout SVGs (`pinout-eink.svg`, `pinout-tft.svg`), block-level wiring SVGs (`wiring-eink.svg`, `wiring-tft.svg`), the ring status-language drawing (`ring-status-language.svg`, embedded by `../led-ux.md`), and the touch-UI screenshots (`tft-*.png`) |
| [`hardware/fab/`](https://github.com/ristllin/Nimbus/tree/main/hardware/fab) | PCB manufacturing outputs (Gerbers, ODB++), now at the repo top level - archival, not needed to build or use the firmware |
