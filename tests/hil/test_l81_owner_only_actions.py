"""L81 - only the owner changes the owner's device (v5.0.0): every action checked against who asked.

The bench leg for the caller rule table (lib/core/src/orch_action_rules.cpp, the host
class table is test/test_action_rules). Deterministic: the head is the HIL mock LLM
(no provider spend), scripted to answer every turn with the same orch_model device
action, so who asked is the only thing that changes between legs:

  * a Telegram member (TGINJECT from a chat approved as a user) is refused, and so
    is a guest;
  * a routine (an owner-created one-shot routine on the web chat, which fires
    unattended about two minutes later) is refused;
  * the owner (the web chat, the owner writing) succeeds;
  * the LAN /mcp endpoint and a member are refused the owner-only tools through
    the dispatch seam, while the owner reaches the same handlers.

Each refusal is read back three ways: the main model did not move (/api/orch
orchHost), the device log names the refusal (``orch_model(refused)``), and the
person was told (the member's or the routine's reply). Every change is restored in
a ``finally``: the synthetic tenants are revoked, removed from the allowlist and
dropped, and the head goes back to where it was.

Markers: ``hil`` + ``net`` (serial for TGINJECT and the mock fixture, LAN for the
rest). Needs the ``test`` build (TGINJECT, /api/test/astool) and the mock LLM
reachable from the device (test_l12_mock_llm's custom_backend fixture).
"""

from __future__ import annotations

import json
import time

import pytest

from mock_llm import CLAIM_TURNS, mock_llm  # noqa: F401  (mock_llm is a fixture)
from test_l12_mock_llm import _opost, custom_backend  # noqa: F401  (fixture + helper)
from webrig import LogFollower, make_session

pytestmark = [pytest.mark.hil, pytest.mark.net]

MEMBER_CHAT = "999000781"  # synthetic; never a real Telegram chat
GUEST_CHAT = "999000782"
SWITCH_MODEL = "mock-rbac-switch"  # the scripted head turn: one orch_model action
REFUSED = "orch_model(refused)"
TURN_WAIT_S = 90.0
ROUTINE_WAIT_S = 240.0  # a one-shot routine is due 120 s out; the loop tick adds a little


def _tenant(be, chat: str, role: str) -> None:
    r = be.net.post("/api/telegram/add", {"id": chat, "name": "l81"}, ip=be.ip, timeout=10.0)
    assert r.status_code == 200, f"/api/telegram/add {chat} -> {r.status_code}"
    r = be.net.post("/api/tenant", {"id": chat, "role": role}, ip=be.ip, timeout=10.0)
    assert r.status_code == 200, f"/api/tenant {chat} {role} -> {r.status_code}: {r.text[:120]}"


def _drop_tenant(be, chat: str) -> None:
    be.net.post("/api/tenant", {"id": chat, "role": "unknown"}, ip=be.ip, timeout=10.0)
    be.net.post("/api/telegram/remove", {"id": chat}, ip=be.ip, timeout=10.0)
    be.net.post("/api/tenant", {"id": chat, "remove": "1"}, ip=be.ip, timeout=10.0)


def _follow(be) -> LogFollower:
    """Follow the device log (/api/log keeps only its last lines, so one read after a
    turn would miss the turn's own)."""
    return LogFollower(make_session(be.tok), be.ip)


def _wait_log(log: LogFollower, needle: str, timeout: float) -> str:
    """What the log gained since the follower started, once it holds `needle` ("" on timeout)."""
    end = time.time() + timeout
    while time.time() < end:
        if needle in log.text:
            return log.text
        time.sleep(2.0)
    return ""


def _astool(be, chat: str, tool: str, args: dict) -> dict:
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    r = be.net.post("/api/test/astool", {"chat": chat, "body": body}, ip=be.ip, timeout=15.0)
    if r.status_code == 404:
        pytest.skip("/api/test/astool is missing: flash the test build")
    assert r.status_code == 200, f"astool {tool} as {chat} -> {r.status_code}: {r.text[:160]}"
    return r.json()


def _lan_mcp(be, tool: str, args: dict) -> dict:
    import requests

    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    r = requests.post(
        f"http://{be.ip}/mcp",
        data=body,
        headers={"Content-Type": "application/json", "X-Nimbus-Token": be.tok},
        timeout=15.0,
    )
    assert r.status_code == 200, f"/mcp {tool} -> {r.status_code}: {r.text[:160]}"
    return r.json()


def _text(rpc: dict) -> tuple:
    """(isError, text) of a JSON-RPC tools/call answer (a result or an error)."""
    if "error" in rpc:
        return True, str(rpc["error"].get("message", ""))
    res = rpc.get("result", {})
    return bool(res.get("isError")), "\n".join(c.get("text", "") for c in res.get("content", []) or [])


@pytest.fixture
def rbac_head(custom_backend):  # noqa: F811
    """The head on the mock, scripted to switch the main model to a keyed provider's
    own current model; the members approved; everything restored afterwards."""
    be = custom_backend
    orch = be.orch()
    prior_host = orch.get("orchHost", "")
    provs = orch.get("providers", {})
    target = next((p for p in ("anthropic", "openai", "mistral", "zai") if provs.get(p, {}).get("hasKey")), "")
    model = provs.get(target, {}).get("orchModel", "") if target else ""
    if not target or not model:
        pytest.skip(f"no keyed provider with a model to switch to (providers={sorted(provs)})")
    CLAIM_TURNS[SWITCH_MODEL] = {
        "reply": "Switched the main model.",
        "device": [{"type": "orch_model", "provider": target, "model": model}],
    }
    r = _opost(be.net, be.ip, be.tok, {"orchHost": "custom", "convReset": "1"})
    assert r.status_code == 200, f"orchHost=custom -> {r.status_code}"
    be.set_model(SWITCH_MODEL)
    _tenant(be, MEMBER_CHAT, "user")
    _tenant(be, GUEST_CHAT, "guest")
    try:
        yield be, target, model
    finally:
        _drop_tenant(be, MEMBER_CHAT)
        _drop_tenant(be, GUEST_CHAT)
        _opost(be.net, be.ip, be.tok, {"orchHost": prior_host, "convReset": "1"})
        CLAIM_TURNS.pop(SWITCH_MODEL, None)


def _inject(be, chat: str, text: str, timeout_s: float = 90.0) -> None:
    """TGINJECT one text message from `chat`, waiting out a page that is still running
    (the poll task runs one injected page per cycle and answers ``busy`` meanwhile)."""
    page = {
        "ok": True,
        "result": [
            {
                "update_id": 81000 + int(time.time()) % 1000,
                "message": {
                    "message_id": 1,
                    "date": int(time.time()),
                    "chat": {"id": int(chat), "type": "private"},
                    "from": {"id": int(chat), "first_name": "L81"},
                    "text": text,
                },
            }
        ],
    }
    line = "TGINJECT " + json.dumps(page, separators=(",", ":"))
    deadline = time.time() + timeout_s
    while True:
        out = be.device.cmd(line, "TGINJECT", timeout=15.0)
        if "busy" not in out:
            break
        if time.time() > deadline:
            pytest.fail(f"the previous injected page never finished running: {out}")
        time.sleep(2.0)
    assert "injected=1" in out or "text=1" in out, f"the message was not injected: {out}"


@pytest.mark.parametrize("chat", [MEMBER_CHAT, GUEST_CHAT], ids=["member", "guest"])
def test_a_member_or_guest_cannot_move_the_main_model(rbac_head, chat):
    be, _target, _model = rbac_head
    be.wait_idle()
    be.server.reset()
    with _follow(be) as log:
        _inject(be, chat, "switch your main model please")
        assert be.server.wait_for_request(SWITCH_MODEL, count=1, timeout=TURN_WAIT_S), (
            "the member's turn never reached the head"
        )
        seen = _wait_log(log, REFUSED, TURN_WAIT_S)
    assert seen, "no orch_model refusal in the device log for the member's turn"
    assert "only the device's owner can change the main model" in seen
    assert be.orch().get("orchHost") == "custom", "a member moved the owner's main model"


def test_a_routine_cannot_move_the_main_model(rbac_head):
    be, _target, _model = rbac_head
    be.wait_idle()
    be.server.reset()
    sched = json.dumps({"kind": "once", "in_seconds": 120})
    r = be.net.post(
        "/api/loops",
        {
            "action": "create",
            "name": "l81-routine",
            "prompt": "switch the main model",
            "chatId": "web",
            "schedule": sched,
        },
        ip=be.ip,
        timeout=10.0,
    )
    assert r.status_code == 200, f"/api/loops create -> {r.status_code}"
    try:
        with _follow(be) as log:
            assert be.server.wait_for_request(SWITCH_MODEL, count=1, timeout=ROUTINE_WAIT_S), "the routine never fired"
            seen = _wait_log(log, REFUSED, TURN_WAIT_S)
        assert seen, "no orch_model refusal in the device log for the routine"
        assert "in a message they write" in seen
        assert be.orch().get("orchHost") == "custom", "a routine moved the owner's main model"
    finally:
        # A one-shot routine retires once it fires; drop it if it never did.
        for lp in be.net.get_json("/api/loops", ip=be.ip, timeout=10.0) or []:
            if isinstance(lp, dict) and lp.get("name") == "l81-routine":
                be.net.post("/api/loops", {"action": "delete", "id": lp.get("id", "")}, ip=be.ip, timeout=10.0)


def test_the_owner_moves_the_main_model(rbac_head):
    be, target, model = rbac_head
    be.wait_idle()
    be.server.reset()
    with _follow(be) as log:
        r = be.net.post("/api/chat", {"text": "switch your main model"}, ip=be.ip, timeout=10.0)
        assert r.status_code == 200, f"/api/chat -> {r.status_code}"
        assert be.server.wait_for_request(SWITCH_MODEL, count=1, timeout=TURN_WAIT_S), (
            "the owner's turn never reached the head"
        )
        seen = _wait_log(log, f"orch_model({target}/{model})", TURN_WAIT_S)
        other = log.text
    if not seen:
        if "orch_model(no-key)" in other or "orch_model(invalid)" in other:
            pytest.skip(f"the bench's {target}/{model} is not a selectable head: {other[-300:]}")
        pytest.fail(f"the owner's switch did not apply: {other[-400:]}")
    assert REFUSED not in seen
    assert be.orch().get("orchHost") == target


def test_owner_only_tools_by_caller(rbac_head):
    """The dispatch seam on hardware: a member and the LAN endpoint are refused the
    owner-only tools, the owner's chat reaches the same handlers."""
    be, _target, _model = rbac_head
    cases = [
        ("memory.config", {"action": "update", "max_vectors": 1}),
        ("session.terminate", {"id": "job9999"}),
        ("models.list", {"refresh": True}),
    ]
    for tool, args in cases:
        err, text = _text(_astool(be, MEMBER_CHAT, tool, args))
        assert err and "only the device's owner" in text, f"member {tool}: {text[:200]}"
        err, text = _text(_lan_mcp(be, tool, args))
        assert err and "LAN connection cannot" in text, f"LAN {tool}: {text[:200]}"
    # The owner reaches the handler (a view, an unknown job, a cached list: nothing changes).
    err, text = _text(_astool(be, "web", "memory.config", {"action": "view"}))
    assert not err, text[:200]
    err, text = _text(_astool(be, "web", "session.terminate", {"id": "job9999"}))
    assert "no running session" in text, text[:200]
