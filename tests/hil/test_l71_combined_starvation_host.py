"""Host tier for L71's history reader (no board).

Since v4.6.1 /api/mem/episodic answers 503 busy when the card lock is held for more
than 150 ms, and the admission check answers a text/plain busy under the ballast. The
leg read either as an empty history, which lowered its id floor to nothing (every
earlier reply counted as this turn's) or emptied the re-read of a reply that had
landed. The reader now treats every busy answer as "retry".
"""

from __future__ import annotations

import json

import pytest

import test_l71_combined_starvation as l71

pytestmark = pytest.mark.host

ROWS = {
    "messages": [
        {"id": "m00000012", "role": "assistant", "kind": "message", "text": "the reply"},
        {"id": "m00000011", "role": "user", "kind": "message", "text": "the question"},
    ]
}


def _json(body: str):
    return lambda: json.loads(body)


def test_a_history_page_is_read():
    page = l71.history_page(200, _json(json.dumps(ROWS)))
    assert l71.assistant_rows(page) == {"m00000012": "the reply"}


def test_an_empty_history_is_a_page():
    assert l71.history_page(200, _json('{"messages":[]}')) == {"messages": []}


@pytest.mark.parametrize(
    "status,body",
    [
        (503, '{"error":"busy","message":"The device is busy right now. Try again in a few seconds."}'),
        (503, "busy"),  # the admission check's text/plain answer
        (200, "busy"),
        (200, '{"error":"busy"}'),  # no messages list: not a history page
        (401, '{"error":"auth"}'),
    ],
)
def test_a_busy_or_foreign_answer_is_never_an_empty_history(status, body):
    assert l71.history_page(status, _json(body)) is None


class _Net:
    """Answers each GET from a script of (status, body)."""

    def __init__(self, script):
        self.script, self.calls = list(script), 0

    def get(self, path, timeout=5.0):
        self.calls += 1
        status, body = self.script.pop(0) if len(self.script) > 1 else self.script[0]

        class R:
            status_code = status
            text = body

            @staticmethod
            def json():
                return json.loads(body)

        return R()


def test_replies_retry_a_busy_card_until_the_page_comes(monkeypatch):
    monkeypatch.setattr(l71.time, "sleep", lambda _s: None)
    net = _Net([(503, '{"error":"busy"}'), (503, "busy"), (200, json.dumps(ROWS))])
    assert l71._replies(net) == {"m00000012": "the reply"}
    assert net.calls == 3


def test_replies_fail_loudly_when_the_card_stays_busy(monkeypatch):
    clock = {"t": 0.0}
    monkeypatch.setattr(l71.time, "time", lambda: clock["t"])
    monkeypatch.setattr(l71.time, "sleep", lambda s: clock.__setitem__("t", clock["t"] + s))
    with pytest.raises(AssertionError, match="stayed busy"):
        l71._replies(_Net([(503, '{"error":"busy"}')]), budget_s=5.0)
