"""L29 - THE RELEASE GATE (on-hardware legs).

Every failure class from the 2026-08-24 incident day gets a test here that
asserts through a REAL seam, so a firmware release candidate cannot ship the same
bug twice. Each leg names the bug it retroactively catches:

  * CUM-173  device tunnel served a 502 for every request (loopback refused)
  * CUM-167  solide_s3 white screen (pixels never reached the glass)
  * CUM-160  touch-180 (taps landed rotated)
  * N15      watchdog starvation / tunnel-slot hold -> reset
  * OTA      a bad image must roll back, not boot-loop

These are HARDWARE tests: they collect cleanly with no board (`--collect-only`)
and only run under `--allow-hardware`. The definitive display/touch checks end in
a MANDATORY human glance (require_manual) whose result is RECORDED - a green log
line is never accepted as proof that pixels reached the glass.

Run:
    pytest tests/hil/test_l29_release_gate.py -m hil  --allow-hardware --port <port>
    pytest tests/hil/test_l29_release_gate.py -m net  --allow-hardware   # CLOUDLOOP leg
"""

from __future__ import annotations

import os
import re
import time

import pytest


# --- helpers -----------------------------------------------------------------
def _require_wifi(device) -> None:
    """Skip loudly if the board is not on Wi-Fi (the tunnel/loopback legs need the
    local web server reachable at its STA IP)."""
    m = device.status()
    line = m.string
    if "wifi=1" not in line and "ip=" not in line:
        pytest.skip("device is not on Wi-Fi; the tunnel-serving legs need the STA up")


def _board_family(device) -> str:
    """Return solide_s3 / freenove_s3 from the STATUS board= field ('' if absent)."""
    try:
        line = device.status().string
    except Exception:
        return ""
    m = re.search(r"board=(\S+)", line)
    return m.group(1) if m else ""


# ============================================================================
# CUM-173: the device's own tunnel-serving loopback must return the local page,
# not a 502. Pre-fix, doLoopback() connected to 127.0.0.1 (no lwIP loopback
# netif) then WiFi.localIP() (no hairpin) - both refused in ms, so CLOUDLOOP
# returned -1 / the online handler framed a 504/502. This leg FAILS on that code
# and PASSES once the in-process dispatch seam lands.
# ============================================================================
@pytest.mark.net
class TestTunnelLoopback:
    def test_cloudloop_serves_the_local_page(self, device):
        _require_wifi(device)
        # CLOUDLOOP <path> -> <status>\n<body preview>
        m = device.cmd_re(
            "CLOUDLOOP /api/state",
            r"CLOUDLOOP\s+/api/state\s+->\s+(-?\d+)",
            timeout=20.0,
        )
        status = int(m.group(1))
        assert status == 200, (
            f"tunnel loopback returned {status}, not 200 - the device answers its own "
            "tunneled requests with an error (CUM-173: 127.0.0.1 / self-IP connect "
            "refused). A browser through d.cumulo-nimbus.ai would see a 502."
        )
        # The body preview must be the real /api/state JSON, not empty.
        body = device.expect("{", timeout=3.0)
        assert '"' in body, "loopback returned a 200 with no page body"

    def test_cloudloop_serves_the_ui_root(self, device):
        _require_wifi(device)
        m = device.cmd_re("CLOUDLOOP /", r"CLOUDLOOP\s+/\s+->\s+(-?\d+)", timeout=20.0)
        assert int(m.group(1)) == 200, "the device UI root did not serve over the loopback path"


# ============================================================================
# CUM-167: pixels must actually reach the glass. TFTFILL? drives the real
# full-panel path and reads pixels back from the far corners; TFTPWR? confirms
# the panel reports display-on; then a MANDATORY human glance is the definitive
# proof (register health cannot see the white-screen failure mode).
# ============================================================================
@pytest.mark.hil
class TestRenderToGlass:
    def test_gram_holds_a_full_frame(self, device):
        # Three fills (red/green/blue); each line ends OK or MISMATCH.
        device.drain(quiet=0.2)
        device.send("TFTFILL?")
        results = {}
        deadline = time.time() + 8.0
        for _ in range(3):
            line = device.expect_re(
                r"TFTFILL\s+(\w+)\s+want=0x([0-9A-Fa-f]+).*\b(OK|MISMATCH)\b",
                timeout=max(0.5, deadline - time.time()),
            )
            results[line.group(1)] = line.group(3)
        assert results, "TFTFILL? produced no readback lines"
        bad = {k: v for k, v in results.items() if v != "OK"}
        assert not bad, f"GRAM/blit/window readback mismatched: {bad} (panel data path is broken)"

    def test_panel_reports_display_on(self, device):
        # RDDPM (0x0A) bit 2 = display on; the driver also prints the expected MADCTL.
        device.drain(quiet=0.2)
        device.send("TFTPWR?")
        # Read past the expected-MADCTL line to the post-rearm power readback.
        device.expect_re(r"madctl_expect=0x([0-9A-Fa-f]+)", timeout=6.0)
        # A readable, non-zero power register is the minimum; the value itself is
        # panel-specific, so we assert it is not the all-zero 'nothing answered'.
        pm = device.expect_re(r"after-rearm rddpm=0x([0-9A-Fa-f]+)", timeout=6.0)
        assert int(pm.group(1), 16) != 0, "panel power register read back all zero (panel not answering)"

    def test_panel_recovers_from_a_silent_reset(self, device):
        # TFTBREAK resets the panel behind the driver (white-screen on demand);
        # the health watchdog must notice and repaint WITHOUT a restart.
        h0 = device.cmd_re("TFTHEALTH?", r"heals=(\d+)", timeout=6.0)
        heals0 = int(h0.group(1))
        device.cmd("TFTBREAK", "TFTBREAK", timeout=6.0)
        # Poll until the heal counter increments and health returns.
        deadline = time.time() + 12.0
        healed = False
        while time.time() < deadline:
            time.sleep(1.0)
            m = device.cmd_re("TFTHEALTH?", r"healthy=(\d+)\s+heals=(\d+)", timeout=6.0)
            if int(m.group(2)) > heals0 and int(m.group(1)) == 1:
                healed = True
                break
        assert healed, "panel did not self-heal after TFTBREAK (white-screen recovery is broken)"

    @pytest.mark.hil
    def test_human_confirms_pixels_reach_the_glass(self, device, require_manual):
        # THE definitive CUM-167 catch. Registers and readback can all look healthy
        # while the glass is blank white (field-proven). A human must look.
        device.send("TFTFILL?")  # leave a solid color on the panel to judge against
        time.sleep(0.5)
        require_manual.confirm(
            "LOOK AT THE SCREEN. It should show a solid color fill (red, then green, "
            "then blue), NOT a blank white/dark panel. Is the panel clearly lit and "
            "showing the color?",
            timeout=90.0,
        )


# ============================================================================
# CUM-160: touch must land where the finger is. The console inject path carries
# post-orient (logical) coordinates and deliberately bypasses the read+orient
# mirror, so the 180 symptom is ONLY reproducible with a physical tap. The
# automated leg proves tap->region mapping; the manual leg (opt-in, bench board
# only, restores NVS) proves the physical orientation under adversarial flip.
# ============================================================================
@pytest.mark.hil
class TestTouchCorrectness:
    def test_injected_tap_opens_and_closes_the_menu(self, device):
        # Tap the gear (top-right) to open the menu, Back to leave. Proves the
        # tap coordinate resolves to the right on-screen target.
        device.ensure_status_idle()
        device.cmd("TAP 300 22", "TAP<", timeout=4.0)  # gear target
        m = device.cmd_re("RENDER?", r"screen=(\w+)", timeout=4.0)
        assert m.group(1).lower().startswith("menu"), f"gear tap did not open the menu (screen={m.group(1)})"
        device.cmd("TAP 20 22", "TAP<", timeout=4.0)  # Back
        device.ensure_status_idle()

    @pytest.mark.manual
    def test_physical_tap_under_adversarial_flip(self, device, require_manual):
        # Invasive: flips the panel orientation in NVS to force the exact stale-
        # calibration interplay of the touch-180 class, asks a human to tap a known
        # target, then RESTORES the original flip. Opt-in and bench-board only so it
        # never mutates a personal unit's config.
        if os.environ.get("NIMBUS_GATE_TOUCH_ADVERSARIAL") != "1":
            pytest.skip("set NIMBUS_GATE_TOUCH_ADVERSARIAL=1 to run the invasive adversarial-flip tap")
        family = _board_family(device)
        if family and family != "freenove_s3":
            pytest.skip(f"adversarial-flip tap runs on the bench freenove only (board={family})")

        # Read the current flip so we can restore it. TFTPWR? prints the expected
        # MADCTL, which encodes the flip (0xE8 flipped, 0x28 unflipped).
        cur = device.cmd_re("TFTPWR?", r"madctl_expect=0x([0-9A-Fa-f]+)", timeout=6.0)
        was_flipped = cur.group(1).lower() == "e8"
        try:
            device.cmd("TFTFLIP", "TFTFLIP", timeout=8.0)  # toggle + persist + restart
            device.wait_reboot(timeout=20.0)
            device.ensure_status_idle()
            require_manual.confirm(
                "Tap the TOP-LEFT corner of the screen. The tap indicator / menu "
                "gear must respond at the corner you touched, NOT the opposite "
                "corner. Did touch track your finger correctly?",
                timeout=90.0,
            )
        finally:
            # Restore the original orientation regardless of the outcome.
            now = device.cmd_re("TFTPWR?", r"madctl_expect=0x([0-9A-Fa-f]+)", timeout=6.0)
            if (now.group(1).lower() == "e8") != was_flipped:
                device.cmd("TFTFLIP", "TFTFLIP", timeout=8.0)
                device.wait_reboot(timeout=20.0)


# ============================================================================
# N15 / crash-loop resilience: a wedged loop must be caught by the task watchdog
# and reset the board (not hang forever), and the device must come back.
# ============================================================================
@pytest.mark.hil
class TestCrashLoopResilience:
    def test_watchdog_reboots_a_wedged_loop(self, device):
        before = int(device.status().group("up"))
        device.hang()  # wedge the Arduino loop; TWDT (8s, panic) must fire
        # The reset re-enumerates the CDC endpoint; wait for the READY beacon.
        device.wait_reboot(timeout=20.0)
        after = int(device.status().group("up"))
        assert after < before or after < 15, (
            f"uptime did not reset after HANG (before={before}s after={after}s) - the "
            "watchdog did not reboot the wedged loop (N15 starvation class)"
        )

    def test_boots_without_a_crash_loop(self, device):
        # A clean reboot must come up READY without repeated rst:/panic markers
        # (wait_ready raises BootError on >=2 resets or any panic signature).
        device.reset()  # in-place self-healing reboot
        m = device.status()
        assert int(m.group("up")) >= 0  # a parseable STATUS after one clean boot

    def test_survives_n_reboots_without_a_loop(self, device):
        # Boot-loop DETECTION across N reboots (MANIFEST section 3). One clean boot
        # (the test above) is not the same claim: a device can survive a single
        # restart yet cascade into a reset storm under repeated reboots (a flapping
        # power rail, a boot-time init that occasionally faults). This drives N
        # consecutive reboots and asserts every one lands settled, never a loop.
        #
        # N is bounded so the bench leg stays quick; override for a longer soak.
        n = int(os.environ.get("NIMBUS_GATE_REBOOT_CYCLES", "5"))
        prev_up = int(device.status().group("up"))
        for cycle in range(1, n + 1):
            # reset() self-heals and raises if the console never answers again - a
            # board wedged in a reset storm can never confirm the soft REBOOT, so a
            # true boot loop surfaces here as a failure, not a hang.
            device.reset()
            m = device.status(timeout=6.0)
            up = int(m.group("up"))
            # A real reboot resets uptime: it must come back SMALL, and below the
            # last reading, or the "reboot" was a no-op / the device never restarted.
            assert up < 15, (
                f"cycle {cycle}/{n}: uptime={up}s after a reboot is not a fresh boot "
                "(the device did not actually restart, or is stuck past the boot window)"
            )
            assert up <= prev_up or prev_up < 5, f"cycle {cycle}/{n}: uptime did not reset (prev={prev_up}s now={up}s)"
            prev_up = up
        # After the last cycle the device must still be serving the console - a
        # settled, non-looping state, not a board that only answers between resets.
        assert device.ping(timeout=6.0), f"device did not settle after {n} reboots (boot-loop / reset storm)"


# ============================================================================
# OTA: a bad image must roll back after the boot-guard's attempts, never
# boot-loop forever. Driven by the OTASIM console drill (synthetic bad image).
# ============================================================================
@pytest.mark.hil
class TestOtaRollback:
    def test_bad_image_rolls_back_after_failed_boots(self, device):
        # OTASIM crash makes every pending boot abort() (synthetic bad image); the
        # app-level boot guard must flip back to the previous slot within its
        # attempt budget instead of looping forever.
        device.drain(quiet=0.2)
        device.send("OTASIM arm crash")
        try:
            device.expect("OTASIM", timeout=6.0)
        except Exception:
            pytest.skip("OTASIM drill not available on this build")
        try:
            # The device reboots into the bad image, burns attempts, then rolls back.
            device.wait_reboot(timeout=30.0)
            # After rollback it must be alive and serving the console again.
            assert device.ping(timeout=6.0), "device did not recover after an OTA rollback (boot-loop risk)"
        finally:
            # Always clear the sim residue - even if an assertion above failed - so
            # the "sim-arm ..." last-result never survives into a later flash of
            # this bench board and surface to an owner (CUM-264).
            device.send("OTASIM clear")
