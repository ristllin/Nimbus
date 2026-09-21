"""§N410 - LAN MCP endpoint body gate (CUM-410).

POST /mcp used to buffer the whole JSON-RPC body on the internal heap BEFORE the
token check ran. It now mirrors the /api/mem/import gate (CUM-406): auth is decided
on the first body chunk (an unauthenticated caller is refused with nothing buffered),
and an authenticated body past the hard cap is refused with 413, never truncated.
Marked ``@pytest.mark.net`` (needs the device on the LAN + serial for the token).

These tests only call the read-only ``tools/list`` method and refused requests, so
nothing on the device changes and there is nothing to restore in teardown.
"""

from __future__ import annotations

import json

import pytest

from test_l4_network import lan_ip_or_skip
from test_l9_resilience import _webtok

try:  # keep `pytest --collect-only` clean on a host without requests
    import requests
except ImportError:  # pragma: no cover
    requests = None

pytestmark = pytest.mark.net

# Must exceed kMcpMaxBody (64 KB) in src/net/web_memory.cpp.
_OVER_CAP_BYTES = 80 * 1024


def _need_requests():
    if requests is None:
        pytest.skip("requests not installed")


def _mcp(ip, tok, raw: bytes, timeout=20):
    headers = {"Content-Type": "application/json"}
    if tok is not None:
        headers["X-Nimbus-Token"] = tok
    return requests.post(f"http://{ip}/mcp", data=raw, headers=headers, timeout=timeout)


def _tools_list(padding: int = 0) -> bytes:
    req = {"jsonrpc": "2.0", "id": 1, "method": "tools/list"}
    if padding:
        req["params"] = {"_pad": "x" * padding}
    return json.dumps(req).encode()


def test_mcp_unauth_large_body_refused(device, net, secrets, require_secret):
    """No token -> 401 even for a large body: the first-chunk gate drops every byte."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    r = _mcp(ip, None, _tools_list(padding=_OVER_CAP_BYTES))
    assert r.status_code == 401, r.text
    assert "auth required" in r.json().get("error", "")


def test_mcp_over_cap_body_refused_413(device, net, secrets, require_secret):
    """An authenticated body past the cap is refused (413), not silently truncated."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    r = _mcp(ip, tok, _tools_list(padding=_OVER_CAP_BYTES))
    assert r.status_code == 413, r.text
    assert "too large" in r.json().get("error", "")


def test_mcp_within_cap_still_served(device, net, secrets, require_secret):
    """Positive control: a normal authenticated tools/list is answered as before."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    r = _mcp(ip, tok, _tools_list())
    assert r.status_code == 200, r.text
    body = r.json()
    assert body.get("jsonrpc") == "2.0"
    assert "result" in body, body


def test_mcp_empty_body_is_400_not_crash(device, net, secrets, require_secret):
    """An authenticated empty POST (no body chunk ever ran) is a clean 400."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    r = _mcp(ip, tok, b"")
    assert r.status_code == 400, r.text
