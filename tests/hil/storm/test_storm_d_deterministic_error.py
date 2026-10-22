"""Storm leg d: a provider that answers every attempt with the same deterministic error.

Reproduces the class behind L19-13 and L19-14: a request the provider will never accept
(a 400 ``invalid_request_error``) must not be retried in a loop, must not hop to another
provider, and must reach the owner as one honest message, not one per retry.

One episode: one owner message while every request is refused with the same 400, then
one fresh message (the usable probe, answered or honestly refused). Candidate:
``PROVFAULT api.anthropic.com 400 <body>``; v4.5.13: the LAN mock answers every request
with it. Every other provider key is cleared for the leg. Unit A.

Bounds on the release candidate: at most two requests for the message (one attempt and
at most one retry), no turn on another provider, exactly one owner message for the
episode, the probe answered or honestly refused within 120 s.

Run: NIMBUS_STORM_UNIT=A python3 -m pytest tests/hil/storm/test_storm_d_deterministic_error.py --allow-hardware -s
"""

from __future__ import annotations

import json
import re

import pytest

import grading as g
import kit
from mock_llm import FAULT_TURNS

pytestmark = [pytest.mark.hil, pytest.mark.net]

LEG = "d_deterministic_error"
HOST = "api.anthropic.com"
SCENARIO = "mock-invalid-400"
NOTICE_RE = re.compile(r"refused|rejected|didn.t go through|could not|couldn.t|error|invalid|failed|went wrong", re.I)
SWITCH_RE = re.compile(r"switching to|starting on|fail(?:ed)? over", re.I)
BOUNDS = g.Bounds(max_paid_turns=2, max_provider_requests=2, max_identical=5, notices=(1, 1), usable_within_s=120.0)


def test_storm_d_deterministic_error(storm):
    uid = kit.first_update_id()
    with storm.session(kit.keep_for(storm), kit.head_for(storm, SCENARIO)):
        if not storm.control:
            status, body = FAULT_TURNS[SCENARIO]
            out = storm.verb(f"PROVFAULT {HOST} {status} {json.dumps(body, separators=(',', ':'))}", "PROVFAULT")
            assert "armed" in out, out
        try:
            w = kit.Window(storm).open()
            kit.owner_turn(storm, f"storm d {uid}: reply with the single word pong", uid)
            w.close()
            episode = w.replies(NOTICE_RE)
            p = kit.Window(storm).open()
            probe = storm.fresh_message(kit.ANSWER_RE, f"storm d probe {uid}: reply pong", uid + 1, timeout=150.0)
            p.close()
        finally:
            if not storm.control:
                storm.verb("PROVFAULT off", "PROVFAULT", timeout=6.0)
                storm.verb("PARK clear", "PARK", timeout=6.0)
    lines = w.lines + p.lines
    peak, line = g.identical_peak(lines, ignore=kit.DRIVE_LINES)
    m = g.Metrics(
        paid_turns=g.paid_turns(lines),
        provider_requests=w.requests(HOST),
        parked_requests=0,
        notices=len(episode["notices"]) + len(g.log_notices(w.lines)),
        identical_peak=peak,
        identical_line=line[:160],
        usable=probe["usable"],
        usable_s=probe["usable_s"],
    )
    switches = [ln.text for ln in lines if SWITCH_RE.search(ln.text)]
    m.extra.update({"episode": episode, "probeReply": probe["reply"][:200], "switches": switches})
    if switches:
        m.notes.append(f"the turn moved to another provider: {switches[:2]}")
        m.parked_requests = len(switches)
    kit.assert_graded(storm.finish(LEG, m, BOUNDS))
