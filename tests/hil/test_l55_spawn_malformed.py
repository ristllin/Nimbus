"""L55 row 4 on the device: a head turn whose ``session_ops`` arrived as plain text.

Six Lumi turns claimed sub-agents that never ran; one cause was a non-array
``session_ops`` dropped in silence. Lane L's normalizer (lib/core orch_turn.cpp)
reports it as a spawn that did not start, and apply logs it and the spawn ledger.
The shape has to come from the head's own structured turn, so the head runs on the
LAN mock (``mock-spawn-plaintext``, a fixed orch_turn whose ``session_ops`` is a
sentence) and the leg costs no provider tokens. The old leg injected a Telegram
text from an unlisted chat, which only queues an approval, so it could never pass.

The device must: log ``spawn not started (...) - the session_ops field arrived as
plain text, not a list``, log ``spawn ledger spawned=0 claimed=1``, and deliver one
reply carrying the not-started correction, never the reply's claim alone.

Run: python3 -m pytest tests/hil/test_l55_spawn_malformed.py -m "hil and net" --allow-hardware
"""

from __future__ import annotations

import pytest

from device import ExpectTimeout
from mock_llm import mock_llm  # noqa: F401  (fixture)
from test_claim_check_bench import _turn, mock_head  # noqa: F401  (fixture + helper)
from test_l12_mock_llm import custom_backend  # noqa: F401  (fixture)

pytestmark = [pytest.mark.hil, pytest.mark.net]

MODEL = "mock-spawn-plaintext"
NOT_STARTED = "(Correction: the sub-agents did not start. Ask me again and I will retry.)"


def test_plaintext_session_ops_is_refused_not_dropped(mock_head):  # noqa: F811
    be = mock_head
    be.device.drain(quiet=0.3)
    reply = _turn(be, MODEL, "spawn three researchers on this and summarize")
    # The turn's console lines are still in the serial buffer, in order.
    try:
        be.device.expect("the session_ops field arrived as plain text, not a list", timeout=30.0)
    except ExpectTimeout:
        pytest.fail("the plain-text session_ops was not reported as a spawn that did not start (a silent drop)")
    try:
        be.device.expect("spawn ledger spawned=0 claimed=1", timeout=15.0)
    except ExpectTimeout:
        pytest.fail("no spawn ledger line for the refused intent")
    assert NOT_STARTED in reply, f"the reply kept its claim without the not-started correction: {reply!r}"
