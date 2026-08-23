"""§L15 - Outbound MCP client E2E (the device dialing a REMOTE MCP server).

The device is an MCP server (the /mcp endpoint); this suite exercises the CLIENT
half added in N4: the device dials a remote MCP server over Streamable HTTP,
discovers its tools, registers them namespaced ``mcp.<server>.<tool>``, and calls
them - per-server approval fail-closed, RBAC-gated.

Deterministic where possible: discovery is proven by the device's OWN /mcp
``tools/list`` gaining (or not gaining) the namespaced tools, not by LLM output.
The agent round-trip test proves an end-to-end call and, like L13, treats an
external refusal as a LOUD SKIP (external state, not a device regression).

Prerequisites (each missing piece SKIPs with the exact repro step):
  * a board on the LAN: ``NIMBUS_TEST_IP`` + ``NIMBUS_TEST_TOKEN`` (or serial join)
  * a reachable remote MCP server URL in ``NIMBUS_MCP_TEST_URL`` - e.g. run
    ``PORT=3111 npx -y @modelcontextprotocol/server-everything streamableHttp``
    on a LAN host and pass ``http://<host>:3111/mcp`` (must be reachable FROM the
    device, not just from this test host).
  * for the Linear leg, a scratch Linear API token in ``NIMBUS_LINEAR_MCP_TOKEN``.

Markers: ``connectors`` (+ ``agent`` + ``net``). Gated behind ``--allow-hardware``.

Run:  python3 -m pytest tests/hil -m "connectors and not manual" --allow-hardware
"""

from __future__ import annotations

import json
import os
import time

import pytest

from connectors import new_marker, reply_text, run_turn, skip_if_unavailable
from test_l4_network import lan_ip_or_skip

pytestmark = [pytest.mark.connectors, pytest.mark.agent, pytest.mark.net]

_LOCAL_URL_ENV = "NIMBUS_MCP_TEST_URL"
_LINEAR_TOKEN_ENV = "NIMBUS_LINEAR_MCP_TOKEN"
_LINEAR_MCP_URL = "https://mcp.linear.app/mcp"


# ---- device MCP connector plumbing ------------------------------------------
def _patch_connector(net, ip, obj: dict) -> None:
    r = net.post("/api/connectors", {"patch": json.dumps(obj)}, ip=ip, timeout=8.0)
    assert r.status_code == 200, f"connector patch -> {r.status_code}: {r.text[:160]}"


def _delete_connector(net, ip, name: str) -> None:
    net.post("/api/connectors", {"del": name}, ip=ip, timeout=8.0)


def _add_device_mcp(net, ip, name: str, url: str, *, approved: bool, tok: str = "") -> None:
    """Upsert a device-dialed MCP server (kind=mcp, dev=1). appr controls the
    fail-closed approval gate."""
    entry = {"name": name, "kind": "mcp", "url": url, "en": 1, "dev": 1, "appr": 1 if approved else 0}
    if tok:
        entry["tok"] = tok
    _patch_connector(net, ip, entry)


def _device_tool_names(net, ip) -> list[str]:
    """The device's own registry, via its inbound /mcp tools/list. Outbound tools
    appear here once discovered, so this is the deterministic discovery probe."""
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
    import requests

    resp = requests.post(
        f"http://{ip}/mcp?t={net.token()}", data=body, headers={"Content-Type": "application/json"}, timeout=15.0
    )
    assert resp.status_code == 200, f"/mcp tools/list -> {resp.status_code}: {resp.text[:160]}"
    tools = resp.json().get("result", {}).get("tools", [])
    return [t.get("name", "") for t in tools]


def _mcp_tool_names(net, ip, slug: str) -> list[str]:
    return [n for n in _device_tool_names(net, ip) if n.startswith(f"mcp.{slug}.")]


def _kick_sync(net, ip) -> None:
    """Discovery runs on the turn path (catalog()); one turn triggers a sync pass."""
    run_turn(net, ip, "Say READY and nothing else.")
    time.sleep(2.0)


def _local_url_or_skip() -> str:
    url = os.environ.get(_LOCAL_URL_ENV, "").strip()
    if not url:
        pytest.skip(
            f"set {_LOCAL_URL_ENV} to a remote MCP URL reachable FROM the device, e.g. "
            "run `PORT=3111 npx -y @modelcontextprotocol/server-everything streamableHttp` "
            "on a LAN host and pass http://<host>:3111/mcp"
        )
    return url


# ---- fail-closed: an unapproved server is never dialed ----------------------
def test_device_mcp_fail_closed_until_approved(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    url = _local_url_or_skip()
    name = "hiltest_everything"
    slug = "hiltest_everything"
    try:
        # Configured but NOT approved -> no tools registered, even after a turn.
        _add_device_mcp(net, ip, name, url, approved=False)
        _kick_sync(net, ip)
        assert _mcp_tool_names(net, ip, slug) == [], "fail-closed violated: unapproved server was dialed"

        # Approve -> the device discovers and registers the namespaced tools.
        _add_device_mcp(net, ip, name, url, approved=True)
        deadline = time.time() + 40
        found: list[str] = []
        while time.time() < deadline and not found:
            _kick_sync(net, ip)
            found = _mcp_tool_names(net, ip, slug)
        assert found, "approved server exposed no mcp.* tools (discovery failed - check the device can reach the URL)"
        assert any(n == f"mcp.{slug}.echo" for n in found), f"expected mcp.{slug}.echo, saw {found}"
    finally:
        _delete_connector(net, ip, name)


# ---- end-to-end: an approved server's tool round-trips through a turn --------
def test_device_mcp_echo_round_trip(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    url = _local_url_or_skip()
    name = "hiltest_everything"
    marker = new_marker("mcp-echo")
    try:
        _add_device_mcp(net, ip, name, url, approved=True)
        deadline = time.time() + 40
        while time.time() < deadline and not _mcp_tool_names(net, ip, "hiltest_everything"):
            _kick_sync(net, ip)
        turn = run_turn(
            net,
            ip,
            f"Use the echo tool from the hiltest_everything MCP server to echo exactly this text: {marker}",
        )
        skip_if_unavailable(turn, "device MCP (everything)")
        assert marker in reply_text(turn), f"echo marker did not round-trip; reply: {reply_text(turn)[:200]!r}"
    finally:
        _delete_connector(net, ip, name)


# ---- Linear MCP: create + read an issue in a scratch team --------------------
def test_device_mcp_linear_scratch_issue(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = os.environ.get(_LINEAR_TOKEN_ENV, "").strip()
    if not tok:
        pytest.skip(f"set {_LINEAR_TOKEN_ENV} to a scratch Linear API token to run the Linear MCP leg")
    name = "hiltest_linear"
    title = new_marker("mcp-linear")
    try:
        _add_device_mcp(net, ip, name, _LINEAR_MCP_URL, approved=True, tok=tok)
        deadline = time.time() + 40
        while time.time() < deadline and not _mcp_tool_names(net, ip, "hiltest_linear"):
            _kick_sync(net, ip)
        assert _mcp_tool_names(net, ip, "hiltest_linear"), "Linear MCP exposed no tools (check the token/scope)"
        turn = run_turn(
            net,
            ip,
            f"Using the hiltest_linear MCP server, create an issue titled '{title}' in a scratch/test team, "
            "then read it back and quote its title.",
        )
        skip_if_unavailable(turn, "Linear MCP")
        assert title in reply_text(turn), f"Linear issue title did not round-trip; reply: {reply_text(turn)[:200]!r}"
    finally:
        _delete_connector(net, ip, name)
