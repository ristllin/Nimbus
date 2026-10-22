"""Storm leg c: a provider out of credit or refusing payment.

Reproduces 19 Oct 2026 (L19-2, L19-3, L19-8, L19-13): the OpenAI account ran out of
credit; 195 memory saves and 47 photo descriptions were refused, one photo was
downloaded 46 times, and the device said nothing.

Three episodes, one per refusal shape: Anthropic's 400 "credit balance is too low",
a 429 ``insufficient_quota`` and a 402. Each episode is one owner message while the head
provider refuses, then one fresh message (the usable probe). On the candidate the head
is Anthropic with ``PROVFAULT api.anthropic.com <status> <body>`` (nothing leaves the
device); on v4.5.13 the LAN mock is the head and answers every request with the same
status and body. Every other provider key is cleared for the leg. Unit A.

Bounds on the release candidate, per episode: at most one provider request (no retry
of a billing refusal), zero requests to the provider once it is parked (the probe
included), exactly one owner notice across the message and the probe, the probe
answered or honestly refused within 120 s.

Run: NIMBUS_STORM_UNIT=A python3 -m pytest tests/hil/storm/test_storm_c_provider_refusing.py --allow-hardware -s
"""

from __future__ import annotations

import json
import re

import pytest

import grading as g
import kit
from mock_llm import FAULT_TURNS

pytestmark = [pytest.mark.hil, pytest.mark.net]

LEG = "c_provider_refusing"
HOST = "api.anthropic.com"
SHAPES = (("credit-400", "mock-credit-400"), ("quota-429", "mock-quota-429"), ("payment-402", "mock-402"))
NOTICE_RE = re.compile(r"credit|billing|quota|payment|paused|plan", re.I)
BOUNDS = g.Bounds(
    max_paid_turns=2,
    max_provider_requests=1,
    max_identical=5,
    notices=(1, 1),
    max_parked_requests=0,
    usable_within_s=120.0,
)


def _arm(storm, scenario: str) -> None:
    if storm.control:
        storm.set_mock_scenario(scenario)
        return
    status, body = FAULT_TURNS[scenario]
    storm.verb("PARK clear", "PARK", timeout=6.0)
    out = storm.verb(f"PROVFAULT {HOST} {status} {json.dumps(body, separators=(',', ':'))}", "PROVFAULT")
    assert "armed" in out, out


def _disarm(storm) -> None:
    if not storm.control:
        storm.verb("PROVFAULT off", "PROVFAULT", timeout=6.0)
        storm.verb("PARK clear", "PARK", timeout=6.0)


def _episode(storm, name: str, scenario: str) -> g.Metrics:
    uid = kit.first_update_id()
    _arm(storm, scenario)
    try:
        w = kit.Window(storm).open()
        kit.owner_turn(storm, f"storm c {name} {uid}: reply with the single word pong", uid)
        probe = storm.fresh_message(kit.ANSWER_RE, f"storm c {name} probe {uid}: reply pong", uid + 1, timeout=150.0)
        w.close()
    finally:
        _disarm(storm)
    replies = w.replies(NOTICE_RE)
    peak, line = g.identical_peak(w.lines, ignore=kit.DRIVE_LINES)
    parked = None if storm.control else g.requests_after_park(w.lines, "anthropic", g.OVERRIDE_RE)
    m = g.Metrics(
        paid_turns=g.paid_turns(w.lines),
        provider_requests=w.requests(HOST),
        parked_requests=parked,
        notices=w.notices(NOTICE_RE),
        identical_peak=peak,
        identical_line=line[:160],
        usable=probe["usable"],
        usable_s=probe["usable_s"],
    )
    m.extra.update({"shape": name, "replies": replies, "probeReply": probe["reply"][:200]})
    if not storm.control:
        m.extra["park"] = storm.verb("PARK?", "PARK? providers", timeout=6.0).strip()
    return m


def test_storm_c_provider_refusing(storm):
    episodes = []
    with storm.session(kit.keep_for(storm), kit.head_for(storm)):
        for name, scenario in SHAPES:
            m = _episode(storm, name, scenario)
            print(f"[storm] episode {name}: {m}")
            episodes.append(m)
    worst = kit.worst_episode(episodes, BOUNDS)
    summary = [{k: v for k, v in vars(e).items() if k not in ("extra", "notes")} for e in episodes]
    for row, e in zip(summary, episodes):
        row["shape"] = e.extra.get("shape")
    worst.extra["episodes"] = summary  # every episode's counters (the graded one is the worst)
    kit.assert_graded(storm.finish(LEG, worst, BOUNDS))
