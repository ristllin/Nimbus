"""SUPERVISOR ADDENDUM #1 + #3: pure host tests for the panel-navigation policy
(idle_nav_step) and the boot-stream classifier (_BootScan) that ensure_status_idle
and wait_ready depend on. No board: they prove the DECISION logic - "which input
backs THIS screen toward StatusIdle" and "is the board booting, looping, or done" -
so the bench regressions (a blind centre-tap loop on screen=5/9; a fixed 20 s wait
clipping a slow SD-scan boot) are caught by the class, not just re-observed live.
"""

from __future__ import annotations

import pytest

from device import (
    IDLE_HEADER_TAP,
    SCREEN_NAMES,
    BootError,
    Device,
    ExpectTimeout,
    _BootScan,
    idle_nav_step,
)

pytestmark = pytest.mark.host


# --- idle_nav_step: total over every ScreenId, no blind fallback -------------
def test_idle_nav_step_is_total_over_every_screen():
    """Every ScreenId the firmware enum defines maps to a KNOWN action - never an
    exception or an unhandled id that would fall through to a blind tap loop."""
    for sid in range(len(SCREEN_NAMES)):
        assert idle_nav_step(sid) in {"idle", "tap", "restart"}, SCREEN_NAMES[sid]


def test_idle_nav_step_specific_screens():
    idx = SCREEN_NAMES.index
    assert idle_nav_step(idx("StatusIdle")) == "idle"
    assert idle_nav_step(idx("TouchCal")) == "restart"  # no tap region: must restart
    # The bench regression: Ask (5) and ConfigQr (9) are dead space at the centre;
    # both must resolve to the header tap, not a centre tap that loops forever.
    assert idle_nav_step(idx("Ask")) == "tap"
    assert idle_nav_step(idx("ConfigQr")) == "tap"
    # Every screen that draws a header (all but StatusIdle and TouchCal) is a tap.
    for name in SCREEN_NAMES:
        sid = idx(name)
        if name in ("StatusIdle", "TouchCal"):
            continue
        assert idle_nav_step(sid) == "tap", name


def test_header_tap_lands_in_the_back_region():
    """The header target must sit inside the firmware's Back region (0,0,96,44 -
    lib/core/src/tft_screens.cpp drawHeader), which also contains the wider Home
    region, so one coordinate hits Back or Home on any header screen."""
    x, y = IDLE_HEADER_TAP
    assert 0 <= x < 96 and 0 <= y < 44


# --- _BootScan: size the wait from the stream, do not false-flag a loop ------
def _run(lines):
    scan = _BootScan()
    return [scan.feed(ln) for ln in lines]


def test_bootscan_recognizes_ready():
    scan = _BootScan()
    assert scan.feed("[agent] telegram: turn task up") == "progress"
    assert scan.feed("orch: done") == "progress"
    assert scan.feed("READY mode=1 ip=192.168.50.120") == ("ready", 1, "192.168.50.120")


def test_bootscan_legacy_marker():
    assert _BootScan().feed("NSN ready") == ("legacy", None, None)


def test_bootscan_panic():
    assert _BootScan().feed("Guru Meditation Error: Core 0 panic'ed") == "panic"


def test_bootscan_does_not_flag_a_slow_boot_with_one_reset():
    """A slow boot has exactly one rst: then a long progress tail - never a loop."""
    verdicts = _run(
        [
            "rst:0x1 (POWERON)",
            "load:0x3fce2820",
            "[agent] boot",
            "orch: scanning episodic (532 rows)",
            "orch: done",
            "READY mode=1 ip=0.0.0.0",
        ]
    )
    assert "loop" not in verdicts
    assert verdicts[-1] == ("ready", 1, "0.0.0.0")


def test_bootscan_does_not_flag_the_harness_double_reset():
    """reset() + a CDC-reopen USB_UART_CHIP_RESET reaches app lines between the two
    resets (the L7 watchdog transcript), so the second rst: is a fresh cycle, not a
    loop. Progress between the resets clears the counter."""
    verdicts = _run(
        [
            "rst:0x1 (POWERON)",
            "orch: done",
            "READY mode=1 ip=0.0.0.0",  # first boot reached READY / progress
            "PONG",
            "rst:0x15 (USB_UART_CHIP_RESET)",  # host reopen kicked a second reset
            "orch: done",
            "READY mode=1 ip=192.168.50.120",
        ]
    )
    assert "loop" not in verdicts


def test_bootscan_flags_a_real_reboot_loop():
    """A true loop is rst: -> rst: with no app progress between (the board never
    gets off the ground)."""
    verdicts = _run(
        [
            "rst:0x8 (TG0WDT_SYS_RESET)",
            "load:0x3fce2820",  # ROM boot line, not app progress
            "rst:0x8 (TG0WDT_SYS_RESET)",
        ]
    )
    assert verdicts[-1] == "loop"


# --- wait_ready sizing: not clipped by a line budget, still fails on silence -
class _BootRig(Device):
    """Device with _readline scripted from a queue (each pop is one boot line;
    None means the idle window elapsed) and ping() forced, so wait_ready's control
    flow runs host-side with no serial."""

    def __init__(self, lines, ping=False):
        super().__init__(port="/dev/cu.fake")
        self._lines = list(lines)
        self._ping = ping

    def _readline(self, deadline):  # noqa: D401 - queue, not the wall clock
        return self._lines.pop(0) if self._lines else None

    def ping(self, timeout=3.0):
        return self._ping


def test_wait_ready_returns_ready_after_many_progress_lines():
    # 50 progress lines then READY: a fixed line/short-window budget would clip
    # this; sizing from the stream returns the beacon.
    lines = [f"orch: step {i}" for i in range(50)] + ["READY mode=1 ip=10.0.0.9"]
    assert _BootRig(lines).wait_ready(timeout=20.0) == (1, "10.0.0.9")


def test_wait_ready_falls_back_to_ping_on_a_consumed_beacon():
    # Empty stream (beacon eaten by the reopen) but the board answers PING -> ready.
    assert _BootRig([], ping=True).wait_ready(timeout=1.0) == (None, None)


def test_wait_ready_raises_on_a_silent_bricked_board():
    with pytest.raises(ExpectTimeout):
        _BootRig([], ping=False).wait_ready(timeout=1.0)


def test_wait_ready_propagates_a_boot_loop():
    lines = ["rst:0x8 (WDT)", "load:0x1", "rst:0x8 (WDT)"]
    with pytest.raises(BootError):
        _BootRig(lines).wait_ready(timeout=20.0)
