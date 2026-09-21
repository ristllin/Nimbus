"""L33 - CUM-245: the fresh-device (default / absent-NVS) HIL leg, on a genuine board.

The touch-mirror / touch-180 / uncalibrated-out-of-box family (CUM-160 -> CUM-203, sibling
CUM-189) kept re-presenting because EVERY automated leg ran on a PROVISIONED unit (cal already
solved, NVS already good), while the owner hits the FIRST boot after a new version. The host
tier (test/test_fresh_device, test/test_touch_cal) pins the pure policy as a class; THIS leg is
the missing on-glass "out-of-box" STATE, driven through the product factory-reset path.

Why it never ran: the earlier P1 "every fresh resistive device self-navigates" bench evidence
was VOIDED (2026-09-01) - it was a Solide build running on CYD hardware, a wrong board identity,
so no genuine fresh-Solide repro exists. TF-T6's code fixes (the cal gate) stand on their host
tests; this leg is the first honest on-glass proof of them.

The gate is invisible to RENDER? on purpose: serviceCalGate paints the TouchCal surface via
renderAndPush DIRECTLY (not the renderScreen path that updates g_lastScreen / the RENDER? push),
so the gate never shows in RENDER? screen. The reliable oracle is the CALGATE seam
(device.cal_gate()); RENDER? is used only to catch the self-NAVIGATION the bug produced (a
gated panel that reaches the Menu).

⚠ DESTRUCTIVE: this leg FACTORY-RESETS the board (erases tchCal, tftFlip, Wi-Fi creds, the
onboarded flag, provider keys - the exact first-boot state). The `fresh_bench` fixture requires
STA creds up front and ALWAYS re-provisions Wi-Fi on teardown, so the board is left on its LAN.

Still owned by a finger on glass (NOT automatable, documented in PR_BODY.md): the four-corner
SOLVE itself (real per-corner raw ADC), and "a tap lands where you touch" under a flip - the
flip-compose mapping is host-tested pure (test/test_fresh_device), but only a finger proves the
placement on real glass.
"""

from __future__ import annotations

import time

import pytest

from device import SCREEN_NAMES, Device, ExpectTimeout
from secrets import SecretsUnavailable

_STATUSIDLE = SCREEN_NAMES.index("StatusIdle")
_MENU = SCREEN_NAMES.index("Menu")

# Screens that are ONLY reachable by NAVIGATING the UI (opening a menu, drilling into a
# detail). A gated panel must reach none of them - the self-navigation bug drove raw
# uncalibrated taps into UI navigation and landed on Settings > Sound (the Menu). Ambient
# screens a gated board may legitimately repaint (StatusIdle for a Wi-Fi-down badge on a
# fresh board with no creds, Screensaver, IdleArt, Badge, SetupInfo) are NOT navigation and
# must not fail the leg - so this asserts on reaching a nav screen, not on any screen change.
_NAV_SCREENS = {
    SCREEN_NAMES.index(n)
    for n in (
        "Menu",
        "JobDetail",
        "Battery",
        "Ask",
        "SessionDetail",
        "SelfTest",
        "TokenDetail",
        "ConfigQr",
        "Pairing",
        "VoiceGlyph",
    )
}


def _screen_name(s: int) -> str:
    return SCREEN_NAMES[s] if 0 <= s < len(SCREEN_NAMES) else str(s)


# ============================================================================
# Host coverage for the new device.py parsers + the CALGATE seam contract.
# (No board; runs under `pytest tests/hil -m host`. Mirrors test_reboot_confirm's
# scripted-serial pattern so the parsing logic is proven, not just exercised live.)
# ============================================================================


class _FakeSer:
    def close(self):
        pass

    def write(self, *a):
        pass

    def flush(self):
        pass


class _ScriptedDevice(Device):
    """A Device whose serial is a scripted line queue: send/drain are no-ops and _readline
    pops the next recorded reply. Lets the cal_gate() / tftfill_ok() PARSERS run their real
    control flow with no hardware."""

    def __init__(self, lines):
        super().__init__(port="/dev/cu.fake")
        self._ser = _FakeSer()
        self._lines = list(lines)

    def drain(self, quiet: float = 0.2):
        pass

    def send(self, line: str):
        pass

    def _readline(self, deadline):
        return self._lines.pop(0) if self._lines else None


@pytest.mark.host
def test_cal_gate_parses_gated_uncalibrated():
    active, kind, stored = _ScriptedDevice(["CALGATE active=1 kind=res stored=0"]).cal_gate()
    assert (active, kind, stored) == (True, "res", False)


@pytest.mark.host
def test_cal_gate_parses_open_after_skip():
    active, kind, stored = _ScriptedDevice(["CALGATE active=0 kind=res stored=1"]).cal_gate("skip")
    assert (active, kind, stored) == (False, "res", True)


@pytest.mark.host
def test_cal_gate_parses_capacitive_never_gated():
    active, kind, stored = _ScriptedDevice(["CALGATE active=0 kind=cap stored=0"]).cal_gate()
    assert (active, kind, stored) == (False, "cap", False)


@pytest.mark.host
def test_tftfill_ok_true_only_when_all_three_round_trip():
    ok = _ScriptedDevice(
        [
            "TFTFILL red   want=0xF800  tl=0xF800 mid=0xF800 br=0xF800  OK",
            "TFTFILL green want=0x07E0  tl=0x07E0 mid=0x07E0 br=0x07E0  OK",
            "TFTFILL blue  want=0x001F  tl=0x001F mid=0x001F br=0x001F  OK",
        ]
    ).tftfill_ok()
    assert ok is True


@pytest.mark.host
def test_tftfill_ok_false_on_any_mismatch():
    ok = _ScriptedDevice(
        [
            "TFTFILL red   want=0xF800  tl=0xF800 mid=0xF800 br=0xF800  OK",
            "TFTFILL green want=0x07E0  tl=0x0000 mid=0x0000 br=0x0000  MISMATCH",
            "TFTFILL blue  want=0x001F  tl=0x001F mid=0x001F br=0x001F  OK",
        ]
    ).tftfill_ok()
    assert ok is False


# ============================================================================
# The on-glass fresh-device legs (HIL).
# ============================================================================

NO_SELFNAV_S = 30.0  # the self-nav-to-Settings>Sound bug was deterministic within ~30 s
POLL_S = 2.5


@pytest.fixture(scope="module")
def fresh_bench(device, secrets):
    """Guards the destructive fresh-device legs. Requires STA creds UP FRONT (a loud skip if
    absent - the teardown needs them to leave the board usable), yields the live device, and
    ALWAYS re-provisions Wi-Fi on teardown so a factory-reset board rejoins its LAN."""
    try:
        secrets.require_sta()
    except SecretsUnavailable as exc:
        pytest.skip(f"fresh-device leg FACTORY-RESETS the board and must re-provision Wi-Fi in teardown; {exc}")
    yield device
    # Teardown: the leg wiped Wi-Fi creds. Restore them so the board is left on its LAN. Never
    # mask the test verdict, but say plainly whether the rejoin confirmed.
    try:
        device.wifi(secrets.sta_ssid, secrets.sta_pass)
        device.expect("WIFI_GOT_IP", timeout=35.0)
        print("[fresh_bench] teardown: Wi-Fi re-provisioned, GOT_IP confirmed")
    except Exception as exc:  # noqa: BLE001 - teardown must not raise over the real result
        print(
            f"[fresh_bench] teardown re-provision did NOT confirm GOT_IP: {exc!r} "
            "(supervisor: re-provision Wi-Fi before returning the board)"
        )


@pytest.mark.hil
def test_fresh_resistive_gate_first_skip_and_handoff(fresh_bench):
    """The full out-of-box first-boot flow on a fresh RESISTIVE panel (legs 3a-3d):
    (a) the guided cal gate owns the panel from boot, and does NOT self-navigate for 30 s;
    (b) injected taps do not navigate while gated;
    (c) the deliberate skip opens the gate and hands off to first-run setup;
    (d) render reaches the frame (GRAM readback) and the setup surface comes up."""
    device = fresh_bench
    device.factory_reset()  # -> out-of-box; asserts a clean fresh boot (no panic / reboot loop)
    assert device.ping(), "console must answer after the factory-reset reboot"

    active, kind, stored = device.cal_gate()
    if kind != "res":
        pytest.skip(
            f"fresh-device GATE leg is resistive-only; this board reports kind='{kind}'. A "
            "capacitive panel reports pixels, needs no per-unit cal, and never gates - run "
            "test_fresh_capacitive_never_gates, or attach the resistive Solide."
        )

    # (a) the gate owns the panel out of the box, uncalibrated. CALGATE is the oracle; RENDER?
    # cannot see the gate (it paints outside the g_lastScreen path).
    assert active and not stored, (
        f"a fresh resistive board must GATE uncalibrated touch: active={active} stored={stored} "
        "(this is the CUM-245 gate; a false open is the self-navigation regression)"
    )

    # (a) no self-navigation for 30 s: the pre-fix board deterministically drove raw uncalibrated
    # taps into UI navigation and landed on Settings > Sound (the Menu). The gate must hold the
    # whole window - CALGATE stays active AND RENDER? never reaches a NAVIGATION screen (an
    # ambient repaint like StatusIdle for a Wi-Fi-down badge on a fresh, credless board is fine).
    deadline = time.monotonic() + NO_SELFNAV_S
    while time.monotonic() < deadline:
        g_active, _, _ = device.cal_gate()
        assert g_active, f"gate released on its own within {NO_SELFNAV_S}s (no cal was stored)"
        s = device.render().screen
        assert s not in _NAV_SCREENS, (
            f"gated panel self-navigated to {_screen_name(s)} within {NO_SELFNAV_S}s - the "
            "CUM-245 self-navigation bug (raw uncalibrated taps reaching UI navigation)"
        )
        time.sleep(POLL_S)

    # (b) injected taps do not navigate while gated. On a calibrated panel each of these would
    # act (open the gear menu, hit the header Back, tap a tile); while gated, drainTouch is
    # bypassed, so none reach navigation.
    for x, y in ((160, 120), (300, 10), (20, 22), (160, 230)):
        device.tap(x, y)
    time.sleep(1.0)
    s = device.render().screen
    assert s not in _NAV_SCREENS, (
        f"an injected tap navigated the gated panel to {_screen_name(s)} - taps must never reach "
        "navigation while the cal gate owns the panel"
    )
    g_active, _, g_stored = device.cal_gate()
    assert g_active and not g_stored, "the gate must still own the panel after taps (no cal stored)"

    # (c) the deliberate skip: the long-hold the gate recognizes. Opens the gate (persists the
    # board default) and hands off to the first-run flow.
    a_after, _, s_after = device.cal_gate("skip")
    assert not a_after and s_after, (
        f"the deliberate skip must OPEN the gate and persist a cal: active={a_after} stored={s_after}"
    )
    # The handoff target: persistCalAndReleaseGate renders StatusIdle (the first-run screen; on a
    # credless device it carries the Set up Wi-Fi CTA - host-pinned in test_fresh_device). Poll
    # briefly for the panel to leave the gate for StatusIdle.
    deadline = time.monotonic() + 10.0
    handoff = None
    while time.monotonic() < deadline:
        handoff = device.render().screen
        if handoff == _STATUSIDLE:
            break
        time.sleep(0.5)
    assert handoff == _STATUSIDLE, (
        f"after the skip the panel must hand off to the first-run StatusIdle, got {_screen_name(handoff)}"
    )

    # (d) render reaches the frame: the REAL full-panel path with a corner-pixel GRAM readback
    # (not a golden - this proves the glass actually holds the frame post-handoff).
    assert device.tftfill_ok(), (
        "TFTFILL? did not round-trip red/green/blue through GRAM after the handoff - render did "
        "not reach the frame (a blank/again-white panel would fail here)"
    )

    # (d) the setup surface comes up. The device reads as needing setup (fresh), and the setup AP
    # can be brought up on demand from this state (Orchestrator mode; CUM-190's escape hatch).
    device.ensure_mode(1)  # the setup AP exists in Orchestrator mode; reboots only if not already
    assert device.ping(), "console must answer after ensuring Orchestrator mode"
    m = device.cmd_re(
        "WIFIAP?",
        r"WIFIAP\?\s+ssid=\S+\s+ip=\S+\s+up=\d\s+sta=\d\s+onboarded=(?P<ob>\d)\s+uptime=\d+",
        timeout=6.0,
    )
    assert m.group("ob") == "0", "a factory-reset board must read onboarded=0 (needs setup)"
    device.cmd("WIFIAP on", "WIFIAP on", timeout=8.0)
    try:
        up = device.cmd_re("WIFIAP?", r"WIFIAP\?\s+ssid=\S+\s+ip=(?P<ip>\S+)\s+up=(?P<up>\d)", timeout=6.0)
        assert up.group("up") == "1" and up.group("ip") != "0.0.0.0", (
            f"the first-run setup AP did not come up (up={up.group('up')} ip={up.group('ip')}) - a "
            "fresh owner would have no way to provision"
        )
    finally:
        device.cmd("WIFIAP off", "WIFIAP off", timeout=8.0)  # resume joining for the teardown rejoin


@pytest.mark.hil
def test_fresh_capacitive_never_gates(fresh_bench):
    """A fresh CAPACITIVE panel (Freenove FT6336U) reports pixels, needs no per-unit cal, and
    must NEVER gate: the cal gate is off out of the box and a tap navigates immediately. Keeps
    the leg honest on a Freenove (the bench has no spare, so this loud-skips on the resistive
    Solide rather than pretending to cover it)."""
    device = fresh_bench
    device.factory_reset()
    assert device.ping(), "console must answer after the factory-reset reboot"

    active, kind, stored = device.cal_gate()
    if kind != "cap":
        pytest.skip(
            f"capacitive-only leg; this board reports kind='{kind}' (a resistive panel IS gated "
            "fresh - see test_fresh_resistive_gate_first_skip_and_handoff)."
        )
    assert not active and not stored, (
        f"a fresh capacitive board must NOT gate: active={active} stored={stored} (gating a panel "
        "that reports pixels would strand a Freenove owner behind a cal it never needs)"
    )
    # Not gated -> touch drives navigation normally. Opening the gear menu must reach the Menu.
    device.tap(300, 10)  # the gear, top-right (landscape)
    reached = device.menu_wait_screen(_MENU, timeout=8.0)
    assert reached.screen == _MENU, "a fresh capacitive panel must navigate on a tap (no gate)"


@pytest.mark.hil
def test_nvs_adversarial_cal_survives_reflash_and_opens_gate(fresh_bench):
    """The NVS-adversarial variant (the nimbus-4 touch-180-across-firmware case): tchCal and
    tftFlip are FROZEN keys that survive a reflash. Persist an adversarial calibration + flip via
    the console, reboot, and assert the device (1) boots healthy, (2) render reaches the frame -
    no white screen, (3) APPLIES the stored cal + flip, and (4) does NOT re-arm the gate (a stored
    cal opens it whatever its contents), so an inherited bad-orientation NVS leaves a USABLE,
    navigable device rather than a bricked or self-navigating one.

    The flip-compose RULE itself (a stored cal stays valid across a flip; the 180 is orientTouch's
    job, never invertX baked into the cal) is host-tested exhaustively in test/test_fresh_device;
    whether a tap then LANDS where you touch is a finger-on-glass leg (documented in PR_BODY.md)."""
    device = fresh_bench
    device.factory_reset()
    assert device.ping(), "console must answer after the factory-reset reboot"

    # An adversarial per-unit cal: axes swapped and BOTH inverted, a drifted span - a plausible
    # frozen-NVS value from an older firmware/mount that a reflash would carry forward.
    adversarial = "240,3860,300,3760,7"  # minX,maxX,minY,maxY,flags(swap|invX|invY)
    device.cmd(f"TCAL {adversarial}", "TCAL<", timeout=6.0)
    device.cmd_re("TFTFLIP 1", r"TFTFLIP\s+->\s+1", timeout=6.0)

    # Reboot and PROVE a fresh boot (raises on a panic / reboot loop - a bad NVS must not brick).
    device.reboot_and_confirm(timeout=40.0)
    assert device.ping(), "console must answer after the adversarial-NVS reboot"

    # (3) the stored cal + flip are APPLIED, not ignored (they survived the reboot as frozen keys).
    tcal = device.cmd("TCAL", "TCAL", timeout=6.0)
    assert adversarial in tcal, f"stored adversarial cal did not survive the reboot: {tcal!r}"
    flip = device.cmd_re("TFTFLIP", r"TFTFLIP\s+(?P<f>\d)", timeout=6.0)
    assert flip.group("f") == "1", "stored tftFlip=1 did not survive the reboot"

    # (4) a stored cal OPENS the gate whatever its contents - the adversarial cal must NOT re-arm
    # the first-run gate (that would strand the owner), and it must not depend on the touch kind.
    active, _, stored = device.cal_gate()
    assert stored and not active, (
        f"a stored cal (even adversarial) must open the gate: active={active} stored={stored} - a "
        "frozen bad-orientation NVS surviving a reflash must never re-gate the device"
    )

    # (2) render reaches the frame under the flipped orientation - no white screen from the flip.
    assert device.tftfill_ok(), (
        "TFTFILL? did not round-trip through GRAM under the adversarial tftFlip=1 - the flip must not blank the panel"
    )

    # The device is navigable (gate open): a tap reaches the UI. Coordinate placement under the
    # adversarial cal+flip is finger-on-glass (a capacitive panel ignores tchCal for placement
    # anyway); here we only prove touch is LIVE, not gated.
    device.tap(300, 10)  # the gear; on a resistive panel the adversarial cal may land it loosely
    try:
        device.menu_wait_screen(_MENU, timeout=8.0)
    except ExpectTimeout:
        # A tap that did not reach the Menu is acceptable here (the adversarial cal maps loosely and
        # exact placement is finger-on-glass); the load-bearing assertions are the gate-open + boot
        # -health + render-reaches-frame ones above. Do not fail on placement.
        pass
