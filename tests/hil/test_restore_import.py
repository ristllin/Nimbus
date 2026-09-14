"""§N406 - memory restore import endpoint LAN tests (CUM-406).

Exercises POST /api/mem/import on real hardware: the token gate (an unauthenticated
import is refused BEFORE its body is buffered - the DoS-hardening fix), a dry-run
round-trip that writes nothing, and the honest per-tier reply shape. Marked
``@pytest.mark.net`` (needs the device on the LAN + serial for the token); the
orchestrator runs it on the bench.

Auth rides the ``X-Nimbus-Token`` header (CUM-45). These tests do not mutate the
store (dry-run only), so there is nothing to restore in teardown.
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


def _need_requests():
    if requests is None:
        pytest.skip("requests not installed")


def _import(ip, tok, payload, timeout=20):
    headers = {"Content-Type": "application/json"}
    if tok is not None:
        headers["X-Nimbus-Token"] = tok
    return requests.post(
        f"http://{ip}/api/mem/import", data=json.dumps(payload).encode(), headers=headers, timeout=timeout
    )


def test_import_unauth_refused(device, net, secrets, require_secret):
    """No token -> 401, and (the DoS fix) the body is never buffered before that gate."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    # A deliberately large-ish body: an unauth caller must be refused without the
    # device buffering it. We only assert the 401 here; the no-heap-growth property is
    # structural (auth is checked on the first chunk before the buffer is allocated).
    big = {"kind": "vectors", "dryRun": True, "entries": [{"id": f"x{i}", "content": "y" * 64} for i in range(200)]}
    r = _import(ip, None, big)
    assert r.status_code == 401, r.text
    assert r.json().get("ok") is False


def test_import_dryrun_roundtrip(device, net, secrets, require_secret):
    """A token-gated dry-run parses + reports and writes nothing."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    payload = {
        "kind": "vectors",
        "dryRun": True,
        "count": 1,
        "entries": [{"id": "hiltest-v1", "content": "restore probe", "importance": 0.5, "vec": "AQIDBA=="}],
    }
    r = _import(ip, tok, payload)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body.get("ok") is True
    assert body.get("kind") == "vectors"
    assert body.get("dryRun") is True


def test_import_count_mismatch_rejected(device, net, secrets, require_secret):
    """A declared count that disagrees with the array (a truncated body) is rejected."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    payload = {
        "kind": "vectors",
        "dryRun": True,
        "count": 9,
        "entries": [{"id": "hiltest-v1", "content": "x", "vec": "AQIDBA=="}],
    }
    r = _import(ip, tok, payload)
    assert r.status_code == 200, r.text
    assert r.json().get("ok") is False


def test_import_malformed_scratchpad_rejected(device, net, secrets, require_secret):
    """A null/absent scratchpad is rejected, never applied (which would wipe it)."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    r = _import(ip, tok, {"kind": "scratchpad", "dryRun": True, "scratchpad": None})
    assert r.status_code == 200, r.text
    assert r.json().get("ok") is False
