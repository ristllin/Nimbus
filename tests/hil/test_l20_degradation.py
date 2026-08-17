"""§L20 - degradation: what the device does when the storage under it goes away.

The v3.7.0 features all assume a card: files live on it, media is refused
without it, quotas are counted against it. A card can be absent at boot, or -
worse - vanish mid-run from a cold joint, which is Board 1's documented history.
Every one of these paths must degrade to something a person can act on, and
none of them may crash, hang, or silently accept data it cannot keep.

The matrix is driven by ``FAULT sd`` over the LAN rather than by pulling a card,
so it runs unattended and repeatably. The fault registry is TEST-only, so none
of this exists in a production build.

⚠ Every test restores the fault mask in a ``finally``. A leaked ``FAULT sd``
would leave the owner's device pretending it has no card until the next reboot,
and every downstream suite would then fail for the wrong reason.

Markers: ``hil`` + ``net`` - pure LAN.
"""

from __future__ import annotations

import json
import os
import uuid

import pytest

try:
    import requests
except ImportError:  # pragma: no cover
    requests = None

pytestmark = [pytest.mark.hil, pytest.mark.net]

CHAT = "920001"


def _u(rig, path):
    ip, tok = rig
    sep = "&" if "?" in path else "?"
    return f"http://{ip}{path}{sep}t={tok}"


def _fault(rig, cap, on):
    r = requests.post(_u(rig, "/api/fault"), data={"cap": cap, "on": "1" if on else "0"}, timeout=10)
    assert r.status_code == 200, f"fault {cap}={on}: {r.status_code} {r.text}"


def _state(rig):
    return requests.get(_u(rig, "/api/state"), timeout=10).json()


def _call(rig, chat, tool, args=None):
    body = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args or {}}}
    )
    r = requests.post(_u(rig, "/api/test/astool"), data={"chat": chat, "body": body}, timeout=25)
    assert r.status_code == 200, r.text
    return json.dumps(r.json())


@pytest.fixture(scope="module")
def rig():
    if requests is None:
        pytest.skip("requests not installed")
    ip = os.environ.get("NIMBUS_TEST_IP")
    tok = os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        pytest.skip("set NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN to run L20")
    handle = (ip, tok)
    st = requests.get(_u(handle, "/api/state"), timeout=10)
    if st.status_code != 200 or st.json().get("mode") != 1:
        pytest.skip("Orchestrator mode required (MODE 1)")
    if requests.post(_u(handle, "/api/fault"), data={"cap": "all", "on": "0"}, timeout=10).status_code != 200:
        pytest.skip("no fault registry - flash the [env:test] build")
    yield handle
    # RESTORE, always: a leaked fault mask outlives this run.
    try:
        requests.post(_u(handle, "/api/fault"), data={"cap": "all", "on": "0"}, timeout=10)
        # REMOVE, not set-unknown: setRole upserts, so revoking an already-removed
        # row recreates it and the board accumulates dead tenants.
        requests.post(_u(handle, "/api/telegram/remove"), data={"id": CHAT}, timeout=10)
        requests.post(_u(handle, "/api/tenant"), data={"id": CHAT, "remove": "1"}, timeout=10)
    except Exception:  # noqa: BLE001
        pass


@pytest.fixture
def sd_lost(rig):
    """Run the body with the card simulated-absent, then put it back."""
    _fault(rig, "sd", True)
    try:
        yield rig
    finally:
        _fault(rig, "all", False)


@pytest.fixture
def tenant(rig):
    # Approve first: a role can only be given to someone already allow-listed.
    requests.post(_u(rig, "/api/telegram/add"), data={"id": CHAT, "name": "l20"}, timeout=10)
    requests.post(_u(rig, "/api/tenant"), data={"id": CHAT, "role": "user"}, timeout=10)
    yield rig
    requests.post(_u(rig, "/api/tenant"), data={"id": CHAT, "role": "unknown"}, timeout=10)
    requests.post(_u(rig, "/api/telegram/remove"), data={"id": CHAT}, timeout=10)


# ---------------------------------------------------------------------------
# the card going away
# ---------------------------------------------------------------------------


def test_losing_the_card_is_reported_not_hidden(sd_lost):
    """A person can only act on a problem they are told about."""
    st = _state(sd_lost)
    assert st.get("memSd") is False, "the device still claims the card is there"
    assert st.get("sdLost") is True or st.get("memSd") is False


def test_the_device_keeps_serving_with_no_card(sd_lost):
    """Degrade, never die: every surface answers."""
    for path in ("/api/state", "/api/orch", "/api/tenant", "/api/files/list"):
        r = requests.get(_u(sd_lost, path), timeout=15)
        assert r.status_code == 200, f"{path} returned {r.status_code} with no card"


def test_files_are_refused_clearly_rather_than_half_accepted(sd_lost, tenant):
    """The owner's rule: no card, no file - and say so."""
    out = _call(sd_lost, CHAT, "artifact.save", {"project": "l20", "name": "x.txt", "text": "should not land"})
    assert "isError" in out and "true" in out.lower(), f"a file was accepted with no card to keep it on: {out}"
    # The refusal must be legible, not a bare code.
    assert any(w in out.lower() for w in ("sd", "card", "storage")), out

    listing = requests.get(_u(sd_lost, "/api/files/list"), timeout=15).json()
    assert listing.get("present") is False
    assert listing.get("files") == []


def test_conversation_still_works_without_the_card(sd_lost, tenant):
    """Losing storage must not lose the ability to talk."""
    out = _call(sd_lost, CHAT, "memory.episodic", {"limit": 5})
    assert "isError" not in out or "true" not in out.lower(), out
    # And a memory write still lands somewhere (the RAM ring / capped store).
    w = _call(sd_lost, CHAT, "memory.write", {"content": f"L20 degraded note {uuid.uuid4().hex[:8]}"})
    assert "isError" not in w or "true" not in w.lower(), (
        f"the device could not remember anything at all with no card: {w}"
    )


def test_roles_survive_a_missing_card(sd_lost, tenant):
    """Tenancy lives on internal flash, NOT the card - losing the card must not
    silently drop everyone's role, which would either lock the owner out or open
    the device up. Both are worse than the missing card."""
    tenants = requests.get(_u(sd_lost, "/api/tenant"), timeout=15).json()
    assert tenants.get("admins", 0) >= 1, "no admin while the card is gone"
    roles = {t["id"]: t["role"] for t in tenants.get("tenants", [])}
    assert roles.get(CHAT) == "user", "a tenant's role vanished with the card"


def test_privacy_still_holds_with_no_card(sd_lost, tenant):
    """The boundary must not be a feature of the storage tier.

    Degraded mode swaps stores; if the namespace filter lived in the SD path
    only, losing the card would silently make everything readable by everyone.
    """
    other = "920002"
    requests.post(_u(sd_lost, "/api/telegram/add"), data={"id": other, "name": "l20b"}, timeout=10)
    requests.post(_u(sd_lost, "/api/tenant"), data={"id": other, "role": "user"}, timeout=10)
    try:
        secret = f"L20 degraded vault code is ZULU-{uuid.uuid4().hex[:8].upper()}"
        marker = secret.rsplit(" ", 1)[1]
        _call(sd_lost, CHAT, "memory.write", {"content": secret})
        got = _call(sd_lost, other, "memory.search", {"query": secret, "n_results": 20})
        assert marker not in got, "LEAK: the privacy boundary depends on the SD card"
    finally:
        requests.post(_u(sd_lost, "/api/tenant"), data={"id": other, "role": "unknown"}, timeout=10)
        requests.post(_u(sd_lost, "/api/telegram/remove"), data={"id": other}, timeout=10)


def test_recovery_restores_the_card_and_the_files(rig, tenant):
    """Promote back: what was written before the fault is still there."""
    name = f"pre-{uuid.uuid4().hex[:8]}.txt"
    marker = f"L20-{uuid.uuid4().hex[:8]}"
    before = _call(rig, CHAT, "artifact.save", {"project": "l20", "name": name, "text": f"kept {marker}"})
    if "isError" in before and "true" in before.lower():
        pytest.skip("no SD card on this board - nothing to lose or recover")
    try:
        _fault(rig, "sd", True)
        assert _state(rig).get("memSd") is False
        # Invisible while the card is away...
        assert name not in json.dumps(requests.get(_u(rig, "/api/files/list"), timeout=15).json())
    finally:
        _fault(rig, "all", False)

    # ...and back afterwards, with its contents intact.
    listing = json.dumps(requests.get(_u(rig, "/api/files/list?project=l20"), timeout=15).json())
    assert name in listing, "a file did not come back after the card returned"
    body = requests.get(_u(rig, f"/api/files/dl?project=l20&name={name}"), timeout=15).text
    assert marker in body, "the file came back empty or corrupted"
    requests.post(_u(rig, "/api/files/rm"), data={"project": "l20", "name": name}, timeout=10)


# ---------------------------------------------------------------------------
# other capabilities dropping out
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("cap", ["memory", "mic", "speaker", "led", "screen"])
def test_each_capability_can_vanish_without_taking_the_device_down(rig, cap):
    """One missing part must never take the whole device with it."""
    _fault(rig, cap, True)
    try:
        st = requests.get(_u(rig, "/api/state"), timeout=15)
        assert st.status_code == 200, f"the device stopped serving with {cap} faulted"
        # And it reports the fault rather than pretending to be healthy.
        faults = st.json().get("faults") or {}
        assert faults.get(cap) is True, f"/api/state does not report {cap} as faulted"
    finally:
        _fault(rig, "all", False)
    assert requests.get(_u(rig, "/api/state"), timeout=15).status_code == 200


def test_a_turn_survives_losing_the_memory_subsystem(rig, tenant):
    """With memory faulted, tools fail cleanly - they do not hang or crash."""
    _fault(rig, "memory", True)
    try:
        out = _call(rig, CHAT, "memory.search", {"query": "anything", "n_results": 5})
        assert out, "memory.search returned nothing at all (hang or crash)"
        assert requests.get(_u(rig, "/api/state"), timeout=15).status_code == 200
    finally:
        _fault(rig, "all", False)


def test_faults_clear_completely(rig):
    """cap=all is the escape hatch every test depends on - prove it works."""
    for cap in ("sd", "memory", "mic"):
        _fault(rig, cap, True)
    _fault(rig, "all", False)
    st = _state(rig)
    # /api/state reports faults as a MAP of capability -> bool, not a bitmask.
    faults = st.get("faults") or {}
    stuck = [k for k, v in faults.items() if v]
    assert not stuck, f"faults did not clear: {stuck}"
    assert st.get("memSd") is True or st.get("memSd") is None
