"""Summarize now - the owner starts a fold from the web and watches it land (v5.1.2).

Bench row B08, on hardware. Lane F adds POST /api/fold/run (owner-authenticated: the
device sign-in code plus the web-task owner check; it stages a manual catch-up and answers
202) and GET /api/fold/status (summary age, thru id and date, the uncovered span, the last
success, failed cycles, the next try, paid fold calls today and the daily cap, and the
active rung i/n), registered through the deferred route registry; /api/state is unchanged.

The leg seeds the web chat with three turns, asks for a summary through the route, and
waits for it the way the owner does: the status moves (a new last success, the summary
fresh) and the web chat gets exactly one confirmation that starts with the existing
"✓ Compacted." and names what it covered ("Summary updated: covered N messages from D1
to D2"). A route without the device sign-in code is refused. Up to v5.1.1 there is no
route (404): the leg fails there, as it must.

Markers: hil, net, agent. Needs --allow-hardware, NIMBUS_TEST_IP and NIMBUS_TEST_TOKEN on
an Orchestrator-mode board with a provider key. Four paid turns plus one fold (a few cents).
The dedicated bench board only.
"""

from __future__ import annotations

import time

import pytest

from webrig import chat_turn, ip_tok_or_skip, make_session, require_orchestrator

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

RUN = f"FN{int(time.time()) % 100000}"
# A manual cycle walks one rung per pump pass; each rung is up to two requests of 65 s
# (Mistral) or 120 s (Anthropic, OpenAI).
FOLD_WAIT_S = 600
CONFIRM = "✓ Compacted."
# The status fields the plan names (plan v3 protocols, "Summarize now and staleness"), each
# with the spellings accepted until lane F's route lands (bind exactly in lane E phase 2).
STATUS_FIELDS = {
    "summary age": ("summaryAgeH", "summaryAge", "ageH", "age"),
    "thru": ("thru", "thruId"),
    "last success": ("lastOk", "lastSuccess", "lastFold"),
    "failed cycles": ("failedCycles",),
    "next try": ("nextTry", "nextTryEpoch"),
    "paid today": ("paidToday", "paidCount", "paid"),
    "daily cap": ("cap", "dailyCap", "paidCap"),
    "rung": ("rung", "activeRung"),
}

_S = None


def _status(ip: str) -> dict:
    r = _S.get(f"http://{ip}/api/fold/status", timeout=10)
    assert r.status_code == 200, f"GET /api/fold/status: HTTP {r.status_code} (no Summarize now on this image)"
    return r.json()


def _field(st: dict, concept: str):
    for name in STATUS_FIELDS[concept]:
        if name in st:
            return st[name]
    return None


def _confirmations(ip: str, since_id: str) -> list:
    rows = _S.get(f"http://{ip}/api/mem/episodic", params={"session": "web", "limit": 40}, timeout=10).json()
    return [
        m.get("text", "")
        for m in rows.get("messages", [])
        if m.get("role") == "assistant" and m.get("id", "") > since_id and m.get("text", "").startswith(CONFIRM)
    ]


def _newest_id(ip: str) -> str:
    rows = _S.get(f"http://{ip}/api/mem/episodic", params={"session": "web", "limit": 5}, timeout=10).json()
    return max((m.get("id", "") for m in rows.get("messages", [])), default="")


@pytest.fixture(scope="module")
def board():
    global _S
    ip, tok = ip_tok_or_skip("FOLDNOW")
    _S = make_session(tok)
    require_orchestrator(_S, ip, "FOLDNOW")
    return ip, tok


def test_summarize_now_runs_reports_and_confirms_once(board):
    ip, tok = board
    for i, text in enumerate(
        (f"{RUN}: my cat is called Pebble.", f"{RUN}: I water the ferns on Saturdays.", f"{RUN}: reply with OK.")
    ):
        assert chat_turn(_S, ip, text, timeout_s=150), f"seed turn {i + 1} got no reply"
    before = _status(ip)
    missing = [c for c in STATUS_FIELDS if _field(before, c) is None]
    assert not missing, f"/api/fold/status lacks {missing}: {sorted(before)}"
    since = _newest_id(ip)

    import requests

    refused = requests.post(f"http://{ip}/api/fold/run", timeout=10)
    assert refused.status_code in (401, 403), f"an unauthenticated Summarize now was not refused: {refused.status_code}"
    r = _S.post(f"http://{ip}/api/fold/run", timeout=10)
    assert r.status_code == 202, f"POST /api/fold/run: HTTP {r.status_code}"

    deadline = time.time() + FOLD_WAIT_S
    after = before
    while time.time() < deadline:
        time.sleep(10)
        after = _status(ip)
        if _field(after, "last success") != _field(before, "last success"):
            break
    assert _field(after, "last success") != _field(before, "last success"), (
        f"no new fold success within {FOLD_WAIT_S} s: {after}"
    )
    age = _field(after, "summary age")
    assert age is not None and float(age) < 24, f"the summary is not fresh after Summarize now: {age}"
    time.sleep(10)  # a second confirmation would land in this window
    confirms = _confirmations(ip, since)
    assert len(confirms) == 1, f"expected one confirmation, got {confirms}"
    assert "Summary updated: covered" in confirms[0], f"the confirmation does not say what it covered: {confirms[0]!r}"


def test_compact_command_maps_to_the_same_catch_up(board):
    ip, tok = board
    for text in (f"{RUN}: the spare key is with the neighbour.", f"{RUN}: reply with OK."):
        assert chat_turn(_S, ip, text, timeout_s=150), "seed turn got no reply"  # new rows to fold
    since = _newest_id(ip)
    reply = chat_turn(_S, ip, "/compact", timeout_s=60)
    assert "Compacting" in reply, f"unexpected /compact reply: {reply[:120]!r}"
    deadline = time.time() + FOLD_WAIT_S
    while time.time() < deadline and not _confirmations(ip, since):
        time.sleep(10)
    assert len(_confirmations(ip, since)) == 1, "no single confirmation after /compact"
