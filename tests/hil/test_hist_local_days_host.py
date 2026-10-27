"""Host tests for the HIST bench leg's readers (no board).

The leg reads day markers out of the head's prompt in two shapes (the message form the
providers send, and the block form with the history switch off), so a changed marker
shape fails here instead of passing a bench run vacuously.
"""

from __future__ import annotations

import calendar

import pytest

from test_hist_local_days import NO_MARKER, PROBE_AT, TURNS, marker_before, stamps_ok, utc

pytestmark = pytest.mark.host

MESSAGE_FORM = (
    "[user] (Sat 24 Oct) HISTMARK1 H1: reply with just OK.\n[assistant] OK\n"
    "[user] (Sun 25 Oct) HISTMARK2 H1: reply with just OK.\n[assistant] OK\n"
    "[user] HISTMARK3 H1: reply with just OK.\n[assistant] OK\n"
)
BLOCK_FORM = (
    "## RECENT CONVERSATION (this chat, oldest first)\n(Sat 24 Oct)\n- user: HISTMARK1 H1: reply with just OK.\n"
    "- assistant: OK\n(Sun 25 Oct)\n- user: HISTMARK2 H1: reply with just OK.\n- assistant: OK\n"
    "- user: HISTMARK3 H1: reply with just OK.\n- assistant: OK\n"
)


@pytest.mark.parametrize("history", [MESSAGE_FORM, BLOCK_FORM])
def test_marker_before_reads_both_history_shapes(history):
    assert marker_before(history, "HISTMARK1 H1") == "Sat 24 Oct"
    assert marker_before(history, "HISTMARK2 H1") == "Sun 25 Oct"
    assert marker_before(history, "HISTMARK3 H1") == NO_MARKER
    assert marker_before(history, "HISTMARK9 H1") is None


def test_the_placed_clock_matches_the_calendar():
    # 25 Oct 2026 is a Sunday and BST ends at 01:00 UTC: the instants the leg places the clock at.
    assert calendar.timegm((2026, 10, 25, 1, 0, 0)) == utc(25, 1, 0)
    assert [t for t, _ in TURNS] == sorted(t for t, _ in TURNS)
    assert TURNS[-1][0] < PROBE_AT
    assert TURNS[1][0] == utc(25, 0, 0) - 1800  # 23:30 UTC Saturday = 00:30 BST Sunday
    assert [want for _, want in TURNS] == ["Sat 24 Oct", "Sun 25 Oct", NO_MARKER, NO_MARKER, "Mon 26 Oct"]


def test_stamps_ok_wants_the_stamp_just_after_the_placed_clock():
    at = utc(24, 22, 30)
    assert stamps_ok(f"from:web,at:{at + 4}", at)
    assert stamps_ok(f"at:{at}", at)
    assert not stamps_ok("from:web", at)  # no stamp
    assert not stamps_ok(f"from:web,at:{at - 1}", at)  # before the clock was placed
    assert not stamps_ok(f"from:web,at:{at + 3600}", at)  # long after
    assert not stamps_ok(f"from:web,att:{at}", at)  # a different tag
