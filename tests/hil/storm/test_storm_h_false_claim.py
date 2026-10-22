"""Storm leg h: a model that claims an action it did not take.

Reproduces 14 Oct and 19 Oct 2026 (L19-11, L19-50): the assistant replied "approved"
to a skill command it never ran, and said a standing rule was saved when the memory
write had been refused.

Drive: the head runs on the LAN mock with a scripted reply, "Saved it to memory.", from
a turn that wrote nothing; then one fresh message on the plain mock reply. Same drive on
both images (no spend). Unit A.

Bounds on the release candidate: one turn and at most two head requests, exactly one
correction in the owner's reply (the claim is never passed on as true), no log line
repeated more than 5 times, the fresh message answered within 120 s.

Run: NIMBUS_STORM_UNIT=A python3 -m pytest tests/hil/storm/test_storm_h_false_claim.py --allow-hardware -s
"""

from __future__ import annotations

import re

import pytest

import grading as g
import kit

pytestmark = [pytest.mark.hil, pytest.mark.net]

LEG = "h_false_claim"
SCENARIO = "mock-claim-nothing"
CORRECTION_RE = re.compile(r"correction|has not been saved|not saved|wasn.t saved|was not saved", re.I)
BOUNDS = g.Bounds(max_paid_turns=2, max_provider_requests=2, max_identical=5, notices=(1, 1), usable_within_s=120.0)


def test_storm_h_false_claim(storm):
    uid = kit.first_update_id()
    with storm.session(None, storm.head_mock(SCENARIO)):
        tailed0 = storm.state().get("claimsTailed")
        w = kit.Window(storm).open()
        kit.owner_turn(storm, f"storm h {uid}: remember that the bench plant is watered on Mondays", uid)
        w.close()
        storm.set_mock_scenario("mock-echo")
        probe = storm.fresh_message(kit.ANSWER_RE, f"storm h probe {uid}: say hi", uid + 1, timeout=150.0)
        tailed1 = storm.state().get("claimsTailed")
    claims = [r.get("text", "") for r in w.rows if r.get("role") == "assistant"]
    corrected = [t for t in claims if CORRECTION_RE.search(t)]
    peak, line = g.identical_peak(w.lines, ignore=kit.DRIVE_LINES)
    m = g.Metrics(
        paid_turns=g.paid_turns(w.lines),
        provider_requests=w.mock_requests,
        parked_requests=0,
        notices=len(corrected),
        identical_peak=peak,
        identical_line=line[:160],
        usable=probe["usable"],
        usable_s=probe["usable_s"],
    )
    m.extra.update({"replies": claims[:4], "claimsTailed": [tailed0, tailed1], "probeReply": probe["reply"][:200]})
    if claims and not corrected:
        m.notes.append(f"the false claim reached the owner as true: {claims[0][:80]!r}")
    if not claims:
        m.notes.append("no reply reached the owner chat for the claim turn")
        m.notices = 0
    kit.assert_graded(storm.finish(LEG, m, BOUNDS))
