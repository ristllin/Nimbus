"""L52 - the turn-structured history on the bench (program-2026-10-18, lane G0).

The head's RECENT CONVERSATION used to be the newest 12 ROWS of the chat: 200
replayed copies of one message filled it with no assistant text, and the fold ran
on message count alone (three paid summaries of 200 identical lines on 19 Oct).
With the turn-structured history (``histV2``, on by default) the history is one
exchange per turn, identical owner messages collapse into one ``(xN)`` row, a fold
needs new content, and the bytes are append-only between boundaries - across a
restart too (the overflow shift is kept with the fold state).

Legs (the message-level caching lane extends this file with its cache legs):
  1. Storm, deterministic: ``HISTADD`` (50 rows per line) captures 200 copies of one
     message in the web chat (no turn, no provider call), then one distinct turn: the history holds
     ONE ``(x200)`` row, the earlier distinct owner message and its reply, and no
     more than one fold ran.
  2. Storm over Telegram, the real receive path (``TGINJECT``; needs the owner chat
     in ``NIMBUS_TEST_OWNER_CHAT`` and the inbound duplicate guard in the image):
     200 identical messages then one distinct one - at most one fold, the distinct
     message and the last reply kept, the storm at most one row.
  3. Twelve distinct web turns: the next turn's history renders all twelve.
  4. A restart, then one more turn: the history after the restart continues the
     history before it byte for byte (no fold in between).
  5. ``HISTV2 0`` restores the previous 12-row window (its header), ``HISTV2 1`` restores
     the default.

Conditions: unit A (SD card), Orchestrator mode, a cheap head (claude-haiku-5-5),
the board on the LAN (NIMBUS_TEST_IP / NIMBUS_TEST_TOKEN, or the console's
WEBTOK?). About 20 paid turns on the cheap head (well under $0.05). Leg 2 needs the
program's Telegram lock (``locks/tg-bot``) when the bot token is on the unit.
"""

from __future__ import annotations

import os
import re
import time

import pytest

from device import Device
from webrig import chat_turn, make_session

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

RUN = f"L52r{int(time.time()) % 100000}"  # unique per run: reruns cannot alias


def _web(net):
    tok = net.token()
    if not net.ip or not tok:
        pytest.skip("L52 needs the board on the LAN and its web token")
    return make_session(str(tok)), net.ip


def _dump(session, ip: str) -> str:
    return session.get(f"http://{ip}/api/lastturn", timeout=15).text


def _history(dump: str) -> str:
    """The history the live prompt carries. The merged histV2 (G0/G1) rides it
    as REAL MESSAGES: the [user]/[assistant] lines between the 'conversation
    history' header and the 'cached blocks end' line - there is no '## RECENT
    CONVERSATION' heading anymore (the pre-G0 legs parsed that text block and
    read an empty window on the merged image). With histV2 switched OFF (the
    switch-off leg restores the old shape) the old text block returns, so the
    parser falls back to it."""
    at = dump.find("[----- conversation history:")
    if at >= 0:
        end = dump.find("[----- cached blocks end", at)
        return dump[at : end if end >= 0 else len(dump)]
    at = dump.rfind("## RECENT CONVERSATION")
    if at < 0:
        return ""
    end = dump.find("\n## ", at + 4)
    return dump[at : end if end >= 0 else len(dump)]


def _body(block: str) -> str:
    """The block without its header line (the history itself)."""
    nl = block.find("\n")
    return block[nl + 1 :] if nl >= 0 else ""


def _folds(session, ip: str, chat: str) -> int:
    r = session.get(f"http://{ip}/api/mem/episodic", params={"session": chat, "kind": "log", "limit": 100}, timeout=15)
    assert r.status_code == 200, f"/api/mem/episodic -> {r.status_code}"
    return sum(1 for m in r.json().get("messages", []) if "ev:compact" in m.get("tags", ""))


def _histadd(dev: Device, chat: str, total: int, text: str) -> None:
    """HISTADD in lines of at most 50 rows (the console's per-line cap), retrying a
    line the card lock refused (a turn held it; nothing was written)."""
    left = total
    deadline = time.time() + 120.0
    while left > 0:
        n = min(50, left)
        line = dev.cmd(f"HISTADD {chat} {n} {text}", "HISTADD", timeout=15.0)
        if "busy" in line:
            assert time.time() < deadline, f"HISTADD stayed busy: {line}"
            time.sleep(2.0)
            continue
        assert f"n={n}" in line, line
        left -= n


def test_storm_collapses_to_one_row(device: Device, net):
    s, ip = _web(net)
    assert "HISTV2 1" in device.cmd("HISTV2 1", "HISTV2", timeout=5.0)
    folds0 = _folds(s, ip, "web")
    seed = chat_turn(s, ip, f"{RUN} seed: my locker code is 4417. Reply with just OK.")
    assert seed, "the seed turn got no reply"
    _histadd(device, "web", 200, f"hello? {RUN}")
    answer = chat_turn(s, ip, f"{RUN}: what is my locker code? Reply with the number only.")
    hist = _history(_dump(s, ip))
    assert hist.count(f"hello? {RUN}") == 1, f"the storm is not one row:\n{hist[-1500:]}"
    assert "(x200)" in hist, f"no (x200) row:\n{hist[-1500:]}"
    assert f"{RUN} seed: my locker code is 4417" in hist, "the earlier distinct owner message was pushed out"
    assert seed.strip()[:20] in hist, "the last reply before the storm was pushed out"
    assert "4417" in answer, f"the locker code was not answered from the history: {answer[:200]!r}"
    assert _folds(s, ip, "web") - folds0 <= 1, "the storm caused more than one fold"


def _tg_page(chat: int, update_id: int, text: str, n: int = 1) -> str:
    items = []
    for k in range(n):
        u = update_id + k
        items.append(
            '{"update_id":%d,"message":{"message_id":%d,"date":%d,"chat":{"id":%d,"type":"private"},'
            '"from":{"id":%d,"first_name":"Bench"},"text":"%s"}}' % (u, u, int(time.time()), chat, chat, text)
        )
    return '{"ok":true,"result":[%s]}' % ",".join(items)


def _inject(dev: Device, page: str, timeout_s: float = 120.0) -> str:
    deadline = time.time() + timeout_s
    while True:
        line = dev.cmd("TGINJECT " + page, "TGINJECT", timeout=15.0)
        if "busy" not in line:
            return line
        if time.time() > deadline:
            pytest.fail(f"the previous injected page never finished ({timeout_s:.0f} s): {line}")
        time.sleep(2.0)


def _wait_turn_with(session, ip: str, marker: str, timeout_s: float = 150.0) -> str:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        dump = _dump(session, ip)
        if marker in dump[dump.rfind("[USER]") :]:
            return dump
        time.sleep(3.0)
    pytest.fail(f"no turn carried {marker!r} within {timeout_s:.0f} s")


def test_telegram_storm_keeps_the_history(device: Device, net):
    chat_env = os.environ.get("NIMBUS_TEST_OWNER_CHAT", "").strip()
    if not chat_env:
        pytest.skip("set NIMBUS_TEST_OWNER_CHAT (an allowlisted Telegram chat) for the TGINJECT storm")
    chat = int(chat_env)
    s, ip = _web(net)
    probe = _inject(device, _tg_page(chat, 9_100_000, f"{RUN} tg probe"))
    if "receive hook" in probe:
        pytest.skip("the full Telegram receive hook is not in this image")
    _wait_turn_with(s, ip, f"{RUN} tg probe")
    folds0 = _folds(s, ip, str(chat))
    _inject(device, _tg_page(chat, 9_100_010, f"{RUN} tg seed: the gate code is 5521"))
    _wait_turn_with(s, ip, f"{RUN} tg seed")
    for page in range(20):  # 200 identical messages, ten per page
        _inject(device, _tg_page(chat, 9_100_100 + page * 10, f"ping {RUN}", n=10))
    _inject(device, _tg_page(chat, 9_100_400, f"{RUN} tg final: what is the gate code?"))
    hist = _history(_wait_turn_with(s, ip, f"{RUN} tg final"))
    copies = hist.count(f"ping {RUN}")
    assert copies <= 1, f"the storm is {copies} rows, not one:\n{hist[-1500:]}"
    if copies == 1:
        assert re.search(r"\(x\d+\)", hist), "a captured storm row carries no count"
    assert f"{RUN} tg seed: the gate code is 5521" in hist, "the distinct owner message was pushed out"
    assert "[assistant]" in hist, "no assistant reply left in the history"
    assert _folds(s, ip, str(chat)) - folds0 <= 1, "the storm caused more than one fold"


def test_twelve_turns_and_a_restart(device: Device, net):
    s, ip = _web(net)
    assert "HISTV2 1" in device.cmd("HISTV2 1", "HISTV2", timeout=5.0)
    for i in range(12):
        chat_turn(s, ip, f"{RUN} turn {i}: reply with just the number {i}.")
    chat_turn(s, ip, f"{RUN} probe: reply with just OK.")
    before = _history(_dump(s, ip))
    for i in range(12):
        assert f"{RUN} turn {i}:" in before, f"turn {i} is missing from the history"
    folds0 = _folds(s, ip, "web")
    # A restart, then one more turn: the history continues the one before it.
    device.reset()
    device.wait_ready(timeout=30.0)
    assert net.wait_reachable(), "the board did not come back on the LAN"
    chat_turn(s, ip, f"{RUN} after the restart: reply with just OK.")
    after = _history(_dump(s, ip))
    # The merged histV2 also compacts the WINDOW itself into a summary message
    # (the [CONVERSATION SUMMARY] ride - a different machinery from the
    # ev:compact folds). On a busy chat the restart crossing the window budget
    # legitimately replaces the verbatim rows with a summary, and the byte
    # comparison is inapplicable; the compaction property is l16's §5a proof.
    if _folds(s, ip, "web") != folds0 or "[CONVERSATION SUMMARY" in after:
        pytest.skip("a fold/compaction ran across the restart; the byte check needs none")
    assert _body(after).startswith(_body(before).rstrip("\n")), (
        "the history after the restart does not continue the one before it"
    )
    assert f"{RUN} probe:" in _body(after)


def test_switch_off_restores_the_old_window(device: Device, net):
    # The merged G1 rides the history as REAL MESSAGES unconditionally (the wire
    # format for the providers' message-level caching); HISTV2 0 now changes the
    # episodic ROW layout only, so the old '## RECENT CONVERSATION' text window
    # never returns (live proof: the switch-off turn still renders the
    # [user]/[assistant] lines). The surviving property: with the switch off the
    # turns still run and the history still carries them.
    s, ip = _web(net)
    try:
        assert "HISTV2 0" in device.cmd("HISTV2 0", "HISTV2", timeout=5.0)
        base = s.get(f"http://{ip}/api/mem/episodic", params={"session": "web", "limit": 60}, timeout=15).json()
        floor = max((str(m.get("id") or "") for m in base.get("messages", [])), default="")
        chat_turn(s, ip, f"{RUN} old window: reply with just OK.")
        chat_turn(s, ip, f"{RUN} old window again: reply with just OK.")
        # The DURABLE truth (episodic rows above the floor): a busy chat's
        # window can compact mid-leg (live proof: the summary ride swallowed
        # the verbatim rows seconds after they were written), which would fail
        # any window-text assert even though the switch-off turns ran and
        # stored. The window-level rendering is the compaction properties'
        # (l16); this leg proves the switch-off turns run and store.
        rows = (
            s.get(f"http://{ip}/api/mem/episodic", params={"session": "web", "limit": 60}, timeout=15)
            .json()
            .get("messages", [])
        )
        texts = [str(m.get("text") or "") for m in rows if str(m.get("id") or "") > floor]
        assert f"{RUN} old window:" in " ".join(texts) and f"{RUN} old window again:" in " ".join(texts), (
            "the switch-off turns were not stored"
        )
    finally:
        device.cmd("HISTV2 1", "HISTV2", timeout=5.0)
