"""L34 host-tier - the pure parsers + coherence logic behind the sleep/wake soak
(CUM-248). No board: these run in ``pytest -m host`` and in ``--collect-only``.

The soak's per-cycle safety rests entirely on these: a torn ``SLEEP?`` line must be
a loud ValueError (never silently "coherent"), the coherence rule must match the
firmware's boardCanWakeOnTouch(), and WAKE? must parse the deep-sleep proof. Each is
covered here so the sizing/oracle logic is verified with no hardware.
"""

from __future__ import annotations

import pytest

from test_l34_sleep_wake import parse_sleep_plan, parse_wake_info, sleep_plan_is_coherent

pytestmark = pytest.mark.host


# --- SLEEP? parsing ----------------------------------------------------------
def test_parse_sleep_plan_freenove():
    plan = parse_sleep_plan("SLEEP tapWakes=1 pin=17 level=0 ctrl=ft6336u timer=0 canWakeOnTouch=1")
    assert plan == {
        "tapWakes": True,
        "pin": 17,
        "level": "0",
        "ctrl": "ft6336u",
        "timer": 0,
        "canWakeOnTouch": True,
    }


def test_parse_sleep_plan_solide_unwired():
    plan = parse_sleep_plan("SLEEP tapWakes=0 pin=-1 level=- ctrl=xpt2046 timer=4 canWakeOnTouch=0")
    assert plan["tapWakes"] is False
    assert plan["pin"] == -1
    assert plan["level"] == "-"
    assert plan["ctrl"] == "xpt2046"
    assert plan["timer"] == 4
    assert plan["canWakeOnTouch"] is False


def test_parse_sleep_plan_ignores_leading_noise():
    raw = "boot spew line\nSLEEP tapWakes=1 pin=17 level=0 ctrl=ft6336u timer=8 canWakeOnTouch=1\ntrailing"
    assert parse_sleep_plan(raw)["timer"] == 8


def test_parse_sleep_plan_missing_field_is_loud():
    # A torn arm report is itself the failure mode the soak hunts - never swallow it.
    with pytest.raises(ValueError):
        parse_sleep_plan("SLEEP tapWakes=1 pin=17 ctrl=ft6336u")  # no level/timer/canWakeOnTouch
    with pytest.raises(ValueError):
        parse_sleep_plan("no marker here")


# --- coherence rule (mirrors boardCanWakeOnTouch) ----------------------------
def test_coherence_accepts_freenove_and_solide():
    ok, _ = sleep_plan_is_coherent(parse_sleep_plan(
        "SLEEP tapWakes=1 pin=17 level=0 ctrl=ft6336u timer=0 canWakeOnTouch=1"))
    assert ok
    ok, _ = sleep_plan_is_coherent(parse_sleep_plan(
        "SLEEP tapWakes=0 pin=-1 level=- ctrl=xpt2046 timer=4 canWakeOnTouch=0"))
    assert ok


def test_coherence_rejects_tap_on_non_rtc_pin():
    # Claims a tap wakes it while pointing at a non-RTC GPIO (>21): incoherent.
    ok, why = sleep_plan_is_coherent(parse_sleep_plan(
        "SLEEP tapWakes=1 pin=45 level=0 ctrl=ft6336u timer=0 canWakeOnTouch=1"))
    assert not ok and "RTC" in why


def test_coherence_rejects_tap_with_no_level_armed():
    ok, why = sleep_plan_is_coherent(parse_sleep_plan(
        "SLEEP tapWakes=1 pin=17 level=- ctrl=ft6336u timer=0 canWakeOnTouch=1"))
    assert not ok and "level" in why


def test_coherence_rejects_level_armed_with_no_tap_wake():
    # A level armed on a board that reports no tap wake is a contradiction.
    ok, why = sleep_plan_is_coherent(parse_sleep_plan(
        "SLEEP tapWakes=0 pin=-1 level=0 ctrl=xpt2046 timer=4 canWakeOnTouch=0"))
    assert not ok and "level" in why


def test_coherence_rejects_tapwakes_canwake_disagreement():
    ok, why = sleep_plan_is_coherent(parse_sleep_plan(
        "SLEEP tapWakes=1 pin=17 level=0 ctrl=ft6336u timer=0 canWakeOnTouch=0"))
    assert not ok and "canWakeOnTouch" in why


# --- WAKE? parsing -----------------------------------------------------------
def test_parse_wake_info_deep_sleep_timer():
    info = parse_wake_info("WAKE reset=deep-sleep cause=timer poweroff=1")
    assert info == {"reset": "deep-sleep", "cause": "timer", "poweroff": True}


def test_parse_wake_info_power_on():
    info = parse_wake_info("WAKE reset=power-on cause=none poweroff=0")
    assert info["reset"] == "power-on" and info["cause"] == "none" and info["poweroff"] is False


def test_parse_wake_info_loud_on_garbage():
    with pytest.raises(ValueError):
        parse_wake_info("WAKE reset=deep-sleep")  # no cause/poweroff
    with pytest.raises(ValueError):
        parse_wake_info("unrelated line")
