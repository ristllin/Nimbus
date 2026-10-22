"""Storm leg b: the same Telegram page re-served, the settings store healthy.

Reproduces the 21 Oct 2026 storm (L19-1): one page of six owner messages re-served 50
times, 51 paid turns and 51 duplicate sends in an hour.

Drive: the same owner message delivered 50 times on the candidate (``TGINJECT``, the
real receive path with one update id) and 20 times on v4.5.13 (the bound for the
control image; the test inject route, the LAN mock as its head), then one fresh
message. Unit L.

Bounds on the release candidate: at most one paid turn and one turn's head requests,
exactly one owner notice (the duplicate guard's), no log line repeated more than 5
times, the fresh message answered within 180 s.

Run: NIMBUS_STORM_UNIT=L python3 -m pytest tests/hil/storm/test_storm_b_page_reserved.py --allow-hardware -s
"""

from __future__ import annotations

import pytest

import kit

pytestmark = [pytest.mark.hil, pytest.mark.net]

LEG = "b_page_reserved"
COPIES_CANDIDATE = 50
COPIES_CONTROL = 20  # the control image spends a turn per copy: bounded (DoD)
BOUNDS = kit.replay_bounds(COPIES_CANDIDATE)


def test_storm_b_page_reserved(storm):
    copies = COPIES_CONTROL if storm.control else COPIES_CANDIDATE
    with storm.session(kit.keep_for(storm), kit.head_for(storm)):
        m = kit.replay_storm(storm, LEG, copies)
    kit.assert_graded(storm.finish(LEG, m, BOUNDS))
