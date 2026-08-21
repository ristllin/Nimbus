# Manual bench validation - Freenove ESP32-S3 CYD (FNK0104B)

These are the hardware checks that firmware cannot self-verify. Run them on the
physical Freenove board. Each step says what to flash, what to do, and the pass
condition. Already validated during development: **display first light** (panel
lights, correct colors) and **capacitive touch reads** (via `tfttouch-cyd`).

## Console note (important)

The ESP32-S3 USB-Serial-JTAG on this board is finicky over macOS serial: opening
the port can bounce the chip into ROM download mode (firmware halts, panel freezes
on its last frame). Prefer the **on-screen / by-ear** bring-up sketches below,
which need no console. If you do need the console (`STATUS`, `SELFTEST`), flash
`test-cyd`, open the monitor once right after upload, and avoid re-opening.

MAC-verify the port before every flash: it is `28:84:85:42:18:fc`.

```bash
pio run -e <env> -t upload --upload-port /dev/cu.usbmodemXXXX
```

## 1. Display + touch (bring-up sketch, on-screen)

- Flash `tfttouch-cyd`.
- The screen flashes white then black on boot. Touch it: the screen fills a solid
  color by quadrant (top-left red, top-right green, bottom-left blue, bottom-right
  yellow with the shipped default), black when you lift off.
- **Pass:** touching changes the color, and the four corners map sensibly. If the
  mapping is rotated/mirrored, adjust the default in `src/main.cpp` (the capacitive
  touch `Calibration` - currently `swapXY=true, invertY=true`).

## 2. Audio - speaker + microphone (bring-up sketch, by ear)

- Flash `audio-cyd`. It loops: a 440 Hz beep, then a ~1.5 s record, then plays the
  recording back.
- **Pass (speaker):** you hear a clean beep each cycle. The external power amp is
  enabled in codecInit (GPIO1, active-low); if the beep is silent but the codec
  reports OK, probe GPIO1 (should read LOW while audio is up) and confirm the amp
  chip has power.
- **Pass (microphone):** speak during the "recording..." window; you hear yourself
  played back a moment later.
- If silent: the beep alone failing points at the codec DAC / I2S TX / amp; the
  beep working but no echo points at the codec ADC / mic / I2S RX. The ES8311
  register init or MCLK clocking is the usual cause - this path is new.
- Mic note: the ES8311 RX shares the speaker's stereo I2S slot config, so a mono
  mic arrives interleaved. The record path now collapses each frame to the louder
  slot (codecStereoToMono), so playback should be clean mono at normal pitch. If
  the echo is half-speed / low-pitched or every other sample is silent, the
  de-interleave is not taking effect - check that the codec (kCodec) build path is
  active for this board.

## 3. Touch inside the real UI (production firmware)

- Flash `test-cyd`. If this is the board's first Nimbus boot, set the panel over
  the console once: send `SCREEN tft` (persists and restarts). It then boots into
  the color UI.
- Complete Wi-Fi setup (see the setup wizard) so the device leaves the onboarding
  screen, OR open the settings menu.
- **Pass:** tapping menu rows / cards highlights and navigates at the spot you
  touch. If taps land offset, revisit the touch calibration default (step 1).

## 4. microSD (SDMMC)

- Insert a **FAT32** card (reformat cards over 32 GB - they ship exFAT).
- With `test-cyd` running, over the console run `STATUS` and read the `sd=` field,
  or run `SDCHECK`.
- **Pass:** `sd=present` and `SDCHECK` reports a size. This path is new (SDMMC, not
  SPI) and was not bench-validated during development.

## 5. Battery (only if a 1S Li-ion pack is fitted)

- Fit a single-cell Li-ion pack to the battery port. On `test-cyd`, `STATUS`
  reports the pack voltage.
- **Pass:** a plausible per-cell voltage (3.3-4.2 V). The boot log no longer prints
  "not configured as analog channel" (the sense pin is now GPIO 9). If the voltage
  reads wrong, set the divider in web Settings (this board is divide-by-2).

## 6. On-screen ring (Notifier mode, no physical ring)

- With `test-cyd` in Notifier mode, drive a session from a host broker (or inject a
  frame). The color panel draws a ring of dots on the right, cards on the left.
- **Pass:** each active session shows an arc of dots in its status color, and the
  focused session's cursor glow tracks - the same information the LED ring gives on
  the hand-built boards. The single on-board RGB LED (GPIO 42) is a status pixel,
  not the ring.
- **Not a bug:** the ring/panel shows literally whatever the connected broker sends.
  If your broker has stale sessions from earlier test runs, you may briefly see more
  "active" dots than you expect, mostly dim (Idle sessions render dim/white by
  design). This self-heals within `ambientHoldFor(posture)` of no further frames for
  a stale session (5s Dark / 30s Calm / 300s Full) - the device's own timeout, not a
  reconnect you need to trigger. If it never converges, the broker itself is the
  thing to check (it should prune dead sessions before it connects), not the device.

## 7. Wi-Fi, memory, a full turn

- These reuse the existing HIL layers (they are board-agnostic): `test_l4`
  (Wi-Fi), `test_l23` (stack health). Run the Freenove-specific layer:

```bash
python3 -m pytest tests/hil/test_l27_freenove.py -m "hil and not manual" --allow-hardware
```

- For Orchestrator mode: add a provider key, then over Telegram or by voice
  (hold-to-talk on the mic bar) run one turn and confirm a reply.
- **Pass:** the device completes one turn end to end.

## What was already proven vs. what needs you

| Subsystem | Status |
|---|---|
| Display (panel, colors, backlight) | Validated on hardware (dev) |
| Capacitive touch reads | Validated on hardware (dev) |
| Touch orientation in the UI | Needs step 3 |
| Speaker (ES8311 DAC + amp) | Power-amp enable (GPIO1, active-low) landed; confirm the beep in step 2 |
| Mic (ES8311 ADC) | Stereo-to-mono de-interleave landed; confirm clean echo in step 2 |
| microSD (SDMMC) | Needs step 4 - NOT yet bench-validated |
| Battery sense | Needs step 5 |
| On-screen ring | Needs step 6 |
| Wi-Fi / memory / a turn | Needs step 7 |
