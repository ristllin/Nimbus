"""Host check for the L29 tunnel-loopback legs' Wi-Fi precheck (no device).

TestTunnelLoopback runs first in its session, right after the session fixture's
reset. On unit L (v4.5.14 release run, 3 of 3 runs) its one STATUS, sent with a 3 s
timeout, landed in the boot and timed out, so the leg failed before CLOUDLOOP ran.
These run the precheck against a console that ignores STATUS while it boots.
"""

from __future__ import annotations

import re

import pytest

import test_l29_release_gate as l29
from device import ExpectTimeout
from net import Net

pytestmark = pytest.mark.host

UP = "STATUS fw=v4.5.14 build=b mode=1 wifi=1 ip=192.0.2.61 rssi=-42 heap=33000 scr=tft uptime=4100"
DOWN = UP.replace("wifi=1 ip=192.0.2.61", "wifi=0 ip=0.0.0.0")


class BootingConsole:
    """Answers STATUS only after ``busy`` requests (the board is still booting)."""

    def __init__(self, busy: int, line: str = UP):
        self.busy, self.line, self.sent = busy, line, 0

    def send(self, line) -> None:
        assert str(line) == "STATUS", line
        self.sent += 1

    def expect_re(self, pattern: str, timeout: float = 3.0):
        if self.sent <= self.busy:
            raise ExpectTimeout(pattern, ["[agent] loops: begin - 1 loop(s), clock=1"])
        m = re.search(pattern, self.line)
        assert m, (pattern, self.line)
        return m

    def cmd_re(self, line: str, pattern: str, timeout: float = 3.0):  # Device.status()
        self.send(line)
        return self.expect_re(pattern, timeout)

    def status(self, timeout: float = 3.0):
        from device import STATUS_PATTERN

        return self.cmd_re("STATUS", STATUS_PATTERN, timeout)


@pytest.fixture(autouse=True)
def _no_sleep(monkeypatch):
    import net as net_mod

    monkeypatch.setattr(net_mod.time, "sleep", lambda _s: None)


def test_the_precheck_waits_out_a_booting_console():
    console = BootingConsole(busy=2)
    l29._require_wifi(Net(console))  # no skip, no timeout
    assert console.sent == 3


def test_the_old_single_status_read_timed_out_into_the_boot():
    """The counter-test: the release-run shape (one Device.status() with 3 s) fails."""
    with pytest.raises(ExpectTimeout):
        BootingConsole(busy=1).status()


def test_a_station_that_stays_down_skips(monkeypatch):
    monkeypatch.setattr(Net, "wait_station", lambda self, *a, **k: _join(None))
    with pytest.raises(pytest.skip.Exception, match="not on Wi-Fi"):
        l29._require_wifi(Net(BootingConsole(busy=0, line=DOWN)))


def _join(ip):
    from net import Join

    return Join(ip, False, (), 1)


def test_a_console_that_never_answers_fails_by_name(monkeypatch):
    """A silent console (the S3 USB-CDC wedge after a reset) is not "not on Wi-Fi"."""
    monkeypatch.setattr(Net, "wait_station", lambda self, *a, **k: _join(None)._replace(mode=None))
    with pytest.raises(pytest.fail.Exception, match="did not answer STATUS"):
        l29._require_wifi(Net(BootingConsole(busy=99)))
