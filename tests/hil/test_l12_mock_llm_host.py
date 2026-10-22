"""Host checks for the L12 head-custom leg (no device).

In the v4.5.14 release run the head-custom leg counted two requests on its mock
model on both units. The kept mock dump of the bench rerun (unit L, 22 Oct 2026)
showed what the second one was: the previous leg's sub-agent finished and its
unattended synthesis turn ran on the head, which the leg had just switched to the
mock. The head-custom wire itself sent one request. These pin the reader that tells
the turn's own request apart, and the wait for an idle device between legs.
"""

from __future__ import annotations

import json

import pytest

import test_l12_mock_llm as l12
from mock_llm import RecordedRequest
from test_l12_mock_llm import Backend, requests_for_turn, user_section

pytestmark = pytest.mark.host

ASK = "say hello (1792638782)"
PROMPT = "[CHANNEL] This message arrived via the developer serial console.\n\n"


def _rec(user: str, ts: float) -> RecordedRequest:
    body = {
        "model": "mock-toolloop",
        "messages": [{"role": "system", "content": "autonomous"}, {"role": "user", "content": user}],
        "response_format": {"type": "json_schema", "json_schema": {"name": "orch_turn"}},
    }
    return RecordedRequest("POST", "/v1/chat/completions", {}, json.dumps(body).encode(), ts, body)


HEAD = _rec(PROMPT + "[USER]\n" + ASK, 1792638782.3)
# The synthesis turn's prompt (shortened from the dump): no owner message at all.
SYNTHESIS = _rec(
    PROMPT + "- user: say hello (1792638700)\n[SYSTEM]\nOne or more of your sub-agents just finished "
    "(others may still be running - see [ACTIVE SESSIONS]). There is no new user message.",
    1792638784.2,
)


def test_the_turns_own_request_is_told_apart_from_a_synthesis_turn():
    recs = [HEAD, SYNTHESIS]
    assert len(recs) == 2  # what the old count saw
    assert requests_for_turn(recs, ASK) == [HEAD]
    assert user_section(SYNTHESIS) == ""  # an earlier owner text in the history is not this turn's
    assert requests_for_turn([SYNTHESIS], ASK) == []


def test_a_second_round_of_the_same_turn_still_counts_twice():
    """The counter-test: a tool loop on the head-custom path (the regression the leg
    exists for) repeats the turn's own owner message, so it is still caught."""
    round2 = _rec(PROMPT + "[USER]\n" + ASK, 1792638783.0)
    assert len(requests_for_turn([HEAD, round2, SYNTHESIS], ASK)) == 2


def _backend(monkeypatch, busy: list) -> Backend:
    seq = iter(busy)
    monkeypatch.setattr(l12, "_oget", lambda net, ip, tok, path: {"busy": next(seq), "async": 0})
    monkeypatch.setattr(l12.time, "sleep", lambda _s: None)
    return Backend(None, None, None, "192.0.2.1", "tok", "192.0.2.2")


def test_wait_idle_needs_consecutive_quiet_polls(monkeypatch):
    # A sub-agent finishes (busy 1 -> 0), the synthesis turn starts (1), then quiet.
    be = _backend(monkeypatch, [1, 0, 1, 0, 0, 0])
    be.wait_idle(timeout=60.0)


def test_wait_idle_fails_on_a_device_that_never_settles(monkeypatch):
    clock = iter(range(0, 10_000, 3))
    monkeypatch.setattr(l12.time, "time", lambda: next(clock))
    be = _backend(monkeypatch, [1] * 1000)
    with pytest.raises(pytest.fail.Exception, match="did not go idle"):
        be.wait_idle(timeout=30.0)
