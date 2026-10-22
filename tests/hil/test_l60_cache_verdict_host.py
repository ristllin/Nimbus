"""L60 host tier: the replayed-turn cache verdict. No board.

The replay leg's absolute 1,000-token write bound came from the lab harness, whose
per-turn block is about 300 tokens; on unit A the per-turn block is about 2,000
tokens and is rightly written fresh each turn (22 Oct: turn 2 read 20,500 of a
20,458-token prefix and wrote 1,993). The verdict now bounds the write by the
per-turn block; the fault the bound exists for, a re-written stable prefix, still
fails.
"""

from __future__ import annotations

import pytest

from test_l60_prompt_cache import replay_cache_verdict

pytestmark = pytest.mark.host


def test_unit_a_replay_on_22_oct_passes():
    assert replay_cache_verdict(20500, 1993, 20458, 2039) == ""


def test_a_rewritten_stable_prefix_fails():
    # The 19 Oct shape: the whole prefix written again on the replay.
    assert "wrote" in replay_cache_verdict(20500, 12236, 20458, 2039)


def test_a_cold_prefix_fails():
    assert "read" in replay_cache_verdict(9000, 1500, 20458, 2039)


def test_an_implausible_prefix_fails():
    assert "implausible" in replay_cache_verdict(900, 100, 800, 2039)
