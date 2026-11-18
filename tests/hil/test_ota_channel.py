"""The owner's update channel switch (RB-OTA2), on hardware over the LAN.

GET /api/ota/channel shows the channel; POST /api/ota/channel with the form field
channel=candidate|stable sets it (otaupd::setChannel, which fails closed). The route has
the checks every owner route has: the device sign-in code in the X-Nimbus-Token header
(a ?t= query never counts), GET and POST only, and the value from the POST body only.
/ota candidate|stable is the same switch from the owner's chat; the web chat is the
owner's own channel, so it runs there too (the member refusal is host-tested in
test/test_ota_channel and test_orch_command_parse).

Every leg leaves the device on the stable channel. Up to RB-OTA1 there is no route: the
first GET fails there, as it must. No paid turn: commands never reach the model.

Markers: net. Needs --allow-hardware, NIMBUS_TEST_IP and NIMBUS_TEST_TOKEN on a board
running a build with RB-OTA2. The /ota leg needs Orchestrator mode (else it skips).
"""

from __future__ import annotations

import pytest

from webrig import chat_turn, ip_tok_or_skip, make_session, require_orchestrator, requests

pytestmark = [pytest.mark.net]

ROUTE = "/api/ota/channel"
STABLE_LINE = "Update channel: stable "
CANDIDATE_LINE = "Update channel: candidate "


@pytest.fixture(scope="module")
def owner():
    ip, tok = ip_tok_or_skip("the update channel switch")
    s = make_session(tok)
    r = s.get(f"http://{ip}{ROUTE}", timeout=10)
    assert r.status_code == 200, f"GET {ROUTE}: HTTP {r.status_code} (no owner switch on this image)"
    yield ip, tok, s
    # Whatever happened above, the device goes back to stable.
    r = s.post(f"http://{ip}{ROUTE}", data={"channel": "stable"}, timeout=10)
    assert r.status_code == 200 and r.json()["channel"] == "stable", r.text


def _channel(ip: str, s) -> str:
    r = s.get(f"http://{ip}{ROUTE}", timeout=10)
    assert r.status_code == 200, r.text
    j = r.json()
    assert j["line"].startswith(f"Update channel: {j['channel']} "), j
    return j["channel"]


def _post(ip: str, s, value: str):
    return s.post(f"http://{ip}{ROUTE}", data={"channel": value}, timeout=10)


def test_set_and_show_both_ways(owner):
    ip, _tok, s = owner
    r = _post(ip, s, "candidate")
    assert r.status_code == 200, r.text
    j = r.json()
    assert j["ok"] is True and j["channel"] == "candidate" and j["line"].startswith(CANDIDATE_LINE), j
    assert _channel(ip, s) == "candidate"
    r = _post(ip, s, "stable")
    assert r.status_code == 200 and r.json()["channel"] == "stable", r.text
    assert r.json()["line"].startswith(STABLE_LINE)
    assert _channel(ip, s) == "stable"


def test_no_sign_in_code_changes_nothing(owner):
    ip, tok, s = owner
    bare = requests.Session()
    for method in ("get", "post"):
        r = getattr(bare, method)(f"http://{ip}{ROUTE}", data={"channel": "candidate"}, timeout=10)
        assert r.status_code == 401, f"{method} without the code: {r.status_code}"
        assert "channel" not in r.text
    # A query token never counts (CUM-45), nor a wrong code in the header.
    r = bare.post(f"http://{ip}{ROUTE}", params={"t": tok}, data={"channel": "candidate"}, timeout=10)
    assert r.status_code == 401
    r = bare.post(
        f"http://{ip}{ROUTE}", headers={"X-Nimbus-Token": "0" * len(tok)}, data={"channel": "candidate"}, timeout=10
    )
    assert r.status_code == 401
    assert _channel(ip, s) == "stable"


def test_wrong_method_or_value_changes_nothing(owner):
    ip, _tok, s = owner
    # A GET never sets, whatever it carries.
    r = s.get(f"http://{ip}{ROUTE}", params={"channel": "candidate"}, timeout=10)
    assert r.status_code == 200 and r.json()["channel"] == "stable", r.text
    # Only GET and POST are bound: anything else meets the catch-all, which redirects a
    # LAN peer to the bare page.
    for method in ("PUT", "DELETE", "PATCH"):
        r = s.request(method, f"http://{ip}{ROUTE}", data={"channel": "candidate"}, timeout=10, allow_redirects=False)
        assert r.status_code == 302 and r.headers.get("Location") == "/", f"{method}: {r.status_code}"
    # The value comes from the POST body only.
    r = s.post(f"http://{ip}{ROUTE}", params={"channel": "candidate"}, timeout=10)
    assert r.status_code == 400 and r.json()["ok"] is False, r.text
    for bad in ("", "beta", "candidate x"):
        r = _post(ip, s, bad)
        assert r.status_code == 400 and r.json()["channel"] == "stable", f"{bad!r}: {r.text}"
    assert _channel(ip, s) == "stable"


def test_ota_command_from_the_owner_chat(owner):
    ip, _tok, s = owner
    require_orchestrator(s, ip, "the /ota command")
    reply = chat_turn(s, ip, "/ota candidate", timeout_s=30)
    assert CANDIDATE_LINE in reply, reply
    assert _channel(ip, s) == "candidate"
    reply = chat_turn(s, ip, "/ota", timeout_s=30)
    assert CANDIDATE_LINE in reply, reply
    reply = chat_turn(s, ip, "/ota stable", timeout_s=30)
    assert STABLE_LINE in reply, reply
    assert _channel(ip, s) == "stable"
    reply = chat_turn(s, ip, "/ota beta", timeout_s=30)
    assert "/ota candidate" in reply, reply
    assert _channel(ip, s) == "stable"
