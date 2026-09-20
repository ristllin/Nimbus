"""Shared web-tier auth rig for the L15-L20 LAN suites (CUM-418 item 1).

Since CUM-45 the device rejects the web token passed as a ``?t=`` query string
(``src/net/webui.cpp`` ``webAuthOk``): only the ``X-Nimbus-Token`` HEADER (or a
form field) is accepted. The L15-L20 tiers historically built ``?t=`` URLs, so
every request 401'd, and each tier's rig read that 401 as "not Orchestrator mode"
and skipped the WHOLE tier green - the tier reported green-by-skip in every run
since CUM-45 (verified live: ``?t=`` -> 401, header -> 200 on the same board and
token).

This module is the one place the auth contract lives (the same one
``tests/hil/net.py`` already uses): a shared ``requests.Session`` that carries the
token as the header on every request, and a gate that tells the three outcomes
APART instead of collapsing them to one skip:

  * env not set / board unreachable  -> skip (nothing to run against)
  * 401                              -> FAIL loudly (the token was rejected - the
                                        exact regression that skipped the tier green)
  * 200 but mode != 1                -> skip "Orchestrator mode required (MODE 1)"

Nothing here touches the network at import time, so ``pytest --collect-only`` stays
clean on a device-less box.
"""

from __future__ import annotations

import os

import pytest

try:
    import requests
except ImportError:  # pragma: no cover - collection must still succeed
    requests = None


def make_session(tok: str):
    """A requests.Session that authenticates every request with the device web
    token as the ``X-Nimbus-Token`` header (never a ``?t=`` query, which the device
    rejects since CUM-45). One session per tier keeps the connection warm and puts
    the auth contract in exactly one place."""
    s = requests.Session()
    s.headers["X-Nimbus-Token"] = tok
    return s


def ip_tok_or_skip(label: str):
    """(ip, tok) from NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN, or a loud skip."""
    if requests is None:
        pytest.skip("requests not installed")
    ip = os.environ.get("NIMBUS_TEST_IP")
    tok = os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        pytest.skip(f"set NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN (WEBTOK?) to run {label}")
    return ip, tok


def require_orchestrator(session, ip: str, label: str) -> dict:
    """GET /api/state through the header-authed ``session`` and gate the tier,
    telling a REJECTED token (401 -> loud fail) apart from a healthy board that is
    simply not in Orchestrator mode (loud skip). Returns the parsed /api/state.

    A 401 here is never a skip: reading it as "not MODE 1" is the precise bug that
    skipped these tiers green from CUM-45 until CUM-418."""
    try:
        r = session.get(f"http://{ip}/api/state", timeout=10)
    except requests.RequestException as exc:  # configured but unreachable
        pytest.fail(f"{label}: NIMBUS_TEST_IP is set but /api/state is unreachable: {exc}")
    if r.status_code == 401:
        pytest.fail(
            f"{label}: /api/state returned 401 with the X-Nimbus-Token header - the "
            "web token was rejected. Refresh NIMBUS_TEST_TOKEN from WEBTOK? on the "
            "target board. A 401 must NOT be read as 'not Orchestrator mode' and "
            "skipped (that skipped this whole tier green from CUM-45 until CUM-418)."
        )
    if r.status_code != 200:
        pytest.fail(f"{label}: /api/state -> {r.status_code} (expected 200): {r.text[:160]}")
    st = r.json()
    if st.get("mode") != 1:
        pytest.skip("Orchestrator mode required (MODE 1)")
    return st
