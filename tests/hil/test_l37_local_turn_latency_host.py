"""L37 host-tier - the local-turn wait log parser (CUM-462). No board; runs in
``pytest -m host`` and in ``--collect-only``.

The board log line is the only evidence the L37 leg reads, so the parser must take
the channel and the wait whole, and a missing or renamed line must fail loud.
"""

from __future__ import annotations

import pytest

from test_l37_local_turn_latency import LOCAL_TURN_MAX_WAIT_MS, parse_waited

pytestmark = pytest.mark.host


def test_parse_voice_wait():
    assert parse_waited("telegram: local turn from voice waited 12 ms") == ("voice", 12)


def test_parse_inside_a_prefixed_serial_line():
    assert parse_waited("[agent] telegram: local turn from web waited 30017 ms") == ("web", 30017)


@pytest.mark.parametrize(
    "line",
    [
        "",
        "telegram: local turn from voice waited ms",
        "telegram: poll ok",
        "VOICE inject <- latency probe 0",
    ],
)
def test_anything_else_fails_loud(line):
    with pytest.raises(ValueError):
        parse_waited(line)


def test_bar_is_the_issue_bar():
    # "Within about 1 s": a pre-fix 30 s wait can never pass, a healthy one always does.
    assert 1000 <= LOCAL_TURN_MAX_WAIT_MS < 5000
