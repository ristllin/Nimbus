"""Host tests for the legacy-slot drain in run_scenarios.run_turn (no device).

The web chat's legacy slot contract (the bench F03 fix): a poll while the device
is busy is an interim PEEK - the reply stays readable until a poll on an idle
device RETIRES it. A drain that appends every non-empty read therefore records
the same delivery many times over. These tests pin the drain's side of that
contract: every delivery is recorded exactly once, the mid-turn failover notice
and the final reply both land, and a fan-out's later synthesis is still caught.
"""

from __future__ import annotations

import time

import pytest

import run_scenarios as rs

pytestmark = pytest.mark.host

NOTICE = "mistral hit trouble mid-task - switching to openai"


class _Resp:
    def __init__(self, status_code=200, body=None):
        self.status_code = status_code
        self._body = body if body is not None else {}
        self.text = "ok"

    def json(self):
        return self._body


class FakeChatDev:
    """Serves GET /api/chat from a scripted list of slot states.

    Each state is the legacy body the device would answer for one poll:
    {"reply", "pending", "busy"}. A busy poll is a peek (the slot keeps the
    reply); an idle poll retires it (the next poll answers empty). When the
    script runs out, the device is idle with an empty slot.
    """

    def __init__(self, script):
        self.script = list(script)
        self.polls = 0

    def get(self, path, timeout=8):
        if path.startswith("/api/chat"):
            self.polls += 1
            state = self.script.pop(0) if self.script else {"reply": "", "pending": True, "busy": 0}
            return _Resp(200, state)
        return _Resp(200, {})

    def post(self, path, body, timeout=12):
        return _Resp(200, {"ok": True})

    def orch(self):
        return {"jobs": []}

    def state(self):
        return {"jobs": 0}


class Clock:
    def __init__(self):
        self.t = 1000.0

    def time(self):
        return self.t

    def sleep(self, s):
        self.t += s


@pytest.fixture
def clock(monkeypatch):
    c = Clock()
    monkeypatch.setattr(time, "time", c.time)
    monkeypatch.setattr(time, "sleep", c.sleep)
    return c


def _idle():
    return {"reply": "", "pending": True, "busy": 0}


def test_a_busy_window_peek_is_recorded_once(clock):
    # The F03 shape: the reply is delivered while the turn is still wrapping up,
    # so the drain's first read is a peek and the slot answers the same reply on
    # every later poll until an idle one retires it.
    dev = FakeChatDev(
        [
            _idle(),  # the stale-reply clear
            {"reply": "At most 155 g of protein is left today.", "pending": False, "busy": 1},
            {"reply": "At most 155 g of protein is left today.", "pending": False, "busy": 1},
            {"reply": "At most 155 g of protein is left today.", "pending": False, "busy": 0},
        ]
    )
    _first, all_replies = rs.run_turn(dev, "how much protein is left?")
    assert all_replies == ["At most 155 g of protein is left today."]


def test_the_mid_turn_notice_and_the_final_reply_both_land_once(clock):
    dev = FakeChatDev(
        [
            _idle(),
            {"reply": NOTICE, "pending": False, "busy": 1},
            {"reply": NOTICE, "pending": False, "busy": 1},  # the peek re-read
            {"reply": "The 11th Fibonacci number is 89.", "pending": False, "busy": 1},
            {"reply": "The 11th Fibonacci number is 89.", "pending": False, "busy": 0},
        ]
    )
    _first, all_replies = rs.run_turn(dev, "what is the 11th Fibonacci number?")
    assert all_replies == [NOTICE, "The 11th Fibonacci number is 89."]


def test_a_fanouts_later_synthesis_is_still_caught(clock):
    # Dedupe must not close the late window early: the retiring re-read of the
    # head reply keeps the drain watching, so the synthesis delivery lands too.
    dev = FakeChatDev(
        [
            _idle(),
            {"reply": "On it.", "pending": False, "busy": 1},
            {"reply": "On it.", "pending": False, "busy": 0},
            {"reply": "The sub-agent finished: 89.", "pending": False, "busy": 1},
            {"reply": "The sub-agent finished: 89.", "pending": False, "busy": 0},
        ]
    )
    _first, all_replies = rs.run_turn(dev, "spawn a job for the 11th Fibonacci number")
    assert all_replies == ["On it.", "The sub-agent finished: 89."]
