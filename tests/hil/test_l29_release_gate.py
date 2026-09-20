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

from device import ExpectTimeout


# --- helpers -----------------------------------------------------------------
def _panel_reads_gated(device) -> bool:
    """True on a readback-gated (shared-MISO) board where panel register reads are
    not a trustworthy liveness signal (CUM-392/423). Keyed on the firmware's own
    ``PROBES ? panelReadsGated=1`` report. A board that does not answer PROBES (an
    older build) is treated as non-gated so the strict assertions still apply."""
    try:
        m = device.cmd_re("PROBES ?", r"panelReadsGated=(\d)", timeout=6.0)
    except ExpectTimeout:
        return False
    return m.group(1) == "1"


def _scrok(device) -> str:
    """STATUS scrok= tri-state: '1' (answering), '0' (a disqualifier held), or
    'unknown' (a shared-MISO board that cannot self-check)."""
    m = re.search(r"scrok=(\w+)", device.status().string)
    return m.group(1) if m else ""


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
        gated = _panel_reads_gated(device)
        device.drain(quiet=0.2)
        device.send("TFTPWR?")
        # Read past the expected-MADCTL line to the post-rearm power readback (which
        # carries BOTH the power register and the status register).
        device.expect_re(r"madctl_expect=0x([0-9A-Fa-f]+)", timeout=6.0)
        pm = device.expect_re(
            r"after-rearm rddpm=0x([0-9A-Fa-f]+)\s+rddst=0x([0-9A-Fa-f]+)", timeout=6.0
        )
        rddpm = int(pm.group(1), 16)
        rddst = int(pm.group(2), 16)
        if gated:
            # Shared-MISO board (CUM-392/423): RDDPM is NOT panel-specific-readable
            # here (this Solide reads 0x00 at every width), so rddpm!=0 is not a
            # liveness signal. The honest tri-state instead: RDDST still answers with
            # its status signature (non-zero), and scrok reports 'unknown' - not '0',
            # which would be a hard disqualifier (a genuinely dead panel reads rddst=0
            # and scrok='0', so this still fails on the fault the strict check
            # guarded). The definitive pixel proof is the human glance below.
            assert rddst != 0, (
                f"gated board: RDDST read back all zero (panel not answering): "
                f"rddpm=0x{rddpm:08x} rddst=0x{rddst:08x}"
            )
            assert _scrok(device) == "unknown", (
                "gated board did not report scrok=unknown - liveness classification "
                "drifted (a shared-MISO board cannot self-check pixels)"
            )
            return
        # Non-gated board: RDDPM IS a real display-on signal - keep the strict check,
        # and its value is panel-specific so we only assert it is not the all-zero
        # 'nothing answered'.
        assert rddpm != 0, "panel power register read back all zero (panel not answering)"

    def test_panel_recovers_from_a_silent_reset(self, device):
        # TFTBREAK resets the panel behind the driver (white-screen on demand);
        # the health watchdog must notice and repaint WITHOUT a restart.
        if _panel_reads_gated(device):
            # Shared-MISO board (CUM-392/423): no register read can OBSERVE the
            # TFTBREAK, so the heal-counter drill cannot run honestly here (the
            # counter only advances while the register probe is on, and this board
            # gates that off). The repaint path is still driven - the watchdog
            # rearm()s unconditionally - and L21's own test_panel_recovers_from_a_
            # silent_reset exercises it with the probe explicitly enabled. What this
            # gate proves on a gated board is the honest tri-state: the firmware
            # stays alive and self-reports healthy across the break (a wedge or a
            # latched-unhealthy state IS a real fault), RDDST still answers, and
            # scrok reports 'unknown'. The definitive pixel proof is the human
            # glance in test_human_confirms_pixels_reach_the_glass.
            h0 = device.cmd_re("TFTHEALTH?", r"healthy=(\d+)\s+heals=(\d+)", timeout=6.0)
            assert int(h0.group(1)) == 1, "panel already unhealthy before the drill"
            device.cmd("TFTBREAK", "TFTBREAK", timeout=8.0)
            time.sleep(3.0)
            assert device.ping(timeout=6.0), "console wedged after TFTBREAK (no in-place recovery)"
            h1 = device.cmd_re("TFTHEALTH?", r"healthy=(\d+)\s+heals=(\d+)", timeout=6.0)
            assert int(h1.group(1)) == 1, (
                "panel latched unhealthy after TFTBREAK - the repaint watchdog did "
                "not rearm the panel on a gated board"
            )
            st = device.cmd_re("TFTPWR?", r"rddst=0x([0-9A-Fa-f]+)", timeout=6.0)
            assert int(st.group(1), 16) != 0, "RDDST read back zero (panel not answering) after the break"
            assert _scrok(device) == "unknown", "gated board did not report scrok=unknown after the break"
            return

        # Non-gated board: the strict heal-counter drill. heals only counts while
        # the register probe is on (the shipped default is off, so the watchdog
        # rearm()s without classifying), so enable it for the window and restore it.
        device.cmd("PANELPROBE 1", "PANELPROBE", timeout=5.0)
        try:
            h0 = device.cmd_re("TFTHEALTH?", r"healthy=(\d+)\s+heals=(\d+)", timeout=6.0)
            assert int(h0.group(1)) == 1, "panel already unhealthy before the drill"
            heals0 = int(h0.group(2))
            brk = device.cmd("TFTBREAK", "TFTBREAK", timeout=8.0)
            m = re.search(r"healthy=(\d)", brk)
            if m and m.group(1) == "1":
                # TFTBREAK could not inject a reset: this board's TFT_RST is not
                # wired to a GPIO (Freenove CYD, tft.rst=-1), so holdReset() is a
                # no-op. The heal counter is honest (it ticks only on a real
                # classified reset), so skip loudly rather than assert a heal that
                # had no cause to happen. This leg needs a board with TFT_RST wired.
                pytest.skip(
                    "TFTBREAK did not induce a silent reset (healthy=1 right after): "
                    "this board's TFT panel RST is not wired to a GPIO, so the "
                    "register-probe heal path cannot be exercised here."
                )
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
        finally:
            device.cmd("PANELPROBE 0", "PANELPROBE", timeout=5.0)  # shipped default

    @pytest.mark.hil
    @pytest.mark.manual
    def test_human_confirms_pixels_reach_the_glass(self, device, require_manual):
        # THE definitive CUM-167 catch. Registers and readback can all look healthy
        # while the glass is blank white (field-proven). A human must look.
        #
        # This is a require_manual step, so it MUST carry @pytest.mark.manual: an
        # automated `-m "hil and not manual"` run has no operator, and without the
        # marker this collected under that run and failed (stdin is not a TTY). It
        # is the pixel-truth oracle for the gated-board panel tests above.
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
        #
        # The oracle is MENU? open=1/0, NOT a RENDER? screen NAME: the console emits
        # a numeric screen id (RENDER screen=%d) and ScreenId wire numbers are frozen
        # and positionally mirrored, so a name is not on the wire. MENU? open=1 is
        # what actually proves the gear tap landed on the menu (it is the same oracle
        # L21's passing test_tap_opens_and_closes_the_menu uses).
        device.ensure_status_idle()
        device.cmd("TAP 300 22", "TAP<", timeout=4.0)  # gear target
        time.sleep(0.6)
        opened = device.cmd("MENU?", "MENU ", timeout=4.0)
        assert "open=1" in opened, f"gear tap did not open the menu: {opened!r}"
        device.cmd("TAP 20 22", "TAP<", timeout=4.0)  # Back
        time.sleep(0.6)
        closed = device.cmd("MENU?", "MENU ", timeout=4.0)
        assert "open=0" in closed, f"Back did not close the menu: {closed!r}"
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
        # (reboot_and_confirm's wait_ready raises BootError on >=2 resets or any
        # panic signature). reboot_and_confirm PROVES the restart took on native
        # USB-CDC (a soft REBOOT that the chip ignores is escalated to a hard reset)
        # and returns the fresh uptime - so this no longer races the boot stream.
        up = device.reboot_and_confirm()
        assert up >= 0  # a parseable, settled STATUS after one confirmed clean boot

    def test_survives_n_reboots_without_a_loop(self, device):
        # Boot-loop DETECTION across N reboots (MANIFEST section 3). One clean boot
        # (the test above) is not the same claim: a device can survive a single
        # restart yet cascade into a reset storm under repeated reboots (a flapping
        # power rail, a boot-time init that occasionally faults). This drives N
        # consecutive reboots and asserts every one lands settled, never a loop.
        #
        # N is bounded so the bench leg stays quick; override for a longer soak.
        n = int(os.environ.get("NIMBUS_GATE_REBOOT_CYCLES", "5"))
        prev_up = device._uptime_or_none() or 0
        for cycle in range(1, n + 1):
            # reboot_and_confirm PROVES each restart took (escalating a no-op soft
            # REBOOT to an esptool hard reset) and raises if the board never comes
            # back - so a true boot loop surfaces here as a failure, not a hang, and
            # a reboot the chip ignored is caught rather than passing as "fresh".
            up = device.reboot_and_confirm(timeout=25.0)
            # A real reboot resets uptime: it comes back SMALL (reboot_and_confirm
            # guarantees a fresh boot or raises), and below the last reading.
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
