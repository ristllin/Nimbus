"""Summarize now - the owner starts a fold from the web and watches it land (v5.1.2).

Bench row B08, on hardware. Lane F adds POST /api/fold/run (owner-authenticated: the
device sign-in code plus the web-task owner check; it stages a manual catch-up and answers
202) and GET /api/fold/status (summary age, thru id and date, the uncovered span, the last
success, failed cycles, the next try, paid fold calls today and the daily cap, and the
active rung i/n), registered through the deferred route registry; /api/state is unchanged.

The leg seeds the web chat with three turns, asks for a summary of the web chat through the
route (chat=web: with no chat the route folds the owner's Telegram chat when one is set),
and waits for it the way the owner does: the status moves (a new lastOk, state "fresh") and
the web chat gets exactly one confirmation, an assistant row tagged ev:foldnote that starts
with the existing "✓ Compacted." and names what it covered ("Summary updated: covered N
messages from D1 to D2."). A request without the device sign-in code is refused (401) and a
chat the device does not know is 404. Up to v5.1.1 there is no route: the leg fails there,
as it must. A second manual request inside the two-minute cooldown waits for it, so the
/compact leg's confirmation can take the cooldown plus the fold.

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
CHAT = "web"
# GET /api/fold/status as lane F shipped it (src/net/fold_routes.cpp; never null).
STATUS_FIELDS = (
    "summaryAgeH", "lastOk", "lastOkDate", "thru", "thruDate", "uncovered", "failedCycles", "firstFail", "nextTry",
    "nextTryInS", "paidToday", "cap", "running", "manual", "manualBusy", "rung", "queued", "cooldownS", "state",
    "canRun", "why",
)  # fmt: skip

_S = None


def _status(ip: str) -> dict:
    r = _S.get(f"http://{ip}/api/fold/status", params={"chat": CHAT}, timeout=10)
    assert r.status_code == 200, f"GET /api/fold/status: HTTP {r.status_code} (no Summarize now on this image)"
    return r.json()


def _confirmations(ip: str, since_id: str) -> list:
    rows = _S.get(f"http://{ip}/api/mem/episodic", params={"session": "web", "limit": 40}, timeout=10).json()
    return [
        m.get("text", "")
        for m in rows.get("messages", [])
        if m.get("role") == "assistant"
        and m.get("id", "") > since_id
        and m.get("text", "").startswith(CONFIRM)
        and "ev:foldnote" in m.get("tags", "")
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
    missing = [f for f in STATUS_FIELDS if before.get(f) is None]
    assert not missing, f"/api/fold/status lacks {missing}: {sorted(before)}"
    since = _newest_id(ip)

    import requests

    for path in ("/api/fold/run", "/api/fold/status"):
        send = requests.post if path.endswith("run") else requests.get
        refused = send(f"http://{ip}{path}", timeout=10)
        assert refused.status_code == 401, f"{path} without the sign-in code: HTTP {refused.status_code}"
    unknown = _S.post(f"http://{ip}/api/fold/run", data={"chat": f"nobody-{RUN}"}, timeout=10)
    assert unknown.status_code == 404, f"a chat the device does not know: HTTP {unknown.status_code}"
    r = _S.post(f"http://{ip}/api/fold/run", data={"chat": CHAT}, timeout=10)
    assert r.status_code == 202, f"POST /api/fold/run: HTTP {r.status_code}"

    deadline = time.time() + FOLD_WAIT_S
    after = before
    while time.time() < deadline:
        time.sleep(10)
        after = _status(ip)
        if after["lastOk"] != before["lastOk"]:
            break
    assert after["lastOk"] != before["lastOk"], f"no new fold success within {FOLD_WAIT_S} s: {after}"
    assert after["state"] == "fresh" and 0 <= float(after["summaryAgeH"]) < 24, (
        f"the summary is not fresh after Summarize now: state {after['state']}, age {after['summaryAgeH']} h"
    )
    assert int(after["failedCycles"]) == 0 and after["thru"], f"a landed fold left {after}"
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
