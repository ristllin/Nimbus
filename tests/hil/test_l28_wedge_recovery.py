"""CUM-141: host tests for the console-wedge detection + in-place recovery.

Pure host tests (no board): they exercise the classifier and the one-shot
recovery of the WedgeSentinel with a fake device, so the hardening itself is
covered rather than only exercised live. The pytest-hook wiring (fail-fast on a
known wedge) is verified separately on hardware, but the decision logic it
depends on is proven here.
"""

import pytest

from wedge_guard import WedgeSentinel, is_wedge_candidate

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
    """Minimal stand-in for the HIL Device: records calls, scriptable liveness.

    ``alive_before_reset`` is what the confirming ping sees BEFORE any recovery (a
    live console == not a wedge); ``alive_after_reset`` is what it sees after the
    bus reset. ``ping`` returns the right one based on whether bus_reset ran."""

    def __init__(
        self,
        alive_after_reset: bool,
        alive_before_reset: bool = False,
        bus_reset_ok: bool = True,
    ):
        self.alive_after_reset = alive_after_reset
        self.alive_before_reset = alive_before_reset
        self.bus_reset_ok = bus_reset_ok
        self.reset_done = False
        self.calls = []

    def bus_reset(self) -> bool:
        self.calls.append("bus_reset")
        self.reset_done = True
        return self.bus_reset_ok

    def close(self) -> None:
        self.calls.append("close")

    def open(self) -> "FakeDevice":
        self.calls.append("open")
        return self

    def ping(self, timeout: float = 3.0) -> bool:
        self.calls.append("ping")
        return self.alive_after_reset if self.reset_done else self.alive_before_reset


def make_sentinel(dev, enabled=True):
    s = WedgeSentinel(enabled=enabled, sleep=lambda _s: None, log=lambda *_a: None)
    s.device = dev
    return s


# --- candidate classifier ----------------------------------------------------
def test_console_timeouts_are_candidates():
    assert is_wedge_candidate(ExpectTimeout("STATUS timed out"))
    assert is_wedge_candidate(DeviceLostError("serial node did not re-enumerate"))
    assert is_wedge_candidate(DeviceError("console unresponsive even after a bus reset"))
    assert is_wedge_candidate(DeviceError("serial port not open; call open()"))


def test_real_failures_are_not_candidates():
    # A firmware crash / boot-loop is a genuine finding - never masked as a wedge.
    assert not is_wedge_candidate(BootError("Guru Meditation Error: PANIC"))
    # A plain assertion failure is the test's own result.
    assert not is_wedge_candidate(AssertionError("expected screen=Menu, got StatusIdle"))
    # An unrelated DeviceError message is not a wedge signature.
    assert not is_wedge_candidate(DeviceError("wifi did not associate"))


# --- confirm-then-recover ----------------------------------------------------
def test_live_console_is_not_recovered_no_gratuitous_reset():
    # The key guard: an ExpectTimeout on a LIVE console (an ordinary content miss)
    # must NOT trigger a USB reset. The confirming ping answers -> no recovery.
    dev = FakeDevice(alive_after_reset=False, alive_before_reset=True)
    s = make_sentinel(dev)
    assert s.try_recover() is False
    assert not s.wedged and not s.recovered
    assert dev.calls == ["ping"]  # only the confirming probe; NO bus_reset
    assert not s.recovery_attempted  # a later real wedge can still recover


def test_recovery_succeeds_when_console_comes_back():
    # Dead before (confirmed wedge), back after the reset.
    dev = FakeDevice(alive_after_reset=True, alive_before_reset=False)
    s = make_sentinel(dev)
    assert s.try_recover() is True
    assert s.recovered and not s.wedged
    # Confirming ping, then the real in-place sequence - never a REBOOT.
    assert dev.calls == ["ping", "bus_reset", "close", "open", "ping"]


def test_recovery_marks_wedged_when_console_stays_dead():
    dev = FakeDevice(alive_after_reset=False, alive_before_reset=False)
    s = make_sentinel(dev)
    assert s.try_recover() is False
    assert s.wedged and not s.recovered


def test_recovery_is_one_shot():
    dev = FakeDevice(alive_after_reset=False, alive_before_reset=False)
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

    # Dead before (confirmed wedge) so recovery is actually attempted.
    s = make_sentinel(Boom(alive_after_reset=True, alive_before_reset=False))
    # Recovery must never raise into the run; a throw means "still wedged".
    assert s.try_recover() is False
    assert s.wedged
