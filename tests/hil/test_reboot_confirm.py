"""CUM-418: host tests for the native-USB confirmed-reboot helper.

Pure host tests (no board): they prove the DECISION logic device.reboot_and_confirm
depends on - "did a reboot actually take?", "read the boot stream, not a STATUS
poll the SD scan blocks", and "a failed escalation must not leave the console
closed" - with scripted fakes and recorded boot streams so the hardening itself is
covered, not just exercised live.

The bench symptoms these guard against:
  * rerun 1: a soft REBOOT the S3 ignored over native USB-CDC kept uptime climbing
    (uptime=3254s "after a reboot") and reset()+status() read it as a pass.
  * rerun 2 item 1/3: a REAL reboot (rst: -> READY in ~12 s) whose post-boot SD
    scan blocked STATUS, so the old confirm read uptime=None and wrongly escalated,
    then failed the whole class ("uptime 2857s -> None").
  * rerun 2 item 2: a failed esptool escalation left the serial port CLOSED, and
    every following test errored with "serial port not open".
"""

from types import SimpleNamespace

import pytest

from device import (
    FRESH_BOOT_CEILING_S,
    FRESH_BOOT_UPTIME_S,
    BootError,
    Device,
    DeviceError,
    DeviceLostError,
    _BootScan,
    is_fresh_boot,
)

pytestmark = pytest.mark.host


class _FakeSer:
    """A stand-in serial handle: close() is a no-op, so Device.close() (which calls
    self._ser.close()) works without a real port."""

    def close(self):
        pass


# --- the pure decision function ---------------------------------------------
def test_is_fresh_boot_pure():
    assert is_fresh_boot(3254, 4) is True  # dropped AND small -> real reboot
    assert is_fresh_boot(None, 2) is True  # small, no baseline -> real reboot
    assert is_fresh_boot(3254, 3254) is False  # no-op soft reboot (the bench bug)
    assert is_fresh_boot(3254, 3260) is False  # uptime climbed -> never restarted
    assert is_fresh_boot(100, None) is False  # console never settled
    assert is_fresh_boot(20, FRESH_BOOT_UPTIME_S - 1) is True  # below the fresh bar
    # Above the bar and NOT below the pre-reboot reading -> no restart happened.
    assert is_fresh_boot(FRESH_BOOT_UPTIME_S - 5, FRESH_BOOT_UPTIME_S + 1) is False


def test_is_fresh_boot_rejects_a_large_uptime_that_merely_dropped():
    """The bench-rerun-1 false-positive (SUPERVISOR ADDENDUM #2): a ~6 s dip from a
    ~42-min uptime was accepted as fresh by the old decrease-only test, so
    reboot_and_confirm returned uptime=2494s instead of escalating to a hard reset.
    A fresh boot is a small ABSOLUTE uptime, never tens of minutes in."""
    assert is_fresh_boot(2500, 2494) is False  # dropped 6 s but still ~42 min in
    assert is_fresh_boot(5000, FRESH_BOOT_CEILING_S) is False  # exactly at the ceiling
    assert is_fresh_boot(5000, FRESH_BOOT_CEILING_S + 1) is False  # past the ceiling
    # A slow SD-scan boot: past the fast bar but a small absolute uptime that
    # dropped from a large before -> still a genuine fresh boot (must not escalate).
    assert is_fresh_boot(5000, FRESH_BOOT_UPTIME_S + 10) is True
    assert is_fresh_boot(5000, FRESH_BOOT_CEILING_S - 1) is True


# --- _BootScan: the reset marker is the fresh-boot proof ---------------------
def test_bootscan_tracks_a_seen_reset():
    scan = _BootScan()
    assert scan.saw_reset is False
    scan.feed("orch: booting")
    assert scan.saw_reset is False
    scan.feed("rst:0x1 (POWERON_RESET)")
    assert scan.saw_reset is True  # a rst: line is the chip's own restart proof
    scan.feed("READY mode=1 ip=10.0.0.9")
    assert scan.saw_reset is True  # sticky for the rest of the boot


# --- recorded boot streams: _scan_boot reports what the stream showed --------
# A real console REBOOT on the bench Solide: rst: -> ROM/app lines -> READY, with a
# heavy episodic scan in the middle (the reason STATUS stays busy for seconds).
REAL_REBOOT = [
    "rst:0x1 (POWERON_RESET),boot:0x8 (SPI_FAST_FLASH_BOOT)",
    "load:0x3fce2820,len:0x1234",
    "[agent] boot",
    "orch: scanning episodic (532 rows)",
    "orch: done",
    "READY mode=1 ip=192.168.50.120",
]


class _StreamRig(Device):
    """Device with _readline scripted from a recorded boot stream (each pop is one
    line; None means the idle window elapsed), plus a scripted STATUS uptime and
    PING, so _scan_boot / _confirm_boot run their real control flow with no serial."""

    def __init__(self, lines, uptime=None, ping_ok=True):
        super().__init__(port="/dev/cu.fake")
        self._lines = list(lines)
        self._uptime = uptime
        self._ping_ok = ping_ok

    def _readline(self, deadline):  # a queue, not the wall clock
        return self._lines.pop(0) if self._lines else None

    def reset(self):  # no serial: the recorded stream stands in for a real reboot
        return self

    def ping(self, timeout=3.0):
        return self._ping_ok

    def _uptime_or_none(self, timeout=4.0):
        return self._uptime

    def _settled_uptime(self, timeout=25.0):
        return self._uptime


def test_scan_boot_reports_a_seen_reset_and_ready():
    r = _StreamRig(REAL_REBOOT)._scan_boot(timeout=5.0)
    assert r.saw_reset is True and r.ready is True
    assert r.mode == 1 and r.ip == "192.168.50.120"
    assert r.reset_at is not None  # timed from the first rst:, for boot-recency


def test_scan_boot_no_reset_marker_when_the_beacon_was_eaten():
    # Empty stream (the reopen ate the beacon) but the board answers PING: ready,
    # yet NO reset marker was observed - so _confirm_boot must fall back to uptime.
    r = _StreamRig([], ping_ok=True)._scan_boot(timeout=0.3)
    assert r.saw_reset is False and r.ready is True and r.reset_at is None


def test_scan_boot_propagates_a_reboot_loop():
    r = _StreamRig(["rst:0x8 (TG0WDT_SYS_RESET)", "load:0x1", "rst:0x8 (TG0WDT_SYS_RESET)"])
    with pytest.raises(BootError):
        r._scan_boot(timeout=5.0)


# --- _confirm_boot: the boot stream is the oracle, not a busy STATUS ----------
def test_confirm_boot_trusts_the_stream_when_status_is_busy():
    """THE bench-rerun-2 bug (item 1/3): rst: -> READY happened, but the post-boot
    SD scan keeps STATUS from answering (uptime None). The stream already proved a
    fresh boot, so the confirm must return a small uptime, NEVER None - returning
    None is what wrongly escalated a real reboot and then failed the whole session."""
    up = _StreamRig(REAL_REBOOT, uptime=None)._confirm_boot(before=2857, timeout=5.0)
    assert up is not None and 0 <= up < FRESH_BOOT_CEILING_S


def test_confirm_boot_returns_the_real_uptime_when_status_answers():
    up = _StreamRig(REAL_REBOOT, uptime=12)._confirm_boot(before=2857, timeout=5.0)
    assert up == 12


def test_confirm_boot_escalates_a_no_op_reboot():
    # No rst: in the stream (a REBOOT the chip ignored) and uptime kept climbing:
    # not fresh -> None, so reboot_and_confirm escalates to the hard reset.
    rig = _StreamRig(["orch: heartbeat", "orch: heartbeat"], uptime=3260, ping_ok=True)
    assert rig._confirm_boot(before=3254, timeout=0.3) is None


def test_confirm_boot_propagates_a_reboot_loop():
    rig = _StreamRig(["rst:0x8 (WDT)", "load:0x1", "rst:0x8 (WDT)"], uptime=2)
    with pytest.raises(BootError):
        rig._confirm_boot(before=100, timeout=5.0)


# --- reboot_and_confirm flow: soft -> confirm -> escalate -> confirm ----------
class _FlowRig(Device):
    """reboot_and_confirm's control flow run host-side by scripting the two seams it
    turns on: _confirm_boot (queued results, one pop per call) and esptool_hard_reset
    (which, per its real contract, reopens the console - modeled here by re-setting
    _ser)."""

    def __init__(self, confirm_results, esptool_ok):
        super().__init__(port="/dev/cu.fake")
        self._confirm = list(confirm_results)
        self._esptool_ok = esptool_ok
        self.calls = []

    def _uptime_or_none(self, timeout=4.0):
        return 3254  # the pre-reboot reading

    def reset(self):
        self.calls.append("reset")
        return self

    def _confirm_boot(self, before, timeout):
        self.calls.append("confirm")
        return self._confirm.pop(0) if self._confirm else None

    def esptool_hard_reset(self):
        self.calls.append("esptool")
        self._ser = _FakeSer()  # the real method always reopens; model that here
        return self._esptool_ok


def test_soft_reboot_that_takes_needs_no_hard_reset():
    rig = _FlowRig(confirm_results=[4], esptool_ok=False)
    assert rig.reboot_and_confirm() == 4
    assert rig.calls == ["reset", "confirm"]  # no escalation


def test_no_op_soft_reboot_escalates_to_hard_reset():
    rig = _FlowRig(confirm_results=[None, 5], esptool_ok=True)
    assert rig.reboot_and_confirm() == 5
    assert rig.calls == ["reset", "confirm", "esptool", "confirm"]


def test_raises_when_neither_soft_nor_hard_reset_takes():
    rig = _FlowRig(confirm_results=[None, None], esptool_ok=True)
    with pytest.raises(DeviceError):
        rig.reboot_and_confirm()


def test_raises_when_esptool_is_unavailable_and_soft_was_a_no_op():
    rig = _FlowRig(confirm_results=[None], esptool_ok=False)
    with pytest.raises(DeviceError):
        rig.reboot_and_confirm()
    # esptool was tried once; no second confirm after it reported failure.
    assert rig.calls == ["reset", "confirm", "esptool"]


def test_failed_escalation_leaves_the_console_open():
    """bench rerun 2 item 2: a failed escalation must NOT poison the rest of the
    run. reboot_and_confirm raises, but the console stays open for the next test."""
    rig = _FlowRig(confirm_results=[None], esptool_ok=False)
    rig._ser = _FakeSer()
    with pytest.raises(DeviceError):
        rig.reboot_and_confirm()
    assert rig._ser is not None  # esptool_hard_reset reopened; still usable


# --- esptool_hard_reset: ALWAYS reopens the port (item 2, the real method) ----
class _EsptoolRig(Device):
    """Exercises the REAL esptool_hard_reset with subprocess.run monkeypatched and
    reopen_after_reenumerate stubbed to a live handle, so the reopen-on-every-path
    contract is covered without a board."""

    def __init__(self):
        super().__init__(port="/dev/cu.fake")
        self._ser = _FakeSer()
        self.reopened = 0

    def reopen_after_reenumerate(self, drain_boot=True):
        self.reopened += 1
        self._ser = _FakeSer()
        return self


def _patch_esptool(monkeypatch, returncode):
    monkeypatch.setattr(
        "device.subprocess.run",
        lambda *a, **k: SimpleNamespace(returncode=returncode, stdout="", stderr=""),
    )


def test_esptool_hard_reset_reopens_after_a_successful_reset(monkeypatch):
    _patch_esptool(monkeypatch, returncode=0)
    rig = _EsptoolRig()
    assert rig.esptool_hard_reset() is True
    assert rig.reopened == 1 and rig._ser is not None


def test_esptool_hard_reset_reopens_even_when_it_fails(monkeypatch):
    # The exact rerun-2 item-2 fault: esptool fails, but the console must still be
    # reopened so the next test is not met with "serial port not open".
    _patch_esptool(monkeypatch, returncode=1)
    rig = _EsptoolRig()
    assert rig.esptool_hard_reset() is False
    assert rig.reopened == 1 and rig._ser is not None


def test_esptool_hard_reset_swallows_a_lost_board_without_leaking(monkeypatch):
    # If the board genuinely does not re-enumerate, reopen raises DeviceLostError;
    # esptool_hard_reset swallows it (the caller's confirm / wedge sentinel handle a
    # truly absent board) and still returns a clean bool, never propagating.
    _patch_esptool(monkeypatch, returncode=1)

    class _LostRig(Device):
        def __init__(self):
            super().__init__(port="/dev/cu.fake")
            self._ser = _FakeSer()

        def reopen_after_reenumerate(self, drain_boot=True):
            raise DeviceLostError("did not re-enumerate")

    assert _LostRig().esptool_hard_reset() is False


def test_esptool_hard_reset_without_a_port_is_a_noop():
    dev = Device()  # no port pinned or discovered
    assert dev.esptool_hard_reset() is False


# --- a boot loop / panic is a real finding, never masked as "did not reboot" --
def test_a_boot_loop_or_panic_propagates_not_swallowed():
    rig = _StreamRig(["rst:0x8 (WDT)", "load:0x1", "rst:0x8 (WDT)"])
    with pytest.raises(BootError):
        rig.reboot_and_confirm()
