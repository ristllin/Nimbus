"""§L4 - HIL network tests (the HIL test spec).

AP reachability, STA connect (good password), wrong-password reporting, web GET/POST
+ cross-core race, and persistence-across-reboot. Named regressions (R_F8/R_F9/
R_F16/R_F10) live in test_regressions.py and delegate to the helpers here.

Failure IDs: F7 (AP reachability), F8 (STA connect / case), F9 (auth-fail reported,
no hang), F16 (web config cross-core race), F10 (overrides don't persist w/o SD).

All ``@pytest.mark.net``; conftest loud-skips without ``--allow-hardware``. STA creds
come from env (secrets.py); a missing cred is a LOUD skip, never a pass.
"""

from __future__ import annotations

import os
import time

import pytest

from net import AP_IP, WifiAuthFailure


# ---- shared: join the LAN or LOUD-skip -------------------------------------
def lan_ip_or_skip(device, net, secrets, require_secret) -> str:
    """Provision + join the LAN, returning the IP, or LOUD-skip with the reason.
    Shared by the web tests here and the R_F16/R_F10 regressions.

    Fast path: if ``NIMBUS_TEST_IP`` names an already-joined device that answers,
    reuse it and SKIP the destructive ``device.reset()`` + re-provision. On the
    CH34x bench board opening/resetting serial races the WiFi rejoin (reason=8
    ASSOC_LEAVE), so the reset is what breaks an otherwise-connected device - see
    the serial<->WiFi contention note in AGENTS.md."""
    override = os.environ.get("NIMBUS_TEST_IP")
    if override:
        # The device auto-rejoins from NVS after the fixture's setup reset; poll the
        # known IP until it answers (bounded) before falling back to a real join.
        deadline = time.time() + 30.0
        while time.time() < deadline:
            try:
                net.get_json("/api/state", ip=override, timeout=4.0)
                return override
            except Exception:  # noqa: BLE001 - still rejoining; retry
                time.sleep(2.0)
    require_secret(secrets.require_sta)
    device.reset()
    device.wait_ready(timeout=20.0)
    net.provision(secrets.sta_ssid, secrets.sta_pass)
    try:
        return net.wait_got_ip(timeout=25.0)
    except WifiAuthFailure as exc:
        pytest.skip(f"could not join LAN for web test (reason={exc.reason})")


def ringbright_param(state: dict) -> dict:
    """Pull the RingBrightness entry from /api/state's params array by NAME so tests
    don't hardcode the Param enum int."""
    p = next(
        (
            p
            for p in state.get("params", [])
            if str(p.get("name", "")).lower().replace("_", "").startswith("ringbright")
        ),
        None,
    )
    assert p is not None, (
        f"/api/state params has no RingBrightness entry: {[q.get('name') for q in state.get('params', [])]}"
    )
    return p


# Profile preset table a preview composes from (a throwaway Config carrying ONLY
# the previewed profile, no overrides) - mirrors lib/core/src/profile.cpp's
# kPresets rows for Posture/RingBrightness. Same hardcode-and-comment pattern as
# device.SCREEN_NAMES: update this if profile.cpp's preset table changes.
PREVIEW_PRESETS = {
    0: {"posture": 0, "bright": 10},  # BatterySaver
    1: {"posture": 0, "bright": 30},  # Balanced
    2: {"posture": 1, "bright": 60},  # Desk
}


# ---- ap_up (F7) ------------------------------------------------------------
@pytest.mark.net
def test_ap_up(device, net):
    """ap_up (F7): after boot the softAP is up at 192.168.4.1 and the web server
    answers. If the host can route to the AP subnet, assert ``GET /`` -> 200 + HTML.
    If not joined to ``Nimbus-setup``, do NOT pass silently - assert the device is up
    (the web stack shares the loop) and LOUD-skip the HTTP half with the reason."""
    device.reset()
    device.wait_ready(timeout=20.0)

    if net.reachable(ip=AP_IP, timeout=3.0):
        resp = net.get("/", ip=AP_IP, timeout=5.0)
        assert resp.status_code == 200, f"AP GET / -> {resp.status_code}, expected 200"
        assert "html" in resp.text.lower(), "AP GET / did not return HTML"
    else:
        assert device.ping(), "device not answering; AP stack liveness unconfirmed"
        pytest.skip(
            f"host not joined to Nimbus-setup ({AP_IP} unreachable); AP HTTP "
            "reachability needs the host on the AP subnet"
        )


# ---- sta_connect_ok (F8) ---------------------------------------------------
@pytest.mark.net
def test_sta_connect_ok(device, net, secrets, require_secret):
    """sta_connect_ok (F8): provision a known-good SSID/pass -> ``WIFI_GOT_IP <ip>``,
    the IP is a real dotted quad, and ``PING`` -> ``PONG`` still answers (the STA join
    didn't wedge the loop)."""
    require_secret(secrets.require_sta)
    device.reset()
    device.wait_ready(timeout=20.0)

    net.provision(secrets.sta_ssid, secrets.sta_pass)
    ip = net.wait_got_ip(timeout=25.0)
    octets = ip.split(".")
    assert len(octets) == 4 and all(0 <= int(o) <= 255 for o in octets), f"WIFI_GOT_IP gave a bad address: {ip!r}"
    assert device.ping(), "device stopped answering after STA connect (wedged)"


# ---- sta_auth_fail_reported (F9) -------------------------------------------
@pytest.mark.net
def test_sta_auth_fail_reported(device, net, secrets, require_secret):
    """sta_auth_fail_reported (F9): provision a WRONG password and assert the firmware
    reports the disconnect reason (typically 15 or 2) and does NOT hang - the device
    answers ``PING`` afterward (surfacing the error, not silence)."""
    require_secret(secrets.require_sta)
    device.reset()
    device.wait_ready(timeout=20.0)

    # RESTORE IS UNCONDITIONAL: this test deliberately breaks the device's WiFi
    # creds; without the finally, a mid-test failure leaves the device off the
    # LAN and silently poisons every downstream net/agent test (observed live -
    # one failure here cascaded into five).
    try:
        net.provision(secrets.sta_ssid, secrets.sta_pass + "_WRONG!x")
        reason = net.wait_disconnect_reason(timeout=25.0)  # asserts PING answers, too
        assert reason in (2, 15, 205), (
            f"disconnect reason={reason}; expected an auth-failure code (2/15/205) - the wrong-password signature (F9)"
        )
    finally:
        net.provision(secrets.sta_ssid, secrets.sta_pass)
        net.wait_got_ip(timeout=25.0)


# ---- web_get_root (F16) ----------------------------------------------------
@pytest.mark.net
def test_web_get_root(device, net, secrets, require_secret):
    """web_get_root (F16): ``GET http://<lan-ip>/`` -> 200 + config-page HTML."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    resp = net.get("/", ip=ip, timeout=5.0)
    assert resp.status_code == 200, f"GET / -> {resp.status_code}"
    body = resp.text.lower()
    assert "<html" in body or "<!doctype html" in body.lower(), "GET / did not return an HTML document"
    assert "form" in body or "input" in body, "GET / returned HTML without any config form markers"


# ---- web_get_state (F16) ---------------------------------------------------
@pytest.mark.net
def test_web_get_state(device, net, secrets, require_secret):
    """web_get_state (F16): ``GET /api/state`` -> valid JSON with mode/jobs/profile and
    ``batt.valid == false`` (no battery HW -> NullMonitor reports invalid)."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    st = net.get_json("/api/state", ip=ip, timeout=5.0)
    for key in ("mode", "jobs", "profile", "batt"):
        assert key in st, f"/api/state missing {key!r}: keys={sorted(st)}"
    assert st["mode"] in (0, 1), f"mode={st['mode']!r}, expected 0 or 1"
    assert isinstance(st["jobs"], int) and st["jobs"] >= 0, f"jobs={st['jobs']!r} not a non-negative int"
    # Battery hardware EXISTS since v2.0.0 (2S ADC divider) - batt.valid may be
    # true (ADC build) or false (NullMonitor build). Assert the SHAPE, not the
    # stale no-battery assumption.
    assert isinstance(st["batt"].get("valid"), bool), f"batt.valid not a bool: {st['batt'].get('valid')!r}"
    if st["batt"]["valid"]:
        assert st["batt"].get("millivolts", 0) > 0 or st["batt"].get("mv", 0) > 0, (
            f"valid battery but no voltage field: {sorted(st['batt'])}"
        )


# ---- web_post_config (F16) -------------------------------------------------
@pytest.mark.net
def test_web_post_config(device, net, secrets, require_secret):
    """web_post_config (F16): ``POST /api/config`` sets a profile + a RingBrightness
    override; ``GET /api/state`` reflects both. This exercises the cross-core
    ``g_cfg`` path F16 was about. loopWeb() applies staged edits on the MAIN task, so
    we poll for the apply rather than reading once and racing it."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)

    st0 = net.get_json("/api/state", ip=ip, timeout=5.0)
    bright = ringbright_param(st0)
    pkey = bright["key"]
    target = 33 if int(bright["value"]) != 33 else 44  # distinct from current

    resp = net.post("/api/config", {"profile": "1", f"p_{pkey}": str(target)}, ip=ip, timeout=5.0)
    assert resp.status_code == 200, f"POST /api/config -> {resp.status_code}"

    reflected = None
    for _ in range(10):
        time.sleep(0.3)
        st = net.get_json("/api/state", ip=ip, timeout=5.0)
        cur = next((p for p in st["params"] if p["key"] == pkey), None)
        if cur and int(cur["value"]) == target:
            reflected = st
            break

    assert reflected is not None, (
        f"RingBrightness override {target} never reflected in /api/state (staged edit lost / cross-core race - F16)"
    )
    assert reflected["profile"] == 1, f"profile={reflected['profile']!r}, expected 1 after POST"
    assert device.ping(), "device stopped answering after the concurrent web load"


# ---- web_post_preview (P-D web preview) ------------------------------------
@pytest.mark.net
def test_web_post_preview(device, net, secrets, require_secret):
    """web_post_preview: POST /api/preview drives the ring live to the requested
    profile's look (posture + brightness, read over serial via RENDER? since the
    ring has no HTTP-visible state) WITHOUT touching /api/state's active profile,
    then auto-reverts on its own a few seconds later with no further request."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)

    st0 = net.get_json("/api/state", ip=ip, timeout=5.0)
    live_profile = st0["profile"]
    before = device.render()

    # Preview a profile whose preset (posture, bright) provably differs from
    # what the ring is showing right now, so the change is unambiguous.
    target = next(
        p for p, preset in PREVIEW_PRESETS.items() if preset != {"posture": before.posture, "bright": before.bright}
    )
    want = PREVIEW_PRESETS[target]

    resp = net.post("/api/preview", {"profile": str(target)}, ip=ip, timeout=5.0)
    assert resp.status_code == 200, f"POST /api/preview -> {resp.status_code}"

    previewed = None
    for _ in range(15):
        time.sleep(0.2)
        r = device.render()
        if r.posture == want["posture"] and r.bright == want["bright"]:
            previewed = r
            break
    assert previewed is not None, (
        f"ring never showed profile {target}'s preview (want posture="
        f"{want['posture']} bright={want['bright']}); last render={device.render()!r}"
    )

    # The preview is a look, not a choice: /api/state's active profile must be
    # untouched (previewCfg in main.cpp never touches g_cfg/g_selector).
    st1 = net.get_json("/api/state", ip=ip, timeout=5.0)
    assert st1["profile"] == live_profile, f"preview mutated the active profile: {st1['profile']!r} != {live_profile!r}"

    # Auto-revert (NIMBUS_PREVIEW_MS ~4s): the ring returns to the pre-preview
    # look on its own - no second request from the host.
    reverted = None
    for _ in range(20):
        time.sleep(0.5)
        r = device.render()
        if r.posture == before.posture and r.bright == before.bright:
            reverted = r
            break
    assert reverted is not None, (
        f"ring never auto-reverted after the preview window; "
        f"last render={device.render()!r}, pre-preview was {before!r}"
    )
    assert device.ping(), "device stopped answering after a web preview"


# ---- persist_across_reboot (F10) - xfail until SD/NVS contract fixed --------
@pytest.mark.net
@pytest.mark.xfail(
    strict=True,
    reason="F10: overrides live in the SD-backed config blob; with no "
    "SD, saveConfig()->putBlob no-ops and only mode (NVS int) "
    "survives reboot. Flips to XPASS when persistence is fixed.",
)
def test_persist_across_reboot(device, net, secrets, require_secret):
    """persist_across_reboot (F10): set a RingBrightness override over the web,
    ``REBOOT``, and assert it survived. Expected to FAIL today (no SD -> blob putBlob
    no-ops), so it is xfail(strict): the day the override survives, this XPASSES and
    the strict flag demands the marker be removed - the test encodes the fix."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)

    st0 = net.get_json("/api/state", ip=ip, timeout=5.0)
    bright = ringbright_param(st0)
    pkey = bright["key"]
    target = 27 if int(bright["value"]) != 27 else 39

    net.post("/api/config", {f"p_{pkey}": str(target)}, ip=ip, timeout=5.0)
    applied = False
    for _ in range(10):
        time.sleep(0.3)
        st = net.get_json("/api/state", ip=ip, timeout=5.0)
        cur = next((p for p in st["params"] if p["key"] == pkey), None)
        if cur and int(cur["value"]) == target:
            applied = True
            break
    assert applied, "override never applied pre-reboot (setup failure, not F10)"

    device.send("REBOOT")
    device.wait_reboot(timeout=25.0)
    net.provision(secrets.sta_ssid, secrets.sta_pass)
    try:
        ip2 = net.wait_got_ip(timeout=25.0)
    except WifiAuthFailure as exc:
        pytest.skip(f"could not rejoin LAN after reboot (reason={exc.reason})")

    st2 = net.get_json("/api/state", ip=ip2, timeout=5.0)
    cur2 = next((p for p in st2["params"] if p["key"] == pkey), None)
    assert cur2 is not None, "RingBrightness param missing after reboot"
    # THE F10 ASSERTION: the override must have survived. Fails today (no SD).
    assert int(cur2["value"]) == target and cur2["overridden"] is True, (
        f"override lost across reboot: value={cur2['value']} "
        f"overridden={cur2['overridden']} (F10 - SD-backed blob not persisted)"
    )
