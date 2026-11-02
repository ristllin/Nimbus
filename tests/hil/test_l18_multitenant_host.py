"""Host tier for L18's write check (no board).

On 28 Oct the leg went RED on its positive control: a memory.write that answered "queued
memory <id>, not stored yet ... stored automatically ..." passed the leg's check because
that sentence holds the word "stored". The leg's exact-sentence search then found nothing
(fixed in the firmware: memory.search now finds queued facts), and the log's unrelated
"You do not have access" line got the blame: an RBAC regression that never existed.
"""

from __future__ import annotations

import json

import pytest

import test_l18_multitenant as l18

pytestmark = pytest.mark.host


@pytest.mark.parametrize(
    "result,landed",
    [
        ('{"text": "stored memory m0001 (pinned)"}', True),
        (
            '{"text": "queued memory m0001, not stored yet: tls busy. It is kept on the device and stored '
            'automatically when embeddings work again; keyword search finds it meanwhile."}',
            True,
        ),
        # A full queue says "not stored" too, and must not pass as a write.
        ('{"text": "embedding failed (memory not stored: 16 memories are already waiting)"}', False),
        ('{"text": "this conversation is not approved to use memory"}', False),
        ('{"text": "duplicate of an existing memory, importance raised"}', False),
    ],
)
def test_write_landed(result, landed):
    assert l18.write_landed(result) is landed


@pytest.mark.parametrize(
    "search,marker,stored",
    [
        ("Found 1 memories:\n- [100%] Alice's code at Pier 9 is LOCKER-1\n", "LOCKER-1", True),
        ("Found 1 memories:\n- [pending, not stored yet] Alice's code at Pier 9 is LOCKER-1\n", "LOCKER-1", False),
        # A queued copy next to some other stored row says nothing about this row.
        (
            "Found 2 memories:\n- [90%] another note\n- [pending, not stored yet] the code is LOCKER-1\n",
            "LOCKER-1",
            False,
        ),
        ("No relevant memories found.", "LOCKER-1", False),
        # The leg's _text is json.dumps of the MCP result, so its newlines arrive escaped.
        (json.dumps({"text": "Found 2 memories:\n- [pending, not stored yet] other\n- [100%] is LOCKER-1\n"}), "LOCKER-1", True),
        (json.dumps({"text": "Found 2 memories:\n- [100%] other\n- [pending, not stored yet] is LOCKER-1\n"}), "LOCKER-1", False),
    ],
)
def test_row_stored(search, marker, stored):
    assert l18.row_stored(search, marker) is stored


# The positive control must reach the vector store: a queued row answers through the queue
# boundary, so Bob and Carla finding nothing would prove nothing about the namespace filter.
# The fake echoes the query: a row's marker is random.
def _board(monkeypatch, write_text, tag_of_search):
    state = {"searches": 0}

    def fake_call(_rig, _chat, tool, args):
        if tool == "memory.write":
            return {"text": write_text}
        state["searches"] += 1
        return {"text": f"Found 1 memories:\n- {tag_of_search(state['searches'])} {args['query']}\n"}

    monkeypatch.setattr(l18, "_call", fake_call)
    monkeypatch.setattr(l18.time, "sleep", lambda _s: None)
    return state


QUEUED = "queued memory m0001, not stored yet: tls busy."
PENDING = "[pending, not stored yet]"


def test_a_stored_write_goes_straight_on(monkeypatch):
    state = _board(monkeypatch, "stored memory m0001", lambda _n: "[100%]")
    assert l18._store_secret(("ip", "tok"), "chat")
    assert state["searches"] == 0


def test_a_queued_write_waits_for_the_store(monkeypatch):
    state = _board(monkeypatch, QUEUED, lambda n: PENDING if n < 3 else "[100%]")
    secret, marker = l18._store_secret(("ip", "tok"), "chat")
    assert marker in secret
    assert state["searches"] == 3


def test_a_write_that_never_leaves_the_queue_skips_loudly(monkeypatch):
    state = _board(monkeypatch, QUEUED, lambda _n: PENDING)
    clock = iter(range(0, 100000, 30))
    monkeypatch.setattr(l18.time, "monotonic", lambda: next(clock))
    with pytest.raises(pytest.skip.Exception, match="namespace filter"):
        l18._store_secret(("ip", "tok"), "chat")
    assert state["searches"] > 1
