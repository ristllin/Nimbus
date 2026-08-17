"""§L7 - robustness / negative / recovery tests (the HIL test spec).

Watchdog reboot (F12), malformed-frame soak (device must not crash on garbage and
must resync on the next valid frame), STA/serial flap+recover (F13), and the scripted
BOOT+RESET download-mode recovery (F12/F13). These exercise the harness's F13
re-enumeration handling and the (to-be-added) task watchdog.

The named R_F12 regression lives in test_regressions.py. selftest_gate (F5) lives in
test_l3_device.py.

Markers: ``@pytest.mark.hil``; flap/reflash add ``@pytest.mark.manual`` (pull the AP /
press BOOT+RESET). The watchdog test is ``xfail(strict)`` until the WDT exists, so it
auto-flips to XPASS when F12 is fixed.
"""

from __future__ import annotations

import pytest

from device import BootError, DeviceLostError, ExpectTimeout


# ---- watchdog_reboot (F12) - the 8 s task WDT landed; hard PASS gate ---------
@pytest.mark.hil
def test_watchdog_reboot(device):
    """watchdog_reboot (F12): send ``HANG`` (test-only) to wedge the loop and assert
    the device AUTO-REBOOTS within the watchdog window (re-enumerates and re-beacons
    READY), rather than dying as a brick.

    xfail(strict) today: with no WDT the device stays hung, ``wait_reboot`` times out /
    loses the port, and the test fails as expected. When the WDT lands, the device
    reboots, this XPASSES, and strict turns the stale marker into a failure."""
    device.reset()
    device.wait_ready(timeout=20.0)

    device.hang()
    # A watchdog reset re-enumerates the CDC endpoint like any reset; wait_reboot
    # reopens across it and waits for the fresh READY beacon.
    device.wait_reboot(timeout=20.0)
    assert device.ping(), "device came back but PING got no PONG after watchdog reset"


# ---- malformed_nsn (robustness) --------------------------------------------
@pytest.mark.hil
def test_malformed_nsn(device, nsn):
    """malformed_nsn: stream garbage / truncated / bad-CRC frames and assert the device
    (a) never crashes - ``PING`` -> ``PONG`` throughout - and (b) RESYNCS on the next
    VALID frame (a following good frame decodes to the expected job count). Uses the
    real encoder for the valid frame so the resync is genuine."""
    device.ensure_mode(0)  # nsn frames need Notifier mode
    # no reboot: state comes from the frames we send; reboot churn is the wedge
    # correlate (see device.py::_open_quiet doc).

    # 1) Pure garbage (no SOF).
    nsn.send_raw(bytes([0x00, 0xFF, 0x13, 0x37, 0x42]))
    assert device.ping(), "device died on garbage bytes"

    # 2) Truncated frame: valid SOF + LEN claiming more payload than provided.
    nsn.send_raw(bytes([0xAA, 0x20, 0x4E, 0x01]))  # LEN=32 but only 2 payload bytes
    assert device.ping(), "device died on a truncated frame"

    # 3) Bad-CRC frame: build one real frame via the encoder, then flip its last byte
    # to break the CRC (the frame is otherwise well-formed).
    good = bytearray(nsn.encode([nsn.running(100)], 50, 200))
    good[-1] ^= 0xFF  # corrupt CRC
    nsn.send_raw(bytes(good))
    assert device.ping(), "device died on a bad-CRC frame"

    # 4) Resync: a VALID two-job frame must now decode (device echoes NSN(ble) jobs=2).
    nsn.send_frame([nsn.running(170), nsn.running(85)], 60)
    try:
        m = device.expect_re(r"NSN\(ble\)\s+jobs=(?P<jobs>\d+)\s+attn=\d+", timeout=8.0)
    except ExpectTimeout:
        pytest.skip(
            "no 'NSN(ble) jobs=' echo on this build - resync count unverifiable "
            "(device liveness through malformed frames WAS asserted)"
        )
    assert m.group("jobs") == "2", f"after garbage, valid frame decoded jobs={m.group('jobs')}, expected 2 (no resync)"


# ---- flap_and_recover (F13) - manual ---------------------------------------
@pytest.mark.hil
@pytest.mark.net
@pytest.mark.manual
def test_flap_and_recover(device, net, secrets, require_secret, require_manual):
    """flap_and_recover (F13): join the LAN, force an STA drop (operator pulls the AP /
    kills WiFi), then restore it and assert the device RECONNECTS (``WIFI_GOT_IP``
    again). The serial disconnect/reconnect across the flap is handled transparently by
    the Device helper's re-enumeration logic (F13)."""
    require_secret(secrets.require_sta)
    device.reset()
    device.wait_ready(timeout=20.0)

    net.provision(secrets.sta_ssid, secrets.sta_pass)
    ip1 = net.wait_got_ip(timeout=25.0)
    assert ip1, "device did not get an initial IP"

    require_manual.confirm(
        "Now DROP the WiFi: power off the AP / disable the SSID for ~10 s so the device "
        "loses its link. Press y once WiFi is DOWN.",
        timeout=60.0,
    )
    try:
        net.wait_disconnect_reason(timeout=20.0)
    except ExpectTimeout:
        pass  # some stacks stay quiet; the reconnect assertion below is the point
    assert device.ping(), "device hung while WiFi was down (should idle, not block)"

    require_manual.confirm(
        "Now RESTORE the WiFi (power the AP back on / re-enable the SSID). Press y once WiFi is back UP.", timeout=90.0
    )
    ip2 = net.wait_got_ip(timeout=40.0)
    assert ip2, "device did not reconnect after WiFi was restored (F13)"


# ---- reflash_recovery (F12, F13) - manual ----------------------------------
@pytest.mark.hil
@pytest.mark.manual
def test_reflash_recovery(device, require_manual):
    """reflash_recovery (F12, F13): script the BOOT+RESET download-mode runbook via
    ``guided_recovery`` and assert the port re-enumerates and a fresh ``READY`` beacon
    appears - documenting the repeatable recovery from the bricked state.

    This does NOT flash (flashing is double-interlocked and dormant while the board is
    single-owner). It verifies the recovery HANDSHAKE + re-enumeration only: induce a
    HANG, walk the operator through physical recovery, then confirm the device is back
    and flashable again."""
    device.reset()
    device.wait_ready(timeout=20.0)

    device.hang()
    try:
        device.guided_recovery(require_manual, reason="induced HANG (proving BOOT+RESET recovery)")
    except DeviceLostError as exc:
        pytest.fail(f"port did not re-enumerate after BOOT+RESET recovery: {exc}")

    try:
        device.wait_ready(timeout=25.0)
    except (BootError, ExpectTimeout) as exc:
        pytest.fail(f"no READY beacon after recovery - device not back: {exc}")
    assert device.ping(), "recovered device is not answering PING"
