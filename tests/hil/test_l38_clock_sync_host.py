"""L38 host-tier - the clock-sync parsers and verdict (CUM-504). No board; runs in
``pytest -m host`` and in ``--collect-only``.

The serial lines are the only evidence the L38 leg reads, so a renamed field or log
line must fail loud, and the verdict must reject exactly the boots CUM-504 describes.
"""

from __future__ import annotations

import pytest

from test_l38_clock_sync import (
    SYNC_BUDGET_S,
    fold,
    parse_kick,
    parse_status_clock,
    ready_ip_is_lan,
    verdict,
)

pytestmark = pytest.mark.host

STATUS_SYNCED = (
    "STATUS fw=4.5.8 build=abc mode=1 wifi=1 ip=192.168.50.61 rssi=-45 heap=40000 scr=tft "
    "scrok=1 want=tft minheap=30000 psram=8388608 uptime=31000 board=freenove_s3 clk=1"
)
STATUS_UNSYNCED = STATUS_SYNCED.replace("clk=1", "clk=0")
STATUS_PRE_FIX = STATUS_SYNCED.replace(" clk=1", "")
BOOT_UNSYNCED = ["orch: loops", "[agent] loops: begin - 2 loop(s), clock=0", "READY mode=1 ip=192.168.50.61"]
KICK_LINK_UP = "[agent] loops: SNTP kicked (tz=UTC0, link up)"


def test_parse_status_clock():
    assert parse_status_clock(STATUS_SYNCED) == (1, 1)
    assert parse_status_clock(STATUS_UNSYNCED) == (1, 0)


@pytest.mark.parametrize("line", ["", "PONG", STATUS_PRE_FIX, "READY mode=1 ip=1.2.3.4"])
def test_parse_status_clock_fails_loud(line):
    with pytest.raises(ValueError):
        parse_status_clock(line)


def test_parse_kick_reasons():
    assert parse_kick(KICK_LINK_UP) == "link up"
    assert parse_kick("loops: SNTP kicked (tz=CET-1CEST,M3.5.0,M10.5.0/3, link up)") == "link up"
    assert parse_kick("loops: SNTP kicked (tz=<+03>-3, tz change)") == "tz change"
    assert parse_kick("loops: SNTP kicked (tz=UTC0, retry)") == "retry"
    assert parse_kick("loops: SNTP kicked (tz=UTC0, sync now)") == "sync now"
    assert parse_kick("loops: SNTP kicked (tz=UTC0)") == ""  # the pre-CUM-504 line
    assert parse_kick("loops: clock synced (day=20736) - 2 loop(s) rebased") is None


def test_ready_ip():
    assert ready_ip_is_lan("192.168.50.61")
    assert not ready_ip_is_lan("0.0.0.0")
    assert not ready_ip_is_lan(None)
    assert not ready_ip_is_lan("")


def test_fixed_fast_join_passes():
    """The fixed firmware on bench B: up at READY, kicked on the first tick, synced fast."""
    timeline = [(0.4, KICK_LINK_UP), (2.1, STATUS_UNSYNCED), (4.2, STATUS_SYNCED)]
    bc = fold(BOOT_UNSYNCED, timeline, up_at_ready=True)
    assert verdict(bc) == []
    assert bc.boot_clock == 0
    assert bc.synced_s == 4.2
    assert bc.link_up_kick_s == 0.4


def test_slow_join_passes_too():
    """A unit that joins after READY (unit A) kicks when the link comes up."""
    timeline = [(2.0, STATUS_UNSYNCED.replace("wifi=1", "wifi=0")), (14.0, KICK_LINK_UP), (16.0, STATUS_SYNCED)]
    assert verdict(fold(BOOT_UNSYNCED, timeline, up_at_ready=False)) == []


def test_the_cum504_boot_fails():
    """The bug as reported: up the whole time, never kicked, never synced."""
    timeline = [(t, STATUS_UNSYNCED) for t in range(2, 62, 2)]
    problems = verdict(fold(BOOT_UNSYNCED, timeline, up_at_ready=True))
    assert any("never synced" in p for p in problems)
    assert any("network-up latch never fired" in p for p in problems)


def test_a_pre_fix_image_fails_loud():
    """An old image: no clk= field and the old reasonless kick line."""
    timeline = [(15.0, "loops: SNTP kicked (tz=UTC0)"), (16.0, STATUS_PRE_FIX)]
    problems = verdict(fold(BOOT_UNSYNCED, timeline, up_at_ready=False))
    assert any("predates CUM-504" in p for p in problems)
    assert any("network-up latch never fired" in p for p in problems)


def test_a_sync_borrowed_from_sync_now_is_not_a_pass():
    """Synced, but only because someone pressed Sync now: the latch still never fired."""
    timeline = [(30.0, "loops: SNTP kicked (tz=UTC0, sync now)"), (32.0, STATUS_SYNCED)]
    problems = verdict(fold(BOOT_UNSYNCED, timeline, up_at_ready=True))
    assert problems == [
        "no 'loops: SNTP kicked (..., link up)' after READY: the network-up latch never fired"
        " (kicks seen: [(30.0, 'sync now')])"
    ]


def test_late_sync_fails_the_bar():
    timeline = [(0.5, KICK_LINK_UP), (SYNC_BUDGET_S + 1.0, STATUS_SYNCED)]
    problems = verdict(fold(BOOT_UNSYNCED, timeline, up_at_ready=True))
    assert any("over the" in p for p in problems)


def test_a_clock_carried_over_is_vacuous():
    """A boot that started with a valid clock proves nothing about SNTP."""
    boot = ["[agent] loops: begin - 2 loop(s), clock=1", "READY mode=1 ip=192.168.50.61"]
    timeline = [(0.4, KICK_LINK_UP), (2.0, STATUS_SYNCED)]
    problems = verdict(fold(boot, timeline, up_at_ready=True))
    assert any("vacuous" in p for p in problems)


def test_silent_console_fails():
    problems = verdict(fold(BOOT_UNSYNCED, [(1.0, KICK_LINK_UP)], up_at_ready=True))
    assert any("never answered STATUS" in p for p in problems)


def test_a_lost_boot_line_is_not_a_pass():
    """No 'loops: begin' line means the unsynced start was never seen: fail, not pass."""
    timeline = [(0.4, KICK_LINK_UP), (2.0, STATUS_SYNCED)]
    problems = verdict(fold(["READY mode=1 ip=192.168.50.61"], timeline, up_at_ready=True))
    assert any("cannot confirm the boot started unsynced" in p for p in problems)


def test_a_cut_short_status_line_is_tolerated():
    """A STATUS line split by a read timeout is skipped; the next whole one counts."""
    cut = STATUS_SYNCED[:60]
    timeline = [(0.4, KICK_LINK_UP), (2.0, cut), (4.0, STATUS_SYNCED)]
    bc = fold(BOOT_UNSYNCED, timeline, up_at_ready=True)
    assert verdict(bc) == []
    assert bc.synced_s == 4.0


def test_budget_is_the_issue_bar():
    assert SYNC_BUDGET_S == 60.0
