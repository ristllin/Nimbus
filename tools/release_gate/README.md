# The release gate

A firmware release candidate does not ship until this gate is green. It exists
because of the 2026-08-24 incident day, when a single release put a white screen,
a universal tunnel 502, a rotated touch surface, and reconnect flapping in front
of customers at once. Every one of those failure classes now has a test that
fails on the pre-fix build and passes on the fixed one, so the gate catches the
bug before the release, not the customer after it.

Cost is not a reason to skip a leg. Real flashes, real relay traffic, and a human
looking at the glass are all in scope.

## What each leg catches

| Failure class (2026-08-24) | Gate leg | Where |
|---|---|---|
| White screen, pixels never reach the glass (CUM-167) | driver-pin check + ELF e-paper check + on-hardware render-to-glass with a human glance | `check_driver_pin.py`, `check_elf_symbols.py`, `tests/hil/test_l29_release_gate.py::TestRenderToGlass` |
| Device tunnel serves a 502 for every request (CUM-173) | CLOUDLOOP serves the local page, plus the cloud connected-device-5xx interstitial | `tests/hil/test_l29_release_gate.py::TestTunnelLoopback`, cumulo `e2e/release-gate-tunnel-502.e2e.test.ts` |
| Touch rotated 180 (CUM-160) | injected tap mapping + physical tap under an adversarial flip | `tests/hil/test_l29_release_gate.py::TestTouchCorrectness` |
| Watchdog starvation / tunnel-slot reset (N15) | HANG must trip the task watchdog and reboot | `tests/hil/test_l29_release_gate.py::TestCrashLoopResilience` |
| Reconnect flapping under relay restarts | reconnect-storm resilience | cumulo `e2e/release-gate-reconnect-storm.e2e.test.ts` |
| Bad OTA image boot-loops | OTASIM bad image rolls back within the boot-guard budget | `tests/hil/test_l29_release_gate.py::TestOtaRollback` |
| Panel-heal stops re-arming a slept panel (CUM-231) | past the trust window the panel is re-armed unconditionally; `healthy()` never gates it | `test/test_panel_heal`, `test/test_panel_health` |
| Settings lost across an update (CUM-237) | the OTA flow writes only its own bookkeeping keys, never an owner's Wi-Fi / provider key / touch cal / theme | `check_ota_preserves_nvs.py`, plus the provision -> OTA -> power-cycle -> read-NVS bench leg |

## Running it

Host checks (run in CI, fast):

```
python3 tools/release_gate/run_gate.py --host-only     # exit 1 blocks the release
python3 -m pytest tools/release_gate                   # tests the gate logic itself
python3 tests/release_gate/run_host_legs.py            # one verdict across every host leg
```

`tests/release_gate/run_host_legs.py` aggregates the host unit legs (white-screen,
touch, boot-loop) and the source-guard checks into a single PASS/FAIL, and
`tests/release_gate/MANIFEST.md` maps every failure class to its host or bench leg
with the exact bench procedure.

Full battery (prints the exact hardware and cloud commands to run and paste):

```
python3 tools/release_gate/run_gate.py
```

The hardware legs need the bench and `--allow-hardware`; the render-to-glass and
physical-tap legs end in a recorded human confirmation. The cloud legs run in the
cumulo repo (`pnpm test`, `pnpm e2e`, the browser suite, and the post-deploy
staging smoke).

## Block condition (CUM-174 #6)

Do not cut a release tag or flash client devices until every section printed by
`run_gate.py` is green and pasted into the release record. The host checks fail
the build automatically; the hardware and cloud legs are an explicit checklist so
they cannot be silently skipped.

The driver-pin check passes once the firmware pins a driver off the white-screen
denylist (the tree pins v0.7.2). If a release candidate ever regresses the pin
back onto a denylisted version, the check fails the build - that is the gate doing
its job.
