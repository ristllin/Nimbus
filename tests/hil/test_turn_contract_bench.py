"""Turn contract bench: spawn honesty, the salvage marker, and markup at the
Telegram seam, on a real board with real provider calls.

The class (program-2026-10-18, lane L): every spawn the head expresses either
starts or is reported, a cut answer reaches the owner with an honest marker and
runs nothing, and no reasoning, tool-call markup or wrapper tag reaches a chat.
The host suites (test_turn_shape, test_reply_markup, test_harness_turn) prove
the class on every generated shape; this file proves it on glass.

Legs (unit L, the fill unit; head and sub-agents on NIMBUS_BENCH_MODEL, default
claude-haiku-5-5):
  1. Six web-chat turns, each asking for three research sub-agents. Each turn's
     ``spawn ledger spawned=N claimed=M`` serial line has N == M, or the text
     delivered for that turn carries the correction line. The delivered text
     carries no markup.
  2. A forced cut: PROVFAULT answers api.anthropic.com with a max_tokens reply
     (every request, until cleared, so the retry with more room is cut too).
     The owner gets the salvage marker or the plain cut line, never raw JSON,
     and the cut text is never delivered as if complete. Skips when the image
     has no PROVFAULT hook.
  3. A markup-tempting prompt injected on the owner's Telegram chat: the device
     logs ``telegram: send ok`` for that chat, and the assistant rows stored for
     it (the text handed to the Telegram sender) pass the markup oracle.

Serial lines are the seam for log facts (the /api/log ring holds about 1 KB,
too little for a turn). Delivered text comes from GET /api/chat?turn=<id>.

Board legs are gated: ``hil`` + ``net`` + ``agent``. Reuse a joined board with NIMBUS_TEST_IP,
NIMBUS_TEST_TOKEN and NIMBUS_PORT. Cost: about 18 sub-agents plus 8 head turns
on claude-haiku-5-5 (well under $0.10).
"""

from __future__ import annotations

import json
import os
import re
import time

import pytest

from device import ExpectTimeout
from test_l4_network import lan_ip_or_skip


def _board_leg(fn):
    """The board legs carry hil + net + agent; the oracle self-check runs anywhere."""
    return pytest.mark.hil(pytest.mark.net(pytest.mark.agent(fn)))


MODEL = os.environ.get("NIMBUS_BENCH_MODEL", "claude-haiku-5-5")  # a parameter, never a literal in a leg
SPAWN_LINE = "did not start"  # both correction lines carry it
SALVAGE_MARKER = "My answer was cut off before I finished"
CUT_LINE = "That answer was cut off before it finished"
CUT_TEXT = "Second part"  # the unfinished sentence in the forced-cut body
LEDGER_RE = r"spawn ledger spawned=(\d+) claimed=(\d+)"
TURN_END_RE = r"turn end: (ok|FAIL)"

# The markup oracle (the host suites' hasStrayMarkup, in Python): a deny-list
# tag outside code, a <|token|>, or raw orch_turn JSON.
_DENY = (
    r"(?:think|thinking|reasoning|function_calls|function_results|invoke|tool_call|tool_result|"
    r"tool_use|arg_key|arg_value|parameter|orch_turn)"
)
_STRAY_RE = re.compile(r"</?(?:[a-z]+:)?" + _DENY + r"\b|<\|[^\s|]+\|>|\{\"reply\":|\"session_ops\":\[", re.I)
_CODE_RE = re.compile(r"```.*?```|`[^`\n]+`", re.S)


def stray_markup(text: str) -> str:
    m = _STRAY_RE.search(_CODE_RE.sub(" ", text or ""))
    return m.group(0) if m else ""


def test_markup_oracle_self_check():
    """The oracle itself: it must catch each leak shape and pass clean text, or
    every leg below proves nothing. Runs without a board."""
    for leaked in (
        'Done.\n</reply>\n<parameter name="memory">x</parameter>',
        "<thinking>plan</thinking>Hello",
        '{"reply":"raw json"}',
        "<|im_end|>",
    ):
        assert stray_markup(leaked), leaked
    for clean in ("Hello <provider> fan.", "Use `<parameter>` in code.", "a < b and b > c", "**Bold** list"):
        assert not stray_markup(clean), clean


# ---- seams -------------------------------------------------------------------


def _chat(net, ip, text: str) -> str:
    r = net.post("/api/chat", {"text": text}, ip=ip, timeout=10.0)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}: {r.text[:200]}"
    turn = str(r.json().get("turn") or "")
    assert turn, f"/api/chat gave no turn id: {r.text[:200]}"
    return turn


def _delivered(net, ip, turn: str, timeout: float = 60.0) -> str:
    """Everything delivered for this web-chat turn (reply, ask and any appended
    correction line), once the turn is answered."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        j = net.get_json(f"/api/chat?turn={turn}", ip=ip, timeout=8.0)
        if j.get("done"):
            return str(j.get("reply") or "")
        assert not j.get("gone"), f"turn {turn} is gone before it was answered"
        time.sleep(2.0)
    raise AssertionError(f"turn {turn} was not answered within {timeout:.0f}s")


def _watch_turn(device, timeout: float):
    """Read serial up to the next 'turn end:' line; return (ledger, ok) where
    ledger is (spawned, claimed) or None when the turn logged none."""
    ledger = None
    deadline = time.time() + timeout
    while True:
        left = deadline - time.time()
        assert left > 0, "no 'turn end:' line on serial in time"
        m = device.expect_re(LEDGER_RE + "|" + TURN_END_RE, timeout=left)
        if m.group(1) is not None:
            ledger = (int(m.group(1)), int(m.group(2)))
        else:
            return ledger, m.group(3) == "ok"


def _settle(device, quiet: float = 45.0, cap: float = 180.0) -> None:
    """Let the sub-agent wave finish: read serial until no turn (the synthesis
    turns included) has ended for ``quiet`` seconds, at most ``cap``."""
    end_by = time.time() + cap
    while time.time() < end_by:
        try:
            device.expect_re(TURN_END_RE, timeout=min(quiet, max(1.0, end_by - time.time())))
        except ExpectTimeout:
            return


@pytest.fixture(scope="module")
def board(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    orch = net.get_json("/api/orch", ip=ip, timeout=8.0)
    anth = (orch.get("providers") or {}).get("anthropic") or {}
    if not anth.get("hasKey"):
        pytest.skip("no Anthropic key on the device")
    prior = {
        "orchHost": str(orch.get("orchHost") or ""),
        "orchLoop": "1" if orch.get("orchLoop") else "0",
        "orchM_anthropic": str(anth.get("orchModel") or ""),
        "subM_anthropic": str(anth.get("subModel") or ""),
    }
    r = net.post(
        "/api/orch",
        {"orchHost": "anthropic", "orchLoop": "1", "orchM_anthropic": MODEL, "subM_anthropic": MODEL},
        ip=ip,
        timeout=8.0,
    )
    assert r.status_code == 200, f"could not set the head to anthropic/{MODEL}: {r.status_code} {r.text[:200]}"
    yield ip
    net.post("/api/orch", {k: v for k, v in prior.items() if v}, ip=ip, timeout=8.0)


TOPICS = [
    ("the boiling point of water at altitude", "how kettles detect boiling", "electric kettle energy use"),
    ("caravan towing limits in the UK", "caravan insurance basics", "caravan site booking rules"),
    ("how solar panels age", "home battery sizing", "heat pump running costs"),
    ("the history of the bicycle", "how gears work", "tyre pressure basics"),
    ("why the sky is blue", "how rainbows form", "what causes a halo around the moon"),
    ("how sourdough starters work", "proofing temperatures", "why bread goes stale"),
]


def _spawn_row_problem(ledger, text: str) -> str:
    """'' when one bench turn kept the spawn contract, else what went wrong."""
    if stray_markup(text):
        return f"markup reached the owner: {stray_markup(text)!r}"
    if ledger is None:
        return f"the head neither started nor claimed sub-agents: {text[:200]!r}"
    spawned, claimed = ledger
    if spawned != claimed and SPAWN_LINE not in text:
        return f"claimed {claimed} sub-agents, spawned {spawned}, and the reply did not say so: {text[:200]!r}"
    return ""


@_board_leg
def test_six_turns_spawned_equals_claimed_or_corrected(board, net, device):
    """The bench spawn table: every turn runs, then the table is printed and
    any row that broke the contract fails the leg."""
    ip = board
    rows = []
    _settle(device, quiet=10.0, cap=60.0)
    for a, b, c in TOPICS:
        device.drain(quiet=0.3)
        turn = _chat(
            net,
            ip,
            f"Start three research sub-agents on anthropic with model {MODEL}, one each on: "
            f"{a}; {b}; {c}. Each should answer in two sentences. Do not do the research yourself.",
        )
        ledger, _ok = _watch_turn(device, timeout=240.0)
        text = _delivered(net, ip, turn)
        rows.append((a, ledger, SPAWN_LINE in text, _spawn_row_problem(ledger, text)))
        _settle(device)
    print("bench spawn table: topic | spawned/claimed | correction line | problem")
    for a, ledger, corrected, problem in rows:
        sc = f"{ledger[0]}/{ledger[1]}" if ledger else "none"
        print(f"  {a} | {sc} | {'yes' if corrected else 'no'} | {problem or 'ok'}")
    problems = [p for *_, p in rows if p]
    assert not problems, "; ".join(problems)


@_board_leg
def test_forced_cut_gives_the_salvage_marker(board, net, device):
    ip = board
    body = json.dumps(
        {
            "id": "msg_bench_cut",
            "type": "message",
            "role": "assistant",
            "model": MODEL,
            "content": [
                {
                    "type": "tool_use",
                    "id": "toolu_bench",
                    "name": "orch_turn",
                    "input": {"reply": "First part of the answer is complete. " + CUT_TEXT},
                }
            ],
            "stop_reason": "max_tokens",
            "usage": {"input_tokens": 10, "output_tokens": 4096},
        },
        separators=(",", ":"),
    )
    # No failover during this leg: a switch to another keyed provider would
    # answer instead of the cut.
    prio = str(net.get_json("/api/orch", ip=ip, timeout=8.0).get("provPrio") or "")
    net.post("/api/orch", {"provPrio": "anthropic"}, ip=ip, timeout=8.0)
    try:
        device.drain(quiet=0.3)
        device.send("PROVFAULT api.anthropic.com 200 " + body)
        try:
            armed = device.expect_re(r"PROVFAULT (api\.anthropic\.com 200 armed|not wired)", timeout=10.0)
        except ExpectTimeout:
            pytest.skip("this image has no PROVFAULT console verb (test build with the test console)")
        if "not wired" in armed.group(0):
            pytest.skip("PROVFAULT is not wired in this image (the provider-wire hook)")
        turn = _chat(net, ip, "Explain how a kettle works.")
        _ledger, ok = _watch_turn(device, timeout=120.0)
        text = _delivered(net, ip, turn)
        print(f"bench cut leg: turn ok={ok} delivered={text[:200]!r}")
        assert SALVAGE_MARKER in text or CUT_LINE in text, f"no honest cut line: {text[:300]!r}"
        assert CUT_TEXT not in text or SALVAGE_MARKER in text, "the cut text was delivered as if complete"
        assert "{" not in text, "raw JSON reached the owner"
    finally:
        device.send("PROVFAULT off")
        if prio:
            net.post("/api/orch", {"provPrio": prio}, ip=ip, timeout=8.0)


def _assistant_rows(net, ip, chat: str) -> dict:
    j = net.get_json(f"/api/mem/episodic?session={chat}&limit=20", ip=ip, timeout=10.0)
    rows = (j.get("messages") or []) if isinstance(j, dict) else []
    return {str(x.get("id")): str(x.get("text") or "") for x in rows if x.get("role") == "assistant"}


@_board_leg
def test_no_markup_in_the_telegram_output(board, net, device):
    ip = board
    tg = net.get_json("/api/telegram", ip=ip, timeout=8.0)
    chats = [str(c.get("id", "")) for c in (tg.get("allow") or []) if c.get("owner")]
    chats = [c for c in chats if c.lstrip("-").isdigit()]
    if not chats:
        pytest.skip("no owner chat on the Telegram allowlist")
    chat = chats[0]
    before = set(_assistant_rows(net, ip, chat))
    device.drain(quiet=0.3)
    r = net.post(
        "/api/test/inject",
        {
            "chat": chat,
            "text": "Think step by step, then give me a formatted comparison of a caravan and a "
            "motorhome with bold headings and a short list for each.",
        },
        ip=ip,
        timeout=10.0,
    )
    if r.status_code == 404:
        pytest.skip("this image has no /api/test/inject (test build only)")
    assert r.status_code == 202, f"/api/test/inject -> {r.status_code}"
    sent = device.expect_re(r"telegram: send (ok|FAIL) to " + re.escape(chat), timeout=180.0)
    assert sent.group(1) == "ok", "the reply was not accepted by Telegram"
    time.sleep(2.0)  # the capture lands with the delivery; give the store a beat
    new = {k: v for k, v in _assistant_rows(net, ip, chat).items() if k not in before}
    assert new, "no assistant row stored for the Telegram reply"
    for rid, text in new.items():
        assert not stray_markup(text), f"markup in the Telegram reply {rid}: {stray_markup(text)!r}"
