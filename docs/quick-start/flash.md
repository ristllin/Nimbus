# Flash the firmware

Get the firmware onto the board. Two paths: the browser flasher (easiest), or
the guarded command-line installer.

:::caution Use the port labeled UART
The DevKitC-1 has **two USB-C ports and only one can flash a fresh board**.
Plug into the port silkscreened **`UART`**, not `USB`. On a factory-fresh
board the native `USB` port has **no path into download mode at all** - a
board that "won't flash" on that port is not broken, it is on the wrong port.
:::

## Path 1 - browser flasher (recommended)

Flash straight from a Chromium-based browser (Chrome or Edge), no toolchain
installed:

1. Connect the board's **UART** port to your computer with a **data-capable
   USB cable** (many cables are charge-only and never show a serial port).
2. Open the **[Nimbus web flasher](https://docs.cumulo-nimbus.ai/flash)**
   and click **Install Nimbus**.
3. Pick the serial port when prompted (a CP210x / `usbserial` entry). On a new
   board, choosing "Erase device" is fine; on a board already running Nimbus
   it wipes the saved settings.
4. **Ignore any Wi-Fi prompt the flasher shows afterwards** - Nimbus provisions
   through its own `Nimbus-setup` network, not through the flasher.

> **On trust:** like any first firmware install before secure boot, the
> browser-flashed image is written as-is - it is not cryptographically verified
> on the device the way [over-the-air updates](../ota.md) are (those are signed
> and checked before they apply). If you want to confirm the exact bytes first,
> the image's SHA-256 is published next to it at
> `raw.githubusercontent.com/ristllin/nimbus-fw-releases/webflash/latest/nimbus-webflash.bin.sha256`
> - compare it against the file the flasher downloads.

When it finishes, the board restarts into Nimbus: join the `Nimbus-setup`
Wi-Fi network and continue in the **[setup wizard](setup-wizard.md)**. On a
touchscreen board the panel stays blank/white until the wizard's display step
is answered - expected, not a fault.

## Path 2 - command-line installer

From a clone of the firmware repository, with Python 3 and
[PlatformIO](https://platformio.org/) installed:

```bash
python3 tools/setup_device.py
```

The installer identifies the board for you and confirms before it writes:

- It **discovers connected boards** by their USB descriptor and works out the
  **board family** (Nimbus board or Freenove CYD) from that plus the saved
  settings, so you rarely need to say which board you have.
- **One board** connected? It shows what it found and asks a single question:
  `Install to '<name>' (<board family>, <configured or blank>) on <port>? [Y/n]`.
  **Several boards?** It lists them and lets you pick by number, with an
  **Identify** action (`i2`) that blinks that board's ring or screen for about
  three seconds so you can tell which is which.
- It **never erases saved settings.** An already-configured Nimbus keeps its
  Wi-Fi, keys, pairings, and access token.
- On a new board it asks for the starting **operating mode** (Notifier or
  Orchestrator), and for a Freenove the **panel size** (2.8 / 3.5 / 4.0 inch).
  It seeds the display, orientation, mode, and the board's update type, verifies
  them, then installs the production firmware.

Useful flags: `--port` (skip discovery), `--board solide_s3|freenove_s3` (skip
autodetect), `--size 28|35|40` (Freenove panel), `--mode notifier|orchestrator`
(skip the mode prompt), `--yes` (skip the confirm prompt for CI; needs a single
connected board or an explicit `--port`, plus `--mode` for a blank board).

### The Freenove CYD all-in-one

The [all-in-one board](../hardware/all-in-one-cyd.md) uses the **same**
installer and is auto-detected; no flag is required.

```bash
python3 tools/setup_device.py           # autodetects the Freenove on its USB-C port
python3 tools/setup_device.py --board freenove_s3 --size 35   # or be explicit
```

Two things are specific to this board:

- **One port, no UART bridge.** The CYD has a single USB-C port and rides the
  ESP32-S3's native USB the whole way, so there is no "wrong port" the way the
  DevKitC-1's two-port caution above describes. Just connect a data-capable
  USB-C cable.
- **The panel size sets the update type.** All Freenove sizes share one firmware
  image; the size you pick only labels which typed update the board receives
  (`freenove-28` / `freenove-35` / `freenove-40`). It is always the color
  touchscreen, so there is no display question.

Done? Continue to the **[setup wizard](setup-wizard.md)**. The rest of this
page is reference for reflashing and recovery.

---

## The board's flashing states

A board only ever passes through three states, and the UART-vs-USB trap
exists solely on the first arrow - once Nimbus is installed, any path works:

```mermaid
stateDiagram-v2
  fresh: Factory-fresh board
  nimbus: Nimbus installed
  updated: Updated firmware
  [*] --> fresh
  fresh --> nimbus: first flash - UART port ONLY<br/>(the native USB port has no<br/>path into download mode)
  nimbus --> nimbus: reflash over either USB-C port
  nimbus --> updated: OTA over Wi-Fi<br/>(signed, owner-approved)
  updated --> nimbus: automatic rollback<br/>if the new image can't boot
```

## The two USB-C ports, explained

| Port (silkscreen) | What it is | Starts a flash by itself? |
|---|---|---|
| **UART** | CP2102N bridge, with DTR/RTS wired to the chip's reset and boot pins | **Yes - electrical, regardless of what firmware is running** |
| **USB** | The ESP32-S3's own USB peripheral | Only while the chip's ROM (or Nimbus) owns it |

On the `UART` port, esptool enters download mode electrically - no buttons.
Fresh kits ship a demo that takes over the native USB peripheral, leaving no
software path to download mode on the `USB` port; the port then shows up as a
plain CDC-ACM device whose virtual DTR/RTS do nothing. On the `UART` port the
board appears as a CP210x serial device (`/dev/cu.usbserial-*` or
`/dev/cu.SLAB_USBtoUART`).

## Reflashing a board that already runs Nimbus

Once Nimbus is on the board, **either port works** for reflashing - Nimbus
keeps the native USB port reachable (its test build has a `REBOOT` console
command, and a task watchdog restarts a hung device on its own). A typical
bench reflash:

```bash
pio run -e test -t upload
```

With more than one board connected, always pass `--upload-port` explicitly,
and confirm which board a port belongs to before flashing - the
`usbmodemNNNN` suffix tracks the computer's USB port, not the board.

## Recovery

```mermaid
flowchart TD
  A["Serial is silent - can esptool connect?"] -->|yes| B["Flash normally"]
  A -->|no| C["python3 tools/usb_reset.py<br/>resets the USB link, about 2 s"]
  C --> D["Catch the bootloader:<br/>esptool --before default-reset --after no-reset chip-id"]
  D --> E["pio run -e test -t upload"]
  C -->|still nothing| F["Move the cable to the UART port<br/>and run tools/setup_device.py"]
```

**A silent board is almost never bricked.** If serial goes quiet *and* esptool
cannot connect on the native USB port, the usual cause is stale host-side USB
state, not the board. Recover it in software - no need to unplug anything:

```bash
python3 tools/usb_reset.py    # resets the USB link (equivalent to a replug)
```

This resets the USB **link**, not the chip - it un-wedges a silent serial
device but cannot restart the firmware or enter download mode by itself. With
two boards attached, disambiguate with `--serial` or `--skip` (see the
script's help). Then, to flash, catch the board in its bootloader and hold it
there:

```bash
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port /dev/cu.usbmodem101 \
  --before default-reset --after no-reset chip-id
pio run -e test -t upload --upload-port /dev/cu.usbmodem101
```

If the native USB port still does not respond, the `UART` port always works:
move the cable there and run `python3 tools/setup_device.py`.

:::caution Never pulse the serial control lines
Do not open the port with tools that assert DTR/RTS by default, and never
strobe those lines hoping to reset the board - on this board that can wedge
the USB device silent. The installer and the commands above already handle
the port correctly.
:::

### Recovering the access token

If a damaged or blank display prevents scanning the sign-in QR, the web access
token can be read back over the physical UART, without erasing anything:

```bash
python3 tools/setup_device.py --show-token
```

This temporarily installs the UART diagnostic, prints the token, and restores
the production firmware. It refuses to run on a board without existing Nimbus
settings.

## Which build environment do I want?

| Environment | Install with | What it is |
|---|---|---|
| `esp32s3` | `python3 tools/setup_device.py` | **Production firmware.** Silent serial; what a finished device runs. The installer flashes this for you. |
| `test` | `pio run -e test -t upload` | Production firmware **plus a serial test console** (`STATUS`, `REBOOT`, `RENDER?`, …) for bench work and the HIL harness. Never the flash target for a finished device. |
| `provision` | `pio run -e provision -t upload --upload-port …` | A standalone serial **network diagnostic** - not the product firmware; it has no display UI, setup network, or web settings. Its `provision-uart` variant is what `setup_device.py` uses internally to seed a new board's settings. |
| `tftbringup` | `pio run -e tftbringup -t upload` | **Diagnostic only**: a bare TFT panel test (color bars, backlight fade, touch paint). It replaces the Nimbus firmware entirely - restore with `python3 tools/setup_device.py`. |

If any diagnostic environment was flashed by accident, running
`python3 tools/setup_device.py` puts the production firmware back; saved
settings are unaffected.

---

*How it works → [Hardware reference: first flash of a fresh board](../hardware.md#first-flash-of-a-fresh-board--use-the-uart-port)*
