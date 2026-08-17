"""L8 - Orchestrator control surface over the web (plan ROUND 3 Part A).

Exercises the /api/orch GET/POST + /api/verify routes added in webui.cpp:
state shape, secret non-echo, directive round-trip, priority sanitizing,
model-choice gating, and verify-endpoint validation. All ``net``-marked
(hardware + LAN gated); restores every mutated field in ``finally`` so a
failure can't poison downstream tests (the wrong-password lesson).

The live verify-against-provider test needs a real key on the DEVICE and is
marked ``agent`` on top of ``net`` - it stays deselected in the plain net run.
"""

from __future__ import annotations

import time

import pytest

from test_l4_network import lan_ip_or_skip


def _orch(net, ip):
    return net.get_json("/api/orch", ip=ip, timeout=5.0)


# ---- orch_state_shape -------------------------------------------------------
@pytest.mark.net
def test_orch_state_shape(device, net, secrets, require_secret):
    """GET /api/orch -> JSON with the full control-surface shape, and NO secret
    material: keys/token must appear only as has* booleans."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    st = _orch(net, ip)
    for key in ("running", "providers", "cust", "orchHost", "provPrio", "subPrio", "directive", "mem", "jobs", "hasTg"):
        assert key in st, f"/api/orch missing {key!r}: keys={sorted(st)}"
    for p in ("openai", "anthropic", "mistral"):
        prov = st["providers"][p]
        for key in ("hasKey", "verify", "vts", "orchModel", "subModel", "choices"):
            assert key in prov, f"providers.{p} missing {key!r}"
        assert isinstance(prov["hasKey"], bool)
        assert prov["verify"] in (-1, 0, 1)
    # SECRET NON-ECHO: no field anywhere in the payload may look like a key.
    body = str(st)
    for marker in ("sk-", "sk-ant-", "Bearer "):
        assert marker not in body, f"/api/orch leaked secret material ({marker!r})"


# ---- orch_directive_roundtrip ----------------------------------------------
@pytest.mark.net
def test_orch_directive_roundtrip(device, net, secrets, require_secret):
    """POST sysPrompt -> GET echoes it (NVS write path); restored after."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    before = _orch(net, ip)["directive"]
    probe = f"HIL directive probe {int(time.time())}"
    try:
        resp = net.post("/api/orch", {"sysPrompt": probe}, ip=ip)
        assert resp.status_code == 200, f"POST /api/orch -> {resp.status_code}"
        assert _orch(net, ip)["directive"] == probe
    finally:
        net.post("/api/orch", {"sysPrompt": before}, ip=ip)
        assert _orch(net, ip)["directive"] == before, "directive not restored!"


# ---- orch_priority_sanitized -------------------------------------------------
@pytest.mark.net
def test_orch_priority_sanitized(device, net, secrets, require_secret):
    """provPrio/subPrio: junk tokens are dropped server-side; an all-junk list
    is IGNORED (previous value survives) rather than persisted empty."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    st0 = _orch(net, ip)
    try:
        net.post("/api/orch", {"subPrio": " Anthropic , evilhost, openai ,openai"}, ip=ip)
        assert _orch(net, ip)["subPrio"] == "anthropic,openai", "expected lowercased/deduped/filtered list"
        net.post("/api/orch", {"subPrio": "evilhost,alsojunk"}, ip=ip)
        assert _orch(net, ip)["subPrio"] == "anthropic,openai", "all-junk list must be ignored, not persisted"
    finally:
        net.post("/api/orch", {"subPrio": st0["subPrio"]}, ip=ip)


# ---- orch_model_choice_gated -------------------------------------------------
@pytest.mark.net
def test_orch_model_choice_gated(device, net, secrets, require_secret):
    """orchM_<p>: a model outside the provider's choice list is rejected
    server-side (the 'verified options only' contract, enforced even against a
    hand-crafted POST that bypasses the disabled dropdown)."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    st0 = _orch(net, ip)["providers"]["openai"]["orchModel"]
    try:
        net.post("/api/orch", {"orchM_openai": "gpt-fake-model"}, ip=ip)
        assert _orch(net, ip)["providers"]["openai"]["orchModel"] == st0, "off-list model must not persist"
        choices = _orch(net, ip)["providers"]["openai"]["choices"].split(",")
        net.post("/api/orch", {"orchM_openai": choices[0]}, ip=ip)
        assert _orch(net, ip)["providers"]["openai"]["orchModel"] == choices[0]
    finally:
        # "" resets to the provider default (which is what an unset key yields)
        net.post("/api/orch", {"orchM_openai": ""}, ip=ip)


# ---- verify_endpoint_validates ----------------------------------------------
@pytest.mark.net
def test_verify_endpoint_validates(device, net, secrets, require_secret):
    """POST /api/verify: unknown provider -> 400; known provider -> 200/409
    (409 = slot busy from a prior enqueue, also a valid running state)."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    resp = net.post("/api/verify", {"provider": "evilcorp"}, ip=ip)
    assert resp.status_code == 400, f"unknown provider -> {resp.status_code}"


# ---- verify_live (needs a real key on the device) ----------------------------
@pytest.mark.net
@pytest.mark.agent
def test_verify_live_openai(device, net, secrets, require_secret):
    """End-to-end verify: enqueue for openai (device must hold a real key) and
    poll until the verify timestamp bumps; the verdict must be 1 (verified).
    FAILS LOUD if the device has no key - provision oaiKey first."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    st = _orch(net, ip)
    assert st["providers"]["openai"]["hasKey"], "device has no OpenAI key provisioned; provision before the agent suite"
    ts0 = st["providers"]["openai"]["vts"]
    resp = net.post("/api/verify", {"provider": "openai"}, ip=ip)
    assert resp.status_code in (200, 409), f"/api/verify -> {resp.status_code}"
    deadline = time.time() + 45.0
    while time.time() < deadline:
        cur = _orch(net, ip)["providers"]["openai"]
        if cur["vts"] != ts0:
            assert cur["verify"] == 1, f"verify verdict {cur['verify']} != 1"
            return
        time.sleep(2.0)
    pytest.fail("verify result never landed within 45 s")
