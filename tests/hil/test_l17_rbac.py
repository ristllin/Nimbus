"""§L17 - RBAC on real hardware: roles, quotas, persistence, and the rails.

The scripted version of the hand-run Board 2 verification (2026-07-27). Covers
what the owner asked for by name: create/update/remove tenants, upgrade and
downgrade, per-tenant storage limits, and that a role change actually survives
a restart rather than living in RAM.

Every mutation is restored in a ``finally`` - a leaked test tenant would sit in
the owner's device forever, and a leaked role change could lock the device's
real admin out.

Markers: ``hil`` + ``net`` - PURE LAN (``/api/tenant`` + ``/api/test/*``); no
serial, because repeated console opens wedge the host CDC driver.
"""

from __future__ import annotations

import time

import pytest

from webrig import ip_tok_or_skip, make_session, require_orchestrator

pytestmark = [pytest.mark.hil, pytest.mark.net]

TEST_CHAT = "999000777"  # synthetic; never a real Telegram chat

# The shared header-authed session for this tier, built by the ``rig`` fixture.
_S = None


def _url(ip, tok, path):
    # Token rides the X-Nimbus-Token header on _S, never a ?t= query.
    return f"http://{ip}{path}"


@pytest.fixture
def rig():
    global _S
    ip, tok = ip_tok_or_skip("L17")
    _S = make_session(tok)
    require_orchestrator(_S, ip, "L17")
    if _S.get(_url(ip, tok, "/api/tenant"), timeout=10).status_code != 200:
        pytest.skip("board predates the RBAC build - flash current main first")
    # A role can only be given to someone already approved - the device refuses
    # to pre-seed a role on a chat nobody has admitted. So the fixture performs
    # the real first step.
    _S.post(_url(ip, tok, "/api/telegram/add"), data={"id": TEST_CHAT, "name": "l17"}, timeout=10)
    yield ip, tok
    # RESTORE: the synthetic tenant must never outlive the run - revoked AND
    # off the allowlist.
    _S.post(_url(ip, tok, "/api/tenant"), data={"id": TEST_CHAT, "role": "unknown"}, timeout=10)
    _S.post(_url(ip, tok, "/api/telegram/remove"), data={"id": TEST_CHAT}, timeout=10)
    # ...and drop the row itself. setRole UPSERTS, so a bare "revoke" leaves a
    # dead tenant occupying one of 32 slots on the owner's device forever.
    _S.post(_url(ip, tok, "/api/tenant"), data={"id": TEST_CHAT, "remove": "1"}, timeout=10)


def _tenants(ip, tok) -> dict:
    j = _S.get(_url(ip, tok, "/api/tenant"), timeout=10).json()
    return {t["id"]: t for t in j.get("tenants", [])}, j.get("admins", 0)


def test_legacy_adoption_left_the_device_administrable(rig):
    """An upgraded device must come up with at least one admin, always."""
    ip, tok = rig
    tenants, admins = _tenants(ip, tok)
    assert admins >= 1, "no admin after adoption - the device would be unmanageable"
    assert any(t["role"] == "admin" for t in tenants.values())


def test_tenant_lifecycle_create_update_downgrade_remove(rig):
    """create -> quota -> upgrade -> downgrade -> revoke, verified at each step."""
    ip, tok = rig

    # CREATE as guest
    r = _S.post(_url(ip, tok, "/api/tenant"), data={"id": TEST_CHAT, "role": "guest"}, timeout=10)
    assert r.status_code == 200, r.text
    tenants, _ = _tenants(ip, tok)
    assert tenants[TEST_CHAT]["role"] == "guest"
    # A guest's defaults are the tight ones - and pins are zero by design,
    # because a pin outlives any quota.
    assert tenants[TEST_CHAT]["pins"] == 0
    guest_ttl = tenants[TEST_CHAT]["ttl"]

    # UPDATE the quota
    r = _S.post(
        _url(ip, tok, "/api/tenant"),
        data={"id": TEST_CHAT, "vectors": "3", "bytes": "65536", "ttl": "48", "pins": "0"},
        timeout=10,
    )
    assert r.status_code == 200
    tenants, _ = _tenants(ip, tok)
    assert tenants[TEST_CHAT]["vectors"] == 3
    assert tenants[TEST_CHAT]["bytes"] == 65536
    assert tenants[TEST_CHAT]["ttl"] == 48

    # UPGRADE to user - the role changes and the explicit quota is retained
    # (an admin's deliberate setting is not silently reset by a promotion).
    r = _S.post(_url(ip, tok, "/api/tenant"), data={"id": TEST_CHAT, "role": "user"}, timeout=10)
    assert r.status_code == 200
    tenants, _ = _tenants(ip, tok)
    assert tenants[TEST_CHAT]["role"] == "user"
    assert tenants[TEST_CHAT]["vectors"] == 3

    # DOWNGRADE back to guest, then REVOKE
    assert (
        _S.post(_url(ip, tok, "/api/tenant"), data={"id": TEST_CHAT, "role": "guest"}, timeout=10).status_code
        == 200
    )
    assert _tenants(ip, tok)[0][TEST_CHAT]["role"] == "guest"
    assert (
        _S.post(_url(ip, tok, "/api/tenant"), data={"id": TEST_CHAT, "role": "unknown"}, timeout=10).status_code
        == 200
    )
    tenants, _ = _tenants(ip, tok)
    # Revoked means no access - the row may remain (the admin can still see
    # what they stored) but the role must be unknown.
    assert TEST_CHAT not in tenants or tenants[TEST_CHAT]["role"] == "unknown"
    assert guest_ttl > 0


def test_last_admin_cannot_be_demoted_on_hardware(rig):
    """The device must never become unadministrable - with a readable reason."""
    ip, tok = rig
    tenants, admins = _tenants(ip, tok)
    if admins != 1:
        pytest.skip(f"{admins} admins configured - this asserts the single-admin guard")
    admin_id = next(i for i, t in tenants.items() if t["role"] == "admin")
    r = _S.post(_url(ip, tok, "/api/tenant"), data={"id": admin_id, "role": "user"}, timeout=10)
    assert r.status_code == 409, f"demoting the only admin returned {r.status_code}"
    assert "only admin" in r.json().get("error", "")
    # ...and it really did NOT change.
    tenants, admins = _tenants(ip, tok)
    assert tenants[admin_id]["role"] == "admin" and admins == 1


def test_roles_and_quotas_survive_a_restart(rig):
    """Roles live on flash, not in RAM: prove it with a real reboot."""
    ip, tok = rig
    _S.post(_url(ip, tok, "/api/tenant"), data={"id": TEST_CHAT, "role": "guest"}, timeout=10)
    _S.post(_url(ip, tok, "/api/tenant"), data={"id": TEST_CHAT, "vectors": "7", "ttl": "24"}, timeout=10)

    r = _S.post(_url(ip, tok, "/api/test/reboot"), timeout=10)
    assert r.status_code == 202, "no non-destructive reboot seam on this build"

    time.sleep(5)
    deadline = time.time() + 120
    while time.time() < deadline:
        try:
            if _S.get(_url(ip, tok, "/api/state"), timeout=4).status_code == 200:
                break
        except Exception:  # noqa: BLE001 - still rebooting
            pass
        time.sleep(5)
    else:
        pytest.fail("device did not come back after the staged reboot")

    tenants, admins = _tenants(ip, tok)
    assert tenants[TEST_CHAT]["role"] == "guest", "role did not survive the restart"
    assert tenants[TEST_CHAT]["vectors"] == 7, "quota did not survive the restart"
    assert admins >= 1, "lost the admin across a restart"
