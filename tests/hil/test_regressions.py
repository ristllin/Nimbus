"""§3 - one-per-bug regression tests, named for the failure (the HIL test spec).

Each reproduces the exact scenario and asserts the FIXED behavior. Several are strict
xfail until the fix lands, so they auto-flip to XPASS when it does. Where a regression
IS a full L-layer test, this DELEGATES to that test (or its shared helper) so the two
can't drift; the named ``R_Fx`` id still exists so the plan's §3 checklist is covered.

The host half of R_F10 (the persistence CONTRACT) is a Unity test under
``test/test_config_persist/`` (see README §L1 cross-reference); it is
``pio test -e native``-runnable NOW. This file holds the on-device halves.
The menu-edit persist+apply path (both input drivers) is covered on-device by
``test_l22_menu_persist.py``.
"""

from __future__ import annotations

import time

import pytest

from net import WifiAuthFailure
from test_l4_network import lan_ip_or_skip, ringbright_param
from test_l5_agent import (
    test_agent_turn_live as _agent_turn_live,
    test_orch_boot_ok as _orch_boot_ok,
    test_telegram_roundtrip as _telegram_roundtrip,
)


# ---- R_F8 - SSID case-correction (F8) --------------------------------------
@pytest.mark.net
def test_R_F8_ssid_case(device, net, secrets, require_secret):
    """F8: provisioning the SSID in the WRONG CASE (lower-cased) must still connect,
    because the firmware scan-corrects the case (a lowercased SSID vs its real mixed-case name).
    Vacuous if the known-good SSID is already all-lowercase -> LOUD skip."""
    require_secret(secrets.require_sta)
    ssid = secrets.sta_ssid
    if ssid.lower() == ssid:
        pytest.skip(
            f"SSID {ssid!r} is already lowercase; case-correction is vacuous "
            "- provide a mixed-case NIMBUS_TEST_STA_SSID to test F8"
        )
    device.reset()
    device.wait_ready(timeout=20.0)
    net.provision(ssid.lower(), secrets.sta_pass)  # deliberately wrong case
    try:
        ip = net.wait_got_ip(timeout=30.0)
    except WifiAuthFailure as exc:
        pytest.fail(f"lower-cased SSID did not case-correct + connect (reason={exc.reason}) - F8 regression")
    assert ip, "case-corrected SSID connected but no IP reported"


# ---- R_F9 - wrong-password reason surfaced, no hang (F9) --------------------
@pytest.mark.net
def test_R_F9_wrong_pw_reason(device, net, secrets, require_secret):
    """F9: wrong PSK surfaces a reason code AND the device stays responsive (no silent
    hang)."""
    require_secret(secrets.require_sta)
    device.reset()
    device.wait_ready(timeout=20.0)
    net.provision(secrets.sta_ssid, "definitely-the-wrong-passphrase")
    reason = net.wait_disconnect_reason(timeout=25.0)  # asserts PING answers, too
    assert reason > 0, "no disconnect reason surfaced (F9: firmware went silent)"


# ---- R_F10 - persistence with no SD (F10) - xfail until fixed ---------------
@pytest.mark.net
@pytest.mark.xfail(
    strict=True,
    reason="F10: overrides are SD-blob backed; with no SD only the NVS "
    "mode int survives. XPASS when persistence is fixed.",
)
def test_R_F10_persist_no_sd(device, net, secrets, require_secret):
    """F10 (on-device half): an override must survive a reboot. Expected to FAIL without
    SD today. The host half (persistence CONTRACT) is the Unity test under
    test/test_persist_contract/ - see README."""
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
    assert int(cur2["value"]) == target and cur2["overridden"] is True, (
        f"override lost across reboot: value={cur2['value']} "
        f"overridden={cur2['overridden']} (F10 - SD-backed blob not persisted)"
    )


# ---- R_F11 - orchestrator boots without hanging (F11) ----------------------
@pytest.mark.agent
def test_R_F11_orch_boot_no_hang(device, secrets, require_secret):
    """F11: delegates to test_orch_boot_ok - the direct regression for the boot hang."""
    _orch_boot_ok(device, secrets, require_secret)


# ---- R_F12 - watchdog reboots a hung device (F12) - LANDED (8 s task WDT) ---
@pytest.mark.hil
def test_R_F12_watchdog(device):
    """F12: ``HANG`` -> auto-reboot within the WDT window -> recover. The 8 s task
    watchdog shipped long ago (AGENTS.md: a hung loop panics + reboots in ~8 s);
    this is now a straight PASS gate, not an xfail-until-fix."""
    device.reset()
    device.wait_ready(timeout=20.0)
    device.hang()
    device.wait_reboot(timeout=20.0)
    assert device.ping(), "no liveness after watchdog reset (F12 regression)"


# ---- R_F16 - web config cross-core race (F16) ------------------------------
@pytest.mark.net
def test_R_F16_web_race(device, net, secrets, require_secret):
    """F16: hammer ``POST /api/config`` repeatedly while the encoder log runs, and
    assert ``/api/state`` NEVER returns torn/invalid JSON and the last written value is
    consistent - the direct regression for the AsyncTCP-vs-main-loop ``g_cfg`` race."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)

    st0 = net.get_json("/api/state", ip=ip, timeout=5.0)
    bright = ringbright_param(st0)
    pkey = bright["key"]

    device.inputlog(True)
    last = None
    for i in range(20):
        val = 20 + (i % 40)  # cycle 20..59
        net.post("/api/config", {f"p_{pkey}": str(val)}, ip=ip, timeout=5.0)
        last = val
        st = net.get_json("/api/state", ip=ip, timeout=5.0)  # torn JSON would raise
        cur = next((p for p in st["params"] if p["key"] == pkey), None)
        assert cur is not None, "RingBrightness vanished from params mid-hammer"
        assert 0 <= int(cur["value"]) <= 255, f"RingBrightness read a torn/out-of-range value {cur['value']!r} - F16"
    device.inputlog(False)

    final = None
    for _ in range(10):
        time.sleep(0.3)
        st = net.get_json("/api/state", ip=ip, timeout=5.0)
        cur = next((p for p in st["params"] if p["key"] == pkey), None)
        if cur and int(cur["value"]) == last:
            final = cur
            break
    assert final is not None, f"final RingBrightness never settled to last write {last} - F16 race"
    assert device.ping(), "device unresponsive after the F16 hammer"


# ---- R_F17 - live agent turn + failover (F17) ------------------------------
@pytest.mark.agent
@pytest.mark.net
def test_R_F17_agent_turn(device, secrets, require_secret):
    """F17: delegates to test_agent_turn_live (a live provider turn incl. failover)."""
    _agent_turn_live(device, secrets, require_secret)


# ---- R_F18 - Telegram round-trip, dedicated bot (F18) ----------------------
@pytest.mark.agent
@pytest.mark.net
def test_R_F18_telegram(device, secrets, require_secret):
    """F18: delegates to test_telegram_roundtrip (DEDICATED test bot, never a
    production bot's token)."""
    _telegram_roundtrip(device, secrets, require_secret)


# ---- R_F5 - audio fault isolation (F5, F6) - manual ------------------------
@pytest.mark.audio
@pytest.mark.manual
def test_R_F5_audio_isolation(device, require_manual):
    """F5/F6: ``TEST mic`` + ``TEST spk`` bisection distinguishes a mic fault from a
    speaker/coupling fault - NOT one aggregate number. Runs both single-ended tests and
    asserts they report SEPARATE, interpretable numbers, then has the operator confirm
    both were independently observable."""
    import re

    def kv(rest, key):
        m = re.search(rf"\b{key}=(-?\d+)", rest)
        return int(m.group(1)) if m else None

    device.reset()
    device.wait_ready(timeout=20.0)

    mic = device.selftest("mic", timeout=15.0)
    if mic.group("verdict") == "SKIP":
        pytest.skip(f"TEST mic SKIP ({mic.group('rest').strip()}); audio board absent")
    assert mic.group("verdict") != "FAIL", f"mic FAILED: {mic.group(0)}"
    assert kv(mic.group("rest"), "rms") is not None, "mic result has no rms number to isolate a mic fault"

    spk = device.selftest("spk", timeout=15.0)
    assert spk.group("verdict") != "FAIL", f"spk FAILED: {spk.group(0)}"

    require_manual.confirm(
        "Bisection check: you should have heard the SPEAKER tone AND (separately) "
        "tapping the MIC changes its numbers. Confirm both were observable "
        "independently. [y]",
        timeout=60.0,
    )
