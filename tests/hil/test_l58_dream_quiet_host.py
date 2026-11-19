"""Host checks for the L58 nightly-upkeep legs' stream readers (no device).

Legs 1 and 2 of lane M once failed "no ev:dream row was written" because the row
reader stopped at the EPIQ summary line, before any row, and read 48-character
previews: the reader takes the counted rows whole (EPIQ --full), newest first. Since
v5.2.0 (lane RB-Q2b) the night makes no model call: the judges below read a drill's
window and its newest row, each with a counter-case.
"""

from __future__ import annotations

import pytest

from device import Device
from test_l58_dream_quiet import _dream_rows, night_made_no_turn, night_row_ok

pytestmark = pytest.mark.host


class _Console:
    """A serial stand-in that answers the first command written with ``reply``."""

    def __init__(self, reply):
        self.reply = [ln.encode() + b"\n" for ln in reply]
        self.out = []

    def write(self, data):
        self.out, self.reply = self.out + self.reply, []

    def flush(self):
        pass

    def readline(self):
        return self.out.pop(0) if self.out else b""


def _console(reply):
    dev = Device(port="/dev/null-host-test")
    dev._ser = _Console(reply)
    return dev


OK_ROW = "Nightly dream ok: maintenance pruned=0 deduped=1 (412 memories)"
# A row v5.1.2 wrote before the update: still read, newest first.
OLDER_ROW = "Nightly dream ok: 2 of 2 memories stored (2 new, 0 updated, 0 queued, 0 failed); spent 1295 tokens"


def test_the_dream_rows_are_read_whole_and_newest_first():
    """The EPIQ reply: the summary, then one line per row (log lines interleave)."""
    dev = _console(
        [
            "EPIQ rows=3 ms=41 files=2 bytes=1880 older=1 next=m0000002a",
            f"m00000031 {OK_ROW}",
            "[agent] telegram: getUpdates 401 - bot token rejected (revoked or invalid)",
            f"m0000002f {OLDER_ROW}",
            "m0000002b Nightly dream skipped (maintenance done earlier today)",
        ]
    )
    rows = _dream_rows(dev)
    assert rows[0] == OK_ROW  # newest first, whole
    assert rows[1] == OLDER_ROW
    assert len(rows) == 3


def test_no_rows_reads_as_none():
    dev = _console(["EPIQ rows=0 ms=3 files=0 bytes=0 older=0 next=-"])
    assert _dream_rows(dev) == []


def test_a_window_with_no_turn_passes_and_a_turn_or_a_send_fails():
    quiet = "[agent] dream: start\n[agent] dream: maintenance - decayed, pruned=0 deduped=1 (412 vectors)\n"
    assert night_made_no_turn(quiet) is None
    turn = quiet + "[agent] orchestrator: turn host=anthropic (conv=0 heap=30880)\n"
    assert "model turn" in night_made_no_turn(turn)
    assert "Telegram" in night_made_no_turn(quiet + "[agent] telegram: send chat=1001 (88 B)\n")


def test_the_newest_row_must_be_an_upkeep_row_with_no_spend():
    assert night_row_ok(OK_ROW) is None
    assert night_row_ok("Nightly dream skipped (maintenance done earlier today)") is None
    assert "spend" in night_row_ok(OLDER_ROW)  # the reflection's row (v5.1.2) reads as a failure here
    assert "neither ran" in night_row_ok("Nightly dream skipped (clock not set)")
    assert "not an ev:dream row" in night_row_ok("Conversation compacted: 1200 B")
