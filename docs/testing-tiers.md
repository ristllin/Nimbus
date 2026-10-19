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
| T5 | Hardware-in-the-loop | `python3 -m pytest tests/hil -m "hil and not manual" --allow-hardware` (device lock protocol applies) | on hardware |
| T6 | Live paid (real providers) | evals + HIL "net" layers; each SKIPS cleanly unless its key env var is set (keys via repo-root `.env`) | milestones + gate, budget-capped |

Rules: never mock the thing under test to make a tier pass; hardware claims need hardware
(T5) or an explicit handed-off manual step; T6 results persist to `~/nimbus-evals/`
(JSONL per run with model, scores, token and dollar cost) - never into the repo.

Destructive legs (fresh-device / factory-reset): a few T5 legs FACTORY-RESET the board
(they erase keys, the token, pairing and every owner setting to reach the genuine
out-of-box state). They carry the `destructive` marker, not `hil`, so the standard
`pytest tests/hil -m "hil and not manual" --allow-hardware` run and the release gate
DESELECT them. Run them deliberately and alone with `pytest tests/hil -m destructive
--allow-hardware`; the fixture re-provisions Wi-Fi and clears the adversarial cal/flip on
teardown, but the board is left factory-fresh, so re-onboard it afterward.

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
Full spike evidence and costs are recorded on the tracking issue.

## Fresh-device (default / absent-NVS) leg

The touch-mirror / touch-180 / uncalibrated-out-of-box family kept re-presenting because every
other automated leg runs on a PROVISIONED unit (calibration solved, NVS good), while the owner
hits the FIRST boot after a new version. The fresh device is therefore a first-class test STATE:

- Host (T1): `test/test_fresh_device` and `test/test_touch_cal` pin the class rules a fresh boot
  resolves (per-TouchKind default flags, the first-run cal gate policy over every kind, the
  flip-compose rule that a stored cal stays valid across a display flip, the no-dead-end
  first-run screen selection). A new board or touch class without a measured default fails at
  compile time (a `static_assert` on `TouchKind::Count`).
- Hardware (T5): `tests/hil/test_l33_fresh_device.py` drives the board to the out-of-box state
  through the product factory-reset path plus the test-only `CALGATE clear` (a factory reset
  keeps the touch cal as hardware identity), then on a fresh resistive panel asserts the guided cal
  gate owns the panel (no self-navigation for 30 s), injected taps do not navigate while gated,
  the deliberate long-hold skip hands off to first-run setup, and render reaches the frame
  (`TFTFILL?` GRAM readback). An NVS-adversarial variant persists a hostile `tchCal`/`tftFlip`
  (frozen keys that survive a reflash), restarts, and asserts the device boots healthy, applies
  the stored values, and does not re-arm the gate. The gate is invisible to `RENDER?` by design
  (it paints outside the `g_lastScreen` path), so the `CALGATE` console seam is the oracle for
  gate state; the four-corner SOLVE itself, and whether a tap lands where you touch under a flip,
  stay finger-on-glass steps the runbook hands to a person at the device.

## Sleep/wake and button-feedback legs

Two owner-facing behaviors that a green build cannot prove: that the device wakes
cleanly cycle after cycle from a software power-off, and that every actionable menu
control gives a sound and ring cue. Both split the same way: assert everything a seam
can reach, hand the finger/ears part to a human.

- Sleep/wake soak (T5): `tests/hil/test_l34_sleep_wake.py` runs N cycles (default 20,
  `NIMBUS_SOAK_N`) of the real product power-off path. Each cycle reads the wake-arming
  plan (`SLEEP?`: whether a tap can wake this board and on which touch controller and
  GPIO, the ext0 level, and the timer), asserts it is present and coherent (a torn or
  incoherent arm report is itself the "won't wake" failure), enters deep sleep with a
  test-only timer wake (`POWEROFF <secs>`, so the leg wakes with no finger), proves a
  fresh boot from the boot stream plus `WAKE?` (reset reason deep-sleep, cause timer),
  checks persisted state is intact (mode, screen model, board, web token, touch-cal
  gate), and records wake latency. The soak runs on a flashable test board; the
  Freenove FT6336U-INT variant is a loud skip when no Freenove is attached. The real
  finger tap that pulses the touch INT is an owner manual leg (there is no finger or
  INT jig in the rig).
- Button feedback (T5): `tests/hil/test_l35_button_feedback.py` fires the real
  outcome-to-cue seam over serial (`ACTFB <action> <outcome>`) for a success row
  (Reset) and a failure row (Rescan SD with no card), then reads back the cue that
  fired (`FEEDBACK?`: tone id, ring swell, and screen line) and polls `RENDER?` to
  prove the ring swell ends (no lit arc outlives its window).
  Whether the tone is audible and the ring is the right color to the eye is an owner
  manual leg.
- Host (T1): the pure parsers and the wake-arming coherence rule are covered with no
  board in `tests/hil/test_l34_sleep_wake_host.py` and
  `tests/hil/test_l35_button_feedback_host.py` (the outcome-to-cue mapping itself is
  host-tested in `test/` under `pio test -e native`).

## Clock-sync leg

The clock must set itself from the internet on every boot, including the fast join where
the station is up before start-up finishes (SNTP once hung off a GOT_IP event that
fired before its handler existed, so a strong network meant a clock stuck at 1970).

- Host (T1): `test/test_net_up` drives fake event timelines (IP before the latch is armed,
  IP after, a drop and rejoin between two ticks, retry noise) through the network-up latch
  and the SNTP kick schedule, including a property over every join/arm ordering in a 20 s
  boot window, flap and backoff bounds (no thrash), and an event-only counter-model that
  fails the fast-join orderings.
- Hardware (T5): `tests/hil/test_l38_clock_sync.py` restarts with `REBOOT cold` (a plain
  `REBOOT` carries the RTC-held clock across the reset, which would make the leg vacuous),
  then asserts the boot started unsynced, STATUS `clk=1` within 60 s of READY, and a
  `loops: SNTP kicked (..., link up)` line; a LAN half checks `/api/state` `clock.synced`
  and that the dream routine got a wall-clock next run. A true power cycle is the manual
  leg. The parsers and verdict are host-tested in `tests/hil/test_l38_clock_sync_host.py`.

## Crash-forensics leg

Every crash leaves a durable, readable cause, read on the next boot from the core dump.

- Host (T1): `test/test_crash_forensics` (the reset table over every value, the boot and
  crash line formats over every backtrace depth with a planted register and stack
  pattern that must never print, the dedupe state machine, the crash notice copy for
  every request kind, the forensic window), `test/test_errlog` (no line lost before
  mount over every attach point and both tiers, the one-handle open count over random
  runs, degrade and recovery), `test/test_orch_pending_events` (rows from boots that
  never synced survive until a clock exists) and `test/test_fresh_device` (a blank dump
  partition reads as no crash; the forensics path writes no NVS).
- Hardware (T5): `tests/hil/test_l39_crash_forensics.py` fires each `PANIC` kind over the
  test console and reads the next boot's `boot:` and `crash:` lines over the LAN, then a
  plain reboot (stale, `new=0`), a USB chip reset (`reason=usb(0x15)`), offline boots,
  a panic armed into a real turn (`PANIC <kind> arm=turn [round=N]`, test builds only:
  it fires at the transport body write of round N, so the dump fits 64 KB with the turn
  task deep in a TLS exchange and names the frame), and the destructive factory-reset
  and early-boot-abort legs. Its parsers and verdicts are host-tested in
  `tests/hil/test_l39_crash_forensics_host.py`; `tools/release_gate/test_panic_guard.py`
  and the release gate's ELF check keep the armed panic out of every shipped image.

## The release gate

A cross-cutting battery that must be green before any firmware tag or client flash.
It exists because the 2026-08-24 incident shipped a white screen, a universal tunnel
502, a rotated touch surface, and reconnect flapping in one release. Each of those
classes now has a test that fails on the pre-fix build and passes on the fix.

- Host checks (T0-adjacent, run in CI): `python3 tools/release_gate/run_gate.py --host-only`.
  The driver-pin check refuses a build on a known white-screen driver; the ELF check
  keeps the e-paper footprint out of the TFT image; the OTA-NVS check proves the update
  flow writes only its own bookkeeping keys, never an owner's settings.
  `python3 -m pytest tools/release_gate` tests the gate logic.
- One host verdict across every failure class: `python3 tests/release_gate/run_host_legs.py`
  runs each host leg (the white-screen, touch, boot-loop, and settings-across-OTA unit and
  source-guard legs) and prints a single PASS/FAIL. `tests/release_gate/MANIFEST.md` maps
  every lose-every-customer class to its leg, says whether it runs on the host or on
  hardware, and gives the exact board, steps, and pass criteria for each hardware leg.
- On-hardware legs (T5): `tests/hil/test_l29_release_gate.py` - loopback serves the local
  page (not a 502), render reaches the glass (with a recorded human glance), touch lands
  where tapped, a wedged loop is caught by the watchdog, a bad OTA image rolls back.
- Cloud legs (in the cloud repo, run by `pnpm e2e`): the connected-device 5xx interstitial,
  the reconnect storm, and the read-only live smoke.

`tools/release_gate/run_gate.py` prints the full battery with exact commands and the block
condition. See `tools/release_gate/README.md` for what each leg catches.
