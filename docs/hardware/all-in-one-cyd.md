# Configuration B - all-in-one (Freenove CYD, `screenModel = tft`)

The lowest-effort way to run Nimbus: a single off-the-shelf module with the
screen, touch, microSD, mic, speaker, and an RGB LED already on one board. There
is nothing to wire and nothing to solder. Buy one part, plug in USB-C, flash, and
it is a Nimbus.

The board is the **Freenove ESP32-S3 Display**, model **FNK0104B** (Amazon
`B0FSQF6FKN`): a 2.8" **ILI9341** 240×320 IPS **capacitive** touchscreen, an
**ESP32-S3 N16R8** (16 MB flash, 8 MB PSRAM), on-board microSD, mic and speaker
through an **ES8311** codec, a single WS2812 RGB LED, and USB-C. It runs the same
firmware as the hand-built boards, selected at flash time by
`SOLIDE_BOARD=freenove_s3`, and reports `screenModel = tft`.

This board is a **compile-time identity**: its pinout ships baked into a dedicated
firmware image, so there is no display setting to choose and no risk of a
mis-selected pin map. The Freenove comes in more than one panel size, and each
size is its own firmware image compiled at that panel's resolution (see
[Supported panels](#supported-panels)). For the hand-built boards and everything
shared across the lineup (power, battery sensing, the operating modes), see the
[hardware reference](../hardware.md).

## Supported panels

The Freenove all-in-one ships in three panel sizes. The UI renderer is
parameterized by resolution, so one code path draws every size; each size is a
separate OTA type and firmware image so a board is only ever offered an image
that matches its glass.

| Size | Panel | Resolution | OTA type | Firmware env | Verification |
|---|---|---|---|---|---|
| 2.8" | ILI9341 | 320×240 | `freenove-28` | `esp32s3-cyd` | **Hardware-verified** - display first-light and touch validated on the bench board. |
| 3.5" | ILI9488 | 480×320 | `freenove-35` | `esp32s3-cyd-35` | **Host-verified only** - renderer, goldens, and firmware image proven on the host; no panel owned. |
| 4.0" | 480×480 class | 480×480 | `freenove-40` | `esp32s3-cyd-40` | **Host-verified only** - renderer, goldens, and firmware image proven on the host; no panel owned. |

**What "host-verified only" means.** The firmware for the 3.5" and 4.0" sizes
compiles, links, and passes its own per-size golden framebuffers
(`test/golden_tft/480x320/`, `test/golden_tft/480x480/`), so the layout is proven
to reflow correctly at those resolutions. It has **not** run on a physical 3.5" or
4.0" panel, and the `solide-drivers` display driver still initializes the 2.8"
geometry - so bringing a larger panel up on hardware needs the matching driver
init (panel controller, resolution, orientation) added when a panel is in hand.
The resolution assumed for each size is the standard for that panel class and is
confirmed against hardware before any device-verified claim is made. Release notes
carry the same host-verified labels.

## Happy path - buy one, flash, done

1. **Buy the module.** One part: the Freenove ESP32-S3 Display 2.8" (FNK0104B),
   about $20-25. Nothing else is required for a desk-powered unit. See the
   [bill of materials](bom.md#configuration-b---all-in-one-freenove-cyd).
2. **Plug in USB-C.** Connect the board to your computer with a data-capable
   USB-C cable. The board has a single USB-C port, so there is no wrong port to
   pick.
3. **Flash the firmware.** Use the browser flasher (any Chromium-based browser,
   nothing to install) or the command-line path, and choose the all-in-one
   (Freenove) build. Full steps: [Flash the firmware](../quick-start/flash.md).
4. **Set it up.** The device brings up its setup network on first boot. Follow
   [Set up the device](../quick-start/setup-wizard.md) to join Wi-Fi and, for
   Orchestrator mode, add a provider key.

That is the whole build. Touch is capacitive and self-calibrating, so there is no
touch calibration step, and there is nothing to assemble.

## Pinout

This pinout is fixed in firmware (`SOLIDE_BOARD=freenove_s3`). It is here for
reference and bring-up, not for wiring: on an all-in-one there is nothing to
connect.

### Display and touch

| Peripheral | Bus | Pins | Notes |
|---|---|---|---|
| **Display** ILI9341, 2.8" 240×320 | SPI | SCLK 12 · MOSI 11 · MISO 13 · CS 10 · DC 46 · BL 45 · RST tied to board reset | BGR color order, 40 MHz. |
| **Touch** FT6336U (capacitive) | I²C | SDA 16 · SCL 15 · INT 17 · RST 18 | I²C address 0x38. Reports pixel coordinates and is self-calibrating - no touch calibration needed. |

### Audio, storage, LED, and battery

| Peripheral | Bus | Pins | Notes |
|---|---|---|---|
| **Audio** ES8311 codec (mic + speaker) | I²S + I²C | MCLK 4 · BCLK 5 · WS 7 · data-out (speaker) 8 · data-in (mic) 6 | I²C shared with touch (SDA 16 / SCL 15), address 0x18. One codec handles both record and playback. |
| **microSD** | SDMMC 4-bit | CLK 38 · CMD 40 · D0 39 · D1 41 · D2 48 · D3 47 | On-board slot. Format **FAT32** - over-32 GB cards ship exFAT and will not mount without a reformat. |
| **RGB LED** single WS2812 | RMT | DIN 42 | One pixel, not a ring (see below). |
| **Battery sense** | ADC | GPIO 9 | Divide-by-2 divider for a single-cell (1S) Li-ion pack, if a battery is fitted. |

## Notes

- **No LED ring.** The board has a single RGB LED, not the 45-pixel ring of the
  hand-built boards. So the **Notifier** status ring is drawn on the screen
  instead: each active session gets an arc in the same colors and animations the
  physical ring would show. The battery mode still dims it. See
  [The screen on a board with no ring](../modes-and-signals.md#the-screen-on-a-board-with-no-ring).
- **Capacitive touch, self-calibrating.** The FT6336U reports pixel coordinates
  directly, so the resistive-touch calibration used on the hand-built Touch TFT
  does not apply here and is not shown for this board.
- **One I²C bus.** Touch and the audio codec share the same I²C pins (SDA 16 /
  SCL 15) at different addresses.
- **Audio through one codec.** The ES8311 handles the microphone and the speaker
  together, in place of the separate I²S mic and amp on the hand-built boards.
- **Backlight is a continuous draw**, as on any color panel: the idle path blanks
  it rather than drawing a screensaver. Battery mode sets its lit level - see
  [The screen backlight](../modes-and-signals.md#the-screen-backlight).
