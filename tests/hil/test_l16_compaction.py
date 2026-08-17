"""§L16 - context compaction (the v3.6.0 fold) E2E on real hardware.

The scripted version of the hand-run Board 2 smoke that first proved the cycle
(2026-07-27): plant a marker fact → manual /compact via the web chat → assert the
ev:compact boundary row → filler turns push the marker OUT of the 12-row verbatim
window → the recall question must still be answered - and /api/lastturn must show
the §5a "## CONVERSATION SUMMARY" block carrying the marker while the RECENT
CONVERSATION window does not (the vacuous-test fix from the prism plan review:
without the older-than-window marker, deleting §5a entirely would still pass).

Cost note: the full cycle runs ~10 real LLM turns on the device's configured
provider - marked ``agent`` (deselected by default) like the other paid suites.
Everything it creates lives in the device's own web-chat history (episodic rows
are retention-pruned; no cleanup path exists by design - the fold is idempotent).

Markers: ``net`` + ``agent`` + ``hil``; needs ``--allow-hardware``, a LAN-reachable
Orchestrator-mode board with a provider key, and the serial token (or
``NIMBUS_TEST_TOKEN``).
"""

from __future__ import annotations

import os
import time

import pytest

try:
    import requests
except ImportError:  # pragma: no cover
    requests = None

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

MARKER = f"Quokka-{int(time.time()) % 100000}"  # unique per run - reruns can't alias


def _api(ip: str, tok: str, path: str, **kw):
    sep = "&" if "?" in path else "?"
    return f"http://{ip}{path}{sep}t={tok}", kw


def _chat(ip: str, tok: str, text: str, timeout_s: int = 90) -> str:
    """Send one web-chat message and poll its reply (the single-slot pairing)."""
    url, _ = _api(ip, tok, "/api/chat")
    r = requests.post(url, data={"text": text}, timeout=10)
    assert r.status_code == 200, f"chat send {r.status_code}: {r.text[:200]}"
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        time.sleep(3)
        j = requests.get(url, timeout=10).json()
        if not j.get("pending", True) and j.get("reply"):
            return j["reply"]
    pytest.fail(f"no chat reply within {timeout_s}s for: {text[:60]}")


def _episodic(ip: str, tok: str, **params) -> list:
    url, _ = _api(ip, tok, "/api/mem/episodic")
    r = requests.get(url, params=params, timeout=10)
    assert r.status_code == 200
    return r.json().get("messages", [])


@pytest.fixture(scope="module")
def board():
    if requests is None:
        pytest.skip("requests not installed")
    # Env-driven (the serial-open reset races WiFi rejoin - reuse a joined board):
    #   NIMBUS_TEST_IP=<lan ip>  NIMBUS_TEST_TOKEN=<webtok>
    ip = os.environ.get("NIMBUS_TEST_IP")
    tok = os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        pytest.skip("set NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN (WEBTOK?) to run L16")
    # Orchestrator mode + a keyed provider are prerequisites - skip LOUDLY.
    st = requests.get(f"http://{ip}/api/state?t={tok}", timeout=10).json()
    if st.get("mode") != 1:
        pytest.skip("board is not in Orchestrator mode - flash/MODE 1 first")
    return ip, tok


def test_fold_cycle_end_to_end(board):
    """Marker → /compact → boundary row → fillers past the window → §5a recall."""
    ip, tok = board

    # 1. Plant the marker (the model may also mem_write it - fine; the §5a proof
    #    below is prompt-content-based, not answer-based).
    reply = _chat(ip, tok, f"Remember this and reply OK only: my cat is called {MARKER}.")
    assert reply, "marker turn produced no reply"

    # 2. Manual fold via the deterministic owner command (async - poll the row).
    cmd = _chat(ip, tok, "/compact")
    assert "Compacting" in cmd, f"unexpected /compact reply: {cmd[:120]}"
    boundary = None
    deadline = time.time() + 90
    while time.time() < deadline and boundary is None:
        time.sleep(5)
        for m in _episodic(ip, tok, session="web", kind="log", limit=5):
            if "ev:compact" in m.get("tags", "") and "Conversation compacted" in m.get("text", ""):
                boundary = m
                break
    assert boundary, "no ev:compact boundary row within 90s of /compact"

    # 3. Seven fillers -> 14 message rows: the marker turn falls out of the
    #    12-row verbatim window (the prism vacuous-test fix - without this step
    #    the recall question passes even with §5a deleted).
    for i in range(1, 8):
        _chat(ip, tok, f"Reply with just the number {i}.")

    # 4. The recall question - continuity must survive the fold + window slide.
    answer = _chat(ip, tok, "What is my cat called? Reply with just the name.")
    assert MARKER in answer, f"recall failed: {answer[:200]!r}"

    # 5. The §5a proof off the live prompt dump: summary present and carrying the
    #    marker; the verbatim window does NOT contain it.
    url, _ = _api(ip, tok, "/api/lastturn")
    dump = requests.get(url, timeout=15).text
    assert "## CONVERSATION SUMMARY" in dump, "§5a section missing from the live prompt"
    idx_sum = dump.find("## CONVERSATION SUMMARY")
    idx_win = dump.find("## RECENT CONVERSATION")
    assert idx_sum != -1 and idx_win != -1 and idx_sum < idx_win, "§5a not above the window"
    assert MARKER in dump[idx_sum:idx_win], "marker not in the §5a summary"
    # Window slice ends at the NEXT section header, whatever it is - a named
    # bound (SCRATCHPAD) broke when that section was empty/omitted and the slice
    # swallowed RELEVANT MEMORIES, whose recall bullet legitimately carries the
    # marker (first live run of this test caught exactly that).
    idx_next = dump.find("\n## ", idx_win + 4)
    win_end = idx_next if idx_next != -1 else len(dump)
    assert MARKER not in dump[idx_win:win_end], (
        "marker still inside the verbatim window - the test cannot distinguish §5a from the window; add more fillers"
    )


def test_compact_when_nothing_new_is_still_safe(board):
    """A second immediate /compact must not error, spam, or corrupt state."""
    ip, tok = board
    before = _episodic(ip, tok, session="web", kind="log", limit=10)
    cmd = _chat(ip, tok, "/compact")
    assert "Compacting" in cmd
    time.sleep(30)  # give the fold time to run (or decline) on the pump
    # The device must still serve turns afterwards - the real invariant.
    reply = _chat(ip, tok, "Reply with just the word alive.")
    assert "alive" in reply.lower()
    after = _episodic(ip, tok, session="web", kind="log", limit=10)
    assert len(after) >= len(before)  # rows only ever accumulate


def test_fold_failure_is_bounded_and_recovers(board):
    """Field bug 2026-08-11 ("triple message on Compaction then failure"): a
    failing fold episode must be BOUNDED in owner-visible messages and must
    recover once the provider returns.

    Drives the new FAULT provider cap (every LLM host answers "no response",
    Telegram untouched) against a real Telegram chat via the dedicated test bot
    (NIMBUS_TEST_TG_TOKEN/CHAT - never the owner's bot). Asserts, via the test
    bot's getUpdates: a manual staged fold under the fault emits at most ONE
    message for the whole failure episode (the "Couldn't compact" notice - the
    per-retry "Compacting our conversation" pre-notice is gone), and a fold
    after clearing the fault emits exactly the one success confirmation.
    """
    ip, tok = board
    bot = os.environ.get("NIMBUS_TEST_TG_TOKEN")
    chat = os.environ.get("NIMBUS_TEST_TG_CHAT")
    if not bot or not chat:
        pytest.skip(
            "set NIMBUS_TEST_TG_TOKEN + NIMBUS_TEST_TG_CHAT (dedicated "
            "test bot) to run the fold-failure bound - SKIPPED, not proven"
        )

    def tg_updates(offset):
        r = requests.get(
            f"https://api.telegram.org/bot{bot}/getUpdates", params={"offset": offset, "timeout": 0}, timeout=15
        )
        return r.json().get("result", [])

    def msgs_for_chat(updates):
        return [
            u["message"]["text"]
            for u in updates
            if str(u.get("message", {}).get("chat", {}).get("id")) == str(chat) and "text" in u.get("message", {})
        ]

    def fault(on):
        r = requests.post(
            f"http://{ip}/api/fault", data={"t": tok, "cap": "provider", "on": "1" if on else "0"}, timeout=10
        )
        assert r.status_code == 200, "FAULT provider needs an [env:test] build"

    # Seed the test chat with real history BEFORE the fault - an empty chat's
    # manual fold answers "Nothing to compact yet." and never reaches the
    # failure path (the bound would pass vacuously).
    r = requests.post(
        f"http://{ip}/api/test/inject",
        data={"t": tok, "chat": chat, "text": f"Fold-failure seed {MARKER}: reply briefly."},
        timeout=10,
    )
    assert r.status_code == 202
    time.sleep(45)  # let the seed turn complete + deliver while providers work

    # Drain any queued updates (incl. the seed reply) so counts start clean.
    seen = tg_updates(0)
    offset = (seen[-1]["update_id"] + 1) if seen else 0

    try:
        fault(True)
        r = requests.post(f"http://{ip}/api/test/compact", data={"t": tok, "chat": chat}, timeout=10)
        assert r.status_code == 202
        time.sleep(45)  # a few pump passes - retries must stay silent
        updates = tg_updates(offset)
        texts = msgs_for_chat(updates)
        assert len(texts) <= 1, f"failing fold episode spammed: {texts}"
        for t in texts:
            assert "Compacting our conversation" not in t, "pre-notice is back"
        if updates:
            offset = updates[-1]["update_id"] + 1
    finally:
        fault(False)  # never leave the board provider-faulted

    # Recovery: the same staged fold now folds (ladder + healthy providers) and
    # confirms exactly once.
    r = requests.post(f"http://{ip}/api/test/compact", data={"t": tok, "chat": chat}, timeout=10)
    assert r.status_code == 202
    time.sleep(60)
    texts = msgs_for_chat(tg_updates(offset))
    assert texts.count("✓ Compacted.") == 1, f"expected one confirmation, got {texts}"
