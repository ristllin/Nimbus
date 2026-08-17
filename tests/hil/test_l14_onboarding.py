"""L14 - First-run onboarding wizard + factory reset (webui.cpp).

Exercises the onboarding seam added alongside the Harness-tab web UI overhaul:
  * GET /api/state carries a ``needsOnboarding`` boolean gate,
  * POST /api/onboard/complete is token-gated and enforces the two hard
    requirements (Wi-Fi joined AND >=1 verified LLM provider) before it clears
    the flag.

All checks here are NON-DESTRUCTIVE - they never wipe the device. The full
factory-reset -> wizard round-trip is inherently destructive (it erases Wi-Fi +
keys + bonds and drops the device onto its setup AP, off the LAN the harness
talks over), so it can't be driven from a LAN pytest; it lives as a ``manual``
walkthrough at the bottom, run by hand on a throwaway board.
"""

from __future__ import annotations

import pytest

from test_l4_network import lan_ip_or_skip


# ---- needsOnboarding gate in /api/state ------------------------------------
@pytest.mark.net
def test_state_has_needs_onboarding(device, net, secrets, require_secret):
    """GET /api/state exposes needsOnboarding as a bool. A provisioned board
    that has finished setup (or migrated on upgrade) reports false."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    st = net.get_json("/api/state", ip=ip, timeout=5.0)
    assert "needsOnboarding" in st, f"/api/state missing needsOnboarding: {sorted(st)}"
    assert isinstance(st["needsOnboarding"], bool)
    # This board is on the LAN (provisioned), so the boot migration should have
    # marked it onboarded - the wizard must not re-trigger on an upgraded device.
    assert st["needsOnboarding"] is False, "provisioned board should not need onboarding"


# ---- /api/onboard/complete is token-gated ----------------------------------
@pytest.mark.net
def test_onboard_complete_requires_token(device, net, secrets, require_secret):
    """POST /api/onboard/complete without the auth token -> 401 (strict gate)."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    r = net.post("/api/onboard/complete", data={}, ip=ip, timeout=5.0, auth=False)
    assert r.status_code == 401, f"unauthenticated complete should 401, got {r.status_code}"


# ---- /api/onboard/complete is wired + gate returns a sane verdict ----------
@pytest.mark.net
def test_onboard_complete_wired(device, net, secrets, require_secret):
    """POST /api/onboard/complete with the token returns a wired verdict:
    200 when the board is provisioned AND has a verified provider, else a 409
    naming the unmet gate. Either proves the endpoint + guards exist; a 404/500
    would mean the route never registered."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    r = net.post("/api/onboard/complete", data={}, ip=ip, timeout=8.0)
    assert r.status_code in (200, 409), f"unexpected status {r.status_code}: {r.text[:200]}"
    if r.status_code == 409:
        # Only the two documented gate failures are acceptable here.
        assert any(m in r.text for m in ("no verified provider", "wifi not connected")), (
            f"409 without a known gate reason: {r.text[:200]}"
        )


# ---- Manual: the destructive factory-reset -> wizard round-trip -------------
@pytest.mark.manual
@pytest.mark.net
def test_factory_reset_reonboards_manual(device, net, secrets, require_secret):
    """DESTRUCTIVE - hand-run on a throwaway board only.

    Steps (verify each on the device + a browser):
      1. Settings -> Connectivity -> Factory reset; type ``FACTORY RESET``.
      2. Device paints "Factory reset…", erases NVS, reboots. Confirm SD /mem
         (memories, files) SURVIVES (it is not on the NVS partition).
      3. Device comes up on its ``<name>-setup`` AP (no STA creds). Join it;
         the captive portal opens the page already authenticated (GET /?t=).
      4. GET /api/state.needsOnboarding is now TRUE -> the wizard overlay shows.
      5. Walk it: Wi-Fi (required) -> provider key + verify (required) ->
         skip the rest -> Finish. POST /api/onboard/complete returns ok.
      6. Reload: needsOnboarding is FALSE and the wizard never returns.
    """
    pytest.skip("manual/destructive - wipes the device; run by hand per the docstring")
