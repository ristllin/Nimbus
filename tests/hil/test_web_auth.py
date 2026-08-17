"""Web auth gate - the no-bypass property, locked (the HIL test spec).

Born from a real owner report: *"on a browser, the raw IP lets me straight in but
nimbus.local asks for the token - is that a security breach?"* It was not. The gate
never looks at the request's ``Host``/``Origin``/``Referer``/client IP; the browser
simply stores the token per ORIGIN (``localStorage``), so a browser that identified at
``http://<ip>`` is authenticated there while ``http://<name>.local`` - a different
origin - still shows the gate. Same device, same gate, two storage buckets.

That is easy to *re-break* (one host-conditional shortcut in a handler and raw-IP
requests become trusted), and the symptom looks like a UX quirk rather than a hole.
These tests assert the property directly against real hardware over the LAN:

  * every ``/api`` route 401s with no token and 200s with one - reads included,
  * the answer does not change when the SAME device is addressed by a different
    host string (raw IP vs mDNS name vs a bogus Host header),
  * the two deliberately-ungated responses (``GET /`` and ``/logo.svg``) leak no
    device data - in particular the page shell must never contain the token,
  * ``/api/connect`` hands out a token-bearing URL for EVERY reachable origin, which
    is what makes both addresses sign in with one click.

Markers: ``net`` (LAN + a token). Non-destructive: reads only, no device state changed.
"""

from __future__ import annotations

import pytest

from test_l4_network import lan_ip_or_skip

pytestmark = pytest.mark.net

# Reads that must all be gated. Chosen to span the handler families (webui.cpp,
# web_memory.cpp) so a regression in any one of them trips a test.
GATED_GETS = ["/api/state", "/api/log", "/api/connect", "/api/orch"]


def _requests():
    try:
        import requests  # noqa: PLC0415 - optional harness dep
    except ImportError:  # pragma: no cover
        pytest.skip("requests not installed; `pip install requests`")
    return requests


def test_every_api_get_requires_a_token(device, net, secrets, require_secret):
    """No token -> 401, valid token -> 200, on every gated read.

    This is the assertion that would fail if someone reintroduced an open GET.
    """
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    if not net.token():
        pytest.skip("no web auth token available (set NIMBUS_TEST_TOKEN or use serial)")

    for path in GATED_GETS:
        bare = net.get(path, ip=ip, auth=False, timeout=6.0)
        assert bare.status_code == 401, (
            f"{path} answered {bare.status_code} WITHOUT a token - the auth gate is "
            f"open on this route (body: {bare.text[:160]!r})"
        )

        authed = net.get(path, ip=ip, auth=True, timeout=6.0)
        assert authed.status_code == 200, (
            f"{path} answered {authed.status_code} WITH a valid token - the gate is "
            f"rejecting a legitimate caller (body: {authed.text[:160]!r})"
        )


def test_gate_ignores_the_host_header(device, net, secrets, require_secret):
    """The SAME device, addressed under different host strings, answers identically.

    Sending a `Host:` of `nimbus.local` (and a bogus one) to the raw IP proves the
    decision is token-only. If a host-conditional shortcut ever lands, the raw-IP
    request starts answering 200 unauthenticated and this fails loudly.
    """
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    req = _requests()
    url = f"http://{ip}/api/state"

    for host in ("nimbus.local", "localhost", "evil.example.com"):
        r = req.get(url, headers={"Host": host}, timeout=6.0)
        assert r.status_code == 401, (
            f"GET /api/state with Host:{host} and NO token answered {r.status_code} - "
            "auth must never depend on the host string"
        )


def test_ungated_shell_leaks_no_device_data(device, net, secrets, require_secret):
    """`GET /` and `/logo.svg` are served without a token (the identify gate itself
    needs them) - so assert they carry nothing sensitive. The token must never appear
    in the shell."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = net.token()
    if not tok:
        pytest.skip("no web auth token available to check the shell against")

    shell = net.get("/", ip=ip, auth=False, timeout=8.0)
    assert shell.status_code == 200, "GET / must stay reachable - it IS the identify gate"
    body = shell.text
    assert tok not in body, (
        "the ACCESS TOKEN appears in the unauthenticated page shell - anyone who can "
        "reach the device could read it straight out of the HTML"
    )
    # The shell is static: live values arrive later over gated fetches. Spot-check that
    # the AP password isn't baked in either.
    assert "nimbus1234" not in body, "the setup-AP password is baked into the open shell"


def test_connect_offers_a_token_url_per_origin(device, net, secrets, require_secret):
    """`/api/connect` must hand out a sign-in URL for EVERY reachable origin.

    The browser stores the token per origin, so one URL cannot authenticate both
    addresses. Regression guard for the mislabelled link that sent "nimbus.local" to
    the raw-IP URL and left the name permanently asking for a token.
    """
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    if not net.token():
        pytest.skip("no web auth token available")

    c = net.get_json("/api/connect", ip=ip, timeout=8.0)
    tok = c.get("token", "")
    assert tok, "/api/connect returned no token for an authenticated caller"

    url = c.get("url", "")
    assert url.startswith("http://") and f"t={tok}" in url, (
        f"/api/connect 'url' is not a token-bearing sign-in URL: {url!r}"
    )

    assert f"//{ip}/" in url, f"the IP sign-in URL {url!r} does not address the IP {ip} it was fetched from"

    # lan_ip_or_skip() already proved STA is up, which is exactly the condition under
    # which the device emits mdnsUrl - so a MISSING mdnsUrl is the regression, not a
    # reason to skip. (Skipping here would let a revert of this fix report green.)
    mdns, mdns_url = c.get("mdns", ""), c.get("mdnsUrl", "")
    assert mdns_url, (
        f"device is on the LAN at {ip} but /api/connect offered no mDNS sign-in URL "
        f"(mdns={mdns!r}) - nimbus.local would have no way to obtain a token and would "
        "show the identify gate forever"
    )
    assert f"t={tok}" in mdns_url, (
        f"the mDNS sign-in URL carries no token: {mdns_url!r} - visiting it would "
        "land on the identify gate, which is the exact bug this guards"
    )
    assert mdns and mdns in mdns_url, (
        f"the mDNS URL {mdns_url!r} does not address the advertised name {mdns!r} - a "
        "link labelled with one host must point at that same host"
    )
    # The whole point of two URLs: they must address DIFFERENT origins, since the
    # browser stores the token per origin and one URL cannot sign in at both.
    assert mdns_url != url, (
        "the mDNS and IP sign-in URLs are identical - one origin is left with no way to obtain a token"
    )
