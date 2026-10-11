# Bill of materials

Every part needed to build a Nimbus, with sourcing links and approximate prices.
Use it before you order. There are two configurations: Configuration A is
hand-built on the Solide S3 board from the shared parts below plus a touch
display; Configuration B is a single off-the-shelf all-in-one module with
nothing else to source. All modules are commodity parts; the links are stable
store or search links, and any listing matching the **Module / chip** column
works.

Deeper board-level detail (the carrier PCB, per-pin wiring rationale) lives in
the board-support repository:
[solide-drivers](https://github.com/ristllin/solide-drivers) -
see its `docs/build.md` and `docs/hardware.md`.

Prices are approximate 2026 street prices in USD for AliExpress-class
sourcing, excluding shipping. Sourcing the branded parts from a US distributor
(Adafruit, SparkFun, DigiKey, Mouser) typically adds $15–25 to the total.

## Solide S3 build parts

| Qty | Part | Module / chip | Notes | Purchase | ~Price |
|---|---|---|---|---|---|
| 1 | Dev board | **ESP32-S3-DevKitC-1 N16R8** | 16 MB QIO flash, 8 MB octal PSRAM. The **N16R8** variant specifically - octal PSRAM occupies GPIO 33–37. Espressif's official store sells on AliExpress; also at [Mouser](https://www.mouser.com/c/?q=ESP32-S3-DevKitC-1-N16R8)/DigiKey. | [AliExpress](https://www.aliexpress.com/w/wholesale-esp32-s3-devkitc-1-n16r8.html) | $12 |
| 1 | Carrier PCB | **Custom Nimbus carrier PCB** *(recommended; optional)* | Sockets the DevKit and every module on one board instead of hand-wiring. Order the [`hardware/fab/`](https://github.com/ristllin/Nimbus/tree/main/hardware/fab) Gerbers from a board house ([how to order](https://github.com/ristllin/Nimbus/tree/main/hardware/fab#ordering-the-pcb)); hand-wiring on protoboard also works. | JLCPCB (Gerbers in repo) | $2–10 |
| 1 set | Headers | **2.54 mm female pin headers** | Socket the DevKit and modules onto the carrier PCB so each stays removable. | [AliExpress](https://www.aliexpress.com/w/wholesale-2.54mm-female-pin-header-kit.html) | $2 |
| 1 | LED ring | **WS2812B ring, 45 pixels** | Addressable RGB; 5 V power, 3.3 V logic. | [AliExpress](https://www.aliexpress.com/w/wholesale-ws2812b-led-ring.html) | $6 |
| 1 | Amp | **MAX98357A I²S amplifier** breakout | Class-D, built-in thermal and over-current protection. | [Adafruit 3006](https://www.adafruit.com/product/3006) · [AliExpress](https://www.aliexpress.com/w/wholesale-max98357a.html) | $2–6 |
| 1 | Speaker | **4 Ω · 3 W · ~40 mm** | The standard MAX98357A pairing. Any small 4 Ω 3 W full-range driver (28–45 mm) works. | [AliExpress](https://www.aliexpress.com/w/wholesale-40mm-4ohm-3w-speaker.html) | $2 |
| 1 | Mic | **INMP441** or **ICS-43434** I²S MEMS mic breakout | Separate breakout from the amp. **3.3 V only** - 5 V damages the S3. | [AliExpress](https://www.aliexpress.com/w/wholesale-inmp441-i2s.html) | $2 |
| 1 | Storage | **microSD module (SPI)** | 3.3 V SPI breakout. | [AliExpress](https://www.aliexpress.com/w/wholesale-micro-sd-card-module-spi.html) | $1 |
| 1 | microSD card | 16–32 GB | Must be formatted **FAT32** - over-32 GB cards ship exFAT and will not mount without a reformat. | any brand-name card | $5 |
| 2 | Cells | **18650 Li-ion** | Wired in **series** (2S). Buy brand-name cells from a reputable seller (see the safety note below). A 2S pack of 3500 mAh cells reads about 8.40 V when full. | [18650batterystore.com](https://www.18650batterystore.com/) · local vendor | $12 |
| 1 | Charger/protection | **2S BMS with USB-C charging** | Protection + balance + charging in one module. | [AliExpress](https://www.aliexpress.com/w/wholesale-2s-bms-usb-c-charging-board.html) | $3 |
| 1 | Regulator | **DC-DC buck converter → 5 V** | From the 2S pack (~6.0–8.4 V) to a clean 5 V bus, ≥2 A. | [AliExpress](https://www.aliexpress.com/w/wholesale-dc-dc-buck-converter-5v-3a.html) | $2 |
| 1 each | Battery sense | 220 kΩ + 100 kΩ resistors | Optional but recommended - the pack-voltage divider on GPIO 4. | any electronics supplier · [AliExpress kit](https://www.aliexpress.com/w/wholesale-resistor-kit-metal-film.html) | $1 |
| - | Misc | wire, protoboard/PCB, JST connectors; optional 330 Ω resistor (LED DIN) and 1000 µF capacitor (ring 5 V/GND) | The resistor and capacitor improve LED reliability on long runs. | [AliExpress](https://www.aliexpress.com/w/wholesale-jst-connector-kit-protoboard.html) | $5 |

**Shared subtotal: ≈ $57–69** (electronics only; enclosure, fasteners, and
pack-build consumables are listed separately below).

## Configuration A - Touch TFT

| Qty | Part | Module / chip | Notes | Purchase | ~Price |
|---|---|---|---|---|---|
| 1 | Display | **2.8" ILI9341 SPI TFT, 240×320**, with **XPT2046** resistive touch | The common SPI module with display pads (SCK/SDI/SDO/DC/RESET/CS/LED) plus a touch header (T_CLK/T_DIN/T_DO/T_CS/T_IRQ). Three on-module solder bridges are required - see the [build guide](build-tft.md#the-three-on-module-solder-bridges-do-this-first). | [AliExpress](https://www.aliexpress.com/w/wholesale-2.8-ili9341-spi-touch-xpt2046.html) | $8 |

The resistive touch layer is the only input in this configuration.

**Configuration A total: ≈ $65–77** · [Build guide →](build-tft.md)

## Configuration B - all-in-one (Freenove CYD)

The lowest-effort build: **one part, nothing to wire**. The screen, capacitive
touch, microSD, mic, speaker, and RGB LED are all on the module, so none of the
shared parts above are needed for a desk-powered unit.

| Qty | Part | Module / chip | Notes | Purchase | ~Price |
|---|---|---|---|---|---|
| 1 | All-in-one board | **Freenove ESP32-S3 Display 2.8" (FNK0104B)** | ESP32-S3 N16R8, 2.8" ILI9341 240×320 IPS capacitive touchscreen, on-board microSD, ES8311 mic + speaker, single RGB LED, USB-C. Runs the Nimbus firmware built for `SOLIDE_BOARD=freenove_s3`. | [Amazon B0FSQF6FKN](https://www.amazon.com/dp/B0FSQF6FKN) · [Freenove](https://freenove.com/) | $20–25 |

The only other item is a data-capable **USB-C cable** for flashing. An optional
printed case, Nimbus Light, is on the
[all-in-one page](all-in-one-cyd.md#enclosure-optional-3d-print).

**Configuration B total: ≈ $20–25** · [Pinout & happy-path build →](all-in-one-cyd.md)

## Safety note - 2S Li-ion

Two 18650 cells in series hold real energy.

- Buy brand-name cells from a reputable seller; counterfeits are common.
- Keep the **2S BMS** between the cells and everything else, and never charge
  the pack outside the BMS.
- Tie every ground together.
- If anything gets warm that should not, power off first and debug second. A hot
  SD card or module is an electrical fault.
- The firmware caps LED brightness at 60%. Sustained higher levels can overheat
  and damage the electronics, so do not raise this limit.

## Enclosure, fasteners, and assembly consumables

The parts beyond the electronics that turn the Configuration A modules into a finished unit. A
3D printer and a soldering iron are assumed; quantities are approximate. The
[build photos](build-photos.md) show every step.

| Qty | Part | Notes | Purchase | ~Price |
|---|---|---|---|---|
| 1 set | **3D-printed enclosure** | Print [`nimbus_classic_case.stl`](https://github.com/ristllin/Nimbus/blob/main/hardware/fab/nimbus_classic_case.stl) (GitHub renders it in a 3D viewer) in PLA or PETG. | self-print or a print service | $0–15 |
| ~10 | **M3 brass heat-set inserts** | Melted into the printed case with a soldering iron; screws thread into these. | [AliExpress](https://www.aliexpress.com/w/wholesale-m3-heat-set-insert.html) | $5 |
| assorted | **M3 machine screws** | A short-length assortment, plus a few **longer M3** screws for the microSD-module standoff. | [AliExpress](https://www.aliexpress.com/w/wholesale-m3-screw-assortment-kit.html) | $3 |
| 1 | **Rocker power switch** (SPST) | Cuts the pack to the DC-DC. Generic, ~20 x 12 mm panel cutout. | [AliExpress](https://www.aliexpress.com/w/wholesale-kcd1-rocker-switch.html) | $1 |
| - | **2S pack build materials** | The reference build spot-welds **nickel strip** to the cells (needs a spot welder), insulates with **fishpaper** + **Kapton tape**, and finishes the leads with spade/terminal connectors and silicone hookup wire. Without a spot welder, use pre-tabbed cells you solder, or a **2S 18650 holder with leads**. The [build guide](build-tft.md#build-the-2s-battery-pack) walks through the spot-welded reference pack. | [AliExpress](https://www.aliexpress.com/w/wholesale-18650-nickel-strip-fishpaper-kapton.html) | $3–8 |

**Enclosure + consumables subtotal: ≈ $12–32.** A spot welder, if you buy one
instead of using tabbed cells or a holder, is a separate one-time tool cost.

The [`hardware/fab/`](https://github.com/ristllin/Nimbus/tree/main/hardware/fab)
folder also holds the carrier-PCB manufacturing outputs (Gerbers, ODB++) and
[how to order the board](https://github.com/ristllin/Nimbus/tree/main/hardware/fab#ordering-the-pcb).
