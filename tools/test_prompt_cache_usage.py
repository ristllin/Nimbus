#!/usr/bin/env python3
"""Self-test for prompt_cache_usage.py: both per-turn usage line formats are read.

Runnable directly or under pytest; needs no board. From v4.5.14 the turn's usage is
on its ``turn spend:`` line (one slot per host it billed) and the ``turn ->`` line
carries none, so a reader of the old line alone counts zero turns.
"""

from __future__ import annotations

import importlib.util
import pathlib

_SPEC = importlib.util.spec_from_file_location(
    "prompt_cache_usage", pathlib.Path(__file__).with_name("prompt_cache_usage.py")
)
pcu = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(pcu)


def test_v4514_spend_line_is_one_turn():
    log = "[agent] turn spend: ok serial | anthropic/claude-haiku-5-5 in=16 out=997 cR=124260 cW=10400 n=6\n"
    assert pcu.turns_from(log) == [(16, 997, 124260, 10400)]


def test_spend_line_sums_every_host_billed():
    log = (
        "turn spend: ok web | anthropic/claude-haiku-5-5 in=10 out=5 cR=100 cW=20 n=1"
        " | mistral/mistral-small in=30 out=7 cR=0 cW=0 n=2\n"
    )
    assert pcu.turns_from(log) == [(40, 12, 100, 20)]


def test_failed_turn_with_no_spend_is_not_a_turn():
    assert pcu.turns_from("turn spend: FAIL web | (none)\n") == []


def test_v4513_turn_line_still_reads():
    log = 'orchestrator: turn -> {"reply":"hi"} [tokens in=1 out=2 cacheR=3 cacheW=4]\n'
    assert pcu.turns_from(log) == [(1, 2, 3, 4)]


def test_turn_line_without_usage_is_not_double_counted():
    log = (
        'orchestrator: turn -> {"reply":"hi"}\n'
        "turn spend: ok web | anthropic/claude-haiku-5-5 in=1 out=2 cR=3 cW=4 n=1\n"
    )
    assert pcu.turns_from(log) == [(1, 2, 3, 4)]


if __name__ == "__main__":
    import sys

    import pytest

    sys.exit(pytest.main([__file__, "-q"]))
