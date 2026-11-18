"""Host tests for the history fix's bench rows in bench_rows.py (A07 with the budget pin, A07b).

No board: a fake console and a fake device API stand in, and each row is graded on the log
lines a v5.1.2 image and a v5.1.1 image write. A07 is calibrated to an 85,000 B request body,
which every head had up to v5.1.1; from v5.1.2 the console pins it (BUDGET, lane W) and the
pin is always given back. A07b is the same turn at the head's own budget: nothing left out.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench_driver as bd  # noqa: E402
import bench_rows as br  # noqa: E402

TRIMMED = [
    "10 100 - headloop: round 0 body 96210 B over the 85000 B budget - folded 12754 B of the prompt and input",
    "11 110 - turn fit: left out 24 older conversation message(s) (asked for 11210 B, gave 21600 B)",
    "12 120 - context: sent 16/40 live rows (today 16/40), cut 24 (21600 B), summary age -, round0 84102 B / budget 85000 B, hist 9 ms 0 card reads lock 4 ms",
    "13 130 - turn end: ok host=mistral tokens in=24000 out=300 tools=1 reply=420B src=web",
]
ROOMY = [
    "12 120 - context: sent 40/40 live rows (today 40/40), cut 0 (0 B), summary age -, round0 104210 B / budget 360000 B, hist 9 ms 0 card reads lock 4 ms",
    "13 130 - turn end: ok host=mistral tokens in=30000 out=300 tools=1 reply=420B src=web",
]


class FakeConsole:
    def __init__(self, has_budget_verb: bool = True):
        self.has_budget_verb = has_budget_verb
        self.sent: list = []

    def cmd(self, line: str, expect: str, timeout: float = 8.0) -> str:
        self.sent.append(line)
        if line.startswith("BUDGET "):
            if not self.has_budget_verb:
                return ""  # "ERR unknown 'BUDGET'" never holds the expected text
            return "BUDGET off" if line == "BUDGET 0" else line
        if line.startswith("HISTADD "):
            return "HISTADD chat=web n=1"
        return "TURN <- " + line[5:] if line.startswith("TURN ") else ""


class FakeApi:
    """The device API a row reads: /api/state, /api/errlog (new lines after the turn), /api/chat."""

    ip = "bench.test"

    def __init__(self, turn_lines: list, reply: str = "Three facts about Lisbon: ..."):
        self.turn_lines, self.reply, self.asked = turn_lines, reply, False

    def state(self, timeout=None):
        return {"boot": {"n": 7}, "mem": {"pollStackMin": 5000, "lowWater": 9000}}

    def _reply(self, payload) -> bd.Reply:
        return bd.Reply(200, json.dumps(payload).encode(), 0.01)

    def get(self, path, timeout=None):
        lines = ["1 10 - boot stack ok"] + (self.turn_lines if self.asked else [])
        return bd.Reply(200, "\n".join(lines).encode(), 0.01)

    def post_form(self, path, fields, timeout=None):
        self.asked = True
        return self._reply({"turn": "t1"})

    def get_json(self, path, timeout=None):
        return {"pending": False, "done": True, "reply": self.reply}


def bench(tmp_path, turn_lines, console=None, row="A07") -> br.Bench:
    out = tmp_path / row
    out.mkdir(parents=True, exist_ok=True)
    return br.Bench(
        args=argparse.Namespace(out=str(tmp_path), a02_state=""),
        api=FakeApi(turn_lines),
        ev=bd.Evidence(str(out)),
        console=console or FakeConsole(),
        poll_s=0.0,
        sleep=lambda s: None,
    )


def test_a07_pins_the_budget_fills_distinct_rows_and_gives_the_pin_back(tmp_path):
    b = bench(tmp_path, TRIMMED)
    grade, reason = br.row_a07(b)
    assert (grade, "pinned" in reason) == ("PASS", True), reason
    sent = b.console.sent
    assert sent[0] == "BUDGET 85000" and sent[-1] == "BUDGET 0"
    fills = [ln for ln in sent if ln.startswith("HISTADD web 1 row ")]
    assert len(fills) == br.HISTORY_ROWS == len(set(fills))  # identical rows would collapse into one


def test_a07_is_blocked_when_nothing_was_trimmed_and_still_unpins(tmp_path):
    b = bench(tmp_path, ROOMY)
    assert br.row_a07(b)[0] == "BLOCKED"
    assert b.console.sent[-1] == "BUDGET 0"


def test_a07_gives_the_pin_back_when_the_row_blows_up(tmp_path):
    b = bench(tmp_path, TRIMMED)
    b.api.state = lambda timeout=None: {}  # the unit stops answering mid-row
    try:
        br.row_a07(b)
    except br.Blocked:
        pass
    assert b.console.sent[-1] == "BUDGET 0"


def test_a07_on_an_image_without_the_verb_runs_unpinned_and_says_so(tmp_path):
    b = bench(tmp_path, TRIMMED, FakeConsole(has_budget_verb=False))
    grade, reason = br.row_a07(b)
    assert (grade, "unpinned" in reason) == ("PASS", True)
    assert "BUDGET 0" not in b.console.sent[1:]  # nothing to give back
    with open(os.path.join(str(tmp_path), "A07", "budget.txt")) as f:
        assert "no such verb" in f.read()


def test_a07_fails_when_a_refusal_names_another_budget_under_the_pin(tmp_path):
    lines = [TRIMMED[0].replace("85000 B budget", "360000 B budget")] + TRIMMED[1:]
    grade, reason = br.row_a07(bench(tmp_path, lines))
    assert grade == "FAIL" and "other than the pinned 85000" in reason


def test_a07b_passes_with_nothing_left_out(tmp_path):
    b = bench(tmp_path, ROOMY, row="A07b")
    grade, reason = br.row_a07b(b)
    assert grade == "PASS" and "40/40 live rows sent, cut 0" in reason
    assert b.console.sent[0] == "BUDGET 0"  # no pin left over from another row


def test_a07b_fails_on_a_cut_a_refusal_or_rows_not_sent(tmp_path):
    cut = [ROOMY[0].replace("sent 40/40", "sent 30/40").replace("cut 0 (0 B)", "cut 10 (9000 B)")] + ROOMY[1:]
    grade, reason = br.row_a07b(bench(tmp_path, cut, row="A07b"))
    assert grade == "FAIL" and "cut 10 (9000 B)" in reason and "30 of 40 live rows" in reason
    refused = ["9 90 - headloop: round 1 body 361200 B over the 360000 B budget - folded 4000 B"] + ROOMY
    grade, reason = br.row_a07b(bench(tmp_path, refused, row="A07b"))
    assert grade == "FAIL" and "refused over its budget: [360000]" in reason


def test_a07b_is_blocked_on_an_older_image_and_on_a_floor_head(tmp_path):
    old = ["13 130 - turn end: ok host=mistral tokens in=24000 out=300 tools=1 reply=420B src=web"]
    for lines, why in ((old, "older than v5.1.2"), (TRIMMED, "floor")):
        b = bench(tmp_path, lines, row="A07b")
        try:
            br.row_a07b(b)
            raise AssertionError("not blocked")
        except br.Blocked as e:
            assert why in str(e)


def test_the_new_row_is_listed():
    assert br.ROWS["A07b"] == (br.row_a07b, True) and "A07" in br.ROWS and "A08" in br.ROWS
