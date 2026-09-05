# Development guide

How to build, test, and change the firmware - the distilled version of the
repository's working knowledge. Ground rules for pull requests are in
[CONTRIBUTING.md](https://github.com/ristllin/Nimbus/blob/main/CONTRIBUTING.md).

## Repository layout and the layering rule

Four layers; **a lower layer never includes an upper one**:

```mermaid
flowchart TD
  M["main.cpp - wiring + the main loop"] --> S["src/modes · src/agent · src/net<br/>feature subsystems"]
  S --> H["src/hw - Arduino/board glue"]
  H --> C["lib/core - portable logic<br/>(no Arduino, host-tested)"]
  C -. board support .-> SD["solide-drivers<br/>(pinned library)"]
```

- `lib/core/` - portable C++, no Arduino, 100% host-testable. New logic goes
  here first whenever possible.
- `src/hw/` - glue binding portable logic to real hardware.
- `src/modes/`, `src/agent/`, `src/net/` - the feature subsystems.
- `main.cpp` - wiring and the main loop only.

Board support is the separate
[solide-drivers](https://github.com/ristllin/solide-drivers) repository,
consumed as a pinned PlatformIO library. Nimbus calls its `solide::` seams and
never forks its internals.

**Device builds** (`esp32s3`, `test`, …) fetch it automatically from the pinned
public tag in `platformio.ini` - a fresh clone builds with nothing else.
**Host tests** (`pio test -e native`) additionally need its headers on the
include path, which expects a sibling checkout:

```bash
git clone https://github.com/ristllin/solide-drivers.git ../solide-drivers
git -C ../solide-drivers checkout v0.5.1   # the tag the firmware pins
```

Driver developers can point the device builds at that same sibling by swapping
the commented `symlink://../solide-drivers` line in `platformio.ini`.

## Build environments

| Environment | What it is |
|---|---|
| `native` | Host unit tests (Unity + golden images): `pio test -e native` - keep it at 100% |
| `esp32s3` | Production firmware - silent serial, no test/debug code |
| `test` | Production plus the serial test console - what the HIL harness drives |
| `notifierdbg` | Production plus a device→host status echo for Notifier E2E |
| `bttest` | `test` advertising as "Nimbus-BT" so a bench board is unambiguous |
| `provision` / `provision-uart` | Standalone serial network diagnostic / installer seed tool |
| `beep`, `mictest`, `tftmin`, `tfttouch`, `tftbringup` (+`-uart`) | Single-purpose hardware diagnostics |

`tools/build_all.sh` compiles the whole matrix - a compile-only gate before a
release. The full annotated list is in
[Tools & commands](tools-and-commands.md).

## The verification ladder

"Compiles + links + boots" is **not** verification - that fallacy once shipped
a build whose touch, Wi-Fi, and agent had never been exercised. The ladder:

1. **Portable logic** → `pio test -e native`. Fast, no hardware, must stay at
   100%. Over a thousand cases as of v4.1. These are the C++ Unity suites in
   **`test/`** (one directory per module). Note the two similarly-named trees:
   **`test/`** is host C++ (this step); **`tests/hil/`** is the Python HIL suite
   (step 2); and the **`test` build environment** is a third thing again - the
   firmware plus a serial console the HIL suite drives.
2. **Device behavior** → flash `[env:test]` and run the hardware-in-the-loop
   (HIL) suite:

   ```bash
   python3 -m pytest tests/hil -m "hil and not manual" --allow-hardware   # device suite
   python3 -m pytest tests/hil -m "net and not manual" --allow-hardware   # LAN suite
   ```

   Markers (`hil` / `net` / `agent` / `audio` / `manual`) are all deselected
   by default, and hardware is additionally gated behind `--allow-hardware`,
   so collection stays clean with no device attached. Tests that break device
   state must restore it in a `finally`; manual tests must fail loud when
   unconfirmed.
3. **Anything user-visible** → assert it through the test console
   (`RENDER?`, `STATUS`, echoes) or hand it to a human as an explicit manual
   step.

Every observed field failure and the test that would have caught it is tracked
in the maintainers' failure catalog. Check the known open items before "fixing"
one.

## Golden tests (screens and protocol)

UI screens are pinned as **golden framebuffers** in `test/golden_tft/*.bin`
(240×320 RGB565); several protocol and prompt surfaces have golden text in
`test/golden/`. The flow after an intentional screen change:

1. Run the native suite; the golden test fails and writes the new buffer.
2. Render it for human review: `python3 tools/tftpreview.py contact`
   (the whole UI on one sheet).
3. If it looks right, bless the new buffer and commit it with the change.

The nsn wire protocol is pinned the same way - `test/test_proto/nsn_vectors.h`
is generated from the reference encoder in
[nimbus-notify](https://github.com/ristllin/nimbus-notify) by
`tools/gen_nsn_vectors.py`, byte-locking the device codec to the broker.

## Pre-commit hooks

```bash
pip install pre-commit && pre-commit install
pre-commit run --all-files    # what CI runs
```

The hooks include secret scanning (gitleaks), repo-specific pattern checks,
a complexity gate (lizard - new code only; existing offenders are baselined),
formatting/whitespace hygiene, and ruff for Python. Generated outputs
(`website/docs/**` guides/reference, `docs/sfx-map.md`, the embedded docs
pack) are exempt from hygiene hooks - regenerate them, never hand-edit.

## Serial discipline (how not to wedge a board)

The ESP32-S3's native USB port is fragile in specific, documented ways:

- **Never strobe DTR/RTS.** Open ports quietly (both lines de-asserted before
  open) - see `tests/hil/device.py`. A DTR/RTS strobe can wedge the USB device
  silent.
- **Opening the port restarts the board** - even a quiet open. Hold one serial
  session across commands instead of reconnecting per command; prefer HTTP
  (`/api/state`) for polling.
- **Restart in software only**: the `REBOOT` console command, or the task
  watchdog (~8 s).
- A silent board is almost never bricked - see
  [recovery](quick-start/flash.md#recovery).

### Soak observability over LAN (`GET /api/state`)

Long soak legs read device liveness over HTTP so a serial open never resets the
board mid-run. `GET /api/state` carries these fields for that purpose (all are
read-only and present in both modes):

- `uptimeMs` - milliseconds since boot. Monotonic until the ~49.7-day wrap; a
  drop to a small value between polls means the device restarted.
- `ringBackstopFires` - times the belt-and-braces ring backstop had to clear a
  stuck arc (CUM-11). Stays `0` on a healthy device; the wake-up soak asserts it
  never moves.
- `touch` - touch controller liveness: `failures`, `recoveries`, `busClears`,
  `hardResets` (monotonic counters), `consecFailures` (the current live failure
  streak, `0` when comms is healthy), `lastRecoveryMs`, and `degraded` (true while
  the controller is not confirmed alive). On a capacitive (FT6336U) board the
  counters track the I2C recovery ladder (CUM-248); the sleep/wake soak watches
  `recoveries` climb while `degraded` returns to false, proving the controller
  self-heals instead of going dead until a power-cycle. On a resistive (XPT2046)
  board those counters stay zero (no I2C ladder), but a firmware-side liveness poll
  reads the raw controller and sets `degraded` plus `resistiveDead` when it sees the
  persistent stuck-high (all-`4095`) signature of a dead controller - the honest
  signal that a boot-time "touch up" cannot give.
- `batt.rawPackMv` - the computed pack millivolts latched before the plausibility
  gate rejects it (diagnostics only, never a policy input). An open sense line reads
  near `0` here while a real pack reads ~`7000`, so the two can be told apart over
  HTTP even though both drive `batt.valid` to false. `0` on a board with no voltage
  sense.
- `batt.senseMissing` - true when battery monitoring is on but the reading has been
  invalid across a debounce window (an open sense divider). The detector is fed
  every 30 s regardless of whether the sample is valid (three consecutive invalid
  checks claim the fault, about a minute after boot), so a fault cannot hide
  behind the valid-only telemetry refresh. It stays false on a genuinely desk-powered board
  (monitoring off) and clears the instant a valid sample arrives; the Health panel
  turns it into a "battery sense not detected" row.

## Docs follow every commit

Renamed or moved anything user-visible? Grep for the old wording and fix every
hit in the same commit (`README.md`, `docs/`, `website/docs/`, web UI copy).
Edited a published doc? Re-run `node website/scripts/migrate-docs.mjs` and
commit the regenerated `website/docs/**`. Edited a doc the on-device model
reads? Re-run `python3 tools/gen_docs_pack.py` and commit the regenerated
header. The [docs map](README.md) says which file is canonical for what.

## Hard-won constraints (do not relearn these on hardware)

- ⛔ **No on-device sub-agent concurrency** - no worker task, no second
  concurrent TLS connection, no parallelism in the turn/sub-agent path. The
  single-task, single-TLS, fully-serialized design is deliberate and
  hardware-proven; every attempt at concurrency destabilized the device.
- **Internal SRAM is the scarce pool, not total RAM.** Route churn to PSRAM;
  never "fix" OOM by raising heap floors. See [Memory](memory.md).
- **Never DMA a full frame straight from PSRAM to the panel.** A ~150 KB SPI
  burst sourced from PSRAM shares the S3 external-memory bus and resets the
  ILI9341 (MADCTL to its power-on default), leaving the glass white while every
  register readback and the render task still look healthy. The colour-TFT driver
  stages a PSRAM frame band-by-band through an internal DMA bounce for this
  reason. `tools/check_tft_elf_no_eink.py` guards the companion property (a TFT
  build links zero GxEPD2, so the e-ink SRAM win survives).
- **The two USB-C ports are not interchangeable** - a factory-fresh board
  flashes only through `UART`. See [Flash the firmware](quick-start/flash.md).
- **Structured outputs everywhere** - never ask a model for JSON in prose.
  See [Provider wire](provider-wire.md).

---

*Forking the project and running your own update channel →
[Self-hosted OTA](self-hosted-ota.md)*
