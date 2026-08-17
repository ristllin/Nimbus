# Bill of materials

Every part needed to build a Nimbus, consolidated across both display
configurations, with sourcing links and rough prices. The modules are all
commodity parts - the links below are stable store or search links; any
listing matching the **Module / chip** column works.

Deeper board-level detail (the carrier PCB, per-pin wiring rationale) lives in
the board-support repository:
[solide-drivers](https://github.com/ristllin/solide-drivers) -
see its `docs/build.md` and `docs/hardware.md`.

Prices are approximate 2026 street prices in USD for AliExpress-class
sourcing, excluding shipping. Sourcing the branded parts from a US distributor
(Adafruit, SparkFun, DigiKey, Mouser) typically adds $15–25 to the total.

## Shared parts (both configurations)

| Qty | Part | Module / chip | Notes | Purchase | ~Price |
|---|---|---|---|---|---|
| 1 | Dev board | **ESP32-S3-DevKitC-1 N16R8** | 16 MB QIO flash, 8 MB octal PSRAM. The **N16R8** variant specifically - octal PSRAM occupies GPIO 33–37. Espressif's official store sells on AliExpress; also at [Mouser](https://www.mouser.com/c/?q=ESP32-S3-DevKitC-1-N16R8)/DigiKey. | [AliExpress](https://www.aliexpress.com/w/wholesale-esp32-s3-devkitc-1-n16r8.html) | $12 |
| 1 | LED ring | **WS2812B ring, 45 pixels** | Addressable RGB; 5 V power, 3.3 V logic. | [AliExpress](https://www.aliexpress.com/w/wholesale-ws2812b-led-ring.html) | $6 |
| 1 | Amp | **MAX98357A I²S amplifier** breakout | Class-D, built-in thermal and over-current protection. | [Adafruit 3006](https://www.adafruit.com/product/3006) · [AliExpress](https://www.aliexpress.com/w/wholesale-max98357a.html) | $2–6 |
| 1 | Speaker | **4 Ω · 3 W · ~40 mm** | The standard MAX98357A pairing. Any small 4 Ω 3 W full-range driver (28–45 mm) works. | [AliExpress](https://www.aliexpress.com/w/wholesale-40mm-4ohm-3w-speaker.html) | $2 |
| 1 | Mic | **INMP441** or **ICS-43434** I²S MEMS mic breakout | Separate breakout from the amp. **3.3 V only** - 5 V damages the S3. | [AliExpress](https://www.aliexpress.com/w/wholesale-inmp441-i2s.html) | $2 |
| 1 | Storage | **microSD module (SPI)** | 3.3 V SPI breakout. | [AliExpress](https://www.aliexpress.com/w/wholesale-micro-sd-card-module-spi.html) | $1 |
| 1 | microSD card | 16–32 GB | Must be formatted **FAT32** - over-32 GB cards ship exFAT and will not mount without a reformat. | any brand-name card | $5 |
| 2 | Cells | **18650 Li-ion** | Wired in **series** (2S). Buy brand-name cells from a reputable vendor - see the safety note below. Reference pack: owner-measured 3500 mAh, 8.40 V full. | [18650batterystore.com](https://www.18650batterystore.com/) · local vendor | $12 |
| 1 | Charger/protection | **2S BMS with USB-C charging** | Protection + balance + charging in one module. | [AliExpress](https://www.aliexpress.com/w/wholesale-2s-bms-usb-c-charging-board.html) | $3 |
| 1 | Regulator | **DC-DC buck converter → 5 V** | From the 2S pack (~6.0–8.4 V) to a clean 5 V bus, ≥2 A. | [AliExpress](https://www.aliexpress.com/w/wholesale-dc-dc-buck-converter-5v-3a.html) | $2 |
| 1 each | Battery sense | 220 kΩ + 100 kΩ resistors | Optional but recommended - the pack-voltage divider on GPIO 4. | any electronics supplier · [AliExpress kit](https://www.aliexpress.com/w/wholesale-resistor-kit-metal-film.html) | $1 |
| - | Misc | wire, protoboard/PCB, JST connectors; optional 330 Ω resistor (LED DIN) and 1000 µF capacitor (ring 5 V/GND) | The resistor and capacitor improve LED reliability on long runs. | [AliExpress](https://www.aliexpress.com/w/wholesale-jst-connector-kit-protoboard.html) | $5 |

**Shared subtotal: ≈ $51–55**

## Configuration A - E-paper + knob *(default)*

| Qty | Part | Module / chip | Notes | Purchase | ~Price |
|---|---|---|---|---|---|
| 1 | Display | **WeAct 2.9" 3-color e-paper (SSD1680)**, 296×128 | B/W + red panel; the firmware drives it in fast B/W (~2.2 s refresh). | [WeAct Studio official store](https://weactstudio.aliexpress.com) · [AliExpress](https://www.aliexpress.com/w/wholesale-weact-2.9-epaper-ssd1680.html) | $9 |
| 1 | Knob | **EC11 rotary encoder** with push switch | 3-pin side + 2-pin side, five unmarked pins - see the [build guide](build-eink.md#known-traps). | [AliExpress](https://www.aliexpress.com/w/wholesale-ec11-rotary-encoder.html) | $1 |

**Configuration A total: ≈ $61–65** · [Build guide →](build-eink.md)

## Configuration B - Touch TFT

| Qty | Part | Module / chip | Notes | Purchase | ~Price |
|---|---|---|---|---|---|
| 1 | Display | **2.8" ILI9341 SPI TFT, 240×320**, with **XPT2046** resistive touch | The common SPI module with display pads (SCK/SDI/SDO/DC/RESET/CS/LED) plus a touch header (T_CLK/T_DIN/T_DO/T_CS/T_IRQ). Three on-module solder bridges are required - see the [build guide](build-tft.md#the-three-on-module-solder-bridges-do-this-first). | [AliExpress](https://www.aliexpress.com/w/wholesale-2.8-ili9341-spi-touch-xpt2046.html) | $8 |

There is no knob in this build - the touch layer is the input.

**Configuration B total: ≈ $59–63** · [Build guide →](build-tft.md)

## Safety note - 2S Li-ion

Two 18650 cells in series hold real energy. Buy brand-name cells from a
reputable vendor (counterfeits are common), always keep the **2S BMS** between
the cells and everything else, never charge the pack outside the BMS, tie
every ground together, and stop immediately if anything gets warm that
shouldn't (a hot SD card or module is an electrical fault - power off first,
debug second). The firmware caps LED brightness at 60%: sustained higher
levels have been tested and can overheat and damage internal electronics.
Do not raise this limit.

## Enclosure

No enclosure files are published yet. The
[`hardware/fab/`](https://github.com/ristllin/Nimbus/tree/main/hardware/fab) folder holds PCB manufacturing outputs (Gerbers,
ODB++, CAM) for the carrier PCB - archival reference, not an enclosure.
