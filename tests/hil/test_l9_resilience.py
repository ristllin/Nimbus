"""§L9 - resilience / capability-fault tests (the HIL test spec).

Drives ``nimbus::fault`` (the FAULT console command / ``POST /api/fault``) to mark
each capability SIMULATED-ABSENT and asserts the device keeps running while its
degraded path is the one observed - proving no single missing peripheral takes the
system down, WITHOUT unplugging any hardware. The memory/SD cases are the priority
(the owner's ask: keep operating with no card / a lost card / a full flash).

Injection rides the reliable LAN endpoint (``POST /api/fault``, token as the ``t``
form field); observability is read back from ``GET /api/state`` (hal + faults +
effective ``memSd``). Every test RESTORES the faults it injects in a ``finally`` -
a leaked fault would cascade into every downstream test (the harness's cardinal rule).

Markers: ``@pytest.mark.net`` (LAN + serial for the token). The fault-injection
surface is TEST-only (``#ifdef NIMBUS_TEST``), so these never touch a production build.
"""

from __future__ import annotations


import os

import pytest

from test_l4_network import lan_ip_or_skip

CAPS = ["sd", "memory", "mic", "speaker", "led", "screen"]


def _webtok(device) -> str:
    """The per-device web-auth token.

    ``NIMBUS_TEST_TOKEN`` short-circuits the serial read - and SHOULD be used for
    every LAN suite. Repeated console opens wedge the host CDC driver (observed
    three times in one session while building the v3.6.0 suites: the board keeps
    serving HTTP perfectly while pytest reports "console unresponsive"), and a
    net test that only needs a token has no reason to touch the port at all.
    Falls back to WEBTOK? over the console, retried a few times so a torn serial
    line (V0.1 USB-CDC burst loss) doesn't fail a test whose real subject is
    something else."""
    env = os.environ.get("NIMBUS_TEST_TOKEN")
    if env:
        return env
    last = ""
    for _ in range(6):
        try:
            m = device.cmd_re("WEBTOK?", r"([0-9a-fA-F]{24})", timeout=4.0)
            return m.group(1)
        except Exception as exc:  # noqa: BLE001 - serial read flake; retry
            last = str(exc)
    pytest.skip(f"could not read WEBTOK over serial after retries ({last})")


def _state(net, ip) -> dict:
    return net.get_json("/api/state", ip=ip, timeout=5.0)


def _fault(net, ip, tok, cap, on=True) -> dict:
    r = net.post("/api/fault", {"cap": cap, "on": 1 if on else 0, "t": tok}, ip=ip)
    assert r.status_code == 200, f"POST /api/fault {cap}={on} -> {r.status_code}"
    return r.json()


def _clear(net, ip, tok) -> None:
    net.post("/api/fault", {"cap": "all", "t": tok}, ip=ip)


# ---- token gate ------------------------------------------------------------
@pytest.mark.net
def test_fault_requires_token(device, net, secrets, require_secret):
    """Fault injection changes device behavior, so it is token-gated: no token -> 401."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    r = net.post("/api/fault", {"cap": "mic", "on": 1}, ip=ip, auth=False)  # deliberately bare
    assert r.status_code == 401, f"unauthenticated /api/fault -> {r.status_code}, want 401"


# ---- observability shape ---------------------------------------------------
@pytest.mark.net
def test_state_exposes_hal_and_faults(device, net, secrets, require_secret):
    """/api/state carries HAL health + a faults map + effective memSd - the surface
    the degraded paths are asserted through - and nothing is faulted at baseline."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    st = _state(net, ip)
    assert {"display", "leds", "storage", "memory", "input"} <= set(st["hal"])
    assert set(CAPS) <= set(st["faults"])
    assert "memSd" in st and "memFlashFull" in st
    assert not any(st["faults"].values()), f"stale faults at baseline: {st['faults']}"


# ---- SD loss: the owner's core case ----------------------------------------
@pytest.mark.net
def test_sd_fault_degrades_memory_not_storage(device, net, secrets, require_secret):
    """FAULT sd: the PHYSICAL card stays mounted (storeSD unchanged) but the memory
    subsystem drops into no-card behavior - memSd false + the vector cap tightens to
    the degraded 400 - and the device keeps serving. Clearing restores full capacity."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    base = _state(net, ip)
    try:
        fj = _fault(net, ip, tok, "sd", True)
        assert fj["faults"]["sd"] is True
        st = _state(net, ip)
        assert st["memSd"] is False, "SD fault must flip effective memSd false"
        assert st["storeSD"] == base["storeSD"], "physical storeSD must be untouched"
        if base.get("storeSD"):  # only meaningful when a card was actually present
            ms = net.get_json("/api/mem/stats", ip=ip, timeout=5.0)
            assert ms["maxVectors"] <= 400, f"degraded cap not applied live: {ms['maxVectors']}"
        assert _state(net, ip)["mode"] in (0, 1), "device stopped serving under SD fault"
    finally:
        _clear(net, ip, tok)
    assert _state(net, ip)["memSd"] == base["memSd"], "memSd not restored after clear"


# ---- SD SUDDEN loss: demote -> serve -> promote (WS-B B2, the HIL test spec) ----
@pytest.mark.net
def test_sd_io_demote_promote_cycle(device, net, secrets, require_secret):
    """FAULT sd_io simulates a card that ACKs the mount but fails WRITES - the sudden
    loss the boot mount-latch misses. The watchdog must DEMOTE the memory tier without
    a reboot (appends fall to the RAM ring, vector cap -> 400), keep serving, then
    PROMOTE cleanly when the card answers again. Driven deterministically via the
    SDCHECK console probe (debounce = 2 consecutive each way; a few extra absorb the
    main-loop tick racing alongside)."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    base = _state(net, ip)
    if not base.get("storeSD"):
        pytest.skip("no SD card present - demote/promote needs a card mounted at boot")
    try:
        # Writes now fail; drive the probe until the debounced demote latches.
        _fault(net, ip, tok, "sd_io", True)
        for _ in range(4):
            device.cmd_re("SDCHECK", r"sdlost=(\d)", timeout=4.0)
        st = _state(net, ip)
        assert st["sdLost"] is True, f"sd_io writes failing but not demoted: {st.get('sdLost')}"
        assert st["memSd"] is False, "demote must flip effective memSd false"
        assert st["mode"] in (0, 1), "device stopped serving while demoted"
        ms = net.get_json("/api/mem/stats", ip=ip, timeout=5.0)
        assert ms["maxVectors"] <= 400, f"degraded cap not applied on demote: {ms['maxVectors']}"

        # Card recovers: clear the fault and drive the probe until promote latches.
        _fault(net, ip, tok, "sd_io", False)
        for _ in range(4):
            device.cmd_re("SDCHECK", r"sdlost=(\d)", timeout=4.0)
        st = _state(net, ip)
        assert st["sdLost"] is False, "card recovered but tier not promoted back"
        assert st["memSd"] is True, "promote must restore effective memSd"
    finally:
        _clear(net, ip, tok)
        for _ in range(3):
            device.cmd_re("SDCHECK", r"sdlost=(\d)", timeout=4.0)  # ensure promoted back
    assert _state(net, ip)["sdLost"] is False, "SD left demoted after the suite"


# ---- mic loss --------------------------------------------------------------
@pytest.mark.net
def test_mic_fault_reports_faulted(device, net, secrets, require_secret):
    """FAULT mic: the audio-diagnostic mic endpoint reports faulted (no capture) and
    the device is otherwise unaffected; clearing restores a live capture."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    try:
        _fault(net, ip, tok, "mic", True)
        r = net.post("/api/audio/mic", {"t": tok}, ip=ip).json()
        assert r["ok"] is False and r.get("faulted") is True, f"mic not gated: {r}"
    finally:
        _clear(net, ip, tok)
    r = net.post("/api/audio/mic", {"t": tok}, ip=ip).json()
    assert r["ok"] is True, f"mic capture not restored after clear: {r}"


# ---- every capability survives ---------------------------------------------
@pytest.mark.net
def test_all_capabilities_survive_injection(device, net, secrets, require_secret):
    """Inject EVERY capability fault in turn; after each the device must still serve
    /api/state and report exactly that fault latched - no single missing capability
    (screen, ring, mic, speaker, memory, SD) takes the whole system down."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    try:
        for cap in CAPS:
            fj = _fault(net, ip, tok, cap, True)
            assert fj["faults"][cap] is True, f"{cap} did not latch: {fj}"
            st = _state(net, ip)  # a JSON reply == the web loop survived the fault
            assert st["faults"][cap] is True, f"{cap} not reflected in /api/state"
            _fault(net, ip, tok, cap, False)
            assert _state(net, ip)["faults"][cap] is False, f"{cap} did not clear"
    finally:
        _clear(net, ip, tok)
    assert not any(_state(net, ip)["faults"].values()), "faults left set after suite"
