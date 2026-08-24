"""CUM-141: host tests for the console-wedge detection + in-place recovery.

Pure host tests (no board): they exercise the classifier and the one-shot
recovery of the WedgeSentinel with a fake device, so the hardening itself is
covered rather than only exercised live. The pytest-hook wiring (fail-fast on a
known wedge) is verified separately on hardware, but the decision logic it
depends on is proven here.
"""

import pytest

from wedge_guard import WedgeSentinel, is_console_death

pytestmark = pytest.mark.host


# --- fake exception types matching device.py's hierarchy by NAME -------------
class DeviceError(RuntimeError):
    pass


class ExpectTimeout(DeviceError):
    pass


class DeviceLostError(DeviceError):
    pass


class BootError(DeviceError):
    pass


class FakeDevice:
    """Minimal stand-in for the HIL Device: records calls, scriptable liveness."""

    def __init__(self, alive_after_reset: bool, bus_reset_ok: bool = True):
        self.alive_after_reset = alive_after_reset
        self.bus_reset_ok = bus_reset_ok
        self.calls = []

    def bus_reset(self) -> bool:
        self.calls.append("bus_reset")
        return self.bus_reset_ok

    def close(self) -> None:
        self.calls.append("close")

    def open(self) -> "FakeDevice":
        self.calls.append("open")
        return self

    def ping(self, timeout: float = 3.0) -> bool:
        self.calls.append("ping")
        return self.alive_after_reset


def make_sentinel(dev, enabled=True):
    s = WedgeSentinel(enabled=enabled, sleep=lambda _s: None, log=lambda *_a: None)
    s.device = dev
    return s


# --- classifier --------------------------------------------------------------
def test_console_timeouts_are_wedges():
    assert is_console_death(ExpectTimeout("STATUS timed out"))
    assert is_console_death(DeviceLostError("serial node did not re-enumerate"))
    assert is_console_death(DeviceError("console unresponsive even after a bus reset"))
    assert is_console_death(DeviceError("serial port not open; call open()"))


def test_real_failures_are_not_wedges():
    # A firmware crash / boot-loop is a genuine finding - never masked as a wedge.
    assert not is_console_death(BootError("Guru Meditation Error: PANIC"))
    # A plain assertion failure is the test's own result.
    assert not is_console_death(AssertionError("expected screen=Menu, got StatusIdle"))
    # An unrelated DeviceError message is not a wedge signature.
    assert not is_console_death(DeviceError("wifi did not associate"))


# --- one-shot recovery -------------------------------------------------------
def test_recovery_succeeds_when_console_comes_back():
    dev = FakeDevice(alive_after_reset=True)
    s = make_sentinel(dev)
    assert s.try_recover() is True
    assert s.recovered and not s.wedged
    # Did the real in-place sequence: bus reset, reopen, probe - never a REBOOT.
    assert dev.calls == ["bus_reset", "close", "open", "ping"]


def test_recovery_marks_wedged_when_console_stays_dead():
    dev = FakeDevice(alive_after_reset=False)
    s = make_sentinel(dev)
    assert s.try_recover() is False
    assert s.wedged and not s.recovered


def test_recovery_is_one_shot():
    dev = FakeDevice(alive_after_reset=False)
    s = make_sentinel(dev)
    s.try_recover()
    calls_after_first = list(dev.calls)
    # A second signature must NOT trigger a second bus reset (avoid reset storms).
    s.try_recover()
    assert dev.calls == calls_after_first


def test_recovery_survives_a_raising_device():
    class Boom(FakeDevice):
        def open(self):
            raise DeviceError("opened a stale CDC endpoint (silent)")

    s = make_sentinel(Boom(alive_after_reset=True))
    # Recovery must never raise into the run; a throw means "still wedged".
    assert s.try_recover() is False
    assert s.wedged
