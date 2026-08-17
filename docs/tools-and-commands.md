# Tools & Commands

The catalog of every script, build environment, console command, and remote
owner command in the repository - what each one does and when you reach for it.

Two audiences, one page:

- **User tools** - what you need to install, recover, and manage a device you
  own. Setting one up needs only these.
- **Contributor tools** - everything for working on the firmware itself:
  golden-image review, vector generators, CI gates, build environments, and
  the serial test console.

All host scripts live in `tools/` and run with `python3` from the repository
root unless noted. Most need only the standard library. For a one-line-per-tool
quick index grouped by audience - and which scripts are load-bearing for CI or
the build - see [`tools/README.md`](../tools/README.md).

## User tools

### Device setup & recovery

| Tool | What it does | When you need it |
|---|---|---|
| `setup_device.py` | Guarded firmware installer over the UART port. Reads the board's NVS, reports the immutable factory MAC, and requires that MAC to be typed back before writing anything. On a new board it also asks for the fitted display and operating mode, seeds those two settings, then installs production firmware. Never erases NVS or factory-resets a configured device. | Installing firmware on any board - especially a factory-fresh one, which can only be flashed through the UART port. |
| `usb_reset.py` | Programmatic unbrick for a wedged ESP32-S3 USB-serial port: a libusb bus-level reset, protocol-equivalent to unplugging and replugging the cable. Supports `--skip`, `--serial`, and `--all` to pick the right board when more than one is attached. Needs `pyusb` + Homebrew `libusb`. | Serial has gone silent and esptool cannot connect. Run this instead of physically replugging. |
| `tcal_wizard.py` | Touch calibration wizard for the TFT variant: prompts you to press each of four corners, samples the raw ADC, derives the axis-swap and orientation flags, applies the result over the `TCAL` console command, and verifies it. Needs an `[env:test]`-family build. | A touch panel where taps land away from where you aimed - which is otherwise indistinguishable from broken touch. |
| `connectors_setup.py` | Pushes connector credentials from the repository-root `.env` (gitignored) to a board via the token-gated `POST /api/connectors` upsert. Idempotent; never prints a secret. `--list` shows the board's current connector state. | Configuring or re-configuring a board's external connectors after a flash or NVS change. |

### Telegram owner commands

In Orchestrator mode the device answers a few fixed commands over Telegram
before the model sees the message, so the owner can manage a deployed board
without waiting on (or paying for) a turn. Every command below except `/help`
is owner-only - an allow-listed member can converse but cannot run them - and
each reply names the device and firmware so two boards sharing one bot are never
confused.

| Command | What it does |
|---|---|
| `/help` (or `/start`) | Greet and list the owner commands. Available to anyone who can chat with the bot. |
| `/update` | Install a pending firmware update. |
| `/remind <when> <what>` | Set a one-time reminder - e.g. `/remind 30m take the cake out`. `<when>` is a span (`45s`, `30m`, `2h`, `1d`; a bare number is minutes), 2 minutes to 7 days. At that time the assistant delivers the reminder in this chat. Cancel with `/loop deny <id>`. |
| `/loops` | List the device's routines (and any reminders you've set). |
| `/loop approve\|deny\|off\|on <id>` | Approve, remove, pause, or resume one routine. |
| `/compact` | Summarize this conversation into memory. Runs in the background. |
| `/skill approve\|deny <id>` | Approve a saved skill so it can be used, or remove it. |

A routine or skill the assistant creates on its own stays inactive until the
owner approves it here (or from the web UI). The one exception is a one-time
wakeup the assistant sets for itself (a single follow-up turn, minutes to days
out): it fires once without approval, shows up in /loops like any routine, and
can be cancelled the same way. Managing people, keys, and other settings stays
web-only - see the web control surface, not the bot.

## Contributor tools

Everything below is for working on the firmware or driving a bench board -
everyday use of a device needs none of it.

### Rendering & golden-image review

| Tool | What it does | When you need it |
|---|---|---|
| `golden.py` | Converts the e-ink golden test buffers (296×128 1-bit framebuffers in `test/golden/*.bin`) to PNGs for human review, and pixel-diffs two buffers (differences in red, exit 1 on mismatch). Standard library only. | Reviewing or debugging a red e-ink golden test - `render` to look at a screen, `diff` to see exactly what changed. |
| `tftpreview.py` | The color counterpart of `golden.py` for the TFT golden suite (240×320 RGB565 buffers in `test/golden_tft/*.bin`). Adds `contact` (tile every blessed screen into one labelled sheet) and `regions` (overlay tap targets and fail on targets under the 44 px minimum or on overlaps). | Reviewing TFT screens after a layout change - `contact` shows the whole UI at a glance; `regions` catches the two bugs a pixel diff cannot see. |
| `logo/gen_logo.py` | Generates the Nimbus logo (the dotted teal ring) and every artifact derived from it: `assets/logo*.svg`, the website logo, favicon, social card, and the PROGMEM mark served at `GET /logo.svg`. | Any logo change. Edit the generator and re-run it - never hand-edit its outputs. |

### Test-vector & artifact generators

| Tool | What it does | When you need it |
|---|---|---|
| `gen_nsn_vectors.py` | Regenerates the nsn wire-protocol test vectors from the reference encoder in the sibling `nsnotify` repository, writing `tools/nsn_vectors.json` and the embedded header for the native test suite. | Whenever the reference encoder in `nsnotify` changes - the device codec is byte-locked to it through these vectors. |
| `gen_qr_vectors.py` | Regenerates the QR known-answer vectors from Project Nayuki's reference `qrcodegen` (the algorithm `lib/core/src/qr.cpp` reimplements), pinning the exact configuration the device uses. Needs `pip install qrcodegen`. | Whenever the QR vector list changes. |
| `make_manifest.py` | Builds and signs the OTA release manifest (ECDSA P-256 over a canonical message that must stay byte-identical to `nimbus::ota::buildSigMessage()` - the pairing is golden-tested). Also prints the unsigned message for cross-checks. | Used by the release workflow with the real signing key, and by `tests/hil/test_ota.py` with the committed test key. Run by hand only when debugging a signature mismatch. |
| `git_version.py` | PlatformIO extra script - injects `git describe` as the `NIMBUS_FW_BUILD` build id so `/api/state` pins the exact commit a running unit was flashed from. | Runs automatically on every build; nothing to invoke. |
| `gen_docs_pack.py` | Regenerates the embedded docs pack (`lib/core/include/nimbus/docs_pack_data.h`) from the curated docs the Orchestrator model can read via `docs.list` / `docs.search` / `docs.read`. The pack ships inside the firmware image, so OTA updates it. | After editing any curated file under `docs/` - the header is committed, so CI builds without python. |

### CI gates & consistency checks

| Tool | What it does | When you need it |
|---|---|---|
| `check_param_consumers.py` | Fails the build if any editable settings parameter has no functional consumer in the code - a knob that appears in the menu and web UI but changes nothing. | Runs in CI. Run locally after adding or removing a parameter. |
| `check_status_doc.py` | Fails the build when the status→animation table in `docs/notifier-status-language.md` drifts from the code single-source (`lib/core/src/status_style.cpp`). | Runs in CI. Run locally after touching either side; edit the code first, then fix the doc. |
| `webui_concat_check.py` | Asserts the web UI fragment set (`include/web/ui_*.h`) re-concatenates into a well-formed page, and optionally byte-compares against a blessed snapshot (`--bless` after an intentional page change). | After any web UI fragment edit. |

### Hardware validation & QA

| Tool | What it does | When you need it |
|---|---|---|
| `nsn_send.py` | End-to-end hardware validation: encodes nsn frames with the real broker encoder from `nsnotify` and sends them to a device over Bluetooth, optionally reading back the device's debug status echo over serial. Needs `bleak` with a working macOS Bluetooth entitlement. | Proving the full broker→Bluetooth→device pipeline against real hardware. |
| `ring_check.py` | Quick LED-ring smoke test over serial: drives `LEDTEST` through the console and reports whether the ring data path acknowledges. Errors are console/port problems, not ring verdicts. Needs an `[env:test]`-family build. | A fast eyes-on check that the ring is being driven at all. |
| `backup_device.py` | Pulls a board's user data - artifact-store files, the full vector DB, scratchpad, and state/usage snapshots - over the token-gated web API into a dated folder. Read-only (GETs only); provider keys and tokens are deliberately never exported. | Backing up a device whose SD card is a single point of failure, before a reflash or as routine insurance. |
| `build_all.sh` | Compiles every build environment and flag path in one command - a compile-only gate that proves each target still links. Never uploads or opens a serial port. | Before a release, or after touching flag-gated code that only some environments compile. |

Board-specific bench scripts (a live connector-QA harness, the quality
benchmark, panic-capture, and the connector-shape grid) live in the private
`ops/` tree, not the public repo - they name bench boards and burn real provider
credits, so they are maintainer-only.

### Tool subprojects

Each of these is a self-contained project with its own README:

- **`tools/sounds/`** - the sound-pack pipeline: synthesizes the wordless tone
  set (`gen_tones.py`, seeded + reproducible), curates pools (`palette.py`),
  builds `dist/` + the manifest (`build_assets.py`), regenerates the embedded
  tier (`embed_basic.py` → `src/sfx/sfx_basic_data.h`) and the action→audio map
  (`gen_sfx_map.py` → `docs/sfx-map.md`). Edit the generators, re-run, never
  hand-edit outputs.
- **`tools/harness-lab/`** - runs the real agent harness on the Mac instead of
  the device, for fast iteration on agent behavior. See its `README.md` for the
  build and usage.

The **Battery Lab** - the local dashboard (FastAPI + React) for running
recorded, graphed, comparable battery drain experiments, with thermal protection
and a device simulator - is its own project at
[ristllin/nimbus-battery-lab](https://github.com/ristllin/nimbus-battery-lab).
It drives the firmware's `DRAIN`/`STORAGE` console commands and the token-gated
drain endpoints, which remain in this firmware.

### Build environments

Firmware variants are PlatformIO environments in `platformio.ini`. Build with
`pio run -e <env>`, install with `pio run -e <env> -t upload`. The comments in
`platformio.ini` are the authoritative source; this table summarizes them.

| Environment | What it is |
|---|---|
| `native` | Host-side unit tests (Unity + golden images), no hardware needed: `pio test -e native`. Keep it at 100%. |
| `esp32s3` | Production firmware. Serial stays silent (power and flashing only); no test or debug code. |
| `notifierdbg` | Production firmware plus a device→host status echo over serial, for end-to-end Notifier validation. |
| `test` | Production firmware plus the serial test console (`NIMBUS_TEST`) - what the HIL harness drives. Never the flash target for a production unit. |
| `testuart` | Same as `test`, but the console lives on the UART USB-C port instead of the native USB port (which drops bytes on the V0.1 board). |
| `bttest` | Same as `test`, but advertises over Bluetooth as "Nimbus-BT" so a bench board is unambiguous next to a production "Nimbus". |
| `p2bringup` | Standalone hardware bring-up sketch (no `main.cpp`). |
| `beep` | Minimal speaker diagnostic - isolates the amp and speaker from the full firmware. |
| `mictest` | Minimal I²S microphone bring-up - verifies the mic read path and concurrent speaker+mic loopback. |
| `tftmin` / `tftmin-uart` | The smallest program that puts anything on the TFT panel: init, one solid fill, nothing else. Exists to take every variable out of a blank-screen fault. |
| `tfttouch` / `tfttouch-uart` | Five on-screen buttons that prove display, touch, and calibration together - a swapped or mirrored axis is obvious with five buttons and invisible with one. |
| `tftbringup` / `tftbringup-uart` | Full TFT + touch bring-up: panel init, color order, backlight PWM, and raw touch coordinates, decoupled from the full firmware. |
| `provision` | Standalone serial network diagnostic (not first-time setup - it has no setup network or web UI). |
| `provision-uart` | UART-console variant used internally by `setup_device.py` to seed a new board's display and operating mode. |

The `-uart` variants exist because the DevKit has two USB-C ports: the native
USB port and the UART port. A factory-fresh board can only be flashed through
the UART port, and on some boards the native USB console drops bytes - the
`-uart` environments put the console on the UART port so one cable can both
flash and report.

### Console commands

Builds in the `[env:test]` family compile in a serial command console
(`src/test_console.cpp`). It exists so the HIL harness - and you, at a bench -
can drive and observe the device without a camera or a finger. Production
builds do not include it.

Open the port carefully: opening a serial session resets the board, so hold one
session open rather than reconnecting per command (see `tests/hil/device.py`).

Commands grouped by purpose, one line each. Arguments in angle brackets.

#### Status & diagnostics

| Command | What it does |
|---|---|
| `PING` | Liveness check. |
| `STATUS` | One-line device summary (firmware, mode, network, battery, faults, SFX tier). |
| `RENDER?` | What the screen, posture, and ring are composed to show - the harness's camera substitute. |
| `MENU?` | The current settings-menu view. |
| `LEDSTATE` | What the LED driver is physically driving (raw frame layer vs a fallback pattern) - the physical counterpart of `RENDER?`. |
| `LEDTEST` | Eyes-on LED data-path test: cycles red, green, blue through the real driver. |
| `RAWFRAME?` | Whether the animator currently owns the ring via the raw frame path. |
| `INPUTLOG on\|off` | Echo knob/button/touch events to serial. |
| `SELFTEST [FULL]` | Run the aggregated self-test (FULL adds the audible items). |
| `FAULT <cap> <on\|off>` / `FAULT?` | Inject or clear a capability fault (sd, memory, mic, speaker, led, screen) to prove graceful degradation; query the mask. |
| `SDCHECK` | Probe SD card health and report the current storage tier. |
| `TEST <name>` | Pass a test name through to the board-support self-test. |
| `REBOOT` | Software restart - the only sanctioned way to restart from the host. |
| `HANG` | Deliberately hang the main loop to prove the watchdog recovers (~8 s). |
| `MEMFILL <epi\|vec> <n> [bytes]` | Fill episodic or vector memory with test rows to exercise caps and pruning. |
| `WEBTOK?` | Print the device's web access token (for the HIL harness). |

#### Orchestrator & memory

| Command | What it does |
|---|---|
| `TURN <text>` | Run one live Orchestrator turn; the reply echoes to serial. |
| `VOICE <text>` | Inject a simulated voice transcript so the reply renders on-screen exactly like hold-to-talk - no microphone needed. |
| `PROMPT?` | Dump the last composed system prompt. |
| `CTX? [chat]` / `COMPACT <chat>` | Inspect a conversation's context size; force compaction now. |
| `EPIQ [@<cursor>] <text>` | Run a cold episodic-memory query over deep history and time it; page further back by passing `@<cursor>` from the previous result. |
| `DREAM` | Fire the reserved maintenance-and-reflection loop immediately. |
| `MODE <0\|1>` | Switch operating mode (0 = Notifier, 1 = Orchestrator); persists and restarts. |

#### Display & screen

| Command | What it does |
|---|---|
| `SCREEN <eink\|tft>` / `SCREEN?` | Set the display preference; report both the stored preference and the driver that actually bound. |
| `SAVER [min]` | Force the e-ink screensaver now, or set and persist its idle threshold (0 = off). |
| `DEGHOST` | Force the next e-ink refresh down the full de-ghost path (exercises the stuck-red-plane fix on demand). |
| `TFTHEALTH?` | TFT panel configuration check, heal count, and live backlight state. |
| `TFTHZ [hz]` | Sweep or set the panel SPI clock, measuring pixel-path errors at each step. |
| `TFTFILL?` | Push whole frames through the real blit path and read back the far corners - proves the pixel path with nobody looking at the glass. |
| `TFTID?` | Panel register readback over the shared MISO line. |
| `TFTPWR?` | Read the panel's power/display-on register to tell "powered and displaying" from "merely storing pixels". |
| `TFTFLIP <0\|1>` | Which end of the landscape panel is up - decided by the mounting, so it is dialable live. |
| `TFTBREAK` | Drill: reset the panel behind the driver to reproduce the white screen on demand and prove the watchdog heals it. |
| `PANELPROBE <0\|1>` | Enable or disable the panel health probe. |

#### Input - knob & touch

| Command | What it does |
|---|---|
| `ENC <code>` | Inject a synthetic encoder event so the menu is driveable with no physical knob. |
| `SW?` | Raw debounced button level - isolates a physical-button fault from a decode fault. |
| `TAP <x> <y> [HOLD]` / `TAPUP` | Inject a synthetic tap (or press-and-hold, released by `TAPUP`) - the touch counterpart of `ENC`. |
| `TOUCH?` | Raw touch-controller state, for calibration. |
| `TCAL [minX,maxX,minY,maxY[,flags]]` | Read or set the touch calibration (`tcal_wizard.py` derives it for you). |
| `TOUCHISO?` | Read touch with the panel held in reset - isolates the two devices sharing the MISO line. |

#### Battery & power

| Command | What it does |
|---|---|
| `PROFILE <0\|1\|2>` | Set the battery mode (Dark/Balanced/Full) through the same path the menu and web use. |
| `BATTCAL` | Anchor 100% to the current reading - the owner asserts the pack is full right now. |
| `BATTRESET` | Discard everything the battery model learned by observation (keeps the `BATTCAL` anchor). Needed after a drain campaign. |
| `SLEEPMV [mv]` | Read or set the low-battery deep-sleep threshold (0 = off). A safety knob: 0 disarms the protection persistently - restore the default the moment a pack is connected. |
| `SLEEP` | Enter low-battery deep sleep immediately to test the wake mechanics (knob rotation or the 5-minute timer wakes it; the USB console dies with it). |
| `DRAIN on\|off [deep]` | Battery drain campaign load: pin the ring to a heavy solid-white draw; `deep` runs past the clean shutdown to the real cutoff. |
| `STORAGE <pct\|off>` | Discharge to a storage charge level (~70%) and hold. |

#### Audio & media

| Command | What it does |
|---|---|
| `MICREC` | Record 4 seconds, transcribe it, print the transcript. |
| `SPKSAY <text>` | Synthesize the text and play it on the speaker. |
| `TTSTG <text>` | Synthesize the text and send it to the owner as a Telegram audio message - audible even with a dead bench speaker. |
| `TGSEND` | Telegram document-send smoke test. |
| `SFX <slug>` / `SFXVOL <0-100>` | Play a sound effect by slug; set the master volume. |
| `MEDIATEST` | End-to-end test of the durable media path on the real SD card (write, capture, verify over HTTP). |

#### Network & Bluetooth

| Command | What it does |
|---|---|
| `WIFI <ssid>\|<pass>` | Store Wi-Fi credentials and join. |
| `WIFISCAN` | Scan and print what the radio can actually see. |
| `WIFIKNOWN?` | The saved-network list - names only, never a password. |
| `WIFIRENAME <old>\|<new>` | Correct a mistyped network name while keeping its password. |
| `WIFIAP [on\|off]` | Force the setup network reachable now (stops the station), or resume joining. |
| `BLE?` / `BLEMAC?` | Bluetooth advertising/connection state; the factory Bluetooth address (the ground truth for telling two boards apart). |
| `BONDS?` / `FORGETBONDS` | Bonded-central count and pairing state; erase all bonds. |
| `NSNFEED <hex>` | Feed a synthetic nsn frame through the same decoder path a real Bluetooth frame takes - drives the Notifier UI with no broker and no Bluetooth. |

#### OTA updates

| Command | What it does |
|---|---|
| `OTA?` | Current OTA state and pending update, if any. |
| `OTACHECK` | Poll the release feed now. |
| `OTAAPPLY [dry] [force]` | Apply the pending update (`dry` verifies without installing). |
| `OTAURL <url>` | Point the updater at a different manifest URL. |
| `OTASIM arm <ver>` | Drill: simulate an available update to exercise the whole approval-and-install flow. |

For the full OTA story, see [ota.md](ota.md) and the operations runbook
[ota-operations.md](ota-operations.md).

The device also answers a few fixed owner commands over Telegram - see
[Telegram owner commands](#telegram-owner-commands) under User tools above.
