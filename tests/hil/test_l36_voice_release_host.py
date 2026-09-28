"""L36 host-tier - the pure VOICE? parser (CUM-456). No board; runs in
``pytest -m host`` and in ``--collect-only``.

The status line runs to end of line inside quotes and contains spaces and periods,
so the parser must capture it whole; a missing or mangled line must fail loud.
"""

from __future__ import annotations

import pytest

from test_l36_voice_release import parse_voice_state

pytestmark = pytest.mark.host


def test_parse_offline_notice():
    v = parse_voice_state(
        'VOICE phase=notice outcome=no_network transitions=3 shownMs=127 sttMs=0 '
        'line="No network. Check Wi-Fi and try again."'
    )
    assert v == {
        "phase": "notice",
        "outcome": "no_network",
        "transitions": 3,
        "shown_ms": 127,
        "stt_ms": 0,
        "line": "No network. Check Wi-Fi and try again.",
    }


def test_parse_idle_empty_line():
    v = parse_voice_state('VOICE phase=idle outcome=none transitions=0 shownMs=0 sttMs=0 line=""')
    assert v["phase"] == "idle" and v["line"] == ""


def test_parse_ignores_leading_noise_and_trailing_lines():
    raw = (
        "[agent] something\n"
        'VOICE phase=thinking outcome=none transitions=2 shownMs=110 sttMs=140 line="Thinking. You: hi"\n'
        "RENDER screen=0\n"
    )
    v = parse_voice_state(raw)
    assert v["phase"] == "thinking" and v["stt_ms"] == 140 and v["line"] == "Thinking. You: hi"


def test_parse_loud_on_garbage():
    with pytest.raises(ValueError):
        parse_voice_state("no voice state here")
    with pytest.raises(ValueError):
        parse_voice_state("VOICE phase=idle outcome=none")
