"""L35 host-tier - the pure FEEDBACK? parser (CUM-309). No board; runs in
``pytest -m host`` and in ``--collect-only``.

The toast field runs to end of line and contains spaces (and a trailing period), so
the parser must capture it whole rather than splitting on whitespace - the failure
this covers.
"""

from __future__ import annotations

import pytest

from test_l35_button_feedback import parse_feedback

pytestmark = pytest.mark.host


def test_parse_feedback_success():
    fb = parse_feedback("FEEDBACK seen=1 action=6 outcome=0 sfx=agent_done ring=success toast=Settings reset to defaults.")
    assert fb == {
        "seen": True,
        "action": 6,
        "outcome": 0,
        "sfx": "agent_done",
        "ring": "success",
        "toast": "Settings reset to defaults.",
    }


def test_parse_feedback_failure_multiword_toast():
    fb = parse_feedback("FEEDBACK seen=1 action=1 outcome=2 sfx=error ring=failure toast=No SD card found. Reseat it and rescan.")
    assert fb["sfx"] == "error"
    assert fb["ring"] == "failure"
    # The whole sentence, spaces and trailing period intact.
    assert fb["toast"] == "No SD card found. Reseat it and rescan."


def test_parse_feedback_no_toast_dash():
    fb = parse_feedback("FEEDBACK seen=1 action=9 outcome=1 sfx=agent_spawn ring=ack toast=-")
    assert fb["ring"] == "ack" and fb["toast"] == "-"


def test_parse_feedback_unfired():
    fb = parse_feedback("FEEDBACK seen=0 action=-1 outcome=-1 sfx=- ring=- toast=-")
    assert fb["seen"] is False and fb["action"] == -1


def test_parse_feedback_ignores_leading_noise():
    raw = "some boot line\nFEEDBACK seen=1 action=6 outcome=0 sfx=agent_done ring=success toast=Settings reset to defaults.\n"
    assert parse_feedback(raw)["action"] == 6


def test_parse_feedback_loud_on_garbage():
    with pytest.raises(ValueError):
        parse_feedback("no feedback here")
    with pytest.raises(ValueError):
        parse_feedback("FEEDBACK seen=1 action=6")  # truncated
