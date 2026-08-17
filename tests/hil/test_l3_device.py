"""§L3 - HIL device/serial tests (the HIL test spec).

Boot, selftest gate, encoder events, menu, render-state, notifier broker E2E.
All drive the NIMBUS_TEST serial affordances (plus the existing NIMBUS_NOTIFIER_DEBUG
``renderMenu:`` / ``NSN ..`` / ``ENC ..`` lines); none use a camera.

Ring semantics pinned by lib/core/src/ring_plan.cpp:
  * Active/"Desk" posture (posture=1): ring = seg:<job count>,
  * Passive posture (posture=0):       ring = dark (at most one attention LED = single).

Posture is a device-side precondition; a test whose scenario needs a specific posture
does a LOUD skip naming the posture to set, never a green-by-default.

Gated behind --allow-hardware (see conftest.py). On a device-less box every test here
is a LOUD, REASONED skip.
"""

from __future__ import annotations

import re
import time

import pytest

from device import ExpectTimeout

POSTURE_PASSIVE = 0
POSTURE_ACTIVE = 1  # "Desk" in the plan's UX vocabulary


# ---- boot_ok (F11, F14) ----------------------------------------------------
@pytest.mark.hil
def test_boot_ok(device):
    """boot_ok (F11, F14): reset -> ``READY mode=<n> ip=<..>`` within 20 s, with NO
    ``Guru Meditation`` / ``abort()`` / ``assert failed`` and NO reboot loop
    (>=2 ``rst:`` lines) in the captured boot stream. wait_ready() FAILS on any
    panic marker or rst-loop and only succeeds on the beacon - turning "boots in the
    other mode" from a claim into an assertion.

    A clean beacon over a wedged loop is still a brick, so PING->PONG proves the loop
    is actually running."""
    device.reset()  # pulse reset + reopen across the F13 re-enumeration
    mode, _ip = device.wait_ready(timeout=20.0)
    assert mode in (0, 1, None), f"unexpected boot mode {mode!r}"
    assert device.ping(), "device beaconed READY but PING got no PONG (wedged loop)"


# ---- selftest_gate (F5) ----------------------------------------------------
@pytest.mark.hil
def test_selftest_gate(device):
    """selftest_gate (F5): ``TEST all`` -> a PASS/SKIP verdict; any SKIP must carry a
    REASON token (e.g. ``sd:absent``), never a bare SKIP - no silent aggregate."""
    m = device.selftest("all", timeout=30.0)
    verdict, rest = m.group("verdict"), m.group("rest")
    assert verdict in ("PASS", "SKIP"), f"selftest all FAILED: {m.group(0)}"
    if verdict == "SKIP":
        assert re.search(r"\b\w+:\w+", rest), f"bare SKIP with no reason token: {m.group(0)!r}"


# ---- encoder_events (F1) - manual ------------------------------------------
@pytest.mark.hil
@pytest.mark.manual
def test_encoder_events(device, require_manual):
    """encoder_events (F1): with ``INPUTLOG on`` the firmware echoes every encoder
    event as ``ENC <CW|CCW|CLICK|LONG> ...``. The operator turns the knob both ways
    for ~20 s; assert at least one ENC line arrived (events reach the loop - not "0
    responsive") AND both a CW and a CCW rotation were seen (direction decodes)."""
    device.ensure_status_idle()  # soft precondition - no reset (CDC-race flake)
    device.inputlog(True)

    require_manual.confirm(
        "Turn the encoder knob BOTH ways (several CW and several CCW detents) for "
        "about 20 seconds. Press y the MOMENT you START turning.",
        timeout=30.0,
    )

    lines = device.collect_enc(seconds=20.0)
    device.inputlog(False)

    assert len(lines) > 0, (
        "no ENC events reached the firmware loop while the knob was turned - the exact F1 symptom ('0 responsiveness')"
    )
    kinds = {m.group(1) for ln in lines if (m := re.match(r"ENC\s+(CW|CCW|CLICK|LONG)", ln))}
    assert "CW" in kinds, f"no clockwise detents seen (kinds={sorted(kinds)})"
    assert "CCW" in kinds, f"no counter-clockwise detents seen (kinds={sorted(kinds)})"


# ---- menu_open (F3) - manual -----------------------------------------------
@pytest.mark.hil
@pytest.mark.manual
def test_menu_open(device, require_manual):
    """menu_open (F3): a LONG-press opens the settings menu. The firmware reports
    ``renderMenu: visible=1 items=N sel=S title=..`` on the menu paint (main.cpp) and
    ``RENDER? screen=Menu`` while open. Assert the menu becomes visible with N>0
    items AND a rotate changes the selected index (navigation works) - the long-press
    ->menu path that was never run on hardware."""
    device.ensure_status_idle()  # soft precondition - no reset (CDC-race flake)
    device.inputlog(True)

    require_manual.confirm(
        "LONG-PRESS the encoder (hold the knob down ~1 s) to open the settings menu, "
        "then RELEASE. Press y once you have done the long-press.",
        timeout=30.0,
    )

    try:
        m = device.expect_re(
            r"renderMenu:\s+visible=(?P<vis>\d+)\s+items=(?P<items>\d+)\s+"
            r"sel=(?P<sel>-?\d+)",
            timeout=8.0,
        )
    except ExpectTimeout:
        pytest.fail("no 'renderMenu: visible=..' line after the long-press - the menu did not open (F3)")
    assert m.group("vis") == "1", "menu reported not-visible after long-press"
    items = int(m.group("items"))
    assert items > 0, f"menu opened with {items} items (expected > 0)"
    sel_before = int(m.group("sel"))

    r = device.render(timeout=6.0)
    assert r.screen_name == "Menu", f"RENDER? screen={r.screen_name!r}, expected 'Menu' while menu open"

    require_manual.confirm(
        "Now rotate the knob ONE detent CLOCKWISE to move the menu selection. Press y after you rotate.", timeout=30.0
    )
    m2 = device.expect_re(r"renderMenu:\s+visible=1\s+items=\d+\s+sel=(?P<sel>-?\d+)", timeout=8.0)
    assert int(m2.group("sel")) != sel_before, (
        f"menu selection did not change on rotate ({sel_before}->{m2.group('sel')}) - navigation dead (F3)"
    )


# ---- menu_open AUTOMATED (F3, F18) - no knob needed -------------------------
# ScreenId numbers (device.SCREEN_NAMES order == attention.h ScreenId).
_STATUS_IDLE = 0
_MENU = 3
_CONFIG_QR = 9


@pytest.mark.hil
def test_menu_open_close_injected(device):
    """menu_open (F3/F18), AUTOMATED via the ``ENC`` inject: a synthetic LONG-press
    must render the Menu screen within one refresh, and a second LONG-press must
    return to StatusIdle. This is the regression guard for the live 'long-press
    does nothing' bug - it fails LOUDLY (ExpectTimeout) if the menu never paints,
    instead of needing a human to hold the knob."""
    device.ensure_status_idle()  # soft precondition - no reset (CDC-race flake)

    device.enc_double_click()  # open (double-click since the voice UX:
    # Orch long-press = hold-to-talk)
    r = device.menu_wait_screen(_MENU)  # raises if the menu never appears
    assert r.screen_name == "Menu"

    device.enc_settle("LONG")  # close (Main -> StatusIdle)
    assert device.menu_wait_screen(_STATUS_IDLE).screen == _STATUS_IDLE


@pytest.mark.hil
def test_menu_navigation_injected(device):
    """Rotation while the menu is open keeps the panel on the Menu screen (the menu
    owns the panel and repaints per event). Drives ENC CW/CCW with no knob."""
    device.ensure_status_idle()  # soft precondition - no reset (CDC-race flake)
    device.enc_double_click()  # menu opens on double-click (both modes)
    device.menu_wait_screen(_MENU)
    device.enc_settle("CW")
    assert device.render().screen == _MENU, "rotate dropped the menu off the panel"
    device.enc_settle("CCW")
    assert device.render().screen == _MENU
    device.enc_settle("LONG")  # close
    device.menu_wait_screen(_STATUS_IDLE)


@pytest.mark.hil
def test_config_qr_via_connectivity(device):
    """Config QR is reachable via Settings > Connectivity > Config via QR and the
    device renders the full-screen ConfigQr screen (screen=9). Exercises the P-C/P-D
    menu path end-to-end on hardware: open -> 3x CW to Connectivity -> click ->
    click Config via QR -> ConfigQr."""
    device.ensure_status_idle()  # soft precondition - no reset (CDC-race flake)
    device.enc_double_click()  # menu opens on double-click (both modes)
    device.menu_wait_screen(_MENU)
    # Main rows: Mode(0), Power profile(1), Tune(2), Connectivity(3), ...
    for _ in range(3):
        device.enc_settle("CW")
    device.enc_settle("CLICK")  # -> Connectivity submenu (cursor on WiFi status, row 0)
    assert device.render().screen == _MENU, "Connectivity submenu should still be a Menu list"
    # Rows (settings_menu.h ConnRow): Wifi(0) Bluetooth(1) Forget(2) SdProbe(3)
    # ConfigQr(4) Token(5) Back(6)
    for _ in range(4):  # Wifi(0) -> ... -> Config via QR(4)
        device.enc_settle("CW")
    device.enc_settle("CLICK")  # -> ConfigQr
    r = device.menu_wait_screen(_CONFIG_QR)
    assert r.screen_name == "ConfigQr"
    device.enc_settle("LONG")  # back to Connectivity (still Menu)
    device.enc_settle("LONG")  # back to Main
    device.enc_settle("LONG")  # close
    device.menu_wait_screen(_STATUS_IDLE)


@pytest.mark.hil
def test_raw_switch_reads_idle(device):
    """SW? (F18 diagnostic): the raw debounced encoder switch reads 0 (released)
    when nobody is pressing it. Proves the GPIO48 switch path is wired and readable
    - the tool that tells a physical-button fault apart from a decode/render fault."""
    device.ensure_status_idle()  # soft precondition - no reset (CDC-race flake)
    assert device.sw() == 0, "raw switch reads pressed with no one touching it (GPIO48?)"


@pytest.mark.hil
def test_bluetooth_toggle(device):
    """Connectivity > Bluetooth toggles the BLE radio LIVE (no reboot). In Notifier
    mode BLE boots enabled (advertising); navigating to the Bluetooth row and
    clicking must flip BLE? enabled 1->0, and clicking again 0->1. Restores BLE ON
    in a finally so a mid-test failure can't leave the device's only transport off."""
    device.ensure_mode(0)  # BLE only runs in Notifier mode
    device.wait_ready(timeout=20.0) if False else None
    en, _ = device.ble_state()
    assert en == 1, "BLE not advertising at boot in Notifier mode (Connectivity default on)"
    try:
        device.enc_double_click()  # menu opens on double-click (both modes)
        device.menu_wait_screen(_MENU)
        for _ in range(3):  # Main -> Connectivity (row 3)
            device.enc_settle("CW")
        device.enc_settle("CLICK")  # -> Connectivity submenu, cursor on WiFi status (0)
        device.enc_settle("CW")  # -> Bluetooth (row 1; see ConnRow order)
        device.enc_settle("CLICK")  # toggle OFF
        en, _ = device.ble_state()
        assert en == 0, "Bluetooth toggle did not stop the radio (BLE? still enabled)"
        device.enc_settle("CLICK")  # toggle ON
        en, _ = device.ble_state()
        assert en == 1, "Bluetooth toggle did not restart the radio (BLE? still off)"
    finally:
        # Guarantee BLE ends ON regardless of where an assert fired.
        for _ in range(4):
            e, _c = device.ble_state()
            if e == 1:
                break
            device.enc_settle("CLICK")  # re-toggle until enabled (or give up)
        device.enc_settle("LONG")  # leave the Bluetooth row / submenu
        device.enc_settle("LONG")
        device.enc_settle("LONG")  # close the menu


# ---- render_state (F2, F4) -------------------------------------------------
@pytest.mark.hil
def test_render_three_jobs_desk(device, nsn):
    """render_state (F2, F4): drive 3 nsn Running jobs; in Active/"Desk" posture the
    ring must report ``seg:3``. LOUD-skip in Passive posture (which would correctly
    show ``dark`` and mask the assertion)."""
    device.ensure_mode(0)  # nsn frames need Notifier mode
    # no reboot: state comes from the frames we send; reboot churn is the wedge
    # correlate (see device.py::_open_quiet doc).

    r0 = device.render(timeout=6.0)
    if r0.posture != POSTURE_ACTIVE:
        pytest.skip(
            f"device posture={r0.posture} (Passive); set Active/Desk posture for the seg-count assertion (F2/F4)"
        )

    nsn.send_frame([nsn.running(170), nsn.running(85), nsn.running(20)], brightness=90)
    time.sleep(1.0)  # let the ring recompose (BLE write latency > serial's)

    r = device.render(timeout=6.0)
    assert r.ring == "seg:3", (
        f"ring={r.ring!r}, expected 'seg:3' for 3 jobs in Active posture (F2/F4 - ring never asserted on-device before)"
    )


@pytest.mark.hil
def test_render_passive_dark(device, nsn):
    """render_state (F2, F4): in Passive posture with NO attention jobs the ring is
    dark - the exact "looks dead but is fine" state behind F2. Clear all jobs, then
    assert ``ring=dark``. LOUD-skip in Active posture (which shows ``seg:0``)."""
    device.ensure_mode(0)  # nsn frames need Notifier mode
    # no reboot: state comes from the frames we send; reboot churn is the wedge
    # correlate (see device.py::_open_quiet doc).

    r0 = device.render(timeout=6.0)
    if r0.posture != POSTURE_PASSIVE:
        pytest.skip(f"device posture={r0.posture} (Active); set Passive posture for the dark-ring assertion (F2/F4)")

    nsn.send_frame([], brightness=0)  # clear all
    time.sleep(1.0)  # BLE write latency > serial's

    r = device.render(timeout=6.0)
    assert r.ring == "dark", (
        f"ring={r.ring!r}, expected 'dark' for Passive + no attention (F2 - the state that looked dead)"
    )


@pytest.mark.hil
@pytest.mark.manual
def test_render_menu_screen(device, require_manual):
    """render_state (F3, F4): a long-press opens the menu, and ``RENDER?`` must report
    ``screen=Menu`` - proving the menu surface reaches the panel (F3)."""
    device.ensure_status_idle()  # soft precondition - no reset (CDC-race flake)

    require_manual.confirm("LONG-PRESS the encoder to open the settings menu. Press y after.", timeout=30.0)
    try:
        r = device.render(timeout=6.0)
    except ExpectTimeout:
        pytest.fail("RENDER? did not answer after the long-press (menu path dead)")
    assert r.screen_name == "Menu", f"RENDER? screen={r.screen_name!r}, expected 'Menu' after long-press (F3/F4)"


# ---- notifier_broker_e2e (F2, F4) ------------------------------------------
@pytest.mark.hil
def test_notifier_broker_e2e(device, nsn):
    """notifier_broker_e2e: the three tools/nsn_send.py scenarios drive distinct,
    asserted device state via the REAL broker encoder:

      scenario       frame                              jobs  attn
      -------------  ---------------------------------  ----  ----
      two running    2x Running                          2     0
      + approval     2x Running + 1 AwaitingApproval     3     1
      clear all      empty                               0     0

    For each, assert the ``NSN jobs=<n> attn=<a>`` echo matches; and in Active posture
    the ``RENDER? ring=seg:N`` count matches the job count (attention adds no
    segment; it lights ``single`` in Passive)."""
    device.ensure_mode(0)  # nsn frames need Notifier mode
    # no reboot: state comes from the frames we send; reboot churn is the wedge
    # correlate (see device.py::_open_quiet doc).

    try:
        probe = device.render(timeout=6.0)
    except ExpectTimeout:
        pytest.skip("RENDER? not answering - needs the NIMBUS_TEST/notifierdbg build")
    active = probe.posture == POSTURE_ACTIVE

    scenarios = [
        ("two running", [nsn.running(170), nsn.running(85)], 60, 2, 0),
        ("add an approval", [nsn.running(170), nsn.running(85), nsn.awaiting_approval(32)], 90, 3, 1),
        ("clear all", [], 0, 0, 0),
    ]
    for name, segs, bright, want_jobs, want_attn in scenarios:
        nsn.send_frame(segs, bright)
        try:
            # BLE-tagged echo (net::ble::drain's debug print) - serial no longer
            # carries frames, so the untagged "NSN jobs=.." line is gone with it.
            # A longer timeout than the old serial version: BLE has real
            # scan/connect/write latency a synchronous serial write never had.
            m = device.expect_re(
                r"NSN\(ble\)\s+jobs=(?P<jobs>\d+)\s+attn=(?P<attn>\d+)\s+bright=(?P<b>\d+)", timeout=8.0
            )
        except ExpectTimeout:
            pytest.fail(
                f"[{name}] no 'NSN(ble) jobs=..' echo - frame dropped, BLE "
                "not connected, or not a notifierdbg/NIMBUS_TEST build"
            )
        assert int(m.group("jobs")) == want_jobs, f"[{name}] jobs={m.group('jobs')}, expected {want_jobs}"
        assert int(m.group("attn")) == want_attn, f"[{name}] attn={m.group('attn')}, expected {want_attn}"

        if active:
            time.sleep(0.3)
            r = device.render(timeout=6.0)
            if want_jobs == 0:
                assert r.ring in ("dark", "seg:0"), f"[{name}] ring={r.ring!r}, expected dark/seg:0"
            else:
                assert r.ring == f"seg:{want_jobs}", f"[{name}] ring={r.ring!r}, expected seg:{want_jobs}"
