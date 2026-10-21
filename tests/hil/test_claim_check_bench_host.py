"""Host checks for the claim-check bench legs' readers (no device).

C1 once failed with the turn's reply delivered: the reply reader walked the
episodic page oldest first (the API answers newest first, across every chat) and
returned the oldest assistant row on the page, which a new reply does not change.
These pin the reader: a new reply is a new id wherever it sits on the page and
whatever its text, rows that slide off the page are not new, one turn gives
exactly one reply, and user and log rows are not replies.
"""

from __future__ import annotations

import pytest

from test_claim_check_bench import assistant_replies, new_replies, the_one_reply

pytestmark = pytest.mark.host

REPLY = "Saved it to memory.\n\n(Correction: that has not been saved.)"


def _row(n: int, role: str, text: str, kind: str = "message") -> dict:
    return {"id": f"m{n:08x}", "session": "serial", "role": role, "kind": kind, "text": text}


def _page(*rows: dict) -> dict:
    return {"messages": list(rows)}


OLDER = (
    _row(0x12, "assistant", "the reply before"),
    _row(0x11, "user", "a question"),
    _row(0x10, "assistant", "the oldest reply on the page"),
)


def test_a_new_reply_at_the_top_of_a_newest_first_page_is_found():
    before = assistant_replies(_page(*OLDER))
    after = assistant_replies(_page(_row(0x14, "assistant", REPLY), _row(0x13, "user", "remember it"), *OLDER))
    assert the_one_reply(new_replies(before, after), "mock-claim-saved") == REPLY


def test_a_reply_repeating_an_earlier_text_is_still_new():
    before = assistant_replies(_page(_row(0x12, "assistant", REPLY), *OLDER[1:]))
    after = assistant_replies(_page(_row(0x14, "assistant", REPLY), _row(0x12, "assistant", REPLY), *OLDER[1:]))
    assert new_replies(before, after) == {"m00000014": REPLY}


def test_rows_sliding_off_a_full_page_are_not_new():
    # A limit-3 page: the new reply and its question push the two oldest rows off.
    before = assistant_replies(_page(*OLDER))
    after = assistant_replies(_page(_row(0x14, "assistant", REPLY), _row(0x13, "user", "remember it"), OLDER[0]))
    assert new_replies(before, after) == {"m00000014": REPLY}


def test_user_and_log_rows_are_not_replies():
    page = _page(
        _row(0x16, "system", "memory: embeddings parked", kind="log"),
        _row(0x15, "assistant", "thinking about it", kind="thinking"),
        _row(0x14, "user", "remember it"),
    )
    assert assistant_replies(page) == {}


def test_no_reply_fails_and_two_replies_fail():
    with pytest.raises(pytest.fail.Exception, match="no delivered reply recorded"):
        the_one_reply({}, "mock-claim-saved")
    with pytest.raises(AssertionError, match="exactly one reply"):
        the_one_reply({"m00000014": REPLY, "m00000015": "Done."}, "mock-claim-saved")
