"""Lane DEGRADE - /api/state carries the degraded set on a real unit.

The device says when it runs degraded (nimbus/orch/degraded.h): one short line per
condition on the home screen and the web page, the same set in ``/api/state`` under
``degraded``. Host-tested first (test_degraded, test_degraded_render, the web harness
spec 94_degraded). This leg reads the real unit's ``/api/state``: the key is always
present, its count matches its items, every id is one of the six, every line is a short
printable-ASCII status line with a reason, and the ids are in display order.

What it does NOT prove (OPEN HARDWARE, nothing was driven on a board by the lane): that
the strip is drawn on the glass, that a tap opens the details card, and that each
condition raises and clears against its real cause. Those need a bench unit and a person
looking at the screen (the strip's goldens are the host proof).

Needs ``NIMBUS_TEST_IP`` and ``NIMBUS_TEST_TOKEN``. Read-only: it changes nothing.
"""

from __future__ import annotations

import pytest

from webrig import ip_tok_or_skip, make_session, require_orchestrator

pytestmark = pytest.mark.net

IDS = (
    "head_fallback",
    "provider_parked",
    "memory",
    "store_full",
    "telegram_silent",
    "clock_unsynced",
)
LINE_MAX = 40  # kDegradedLineMax


def test_state_carries_the_degraded_set():
    ip, tok = ip_tok_or_skip("the degraded state leg")
    session = make_session(tok)
    state = require_orchestrator(session, ip, "degraded state")
    assert "degraded" in state, "/api/state has no degraded key: a poller must never meet it absent"
    g = state["degraded"]
    assert set(g) >= {"count", "gen", "items"}
    assert g["count"] == len(g["items"])
    last = -1
    for item in g["items"]:
        assert item["id"] in IDS
        assert 0 < len(item["line"]) <= LINE_MAX
        assert item["line"].isascii() and item["line"].isprintable()
        assert item["why"].strip() and item["why"].isascii()
        assert "—" not in item["line"] + item["why"]
        idx = IDS.index(item["id"])
        assert idx > last, "items are in display order, one per id"
        last = idx
