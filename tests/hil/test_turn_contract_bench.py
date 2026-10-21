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

Legs 1 and 2 run over the web chat and need no Telegram bot. Leg 3 needs a bot
token Telegram accepts; with none, or one it refuses (``tgAuthFail``), it skips
as BLOCKED instead of failing.

Serial lines are the seam for log facts (the /api/log ring holds about 1 KB,
too little for a turn). Delivered text comes from GET /api/chat?turn=<id>. Each
turn starts only when the device is idle (no sub-agent job, no turn ending for a
while) and is read until its own answer, never to the first ``turn end:`` line,
which can belong to a synthesis turn or a routine that was already due.

Board legs are gated: ``hil`` + ``net`` + ``agent``. Reuse a joined board with NIMBUS_TEST_IP,
NIMBUS_TEST_TOKEN and NIMBUS_PORT. Cost: about 18 sub-agents, 8 head turns and
their synthesis turns on claude-haiku-5-5. The device's own usage estimate rose by
about $0.65 on unit A (19 Oct); that overstates the bill, because claude-haiku-5-5
has no built-in price row yet (the Anthropic default rate applies) and the
forced-cut leg's canned usage (4,096 output tokens per request) is counted but
never billed.
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


def _turn_ledgers(device, net, ip, turn: str, timeout: float = 240.0):
    """Run one web-chat turn to its answer and return (ledgers, delivered text).

    ``ledgers`` holds every ``spawn ledger spawned=N claimed=M`` line printed
    while the turn ran, as (spawned, claimed). The turn's own line comes before
    its answer (apply logs it, then delivers), so reading until /api/chat says
    the turn is done catches it. The first ``turn end:`` line is NOT the end of
    this turn: turns run one at a time, and a turn that was already due (a
    synthesis turn, a routine) ends first. Reading to the first ``turn end:``
    attributed another turn's end to bench row 6 on 19 Oct (unit A)."""
    ledgers = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            m = device.expect_re(LEDGER_RE, timeout=3.0)
            ledgers.append((int(m.group(1)), int(m.group(2))))
            continue
        except ExpectTimeout:
            pass
        j = net.get_json(f"/api/chat?turn={turn}", ip=ip, timeout=8.0)
        assert not j.get("gone"), f"turn {turn} is gone before it was answered"
        if j.get("done"):
            try:  # lines still in the host's serial buffer
                while True:
                    m = device.expect_re(LEDGER_RE, timeout=2.0)
                    ledgers.append((int(m.group(1)), int(m.group(2))))
            except ExpectTimeout:
                pass
            return ledgers, str(j.get("reply") or "")
    raise AssertionError(f"turn {turn} was not answered within {timeout:.0f}s")


def _wait_idle(net, ip, device, quiet: float = 30.0, cap: float = 600.0) -> None:
    """Wait until the device is idle before the next turn: no sub-agent job
    (``/api/state`` jobs == 0 on two polls in a row, so a spawn being handed over
    is not missed), then no turn ending for ``quiet`` seconds, so the synthesis
    turn the last results start has run. A sub-agent wave lasts longer than any
    fixed quiet window, so waiting on turn ends alone was not enough."""
    end_by = time.time() + cap
    idle_polls = 0
    while time.time() < end_by:
        jobs = int(net.get_json("/api/state", ip=ip, timeout=8.0).get("jobs") or 0)
        idle_polls = idle_polls + 1 if jobs == 0 else 0
        try:
            if idle_polls < 2:
                device.expect_re(TURN_END_RE, timeout=10.0)  # keep serial drained meanwhile
                continue
            device.expect_re(TURN_END_RE, timeout=min(quiet, max(1.0, end_by - time.time())))
            idle_polls = 0  # a turn just ended: check the jobs again
        except ExpectTimeout:
            if idle_polls >= 2:
                return
    raise AssertionError(f"the device did not go idle within {cap:.0f}s (sub-agents or turns still running)")


def _clip(text: str, n: int = 300) -> str:
    """The text for a report, with an explicit mark when it was shortened (a
    plain slice reads like a reply the device cut mid-word)."""
    return repr(text) if len(text) <= n else f"{text[:n]!r} [report shortened, {len(text)} chars in all]"


@pytest.fixture(scope="module")
def _bench_board(net):
    """The board setup, done once per module: the IP, the head settings it
    replaced (restored when the module ends), or why the first attempt skipped.
    It holds no fixture of narrower scope; ``board`` fills it."""
    state = {}
    yield state
    if state.get("prior"):
        net.post("/api/orch", state["prior"], ip=state["ip"], timeout=8.0)


def _set_bench_head(device, net, secrets, require_secret, state) -> None:
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
    state["ip"] = ip
    state["prior"] = {k: v for k, v in prior.items() if v}


def _cached_board(state: dict, provision) -> str:
    """Run ``provision(state)`` once per module; later legs reuse its IP, or its
    skip at once (a missing key or LAN must not re-provision for every leg)."""
    if "skip" in state:
        pytest.skip(state["skip"])
    if "ip" not in state:
        try:
            provision(state)
        except pytest.skip.Exception as exc:
            state["skip"] = exc.msg
            raise
    return state["ip"]


def test_board_setup_runs_once_and_a_skip_sticks():
    """The board cache itself, without a board."""
    calls = []

    def ok(state):
        calls.append(1)
        state["ip"] = "10.0.0.2"

    state = {}
    assert _cached_board(state, ok) == "10.0.0.2"
    assert _cached_board(state, ok) == "10.0.0.2"
    assert len(calls) == 1

    def no_key(state):
        calls.append(1)
        pytest.skip("no Anthropic key on the device")

    state, calls = {}, []
    for _ in range(2):
        with pytest.raises(pytest.skip.Exception, match="no Anthropic key"):
            _cached_board(state, no_key)
    assert len(calls) == 1


@pytest.fixture
def board(device, net, secrets, require_secret, _bench_board):
    """The joined board's IP with the head on anthropic/MODEL. Function scoped,
    because ``require_secret`` is; the provisioning runs once per module
    (``_cached_board``) and the settings it replaced are restored at module end."""
    return _cached_board(_bench_board, lambda st: _set_bench_head(device, net, secrets, require_secret, st))


TOPICS = [
    ("the boiling point of water at altitude", "how kettles detect boiling", "electric kettle energy use"),
    ("caravan towing limits in the UK", "caravan insurance basics", "caravan site booking rules"),
    ("how solar panels age", "home battery sizing", "heat pump running costs"),
    ("the history of the bicycle", "how gears work", "tyre pressure basics"),
    ("why the sky is blue", "how rainbows form", "what causes a halo around the moon"),
    ("how sourdough starters work", "proofing temperatures", "why bread goes stale"),
]


def _spawn_row_problem(ledgers, text: str) -> str:
    """'' when one bench turn kept the spawn contract, else what went wrong."""
    if stray_markup(text):
        return f"markup reached the owner: {stray_markup(text)!r}"
    if not ledgers:
        return f"no spawn ledger line while this turn ran (it asked for three sub-agents): {_clip(text)}"
    spawned, claimed = ledgers[-1]
    if spawned != claimed and SPAWN_LINE not in text:
        return f"claimed {claimed} sub-agents, spawned {spawned}, and the reply did not say so: {_clip(text)}"
    return ""


def test_a_turn_is_read_to_its_own_answer_not_the_first_turn_end():
    """The row attribution, without a board: the 19 Oct shape (another turn's
    end first, then this turn's ledger line and answer) reads 3/3."""

    class Dev:
        def __init__(self, lines):
            self.lines = list(lines)

        def expect_re(self, pattern, timeout=5.0):
            while self.lines:
                m = re.search(pattern, self.lines.pop(0))
                if m:
                    return m
            raise ExpectTimeout(pattern, [])

    class Net:
        def __init__(self):
            self.polls = 0

        def get_json(self, path, ip=None, timeout=None):
            self.polls += 1
            return {"done": self.polls > 1, "reply": "Starting three research sub-agents."}

    dev = Dev(
        [
            "turn end: ok host=anthropic tokens in=2400 out=300 tools=0 reply=420B",  # a synthesis turn
            "orchestrator: spawn ledger spawned=3 claimed=3",
            "turn end: ok host=anthropic tokens in=7300 out=900 tools=0 reply=180B",
        ]
    )
    ledgers, text = _turn_ledgers(dev, Net(), "ip", "t1", timeout=30.0)
    assert ledgers == [(3, 3)], ledgers
    assert _spawn_row_problem(ledgers, text) == ""
    assert "no spawn ledger line" in _spawn_row_problem([], text)
    assert "report shortened" in _clip("x" * 400) and "report shortened" not in _clip("short")


def test_the_bench_waits_for_running_sub_agents():
    """``_wait_idle`` returns only after two job-free polls and a quiet window."""

    class Dev:
        def expect_re(self, pattern, timeout=5.0):
            raise ExpectTimeout(pattern, [])

    class Net:
        def __init__(self, jobs):
            self.jobs = list(jobs)
            self.polls = 0

        def get_json(self, path, ip=None, timeout=None):
            self.polls += 1
            return {"jobs": self.jobs.pop(0) if self.jobs else 0}

    net = Net([3, 2, 0, 1, 0, 0])
    _wait_idle(net, "ip", Dev(), quiet=0.01, cap=30.0)
    assert net.polls == 6, net.polls  # a job reappearing restarts the count


@_board_leg
def test_six_turns_spawned_equals_claimed_or_corrected(board, net, device):
    """The bench spawn table: every turn runs, then the table is printed and
    any row that broke the contract fails the leg."""
    ip = board
    rows = []
    for a, b, c in TOPICS:
        _wait_idle(net, ip, device)
        device.drain(quiet=0.3)
        turn = _chat(
            net,
            ip,
            f"Start three research sub-agents on anthropic with model {MODEL}, one each on: "
            f"{a}; {b}; {c}. Each should answer in two sentences. Do not do the research yourself.",
        )
        ledgers, text = _turn_ledgers(device, net, ip, turn)
        rows.append((a, ledgers, SPAWN_LINE in text, _spawn_row_problem(ledgers, text)))
    print("bench spawn table: topic | spawned/claimed | correction line | problem")
    for a, ledgers, corrected, problem in rows:
        sc = " ".join(f"{sp}/{cl}" for sp, cl in ledgers) if ledgers else "none"
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
    # The previous leg's sub-agents and synthesis turns finish first, so the
    # canned cut answers this leg's turn and nothing else.
    _wait_idle(net, ip, device)
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
        _ledgers, text = _turn_ledgers(device, net, ip, turn, timeout=120.0)
        print(f"bench cut leg: delivered={_clip(text)}")
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
    # The only leg that needs a working bot: a token Telegram refuses (401) is a
    # bench prerequisite, reported as BLOCKED, never a markup failure.
    orch = net.get_json("/api/orch", ip=ip, timeout=8.0)
    if orch.get("tgAuthFail") or orch.get("tgVerify") == 0:
        pytest.skip("BLOCKED: Telegram rejects this device's bot token (tgAuthFail); set a working test bot token")
    tg = net.get_json("/api/telegram", ip=ip, timeout=8.0)
    if not tg.get("hasToken"):
        pytest.skip("BLOCKED: no Telegram bot token on the device")
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
