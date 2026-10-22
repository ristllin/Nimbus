"""L4 host tier: which /api/state param the web-config legs edit. No board.

The F16 and web_post_config legs demanded the RingBrightness param, which the
device has not listed on a board with no LED ring since CUM-187 (v4.4.1), so they
failed on every ringless board (the v4.5.14 release run on unit A). tunable_param()
edits RingBrightness where there is a ring and a numeric non-ring param elsewhere,
and fails when a ringless board lists a ring param at all.
"""

from __future__ import annotations

import pytest

from test_l4_network import other_value, tunable_param

pytestmark = pytest.mark.host

DWELL = {"key": 8, "name": "dwell_ms", "kind": "num", "value": 300, "min": 100, "max": 1000, "step": 50}
BOOL = {"key": 10, "name": "tg_low_batt_ping", "kind": "bool", "value": 0, "min": 0, "max": 1, "step": 1}
BRIGHT = {"key": 1, "name": "ring_brightness", "kind": "num", "value": 60, "min": 0, "max": 255, "step": 5}


def test_ring_board_edits_ring_brightness():
    assert tunable_param({"hasRing": True, "params": [DWELL, BRIGHT]})["name"] == "ring_brightness"


def test_ringless_board_edits_a_numeric_non_ring_param():
    assert tunable_param({"hasRing": False, "params": [BOOL, DWELL]})["name"] == "dwell_ms"


def test_ringless_board_listing_a_ring_param_fails():
    with pytest.raises(AssertionError, match="ring params on a board with no ring"):
        tunable_param({"hasRing": False, "params": [DWELL, BRIGHT]})


def test_ring_board_without_ring_brightness_fails():
    with pytest.raises(AssertionError, match="no RingBrightness entry"):
        tunable_param({"hasRing": True, "params": [DWELL]})


def test_other_value_is_legal_and_different():
    v = other_value(DWELL, 150)
    assert v != 150 and 100 <= v <= 1000 and (v - 100) % 50 == 0
