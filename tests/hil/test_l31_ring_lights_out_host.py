"""Host check for the L31 legs' Wi-Fi join (no device).

The backstop-counter leg joined Wi-Fi through _provision(), which reads the join's
one ``WIFI_GOT_IP`` line, and then waited for that line again with
``net.wait_got_ip()``. The second wait could only time out, so the leg failed on
both units in the v4.5.14 release run with the station up. These run the leg
against a console that prints exactly what a join prints, once.
"""

from __future__ import annotations

import re
import types

import pytest

import test_l31_ring_lights_out as leg
from device import ExpectTimeout
from net import Net

pytestmark = pytest.mark.host

IP = "192.0.2.61"


class FakeConsole:
    """The serial lines a join and a turn print, each readable once (a real console
    stream: a line expect() consumed is gone)."""

    def __init__(self):
        self.lines: list = []
        self.sent: list = []

    def send(self, line) -> None:
        line = str(line)
        self.sent.append(line)
        if line.startswith("SET "):
            self.lines.append(f"{line.split('=')[0]} ok=1")
        elif line.startswith("WIFI "):
            self.lines += ["WIFI_DISCONNECTED reason=8", f"WIFI_GOT_IP {IP}"]

    def wifi(self, ssid: str, password: str) -> None:
        self.send(f"WIFI {ssid}|{password}")

    def expect_re(self, pattern: str, timeout: float = 5.0):
        while self.lines:
            m = re.search(pattern, self.lines.pop(0))
            if m:
                return m
        raise ExpectTimeout(pattern, [])

    def expect(self, substr: str, timeout: float = 5.0) -> str:
        return self.expect_re(re.escape(substr), timeout).string

    def ensure_mode(self, mode: int) -> "FakeConsole":
        return self

    def turn(self, text: str) -> None:
        self.lines.append("ORCH REPLY [serial]: pong")

    def render(self):
        return types.SimpleNamespace(ring="dark")


class FakeNet(Net):
    def get_json(self, path: str, ip=None, timeout: float = 5.0):
        assert ip == IP, f"the leg read {path} from {ip!r}, not the joined station"
        return {"mode": 1, "ringBackstopFires": 0}


SECRETS = types.SimpleNamespace(
    require_provider_keys=lambda: None, openai_key="", anthropic_key="k", sta_ssid="bench", sta_pass="pw"
)


def test_provision_returns_the_ip_of_the_one_join_line():
    console = FakeConsole()
    assert leg._provision(console, SECRETS) == IP
    with pytest.raises(ExpectTimeout):  # the line is gone: a second wait can only time out
        Net(console).wait_got_ip(timeout=0.0)


def test_the_backstop_leg_joins_once_and_reads_the_joined_station(monkeypatch):
    monkeypatch.setattr(leg.time, "sleep", lambda _s: None)
    console = FakeConsole()
    net = FakeNet(console)
    leg.test_ring_backstop_counter_is_zero_on_a_healthy_turn(console, SECRETS, lambda check: check(), net)
    assert net.ip == IP
    assert sum(1 for s in console.sent if s.startswith("WIFI ")) == 1
