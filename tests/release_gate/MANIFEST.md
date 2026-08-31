# Release-gate MANIFEST - every lose-every-customer failure has a gate

The bar (CUM-174, owner mandate 2026-08-24): every failure class from the field
incident - white screen, universal tunnel 502, touch breakage, crash/boot loop,
reconnect flapping, settings lost across an update - has a test that FAILS on the
pre-fix code and PASSES on the fix, and no firmware tag or client flash ships
until the gate is green.

This file maps each class to its leg(s), says whether the leg runs on the HOST
(always-on, in CI) or the BENCH (needs a real board), and for every bench leg
gives the exact board, steps, and pass criteria. The host legs are aggregated by
`tests/release_gate/run_host_legs.py` (one verdict); the full battery, including
the hardware and cloud checklists, is printed by
`tools/release_gate/run_gate.py`. Cloud legs live in the cumulo repo and are not
duplicated here.

Run the host gate:

```
python3 tests/release_gate/run_host_legs.py     # every host leg, one PASS/FAIL
python3 tools/release_gate/run_gate.py          # + the hw/cloud checklist
```

---

## 1. White screen - pixels never reach the glass (CUM-167, CUM-231)

The 2026-08-24 white screen shipped two ways: a driver pinned to a broken display
init (CUM-167), and a panel-heal watchdog that stopped re-arming a silently-slept
panel (CUM-231). Both are gated.

| Leg | Where | What it pins |
|---|---|---|
| `test/test_panel_health` | HOST | The flip-aware `healthy()` MADCTL compare. Catches the partial MY/MX loss (`got=0xE8`) the v0.7.1 mask waved through, and does not thrash at flip=1 (the v0.7.0 full-byte bug). Regression witnesses for both prior versions are asserted in-file. |
| `test/test_panel_heal` | HOST | The rearm guard (`nimbus::panel::unchangedFrameAction` / `tickHealthAction`, wired into `src/hw/tft_out.cpp`). Past the trust window the panel is re-armed UNCONDITIONALLY; `healthy()` only decides whether to ALSO repaint. This is the exact policy that regressed: an early return when `healthy()` read true left a beacon-slept panel white. |
| `tools/release_gate/check_driver_pin.py` | HOST | Refuses a build pinned to a denylisted white-screen driver version (CUM-167 shipped on v0.6.0; fixed at v0.6.1). |
| `tools/release_gate/check_elf_symbols.py` | HOST (needs `pio run -e esp32s3` first) | Keeps the e-paper driver out of the TFT image and proves the display plumbing linked. |
| `test_l29_release_gate.py::TestRenderToGlass` | **BENCH** | Pixels actually reach the glass. See the bench procedure below. |

### BENCH leg - render reaches the glass (retroactive proof of CUM-167)

- **Board:** solide_s3 (a TFT + ring Nimbus) AND freenove_s3 (a Freenove CYD),
  per family. The RDDPM (0x0A) power-register readback legs are solide-only - the
  Freenove panel has no register readback (MISO), so gate those by board family
  or run them on the TFT board (see the 2026-08-25 note on CUM-174).
- **Steps:**
  1. Flash the release candidate: `pio run -e esp32s3 -t upload` (solide) /
     `pio run -e esp32s3-cyd -t upload` (freenove).
  2. `pytest tests/hil/test_l29_release_gate.py -m hil --allow-hardware` - drives
     `TFTFILL?`/`TFTPWR?`/`TFTBREAK` and asserts the panel reports display-on and
     recovers from a silent reset (RDDPM non-zero on the TFT).
  3. **Mandatory human glance** (`-m manual`): an operator looks at the screen and
     confirms the UI is drawn, not white. Record it in the release checklist.
- **Pass:** RDDPM reads non-zero after a forced `TFTBREAK` reset (panel re-armed,
  solide board); the operator confirms real pixels. On the pre-fix build (driver
  v0.6.0) the glance FAILS (white) - the retroactive proof.
- **NVS-adversarial pass (CUM-228 routing note):** run the render legs twice -
  once with default/no NVS, once with adversarial persisted `tchCal`/`tftFlip`/
  `scrModel` - so a fielded device's state is covered, not just a clean flash.

---

## 2. Touch breakage - taps land in the wrong place (CUM-203, CUM-189)

A stale or wrong-default calibration rotated the touch surface. The per-board
default and the 180 reconciliation are the host-provable core.

| Leg | Where | What it pins |
|---|---|---|
| `test/test_touch_cal::test_board_default_per_kind_orientation` | HOST | `boardDefaultCal(TouchKind)` differs per kind: resistive Solide (XPT2046) is swap-ONLY (the CUM-203 fix, so it stops mirroring one axis out of the box); capacitive Freenove (FT6336U) is swap + invertY (CUM-189, bench-verified). A `(void)kind` shared default can never come back. |
| `test/test_touch_cal` (parse / solve / wizard / `orientTouch`) | HOST | Calibration parse rejects malformed input without mutating; the corner solver derives the axis map; `orientTouch` is the single-source 180 (involutive, applied on top of the cal, never folded in). |

### BENCH leg - a physical tap lands where injected (CUM-203 class)

- **Board:** both families. Run once with default NVS and once with an adversarial
  `tchCal`/`tftFlip` (the stale-calibration interplay).
- **Steps:** `pytest tests/hil/test_l29_release_gate.py -m hil --allow-hardware`
  (`TestTouchCorrectness` - injected tap opens/closes the menu), then the
  `-m manual` physical-tap checklist: tap the four corners, confirm the hit lands
  under the finger. NVS is restored after.
- **Pass:** the injected tap maps to the expected control in both NVS states; the
  operator's physical taps land correctly. The touch-180 only reproduces with a
  real finger under an adversarial flip - hence the bench step.

---

## 3. Crash / boot loop - the device wedges or never boots clean (CUM-174 #4)

| Leg | Where | What it pins |
|---|---|---|
| `test/test_ota_logic::test_rollback_policy` | HOST | `shouldRollback(pending, bootCount)` = the app-level rollback guard: a pending fresh image that fails to go healthy for `kOtaMaxBootAttempts` (3) boots rolls back on the 4th. |
| `test/test_ota_logic::test_boot_healthy` | HOST | `bootHealthy(uptime, wifiEverUp)` - when a fresh image may mark itself valid (120 s crash-free with Wi-Fi up; 600 s on a Wi-Fi-dead site) so a fine image never false-rollbacks. |
| `test/test_fault` | HOST | The capability-fault registry the resilience paths are driven through. |

### BENCH legs - watchdog + boot-loop (device-only; honestly not host-provable)

The watchdog timer, the actual boot-count increment in NVS, and a real reboot
sequence are device behavior. They cannot be proven on the host - only the
*decision* (`shouldRollback`) is pure. These are bench legs:

- **Watchdog starvation:** `pytest tests/hil/test_l29_release_gate.py -m hil
  --allow-hardware` (`TestCrashLoopResilience::test_watchdog_reboots_a_wedged_loop`).
  Drive `HANG` on the serial console; the task watchdog must reboot the device.
  **Pass:** the device resets within the watchdog window and comes back up.
- **Boot-loop detection across N reboots:** `test_boots_without_a_crash_loop` -
  reset the board N times; it must reach a settled screen each time, never a
  reset storm. **Pass:** clean boot every cycle, no reboot loop.
- **OTA bad-image rollback:** `TestOtaRollback::test_bad_image_rolls_back_after_
  failed_boots` - stage a deliberately-bad image (`OTASIM`), let it fail to go
  healthy; the app-level guard flips back to the previous slot. **Pass:** device
  returns on the old slot with `otaLast="rollback vX"`. This is the on-device
  proof of the host `shouldRollback` decision.
- **Relay reconnect storm:** cloud-side; covered in the cumulo e2e
  (`release-gate-reconnect-storm`), NOT duplicated here.

---

## 4. Settings lost across an update (CUM-237)

An owner's provisioned state - Wi-Fi credentials, provider API key, measured touch
calibration, screen model, LED theme, battery config - lives in NVS and MUST
survive a firmware update. The OTA flow may write only its own bookkeeping.

| Leg | Where | What it pins |
|---|---|---|
| `tools/release_gate/check_ota_preserves_nvs.py` | HOST | Source guard over `src/sys/ota_update.cpp`: every `nvs_set_*` / `nvs_erase_key` in the OTA flow targets an `AKEY_OTA_*` bookkeeping key or the `otaSimCrash` drill flag - never a user key. FAILS the build if an update path writes Wi-Fi, a provider key, `tchCal`, `scrModel`, `theme`, etc. Retroactively proven RED by the injected-user-key cases in `tools/release_gate/test_release_gate.py`. |

### BENCH leg - the full survive-an-OTA proof (device-only)

The source guard proves no *code path* writes a user key; the end-to-end proof
that a real update preserves the flash needs a board:

- **Board:** either family, provisioned (Wi-Fi + provider key + a MEASURED touch
  cal + a non-default theme, so there is real user state to lose).
- **Steps:**
  1. Provision the device and record every user NVS key
     (`otaType`-excluded): Wi-Fi, provider key (masked), `tchCal`, `scrModel`,
     `theme`, `sfx*`, battery keys, `devName`.
  2. Run an OTA span end to end (check -> download -> verify -> reboot to the new
     image, `pytest tests/hil/test_ota.py --allow-hardware` or a real `/update`).
  3. Power-cycle the device (pull power, not just a soft restart).
  4. Read NVS back and diff against step 1.
- **Pass:** every user key is byte-identical after the update + power-cycle; only
  the OTA bookkeeping keys (`otaPend`/`otaBoots`/`otaLast`/...) changed. Wi-Fi
  rejoins and the provider still answers without re-entry. On a build that wrote a
  user key during OTA this diff is non-empty - the retroactive failure.

---

## 5. Fresh device - the first boot after a flash (CUM-245, CUM-203, CUM-189, CUM-230, CUM-211)

Every other leg runs on a PROVISIONED unit (cal solved, NVS good, a provider
keyed). The owner hits the FIRST boot after a new version - the exact state that
shipped touch mirrored (CUM-203) and uncalibrated (CUM-189) out of the box. AGENTS.md
section 3 makes that STATE a first-class test target; this class pins it.

| Leg | Where | What it pins |
|---|---|---|
| `test/test_fresh_device::test_every_touchkind_has_a_measured_default` | HOST | The per-kind default-cal table as a CLASS: EVERY `TouchKind` has a measured, valid, non-mirrored default, and no two kinds share one. A new controller class without a default fails - at runtime (`expectedFlagsFor`) and at COMPILE time (a `static_assert` on the `TouchKind::Count` sentinel). Verified RED against the `(void)kind` leak and against a new kind added with no default. |
| `test/test_fresh_device::test_all_board_models_resolve_a_correct_default` | HOST | Reads the driver's own board constants (`kBoardSolideS3`, `kBoardFreenoveS3`), maps each board's `touchKind` to the portable mirror (the same collapse `src/main.cpp` / `src/net/webui.cpp` make), and asserts each ships a correct default - so a mis-declared board `touchKind`, or a new `board_*.h`, is caught here. |
| `test/test_fresh_device::test_stored_cal_survives_a_flip_via_compose` + `..._is_a_bijection_over_the_panel` | HOST | A stored cal stays valid across a display flip: the 180 is `orientTouch`'s job, composed on top, never folded into the cal (CUM-160). The default flags are flip-independent (never `invertX`), and the flip point-reflects the whole panel with no dead zone. |
| `test/test_fresh_device::test_fresh_config_is_all_presets_no_overrides` + `..._absent_blob_leaves_defaults_untouched` | HOST | First-boot config resolution: a fresh `Config` is EXACTLY the Balanced presets over every param, no override leaking from uninitialized storage; an absent blob leaves defaults untouched (all-or-nothing). |
| `test/test_fresh_device::test_fresh_boot_posture_is_profile_seeded` | HOST | Onboarding posture (CUM-230): the fresh-boot ring level is the one the profile seeds (Balanced -> Calm), never an uninitialized Dark (invisible) or Full (drain). |
| `test/test_fresh_device::test_zero_keys_reads_as_no_provider` | HOST | Provider gate on zero keys (CUM-211): a fresh device with no key on any slot reads as no-provider - the input the engine's honest "set up a provider" reply is built on. The engine-level reply itself is `test/test_harness_turn` (not duplicated). |

### BENCH leg - first boot on real glass (CUM-245 leg 3; owed to the bench)

The host legs prove the fresh-boot DECISIONS are pure and correct. The out-of-box
experience on glass needs a board and is NOT host-provable - it is the bench half,
coordinated with the CUM-174 render/touch legs above (run their default-NVS pass):

- **Board:** both families, flashed CLEAN (erase flash, no NVS) so cal is absent.
- **Steps:**
  1. `pio run -e esp32s3 -t erase` then upload the candidate (freenove: `esp32s3-cyd`).
  2. Boot to first-run; BEFORE any calibration, inject the standard corner taps
     (`test_l29_release_gate.py::TestTouchCorrectness`) and assert they map within
     tolerance on the fresh default cal.
  3. Confirm render reaches the glass (section 1 bench leg) and the onboarding
     wizard can be completed - `store::onboarded()` is false out of the box.
- **NVS-adversarial variant:** boot once with an adversarial persisted `tchCal` /
  `tftFlip` (frozen keys survive a reflash) - the nimbus-4 touch-180-across-firmware
  case. Restore NVS after.
- **Pass:** on the fresh default the injected taps land correct on BOTH families and
  the UI is drawn (not white, not mirrored); the pre-CUM-203 build FAILS the resistive
  tap. Shares the render/tap bench harness with sections 1-2 (default-NVS pass).

---

## 6. Tunnel 502 - device serves 502 for every request (CUM-173) - NOT here

Covered by the cloud repo's lifecycle e2e (the connected-device 5xx interstitial)
and the device-side CloudLoop HIL leg
(`test_l29_release_gate.py::TestTunnelLoopback`). Do NOT add a duplicate host leg
in this suite. The large-page (>256 KB) tunnel leg and the reconnect storm are the
cumulo e2e's (CUM-228 routing note); the CloudLoop bench leg is the device half.

---

## Host vs bench at a glance

| Class | Host leg (always-on) | Bench leg (needs a board) |
|---|---|---|
| White screen | panel_health, panel_heal, driver-pin, elf-symbols | render-to-glass + human glance (RenderToGlass) |
| Touch | touch_cal (per-kind default, solve, orientTouch) | injected + physical tap (TouchCorrectness) |
| Crash/boot loop | ota_logic (shouldRollback, bootHealthy), fault | watchdog reboot, N-reboot, OTA rollback (CrashLoop/OtaRollback) |
| Settings-across-OTA | check_ota_preserves_nvs | provision -> OTA -> power-cycle -> read-NVS diff |
| Fresh device / default-NVS | fresh_device (touch class, flip compose, fresh config/posture, zero-key gate) | clean-flash first-boot tap + render + onboarding, NVS-adversarial |
| Tunnel 502 | (cloud repo) | CloudLoop (TunnelLoopback) |
