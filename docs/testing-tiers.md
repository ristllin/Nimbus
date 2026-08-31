# Testing tiers

One taxonomy for the whole product (this repo, the cloud repo, notify, solide-drivers).
Every new test states its tier; every tier has one runner.

| Tier | What | This repo | Runs |
|---|---|---|---|
| T0 | Static gates | pre-commit hooks, param-consumer + status-doc + webui-concat gates | every commit |
| T1 | Unit (host, fast) | `pio test -e native` suites without goldens | every commit |
| T2 | Integration (fakes, seams) | native suites exercising module seams (`test_harness_*`, wire codecs) | every commit |
| T3 | Golden / snapshot | `test/golden*`, webui snapshot, QR/nsn vectors | every commit |
| T4 | End-to-end on simulators | host rigs (`tools/harness-lab`) only (no emulated-device leg: see below) | every push |
| T5 | Hardware-in-the-loop | `python3 -m pytest tests/hil -m "hil and not manual" --allow-hardware` (device lock protocol applies) | bench |
| T6 | Live paid (real providers) | evals + HIL "net" layers; each SKIPS cleanly unless its key env var is set (keys via repo-root `.env`) | milestones + gate, budget-capped |

Rules: never mock the thing under test to make a tier pass; hardware claims need hardware
(T5) or an explicit handed-off manual step; T6 results persist to `~/nimbus-evals/`
(JSONL per run with model, scores, token and dollar cost) - never into the repo.

## T4 emulated-device tier: evaluated, not adopted

T4 runs end-to-end against host rigs (`tools/harness-lab`) only. We evaluated adding
an emulated-device leg on top of the two options that can run ESP32-S3 firmware
without a board, and rejected both.

A full-system CPU emulator (Espressif's QEMU fork) does not model the peripherals
that define this device: no Wi-Fi, no Bluetooth, no I2S audio, no SPI display, and no
addressable-LED output, and the 8 MB octal PSRAM the board boots with is the one
memory setting a tracked bug reports it failing to recognize. What it can run is the
portable, radio-free logic already covered at T1 through T3.

A cloud parts-simulator (Wokwi) does better on the screen and Wi-Fi path: it can drive
the ILI9341 display, the WS2812 ring, an SD card, and internet access, and it models
8 MB octal PSRAM. But it cannot simulate Bluetooth or I2S, so Notifier pairing and all
microphone and speaker behavior stay dark, touch has no interactive panel model, and
headless use in CI needs a paid per-seat token plus metered minutes.

In both cases the coverage gained overlaps the golden and snapshot tiers, while the
failure classes that actually shipped in the 2026-08-24 incident (pairing, audio,
touch placement, real timing) can only be proven on hardware at T5. Emulation is
therefore not adopted. If a hardware-free display and UI regression lane is wanted
later, the cloud parts-simulator is the candidate to reassess, not the CPU emulator.
Full spike evidence and costs are recorded on the CUM-80 issue.

## The release gate

A cross-cutting battery that must be green before any firmware tag or client flash.
It exists because the 2026-08-24 incident shipped a white screen, a universal tunnel
502, a rotated touch surface, and reconnect flapping in one release. Each of those
classes now has a test that fails on the pre-fix build and passes on the fix.

- Host checks (T0-adjacent, run in CI): `python3 tools/release_gate/run_gate.py --host-only`.
  The driver-pin check refuses a build on a known white-screen driver; the ELF check
  keeps the e-paper footprint out of the TFT image; the OTA-NVS check proves the update
  flow writes only its own bookkeeping keys, never an owner's settings (CUM-237).
  `python3 -m pytest tools/release_gate` tests the gate logic.
- One host verdict across every failure class: `python3 tests/release_gate/run_host_legs.py`
  runs each host leg (the white-screen, touch, boot-loop, and settings-across-OTA unit and
  source-guard legs) and prints a single PASS/FAIL. `tests/release_gate/MANIFEST.md` maps
  every lose-every-customer class to its leg, says whether it runs on the host or the bench,
  and gives the exact board, steps, and pass criteria for each bench leg.
- On-hardware legs (T5): `tests/hil/test_l29_release_gate.py` - loopback serves the local
  page (not a 502), render reaches the glass (with a recorded human glance), touch lands
  where tapped, a wedged loop is caught by the watchdog, a bad OTA image rolls back.
- Cloud legs (in the cloud repo, run by `pnpm e2e`): the connected-device 5xx interstitial,
  the reconnect storm, and the read-only live smoke.

`tools/release_gate/run_gate.py` prints the full battery with exact commands and the block
condition. See `tools/release_gate/README.md` for what each leg catches.
