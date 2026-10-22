"""Storm leg f: the device restarts in the middle of a turn.

Reproduces 19 Oct 2026 (L19-31, L19-32, L19-34, L19-47): resets mid-turn and mid-upkeep
whose work was lost or redone, with a re-served Telegram page waiting after the boot.

Drive: one owner message whose turn is cut by a restart, then the same page delivered
again after the boot (Telegram serves an unacknowledged page again), then one fresh
message. Candidate: ``PANIC abort arm=turn round=0`` crashes inside the turn (the head is
Anthropic ``NIMBUS_STORM_MODEL``). v4.5.13 (no crash seam): the LAN mock holds the head
request open and the unit is restarted while it waits. Every other provider key is
cleared for the leg. Unit A.

Bounds on the release candidate: at most two turns (the cut one and the re-served
page), at most eight head requests, exactly one owner notice about the restart, no log
line repeated more than 5 times, the fresh message answered within 180 s.

Run: NIMBUS_STORM_UNIT=A python3 -m pytest tests/hil/storm/test_storm_f_reboot_mid_turn.py --allow-hardware -s
"""

from __future__ import annotations

import re
import time

import pytest

import grading as g
import kit

pytestmark = [pytest.mark.hil, pytest.mark.net]

LEG = "f_reboot_mid_turn"
NOTICE_RE = re.compile(r"restart|restarted|interrupted|cut|send it again|could not be processed", re.I)
BOUNDS = g.Bounds(max_paid_turns=2, max_provider_requests=8, max_identical=5, notices=(1, 1), usable_within_s=180.0)


def _cut_turn(storm, text: str, uid: int) -> str:
    """Start the turn and restart the unit inside it; returns how it was cut."""
    if storm.control:
        storm.set_mock_scenario("mock-hold")
        storm.inject(text, uid)
        got = storm.mock.wait_for_request("mock-hold", count=1, timeout=90.0)
        assert got, "the head request never reached the mock"
        time.sleep(2.0)
        r = storm.net.post("/api/test/reboot", {}, timeout=8.0)
        storm.wait_back()
        storm.set_mock_scenario("mock-echo")
        return f"restart while the head request was held (HTTP {r.status_code})"
    armed = storm.verb("PANIC abort arm=turn round=0", "PANIC", timeout=6.0)
    assert "armed" in armed, armed
    try:
        storm.inject(text, uid, timeout=120.0)
    except Exception as e:  # noqa: BLE001 - the console dies with the crash: expected
        print(f"[storm] console lost in the crash: {type(e).__name__}")
    storm.wait_back(timeout=360.0)  # a crash boot writes its dump and rescans the card: about 3 min on unit A
    return armed.strip()


def test_storm_f_reboot_mid_turn(storm):
    uid = kit.first_update_id()
    text = f"storm f {uid}: reply with the single word pong"
    with storm.session(kit.keep_for(storm), kit.head_for(storm)):
        w = kit.Window(storm).open()
        how = _cut_turn(storm, text, uid)
        kit.owner_turn(storm, text, uid)  # the page served again after the boot
        storm.wait_quiet(quiet_s=20.0, timeout=300.0)
        w.close()
        probe = storm.fresh_message(kit.ANSWER_RE, f"storm f probe {uid}: reply pong", uid + 1)
    replies = w.replies(NOTICE_RE)
    peak, line = g.identical_peak(w.lines, ignore=kit.DRIVE_LINES)
    crash_lines = [ln.text for ln in w.lines if "crash: owner notice" in ln.text or "boot: n=" in ln.text]
    m = g.Metrics(
        paid_turns=g.paid_turns(w.lines),
        provider_requests=w.requests(),
        parked_requests=0,
        notices=w.notices(NOTICE_RE),
        identical_peak=peak,
        identical_line=line[:160],
        usable=probe["usable"],
        usable_s=probe["usable_s"],
    )
    m.extra.update({"cut": how, "replies": replies, "bootLines": crash_lines[:6], "probeReply": probe["reply"][:200]})
    if not storm.control:
        m.extra["guard"] = kit.guard_line(storm)
    kit.assert_graded(storm.finish(LEG, m, BOUNDS))
