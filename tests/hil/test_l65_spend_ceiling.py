"""L65 - the monthly spend ceiling on a real board (lane DEF, owner requirement 21 Oct).

The owner's defensive default: a monthly spend ceiling across ALL providers (default
20 USD). At the ceiling every paid seam refuses before any request, the owner hears it
once per channel, the state survives a restart and a full settings store, raising the
ceiling lifts it at once and the month rollover lifts it with no notice.

No real spend: the head's API host answers from the test console (``PROVFAULT
api.anthropic.com 200 <a canned answer with a big usage>``, a fake provider that keeps
answering), and ``SPENDSET`` seeds this month's count just under the ceiling, so the
next turn crosses it. ``SPENDSET?`` prints the state and the paid-call gate's consults
and refusals per seam; "zero requests" is read from the console: a request the gate
refused prints ``refused - spend ceiling``, one that reached the fake prints ``TEST
override answered``.

Legs (unit L, the TEST build, in order; each reads the state the previous one left):
  1. the crossing: SPENDSET just under, one fake turn, frozen; one notice in the web chat
  2. every seam refuses with zero requests: a serial turn, a web send (409 at once), a
     spawn, image generation, a voice note (AUDIOINJECT, if the clip is on the card),
     a routine (every 5 minutes), a one-time wakeup and the DREAM drill
  3. Telegram: one notice in the owner chat (needs the bench bot; takes locks/tg-bot)
  4. a restart keeps the freeze and sends no second notice
  5. a full settings store (NVSFILL until full): the count still advances, the freeze holds
  6. raising the ceiling over /api/orch lets the next turn run
  7. CLOCK across the month boundary clears the freeze once, with no notice
  8. (after the orchestrator's OTA from the v4.5.14 image, NIMBUS_L65_AFTER_OTA=1) one
     first-boot notice about the ceiling, and not again after a restart

Markers: ``hil`` + ``net``. Needs --allow-hardware, NIMBUS_TEST_IP and NIMBUS_TEST_TOKEN
for the same board, an Anthropic head (the fake answers on api.anthropic.com).
"""

from __future__ import annotations

import calendar
import json
import os
import re
import time

import pytest

from device import ExpectTimeout
from webrig import ip_tok_or_skip, make_session, require_orchestrator

pytestmark = [pytest.mark.hil, pytest.mark.net]

# One fake round: the terminal orch_turn, billed 200K input tokens (about $0.10 on
# claude-haiku-5-5's long-context rate): enough to cross a count seeded 10 cents short.
FAKE_TURN = (
    '{"content":[{"type":"tool_use","id":"tu_1","name":"orch_turn","input":{"reply":"ok"}}],'
    '"stop_reason":"tool_use","usage":{"input_tokens":200000,"output_tokens":10}}'
)
FREEZE_WORDS = "Paid AI calls are paused"
MIGRATION_WORDS = "New in this update: a monthly spend ceiling"
LIMIT_CENTS = 2000

_S = None
_IP = ""


def _spend(device) -> dict:
    """SPENDSET? -> {"state": {...}, "gate": {seam: (consults, refusals)}}."""
    device.drain(quiet=0.3)
    device.send("SPENDSET?")
    state = json.loads(device.expect_re(r"SPENDSET (\{.*\})", timeout=8.0).group(1))
    gate_line = device.expect("SPENDGATE", timeout=5.0)
    gate = {m.group(1): (int(m.group(2)), int(m.group(3))) for m in re.finditer(r"(\S+)=(\d+)/(\d+)", gate_line)}
    return {"state": state, "gate": gate}


def _console(device, line: str, expect: str, timeout: float = 8.0) -> str:
    return device.cmd(line, expect, timeout=timeout)


def _lines(device, seconds: float) -> str:
    out, end = [], time.time() + seconds
    while time.time() < end:
        try:
            out.append(device.expect_re(r".", timeout=max(0.2, end - time.time())).string)
        except ExpectTimeout:
            break
    return "\n".join(out)


def _web_turn(text: str, timeout_s: int = 120):
    """One web chat turn to its own answer: (status, reply)."""
    r = _S.post(f"http://{_IP}/api/chat", data={"text": text}, timeout=10)
    if r.status_code != 200:
        return r.status_code, r.text
    turn = str(r.json().get("turn") or "")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        j = _S.get(f"http://{_IP}/api/chat", params={"turn": turn}, timeout=10).json()
        if j.get("done"):
            return 200, str(j.get("reply") or "")
        time.sleep(2.0)
    pytest.fail(f"turn {turn} was not answered within {timeout_s}s")


def _web_notices(words: str) -> int:
    """How many web chat history rows carry `words` (the page renders this history)."""
    r = _S.get(f"http://{_IP}/api/mem/episodic", params={"session": "web", "limit": 200}, timeout=15)
    assert r.status_code == 200, f"/api/mem/episodic -> {r.status_code}"
    return r.text.count(words)


def _mcp(tool: str, args: dict) -> str:
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    r = _S.post(f"http://{_IP}/mcp", data=body, headers={"Content-Type": "application/json"}, timeout=20)
    assert r.status_code == 200, f"/mcp {tool} -> {r.status_code}: {r.text[:160]}"
    return " ".join(c.get("text", "") for c in r.json().get("result", {}).get("content", []))


@pytest.fixture(scope="module")
def board(device):
    global _S, _IP
    _IP, tok = ip_tok_or_skip("L65")
    _S = make_session(tok)
    require_orchestrator(_S, _IP, "L65")
    try:
        _spend(device)
    except ExpectTimeout:
        pytest.skip("test console has no SPENDSET verb (flash the [env:test] build of this lane)")
    yield device
    for line, expect in (
        ("PROVFAULT off", "PROVFAULT"),
        ("NVSFILL 0", "NVSFILL"),
        (f"SPENDSET limit {LIMIT_CENTS}", "SPENDSET"),
        ("SPENDSET 0", "SPENDSET"),
    ):
        try:
            _console(device, line, expect, timeout=20.0)
        except Exception:  # noqa: BLE001 - restore best-effort; the leg's last state is logged
            pass


def test_1_the_crossing_freezes_and_tells_the_web_chat_once(board):
    device = board
    before = _web_notices(FREEZE_WORDS)
    _console(device, f"SPENDSET limit {LIMIT_CENTS}", "SPENDSET")
    _console(device, f"SPENDSET {LIMIT_CENTS - 10}", "SPENDSET")
    assert _spend(device)["state"]["frozen"] is False
    _console(device, f"PROVFAULT api.anthropic.com 200 {FAKE_TURN}", "PROVFAULT")
    status, reply = _web_turn("Say ok.")
    assert status == 200 and reply, f"the crossing turn did not answer: {status} {reply[:120]}"
    st = _spend(device)["state"]
    assert st["frozen"] is True, f"the turn that crossed the ceiling did not freeze it: {st}"
    assert st["spentCents"] >= LIMIT_CENTS
    time.sleep(12.0)  # the notice pump runs every ~5 s (the Telegram poll task)
    assert _web_notices(FREEZE_WORDS) == before + 1, "the web chat must get exactly one freeze notice"


def _loops() -> list:
    r = _S.get(f"http://{_IP}/api/loops", timeout=10).json()
    return r.get("loops", r) if isinstance(r, dict) else r


def test_2_every_seam_refuses_with_zero_requests(board):
    """Run on an image that has lane K2's park (the photo caption and voice seams answer
    "paused" through it); every other seam is refused by this lane's gate."""
    device = board
    assert _spend(device)["state"]["frozen"] is True, "leg 1 must leave the board frozen"
    gate0 = _spend(device)["gate"]
    device.drain(quiet=0.3)
    # A head turn over serial (the turn core refuses before recall's embed).
    device.send("TURN what is two plus two")
    log = _lines(device, 15.0)
    assert "turn refused - spend ceiling" in log, log[-600:]
    # A web send: answered at once (409), never a message waiting on a reply.
    r = _S.post(f"http://{_IP}/api/chat", data={"text": "hello"}, timeout=10)
    assert r.status_code == 409 and "spend_ceiling" in r.text, f"{r.status_code} {r.text[:200]}"
    # A spawn (the dispatch is refused at the wire before it leaves).
    _mcp("session.spawn", {"task": "list three fruits", "provider": "anthropic", "name": "l65"})
    log += _lines(device, 20.0)
    # Image generation.
    img = _mcp("image.generate", {"prompt": "a red circle"})
    assert "spend ceiling" in img, img[:300]
    # A routine (the owner's, every 5 minutes) and a one-time wakeup (2 minutes): each
    # fire is deferred (not a fire, no request). Wake-ups are set to run without asking
    # for the leg and the owner's mode is put back.
    mode = _S.get(f"http://{_IP}/api/wakeups", timeout=10).json().get("approvalMode", "ask")
    _S.post(f"http://{_IP}/api/wakeups", data={"action": "mode", "value": "auto"}, timeout=10)
    sched = json.dumps({"kind": "interval", "every_seconds": 300})
    _S.post(
        f"http://{_IP}/api/loops",
        data={"action": "create", "name": "l65-routine", "prompt": "Say ok.", "schedule": sched},
        timeout=10,
    )
    wk = _mcp("wakeup.set", {"minutes": 2, "note": "l65 wakeup"})
    try:
        # The DREAM drill (the nightly dream's scheduled path).
        device.send("DREAM")
        log += _lines(device, 30.0)
        assert "scheduled dream refused" in log, log[-800:]
        # A voice note, when the bench clip is on the card (L57's prerequisite).
        if "ERR" not in _console(device, "FSTAT /voice_bench.wav", "FSTAT", timeout=10.0):
            device.send("AUDIOINJECT /voice_bench.wav")
            m = device.expect_re(r"AUDIOINJECT \S+ pcm=\d+ stt_ms=(\d+) outcome=(\S+)", timeout=60.0)
            _console(device, "AUDIOINJECT off", "AUDIOINJECT off")
            assert int(m.group(1)) == 0, "speech to text must not start while paid calls are paused"
        log += _lines(device, 330.0)  # the wakeup (2 min) and the routine (5 min) fire here
    finally:
        for lp in _loops():
            if isinstance(lp, dict) and lp.get("name") == "l65-routine":
                _S.post(f"http://{_IP}/api/loops", data={"action": "delete", "id": lp.get("id", "")}, timeout=10)
        _S.post(f"http://{_IP}/api/wakeups", data={"action": "mode", "value": mode}, timeout=10)
    assert "scheduled wakeup refused" in log, f"the wakeup was not refused: {wk[:120]}"
    assert "scheduled routine refused" in log, "the routine fire was not refused"
    assert "TEST override answered" not in log, "a request reached the provider while frozen"
    gate1 = _spend(device)["gate"]
    for seam in ("head", "image-gen", "dream", "wakeup", "routine"):
        assert gate1[seam][1] > gate0.get(seam, (0, 0))[1], f"the {seam} seam was not refused: {gate1}"


def test_3_telegram_gets_exactly_one_notice(board, secrets):
    """The owner chat's notice (the bench bot; run under locks/tg-bot)."""
    token, chat = getattr(secrets, "tg_test_token", ""), getattr(secrets, "tg_test_chat", "")
    if not token or not chat:
        pytest.skip("no bench Telegram bot (NIMBUS_TG_TEST_TOKEN / NIMBUS_TG_TEST_CHAT)")
    day = time.strftime("%Y-%m-%d", time.gmtime())

    def notices() -> int:
        page = _S.get(f"http://{_IP}/api/tg/archive", params={"chat": str(chat), "day": day, "limit": 200}, timeout=15)
        return sum(1 for row in page.json().get("rows", []) if row.get("dir") == "out" and FREEZE_WORDS in str(row))

    r = _S.post(f"http://{_IP}/api/orch", data={"tgToken": str(token), "tgAllow": str(chat)}, timeout=10)
    assert r.status_code == 200
    try:
        deadline, n = time.time() + 60.0, 0
        while time.time() < deadline:
            n = notices()
            if n:
                break
            time.sleep(5.0)
        assert n == 1, f"the owner chat must get exactly one freeze notice, got {n}"
        time.sleep(20.0)
        assert notices() == 1, "a second freeze notice went to the owner chat"
    finally:
        _S.post(f"http://{_IP}/api/orch", data={"clr_tgToken": "1"}, timeout=10)


def test_4_a_restart_keeps_the_freeze_and_sends_nothing_new(board):
    device = board
    before = _web_notices(FREEZE_WORDS)
    device.reboot_and_confirm()
    st = _spend(device)["state"]
    assert st["frozen"] is True, f"the freeze did not survive the restart: {st}"
    time.sleep(15.0)
    assert _web_notices(FREEZE_WORDS) == before, "a restart must not send the notice again"


def test_5_a_full_settings_store_changes_nothing(board):
    device = board
    fill = _console(device, "NVSFILL 4000", "NVSFILL", timeout=30.0)
    assert "full=1" in fill, f"NVSFILL did not fill the settings store: {fill}"
    try:
        stored = _console(device, "SPENDSET limit 100000", "SPENDSET limit")
        assert "NOT" not in stored, f"the ceiling lives on flash, not in settings: {stored}"
        spent0 = _spend(device)["state"]["spentCents"]
        status, reply = _web_turn("Say ok.")
        assert status == 200 and reply
        spent1 = _spend(device)["state"]["spentCents"]
        assert spent1 > spent0, f"the count did not advance with a full settings store: {spent0} -> {spent1}"
        _console(device, f"SPENDSET limit {LIMIT_CENTS}", "SPENDSET limit")
        device.reboot_and_confirm()
        st = _spend(device)["state"]
        assert st["frozen"] is True and st["spentCents"] >= spent1, f"lost across a restart: {st}"
    finally:
        _console(device, "NVSFILL 0", "NVSFILL", timeout=30.0)


def test_6_raising_the_ceiling_over_api_orch_lets_the_next_turn_run(board):
    device = board
    assert _spend(device)["state"]["frozen"] is True
    r = _S.post(f"http://{_IP}/api/orch", data={"spendCeil": "100000"}, timeout=10)
    assert r.status_code == 200, f"{r.status_code} {r.text[:200]}"
    st = _spend(device)["state"]
    assert st["frozen"] is False and st["limitCents"] == 100000 and st["ownerSet"] is True
    status, reply = _web_turn("Say ok.")
    assert status == 200 and reply, "the turn after raising the ceiling did not run"


def test_7_the_month_rollover_clears_the_freeze_quietly(board):
    device = board
    _console(device, f"SPENDSET limit {LIMIT_CENTS}", "SPENDSET limit")
    _console(device, f"SPENDSET {LIMIT_CENTS + 500}", "SPENDSET")
    time.sleep(12.0)  # this episode's notice goes out
    before = _web_notices(FREEZE_WORDS)
    st = _spend(device)["state"]
    assert st["frozen"] is True
    month = int(st["month"])
    y, m = divmod(month, 100)
    y, m = (y + 1, 1) if m == 12 else (y, m + 1)
    # The 2nd at 00:00:05 UTC: the next month in any zone the bench unit may be set to.
    second = calendar.timegm((y, m, 2, 0, 0, 5, 0, 0, 0))
    _console(device, f"CLOCK {second}", "CLOCK")
    time.sleep(75.0)  # the month rolls once a minute (the Telegram poll task)
    st = _spend(device)["state"]
    assert st["frozen"] is False and st["spentCents"] == 0, f"the rollover did not clear the freeze: {st}"
    assert _web_notices(FREEZE_WORDS) == before, "the rollover must send no notice"


def test_8_first_boot_after_the_update_tells_the_owner_once(board):
    """After the orchestrator's OTA from the v4.5.14 image (no ceiling file yet)."""
    if os.environ.get("NIMBUS_L65_AFTER_OTA") != "1":
        pytest.skip("run right after the OTA from v4.5.14 with NIMBUS_L65_AFTER_OTA=1")
    device = board
    time.sleep(15.0)
    assert _web_notices(MIGRATION_WORDS) == 1, "one first-boot notice about the ceiling"
    device.reboot_and_confirm()
    time.sleep(15.0)
    assert _web_notices(MIGRATION_WORDS) == 1, "the first-boot notice must not repeat"
