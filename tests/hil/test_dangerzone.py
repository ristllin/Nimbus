"""Danger zone v2 (CUM-15) - NON-DESTRUCTIVE confirm-gating LAN tests.

Verifies the guardrails on the destructive endpoints WITHOUT ever triggering them:
each of ``/api/factory-reset``, ``/api/sdreset``, ``/api/sdformat`` must reject a
missing/wrong confirm phrase (400) and an unauthenticated call (401) BEFORE the
deferred erase hook runs. We deliberately never send the correct phrase - the actual
5x factory-reset acceptance is a bench step run with an esptool NVS snapshot taken
first (Freenove), and the SD-format acceptance waits on a scratch card.

Markers: ``@pytest.mark.net`` (LAN + serial for the token).
"""

from __future__ import annotations

import pytest

from test_l4_network import lan_ip_or_skip
from test_l9_resilience import _webtok

try:  # keep `pytest --collect-only` clean on a host without requests
    import requests
except ImportError:  # pragma: no cover
    requests = None

# Every destructive route + the phrase it wants. We send WRONG/absent phrases only.
ROUTES = [
    ("/api/factory-reset", "FACTORY RESET"),
    ("/api/sdreset", "ERASE STORAGE"),
    ("/api/sdformat", "FORMAT CARD"),
]


def _need_requests():
    if requests is None:
        pytest.skip("requests not installed")


@pytest.mark.net
@pytest.mark.parametrize("path,phrase", ROUTES)
def test_danger_requires_auth(device, net, secrets, require_secret, path, phrase):
    """No token -> 401, before anything is erased."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    # No ?t= and no confirm: must be rejected at the auth gate.
    r = requests.post(f"http://{ip}{path}", timeout=8)
    assert r.status_code == 401, f"{path} unauthenticated -> {r.status_code}, want 401"


@pytest.mark.net
@pytest.mark.parametrize("path,phrase", ROUTES)
def test_danger_requires_exact_confirm(device, net, secrets, require_secret, path, phrase):
    """Authenticated but missing / wrong / lowercase confirm -> 400, never erases."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    base = f"http://{ip}{path}"
    # CUM-45 took the token out of URLs: auth rides the X-Nimbus-Token header
    # (or a form-body `t`), never a `?t=` query param.
    hdr = {"X-Nimbus-Token": tok}
    # missing confirm
    assert requests.post(base, headers=hdr, timeout=8).status_code == 400
    # wrong phrase (another action's word, or a partial)
    assert requests.post(base, headers=hdr, data={"confirm": "NOPE"}, timeout=8).status_code == 400
    assert requests.post(base, headers=hdr, data={"confirm": phrase.lower()}, timeout=8).status_code == 400
    assert requests.post(base, headers=hdr, data={"confirm": phrase[:-1]}, timeout=8).status_code == 400
    # A different action's exact phrase must NOT satisfy this route.
    for _, other in ROUTES:
        if other != phrase:
            assert requests.post(base, headers=hdr, data={"confirm": other}, timeout=8).status_code == 400
