"""Connector E2E harness - drive the live orchestrator through its provider
connectors (Mistral Studio: GitHub / Gmail / Notion / Google Drive) and validate
the full create → modify → validate → delete lifecycle *programmatically*.

Design rules (match the repo's HIL discipline):
  * SKIP + WARN, never a silent pass, when a connector isn't configured on the
    device or the external service refuses (a connector isn't a device bug).
  * Break device state (host/loop/enabled-connectors) only inside a context that
    RESTORES it in a finally - one leaked mode cascades into every later test.
  * Validation is agent-round-trip by default (create, then read back a unique
    per-run MARKER in a separate turn); stronger direct-API checks are layered on
    when a service token is present in the env.

Turn driving is over HTTP (never serial - a live turn reboots the console via the
8 s watchdog). We POST /api/chat with a unique marker embedded in the prompt, then
poll /api/lastturn until that marker appears (so we never read a STALE prior turn),
and parse the orch_turn reply JSON out of the turn-anatomy dump.

Mistral connectors attach only on SINGLE-SHOT turns (loop off) and pin at
conversation creation, so the harness sets host=mistral + loop=0 and enables
exactly ONE connector per lifecycle (the ~2-connector ceiling + the conv-reset on
any /api/connectors write are handled for us by the firmware).
"""

from __future__ import annotations

import json
import re
import time
import uuid
from contextlib import contextmanager
from typing import Optional

import pytest

# A live connector turn (Conversations API + a server-side tool call) is slow.
TURN_TIMEOUT = 150.0
POLL_EVERY = 5.0


def new_marker(tag: str) -> str:
    """A unique, greppable marker for one lifecycle step, e.g. NBX-notion-9f3a1c."""
    return f"NBX-{tag}-{uuid.uuid4().hex[:6]}"


# ---- connector inventory ----------------------------------------------------
def list_configured(net, ip) -> list[dict]:
    return net.get_json("/api/connectors", ip=ip, timeout=8.0).get("configured", [])


def find_connector(net, ip, *, prov: str, cid: str) -> Optional[dict]:
    """The device connector row whose provider+connector-id match (cid = the
    Mistral Studio connector name), or None if the owner hasn't added it."""
    for c in list_configured(net, ip):
        if c.get("prov") == prov and (c.get("cid") == cid or c.get("name") == cid):
            return c
    return None


def require_mistral_connector(net, ip, cid: str, human_name: str) -> dict:
    """Return the device row for a Mistral Studio connector, or LOUD-skip with a
    reproducible reason (so a missing connector reads as SKIP, never a pass)."""
    row = find_connector(net, ip, prov="mistral", cid=cid)
    if row is None:
        pytest.skip(
            f"{human_name} connector not configured on the device "
            f"(no mistral connector with cid={cid!r}). Connect it in Mistral Studio "
            f"(console.mistral.ai/build/connectors), then add it via "
            f"POST /api/connectors patch={{\"name\":\"m-{cid}\",\"prov\":\"mistral\","
            f"\"kind\":\"connector\",\"cid\":\"{cid}\",\"en\":1}}."
        )
    return row


# ---- turn driving -----------------------------------------------------------
def _lastturn_text(net, ip) -> str:
    return net.get("/api/lastturn", ip=ip, timeout=8.0).text


def _parse_reply(turn_text: str) -> dict:
    """Pull the orch_turn reply JSON object out of the turn-anatomy dump."""
    m = re.search(r'\{"reply".*\}', turn_text)
    if not m:
        return {}
    try:
        return json.loads(m.group(0))
    except json.JSONDecodeError:
        # tolerate a truncated dump: at least return the reply string
        r = re.search(r'"reply"\s*:\s*"((?:[^"\\]|\\.)*)"', turn_text)
        return {"reply": (r.group(1) if r else "")}


class RebootDuringTurn(AssertionError):
    """The device rebooted mid-turn - /api/lastturn went back to 'no turn has run
    yet' after we submitted one. A real device fault (heap/OOM on the workload),
    reported distinctly from a plain timeout so the failure names its cause."""


def run_turn(net, ip, prompt: str, *, timeout: float = TURN_TIMEOUT) -> dict:
    """Fire ONE orchestrator turn carrying a unique marker; block until THIS turn
    completes (its marker shows up in /api/lastturn), then return the parsed reply
    dict plus the raw turn text under key ``_raw``. Raises RebootDuringTurn if the
    device reboots mid-turn, or AssertionError on timeout."""
    marker = f"turnmark-{uuid.uuid4().hex[:8]}"
    r = net.post("/api/chat", {"text": f"[{marker}] {prompt}"}, ip=ip, timeout=10.0)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}: {r.text[:200]}"
    deadline = time.time() + timeout
    saw_a_turn = False  # once we see any real anatomy, a later "no turn" == reboot
    while time.time() < deadline:
        time.sleep(POLL_EVERY)
        try:
            txt = _lastturn_text(net, ip)
        except Exception:  # noqa: BLE001 - transient during a reboot's WiFi rejoin
            continue
        if marker in txt:
            out = _parse_reply(txt)
            out["_raw"] = txt
            return out
        if "no turn has run yet" in txt:
            if saw_a_turn:
                raise RebootDuringTurn(
                    f"device rebooted during the turn (marker {marker}) - lastturn reset "
                    f"to 'no turn has run yet'. Workload too heavy for the board (heap/OOM)."
                )
        else:
            saw_a_turn = True
    raise AssertionError(f"turn did not complete within {timeout:.0f}s (marker {marker})")


# Phrases that mean "the connector/service refused" (skip+warn) vs a device bug.
_UNAVAILABLE_RE = re.compile(
    r"not enabled|not accessible|does not exist|no access|couldn'?t (find|access)|"
    r"unable to (access|reach)|connector .*(unavailable|not configured)|permission",
    re.I,
)


def reply_text(turn: dict) -> str:
    return str(turn.get("reply", "") or "")


def skip_if_unavailable(turn: dict, human_name: str) -> None:
    """If the agent's reply says the connector/resource was unreachable, SKIP with
    the reason rather than hard-failing (external state, not a device regression)."""
    txt = reply_text(turn)
    if _UNAVAILABLE_RE.search(txt):
        pytest.skip(f"{human_name}: connector/resource unavailable - agent said: {txt[:180]!r}")


# ---- sub-agent dispatch (the offload path) ----------------------------------
def mcp_call(net, ip, tool: str, arguments: dict, *, req_id: int = 1) -> dict:
    """One JSON-RPC tools/call against the device /mcp endpoint. Returns the parsed
    response dict (JSON-RPC envelope)."""
    body = json.dumps(
        {"jsonrpc": "2.0", "id": req_id, "method": "tools/call", "params": {"name": tool, "arguments": arguments}}
    )
    # /mcp takes a RAW JSON body (not form-encoded), token via ?t=.
    import requests

    resp = requests.post(
        f"http://{ip}/mcp?t={net.token()}", data=body, headers={"Content-Type": "application/json"}, timeout=15.0
    )
    assert resp.status_code == 200, f"/mcp -> {resp.status_code}: {resp.text[:160]}"
    return resp.json()


def spawn_subagent(net, ip, provider: str, task: str) -> str:
    """Dispatch a sub-agent on `provider` via the LAN /mcp session.spawn tool.
    NOTE: on the device the MCP registry's session.spawn is NOT wired to the
    orchestrator (h.spawn is null) - spawns go through the head turn - so this
    returns "spawn not supported"; use head_spawn_and_wait for a real dispatch."""
    out = mcp_call(net, ip, "session.spawn", {"provider": provider, "task": task})
    content = out.get("result", {}).get("content", [])
    return " ".join(c.get("text", "") for c in content) if content else json.dumps(out)


def head_spawn_and_wait(net, ip, provider: str, task: str, *, timeout: float = 240.0) -> bool:
    """Drive a REAL sub-agent dispatch the way the product does: ask the head turn
    to spawn on `provider`, then watch /api/log until a job on that backend
    completes (err=0 state=3). Returns True on a clean completion. This is what
    proves the offload path - the connectors for `provider` attach on dispatch
    automatically. Set the head to a working provider (anthropic) + loop on first."""
    import re

    mark = f"hs-{provider}-{uuid.uuid4().hex[:6]}"
    net.post(
        "/api/chat",
        {
            "text": f"[{mark}] Spawn a sub-agent on the {provider} provider to: {task} "
            f"Just start it, do not do it yourself.",
            "t": net.token(),
        },
        ip=ip,
        timeout=10.0,
    )
    deadline = time.time() + timeout
    dispatched = False
    while time.time() < deadline:
        time.sleep(POLL_EVERY)
        try:
            log = net.get("/api/log", ip=ip, timeout=8.0).text
        except Exception:  # noqa: BLE001
            continue
        if re.search(rf"spawned job\d+ -> {provider}:", log):
            dispatched = True
        # a clean terminal poll for a job on this backend
        if dispatched and re.search(r"poll job\d+ err=0 state=3", log):
            return True
        if re.search(rf"{provider} down -> failover", log):
            return False
    return False


def wait_for_jobs_quiescent(net, ip, *, timeout: float = 180.0) -> None:
    """Poll /api/orch until no sub-agent jobs are active (the spawned work finished
    and its synthesis ran). Best-effort - returns on timeout too."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(POLL_EVERY)
        try:
            d = net.get_json("/api/orch", ip=ip, timeout=8.0)
        except Exception:  # noqa: BLE001
            continue
        jobs = d.get("jobs")
        if isinstance(jobs, list) and len(jobs) == 0:
            return
        if jobs is None:  # shape without a jobs array - can't track, give the work time
            time.sleep(POLL_EVERY)
            return


# ---- mode + single-connector context ---------------------------------------
@contextmanager
def mistral_single_connector(net, ip, cid: str):
    """Enter: host=mistral, loop OFF, and ONLY the mistral connector `cid` enabled.
    Exit: restore the prior host/loop and the prior enabled-set. Every
    /api/connectors write resets the Mistral conversation (firmware), so the new
    single-connector set is live on the next turn."""
    prior = net.get_json("/api/orch", ip=ip, timeout=8.0)
    prev_host = prior.get("orchHost", "")
    prev_loop = 1 if prior.get("orchLoop") else 0
    before = [(c["name"], 1 if c.get("en") else 0) for c in list_configured(net, ip) if c.get("prov") == "mistral"]

    def _patch(obj):
        r = net.post("/api/connectors", {"patch": json.dumps(obj)}, ip=ip, timeout=8.0)
        assert r.status_code == 200, f"connector patch -> {r.status_code}: {r.text[:160]}"

    try:
        # enable only the target mistral connector
        target_name = None
        for name, _ in before:
            row = next(c for c in list_configured(net, ip) if c["name"] == name)
            want = 1 if (row.get("cid") == cid or name == cid) else 0
            if want:
                target_name = name
            _patch({"name": name, "en": want})
        assert target_name, f"no mistral connector row matched cid={cid!r}"
        net.post("/api/orch", {"orchHost": "mistral", "orchLoop": "0"}, ip=ip, timeout=8.0)
        time.sleep(3.0)  # staged-config apply on the main loop
        yield
    finally:
        for name, en in before:
            _patch({"name": name, "en": en})
        net.post("/api/orch", {"orchHost": prev_host, "orchLoop": str(prev_loop)}, ip=ip, timeout=8.0)
