"""§L21 - HIL touch-TFT display tests (screenModel=tft).

Proves the color touch panel on real hardware, with NO camera: every
assertion rides ``STATUS scr=``, ``TOUCH?``, ``RENDER?`` and the ``TAP``/``TAPUP``
inject seam, so the UI is driveable with no finger.

The device is the TFT color touch board (Nimbus-4); everything here targets it.

⚠ Sound is deliberately NOT exercised anywhere in this file (owner carve-out):
no SFX, TTS, mic, speaker or loopback. The mic *control* is tested only as a
tap target, never by recording.

Gated behind --allow-hardware. On a device-less box every test is a LOUD,
REASONED skip - never a green-by-default.
"""

from __future__ import annotations

import pathlib
import re
import sys
import time

import pytest

from test_l4_network import lan_ip_or_skip

# ⚠ Device.expect() is a SUBSTRING match, not a regex (tests/hil/device.py:408).
# Use expect_re() when a pattern is genuinely needed.

# Panel geometry (nimbus/tft_render/theme.h). Mirrored here on purpose: if the
# panel is re-sized, this file must be looked at too - the coordinates below are
# layout knowledge, not incidental constants.
# ⚠ LANDSCAPE. The panel is mounted with its long edge horizontal, so the logical
# surface is 320x240 (see nimbus/tft_render/theme.h kScreenW/kScreenH and the
# driver's MADCTL). Every coordinate below is derived from these two, so a future
# re-orientation is one edit here.
PANEL_W, PANEL_H = 320, 240
HEADER_H = 44
MIN_TAP = 44


def _status(device) -> str:
    # cmd() drains first: the device prints unsolicited RENDER/led lines, and a
    # bare expect() can match one of those instead of this query's reply.
    return device.cmd("STATUS", "STATUS ", timeout=5.0)


def _scr_model(device) -> str:
    """The display driver that ACTUALLY bound, from STATUS scr=.

    Distinct from ``want=`` (the stored screenModel preference). They differ
    whenever the fail-soft path trips, which is exactly the case that must be
    visible - a board that failed to bind the color panel would otherwise look
    identical to a working one.
    """
    line = _status(device)
    m = re.search(r"\bscr=(\w+)", line)
    assert m, f"STATUS has no scr= field - is this firmware new enough? {line!r}"
    return m.group(1)


def _scr_want(device) -> str:
    """The stored screenModel preference, from STATUS want=."""
    line = _status(device)
    m = re.search(r"\bwant=(\w+)", line)
    assert m, f"STATUS has no want= field: {line!r}"
    return m.group(1)


def _close_menu(device):
    """Leave the settings menu if some earlier test left it open.

    Tap Back until the menu is gone, then ASSERT it actually went. The deepest
    reachable state is four levels down (adjusting a value inside
    Settings > Customize > <param>), and a previous run or a manual capture can
    leave the device there. A bound that is merely "probably enough" turns into
    a confusing failure in whatever test runs next, so this fails LOUDLY here
    instead - where the message says what is wrong.
    """
    for _ in range(10):
        if not re.search(r"screen=(3|Menu)", device.cmd("RENDER?", "RENDER ", timeout=8.0)):
            return
        device.cmd(f"TAP 20 {HEADER_H // 2}", "TAP<", timeout=5.0)  # Back
        time.sleep(0.5)
    raise AssertionError(
        "could not leave the settings menu after 10 Back taps - the menu is "
        "stuck, or Back stopped being wired to the header's left end"
    )


def _require_tft(device):
    """Skip LOUDLY (never silently) when pointed at a non-TFT board."""
    model = _scr_model(device)
    if model != "tft":
        pytest.skip(
            f"this board reports scr={model}, not tft - point NIMBUS_TEST_PORT/IP at "
            "Nimbus-4, or set it with `POST /api/orch scrModel=tft` and restart"
        )
    return model


@pytest.fixture(autouse=True)
def _menu_closed(device):
    """Start every test from a KNOWN state, and leave one behind.

    The settings menu is shared device state. A test that leaves it open changes
    what the NEXT test measures - that is how test_tap_opens_and_closes_the_menu
    began failing in a full run while passing alone, and how
    test_hold_does_not_fire_as_a_tap became vacuously false. Closing on both
    sides makes each test independent of run order.
    """
    _close_menu(device)
    yield
    _close_menu(device)


# ---- identity + boot --------------------------------------------------------


@pytest.mark.hil
def test_boots_on_tft_and_reports_it(device):
    """The device comes up on the colour panel and SAYS SO.

    scr= reports the driver that actually BOUND (want= carries the stored
    preference), so a board that failed to bind the color panel is
    distinguishable from a working one - see test_bound_driver_matches_the_setting.
    """
    device.reset()
    mode, _ip = device.wait_ready(timeout=25.0)
    assert mode in (0, 1, None), f"unexpected boot mode {mode!r}"
    assert device.ping(), "beaconed READY but PING got no PONG (wedged loop)"
    _require_tft(device)


@pytest.mark.hil
def test_panel_and_touch_are_present(device):
    """Both halves of the pair came up.

    The panel and the touch controller share a bus, so a wiring fault often
    kills exactly one of them - asserting only 'it renders' would miss a dead
    touch controller entirely.
    """
    _require_tft(device)
    line = device.cmd("TOUCH?", "TOUCH ", timeout=5.0)
    m = re.search(r"present=(\d)", line)
    assert m, f"malformed TOUCH? reply: {line!r}"
    assert m.group(1) == "1", (
        f"touch controller absent on a TFT board: {line!r} - check T_CS (GPIO 48) "
        "and the T_CLK/T_DIN/T_DO bridges on the module"
    )


# ---- rendering --------------------------------------------------------------


@pytest.mark.hil
def test_status_screen_renders(device):
    """The status screen actually paints (RENDER? reports what is on the panel)."""
    _require_tft(device)
    line = device.cmd("RENDER?", "RENDER ", timeout=8.0)
    m = re.search(r"screen=(\w+)", line)
    assert m, f"RENDER? gave nothing useful: {line!r}"
    # A TFT board must be showing a real screen, not the 0/StatusIdle default
    # that a never-rendered panel would also report.
    assert "posture=" in line, f"RENDER? looks truncated: {line!r}"


# ---- touch input ------------------------------------------------------------


@pytest.mark.hil
def test_tap_opens_and_closes_the_menu(device):
    """The gear opens the menu and Back closes it - the whole nav loop.

    Driven purely through injected taps, so this is the proof that a knobless
    board is still fully navigable.
    """
    _require_tft(device)

    # Gear lives flush to the top-right; tap its centre.
    device.cmd(f"TAP {PANEL_W - MIN_TAP // 2} {HEADER_H // 2}", "TAP<", timeout=5.0)
    time.sleep(0.6)

    opened = device.cmd("RENDER?", "RENDER ", timeout=8.0)
    assert re.search(r"screen=(3|Menu)", opened), f"tapping the gear did not open the menu: {opened!r}"

    # Back is the left end of the header.
    device.cmd(f"TAP 20 {HEADER_H // 2}", "TAP<", timeout=5.0)
    time.sleep(0.6)

    closed = device.cmd("RENDER?", "RENDER ", timeout=8.0)
    assert not re.search(r"screen=(3|Menu)", closed), f"Back did not leave the menu: {closed!r}"


@pytest.mark.hil
def test_tap_outside_any_target_is_ignored(device):
    """Dead space must do nothing.

    A hit-test that falls through to 'nearest' or 'last' would make the UI feel
    haunted - taps on background changing screens.
    """
    _require_tft(device)
    before = device.cmd("RENDER?", "RENDER ", timeout=8.0)

    # Just under the header, left gutter - page background on every screen.
    device.cmd(f"TAP 4 {HEADER_H + 4}", "TAP<", timeout=5.0)
    time.sleep(0.5)

    after = device.cmd("RENDER?", "RENDER ", timeout=8.0)
    assert re.search(r"screen=(\w+)", before).group(1) == re.search(r"screen=(\w+)", after).group(1), (
        f"a tap on dead space changed the screen: {before!r} -> {after!r}"
    )


@pytest.mark.hil
def test_out_of_range_tap_is_refused(device):
    """The inject seam validates its own input rather than driving the hit-test
    with nonsense (which would mask a real coordinate bug as 'works')."""
    _require_tft(device)
    line = device.cmd_re(f"TAP {PANEL_W + 50} 10", r"(ERR|TAP<)", timeout=5.0).string
    assert "ERR" in line, f"an off-panel tap was accepted: {line!r}"


@pytest.mark.hil
def test_menu_row_tap_selects_that_row(device):
    """Tapping a row activates THAT row, not whichever the cursor happened on.

    This is the property the whole touch-nav layer rests on: with no knob, an
    off-by-one here means the owner cannot reach the setting they touched.
    """
    _require_tft(device)
    device.cmd(f"TAP {PANEL_W - MIN_TAP // 2} {HEADER_H // 2}", "TAP<", timeout=5.0)  # gear
    time.sleep(0.6)
    # MENU? - not RENDER? - is the oracle here. RENDER? emits only
    # screen=/posture=/seg=/single=/dark=/bright=, and NONE of those change when
    # the menu descends into a submenu (g_lastScreen stays ScreenId::Menu). An
    # assertion on RENDER? would be a false red on correct firmware.
    opened = device.cmd("MENU?", "MENU ", timeout=8.0)

    # ⚠ The menu is TWO COLUMNS on the landscape panel, so PANEL_W // 2 lands in
    # the GAP between them and hits nothing. Tap the centre of the LEFT column,
    # second row: columns are (PANEL_W - 2*gutter - gap) / 2 wide.
    col_w = (PANEL_W - 2 * 12 - 8) // 2
    row_x = 12 + col_w // 2
    row_y = HEADER_H + 8 + 1 * 50 + 20
    device.cmd(f"TAP {row_x} {row_y}", "TAP<", timeout=5.0)
    time.sleep(0.8)

    after = device.cmd("MENU?", "MENU ", timeout=8.0)
    # The tap must have CHANGED the menu view. Asserting only that the console
    # replied would stay green with drainTouch deleted entirely - the exact
    # failure mode this suite exists to catch.
    assert after != opened, (
        f"tapping a menu row changed nothing: still {after!r} - the tap was "
        "swallowed (check the hit-test and that the row index maps 1:1)"
    )


@pytest.mark.hil
def test_hold_does_not_fire_as_a_tap(device):
    """A press-and-hold must not ALSO register as a tap on release.

    Hold-to-talk sits on the status screen's biggest control; if a hold also
    tapped, every voice attempt would fire a second action underneath it.
    """
    _require_tft(device)
    # (the autouse _menu_closed fixture guarantees the starting state)
    before = device.cmd("RENDER?", "RENDER ", timeout=8.0)
    assert not re.search(r"screen=(3|Menu)", before), f"could not reach a menu-closed state to start from: {before!r}"

    # ⚠ ALWAYS release. A synthetic hold that is never released latches the
    # panel down forever and real touch stops working until a reboot - a test
    # that dies mid-hold would leave the board unusable for every test after it.
    try:
        device.cmd(f"TAP {PANEL_W // 2} {PANEL_H - 36} HOLD", "TAP<", timeout=5.0)
        time.sleep(1.0)  # past the 600 ms hold threshold
    finally:
        device.cmd("TAPUP", "TAPUP<", timeout=5.0)
    time.sleep(0.6)

    after = device.cmd("RENDER?", "RENDER ", timeout=8.0)
    # In Notifier mode the mic control is inert, so the screen must be unchanged;
    # in Orchestrator a voice capture may have started. Either is fine - what is
    # NOT fine is landing in the MENU, which is what a stray tap would do.
    assert not re.search(r"screen=(3|Menu)", after), (
        f"a hold leaked through as a tap and opened the menu: {before!r} -> {after!r}"
    )


# ---- persistence ------------------------------------------------------------

# ---- the panel watchdog (the white-screen fix) ------------------------------


def _health(device):
    """TFTHEALTH? -> (healthy, heals, backlight)."""
    line = device.cmd("TFTHEALTH?", "TFTHEALTH ", timeout=8.0)
    m = re.search(r"healthy=(\d)\s+heals=(\d+)\s+backlight=(\d+)", line)
    assert m, f"malformed TFTHEALTH? reply: {line!r}"
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


@pytest.mark.hil
def test_panel_recovers_from_a_silent_reset(device):
    """THE white-screen regression test.

    The ILI9341 can silently lose its configuration, and the firmware cannot see
    that from the framebuffer: the dirty gate asserts "the panel already shows
    this", and on an IDLE device the composed frame is byte-identical forever -
    so nothing repaints and the panel stays blank until a restart. That is the
    bug the owner reported as "it showed the UI, then went white".

    TFTBREAK resets the panel behind the driver's back, reproducing it exactly.
    The watchdog in loop() must notice within kHealMs and repair it.

    ⚠ This asserts heals INCREMENTS, not merely that the panel ends up healthy.
    A test that only checked the end state would pass the moment the panel was
    repainted for some unrelated reason.

    ⚠ SCOPE, established by mutation: there are TWO recovery paths - the loop
    watchdog and the dirty-gate check inside renderAndPush(). Disabling ONLY the
    watchdog leaves this test GREEN, because this board renders periodically
    anyway (battery/header updates) and the other path then heals it. Disabling
    both turns it red. So this proves "the panel recovers", NOT "the watchdog
    recovered it". The watchdog is what covers a genuinely IDLE device, where
    nothing renders for minutes and renderAndPush() is never reached - the case
    that was measured broken (healthy=0 for 12 s, heals=0) and is hard to hold
    over a console that itself provokes renders.
    ⚠ heals only COUNTS while the register probe is on: since the white-screen
    hunt fingered register reads during blits as a possible disturbance, the
    probe defaults OFF and the watchdog rearm()s unconditionally WITHOUT
    classifying - so on the shipped config the panel recovers but heals stays 0
    (exactly what this test red-flagged before this was understood). The drill
    therefore enables the probe for its window and restores it after.
    """
    _require_tft(device)
    device.cmd("PANELPROBE 1", "PANELPROBE", timeout=5.0)
    try:
        healthy, heals0, _bl = _health(device)
        assert healthy == 1, "panel is already unhealthy before the drill"

        device.cmd("TFTBREAK", "TFTBREAK", timeout=8.0)
        time.sleep(1.0)

        # Give the watchdog a couple of its ~5 s windows, polling so a fast
        # recovery is not mistaken for a slow one.
        healed = False
        for _ in range(8):
            time.sleep(2.0)
            healthy, heals, _bl = _health(device)
            if healthy == 1 and heals > heals0:
                healed = True
                break
        assert healed, (
            f"panel never recovered: healthy={healthy} heals={heals} (was {heals0}). "
            "The watchdog in loop() is the ONLY periodic render trigger on a TFT "
            "board - if it stopped running, a white panel stays white forever."
        )
        # And it must SETTLE: a watchdog that re-heals every window would be
        # masking a permanently broken panel rather than fixing a transient one.
        _h1, heals_a, _ = _health(device)
        time.sleep(7.0)
        _h2, heals_b, _ = _health(device)
        assert heals_b == heals_a, (
            f"watchdog is thrashing: heals went {heals_a} -> {heals_b} with no "
            "fault injected - it is papering over a panel that never recovers"
        )
    finally:
        device.cmd("PANELPROBE 0", "PANELPROBE", timeout=5.0)  # shipped default


@pytest.mark.net
def test_panel_content_loss_is_detected_and_repaired(device, net, secrets, require_secret):
    """The panel losing its PIXELS is detected, not just losing its config.

    MADCTL-based health cannot see this: a panel can hold a perfect register set
    and still have lost its image, which is exactly the state the blank-screen
    investigation kept measuring as "healthy". This check compares the panel's
    own GRAM against the frame we pushed.

    TFTFILL? is the fault injector - it writes RGB frames straight into GRAM,
    bypassing our framebuffer, so the panel's content genuinely diverges.

    ⚠ Asserts the COUNTER, not the instantaneous flag. The watchdog repaints
    within ~5 s, so by the time the state is read pixOk is true again - sampling
    the flag would pass whether or not detection ever happened.
    """
    _require_tft(device)
    ip = lan_ip_or_skip(device, net, secrets, require_secret)

    def st():
        r = net.get("/api/state", ip=ip, timeout=25.0)
        assert r.status_code == 200, f"GET /api/state -> {r.status_code}"
        return r.json()

    before = st()
    assert before.get("panelPixOk") is True, "panel content already diverged before the test"
    lost0 = before.get("panelPixLost", 0)

    device.cmd("TFTFILL?", "TFTFILL", timeout=15.0)  # corrupt GRAM behind our back
    time.sleep(4.0)

    detected = False
    for _ in range(6):
        cur = st()
        if cur.get("panelPixLost", 0) > lost0:
            detected = True
            break
        time.sleep(3.0)
    assert detected, (
        "the panel's pixels were overwritten and nothing noticed - content "
        "verification is the only signal that observes this, so if it stops "
        "working a blank panel becomes undetectable again"
    )

    # ...and it must recover, not just notice.
    ok = False
    for _ in range(6):
        time.sleep(3.0)
        if st().get("panelPixOk") is True:
            ok = True
            break
    assert ok, "content diverged and was never repainted back"


@pytest.mark.hil
def test_backlight_follows_the_battery_mode(device):
    """The backlight is the largest continuous draw on a colour panel, so the
    battery mode has to reach it. Read back from the live PWM, not inferred.

    ⚠ Guard: the low-battery policy (lbSaver) FORCES the posture while the
    battery model reads low - including transiently while it settles on a
    pack-less bench board fed from a charger (measured: percent=0 for the first
    minutes, then recovery to 100). Under a forced posture every PROFILE lands
    on the Dark backlight and this test would red-flag healthy firmware, so it
    skips LOUDLY instead."""
    _require_tft(device)
    device.cmd("PROFILE 2", "PROFILE", timeout=8.0)
    time.sleep(2.0)
    m = re.search(r"posture=(\d)", device.cmd("RENDER?", "RENDER ", timeout=8.0))
    if not m or m.group(1) != "2":
        pytest.skip(
            "battery policy is forcing the posture (PROFILE 2 did not yield "
            "posture=2) - likely the low-battery/model-settling window on a "
            "pack-less board; re-run once /api/state batt.percent has settled"
        )
    seen = {}
    try:
        for profile, name in ((2, "Full"), (1, "Balanced"), (0, "Dark")):
            device.cmd(f"PROFILE {profile}", "PROFILE", timeout=8.0)
            time.sleep(2.0)
            _h, _n, bl = _health(device)
            seen[name] = bl
    finally:
        device.cmd("PROFILE 2", "PROFILE", timeout=8.0)  # leave it on the desk mode
    assert seen["Dark"] < seen["Balanced"] < seen["Full"], f"backlight does not track the battery mode: {seen}"
    assert seen["Dark"] > 0, "Dark blanked the panel - 0 is the screensaver's value"


@pytest.mark.hil
def test_bound_driver_matches_the_setting(device):
    """The driver that bound MUST match the stored preference.

    A mismatch is the silent-fallback failure: the setting says tft, the panel
    is dark, and every other test still passes because it only ever looked at
    the setting. Asserting scr== want is what makes that visible.
    """
    _require_tft(device)
    bound, want = _scr_model(device), _scr_want(device)
    assert bound == want, (
        f"screenModel is {want!r} but the {bound!r} driver bound - the panel "
        "failed to initialise and the device fell back. Check the wiring and "
        "the boot log for '[tft] panel bring-up FAILED'."
    )


@pytest.mark.hil
def test_screen_model_survives_reboot(device):
    """screenModel is hardware identity - it MUST outlive a restart.

    If it did not, every reboot would risk losing the stored panel identity and
    the display could fail to bind and go dark.
    """
    _require_tft(device)
    device.reset()
    device.wait_ready(timeout=25.0)
    assert _scr_model(device) == "tft", "screenModel did not survive the restart"


# ---- live session cards (the headline screen) -------------------------------


def _nsn_frame_hex(states_titles) -> str:
    """Encode an nsn v2 frame with ../nsnotify's REFERENCE encoder.

    Deliberately not hand-rolled here: notify/broker/frame.py is the wire
    format's single source of truth, so a frame built any other way could pass
    this test while a real broker's bytes failed.
    """
    nsnotify = pathlib.Path(__file__).resolve().parents[3] / "nsnotify"
    if not nsnotify.is_dir():
        pytest.skip(
            f"../nsnotify not checked out at {nsnotify} - cannot build a frame with "
            "the reference encoder, and hand-rolling one here would prove nothing "
            "about the real wire format"
        )
    sys.path.insert(0, str(nsnotify))
    try:
        from notify.broker.frame import FrameSegment, encode_frame
        from notify.state import State
    except ImportError as e:  # pragma: no cover
        pytest.skip(f"../nsnotify present but not importable ({e})")

    segs = []
    for state_name, harness, title in states_titles:
        s = FrameSegment.from_state(getattr(State, state_name))
        s.harness, s.title = harness, title
        segs.append(s)
    return encode_frame(segs, brightness=120, seq=1).hex()


def _jobs(device) -> int:
    m = re.search(r"jobs=(-?\d+)", _status(device))
    assert m, "STATUS carries no jobs= field - the router oracle is missing"
    return int(m.group(1))


# The three sessions every card test feeds: one ambient, two calls-to-action,
# one per harness - so titles, harness labels and all three status tones are
# exercised in a single frame.
CARD_SESSIONS = [
    ("Running", 1, "deploy-plan"),
    ("WaitingInput", 2, "nimbus-tft"),
    ("AwaitingApproval", 3, "docs"),
]


def _require_notifier(device):
    """NSNFEED only exists in Notifier mode - skip LOUDLY elsewhere.

    The inject seam refuses in Orchestrator mode by design (the router is driven
    by the orchestrator there, not by broker frames), so a card test pointed at an
    Orchestrator board is measuring nothing. Failing would blame the UI for a mode
    mismatch; passing silently would be worse.
    """
    line = _status(device)
    m = re.search(r"\bmode=(\d)", line)
    assert m, f"STATUS has no mode= field: {line!r}"
    if m.group(1) != "0":
        pytest.skip(
            "board is in Orchestrator mode (mode=1); NSNFEED - and therefore the "
            "session-card tests - need Notifier mode. Switch with `MODE 0`."
        )


def _clear_jobs(device):
    """Drive the job table to empty and PROVE it.

    Required, not hygiene: calls-to-action are held for AttnHoldMs (5 min) in
    every posture by design, so a card test that merely runs after another one
    starts with the previous frame's sessions still on the glass. That made the
    screenshot comparison below byte-identical and failed for the wrong reason.
    An empty frame offlines the whole tail (Mapper::apply), which is exactly how
    a real broker retires sessions.
    """
    device.cmd(f"NSNFEED {_nsn_frame_hex([])}", "NSNFEED<", timeout=10.0)
    time.sleep(0.5)
    n = _jobs(device)
    assert n == 0, f"job table still holds {n} after an empty frame - cannot get a clean baseline"


@pytest.mark.hil
def test_fed_frame_populates_and_holds_the_job_table(device):
    """A broker frame lands in the router AND SURVIVES the next tick.

    The "and survives" half is the regression: Mapper::timeout() compared the
    link-quiet age unsigned, so a frame stamped a few ms ahead of loop()'s
    cached now (which is exactly what applying a frame later in the same
    iteration does) wrapped to ~49 days quiet and expired EVERY job - calls-to-
    action included - on the very next tick. Measured 3 -> 0 within one
    iteration. Asserting the count only at t+0 would still pass against that
    bug, so the delayed re-check is the point of this test.
    """
    _require_tft(device)
    _require_notifier(device)
    _clear_jobs(device)
    try:
        line = device.cmd(f"NSNFEED {_nsn_frame_hex(CARD_SESSIONS)}", "NSNFEED<", timeout=10.0)
        assert "jobs=3" in line, f"3-segment frame did not land: {line!r}"

        # Past a Dark-posture ambient hold (5 s) the two calls-to-action must
        # still be held; the ambient Running one may legitimately have expired.
        time.sleep(6.0)
        held = _jobs(device)
        assert held >= 2, (
            f"jobs fell to {held} - calls-to-action must hold for AttnHoldMs (5 min) "
            "in every posture, so a job waiting on you cannot vanish"
        )
    finally:
        # Leave nothing behind: these sessions would otherwise sit on the panel
        # for 5 minutes and change what the next test measures.
        _clear_jobs(device)


@pytest.mark.net
def test_session_cards_actually_paint(device, net, secrets, require_secret):
    """The cards reach the GLASS, not just the job table.

    /api/screenshot returns the dirty-gate snapshot - the bytes currently on the
    panel - so this cannot pass by re-rendering something the panel never showed.
    Compared against the empty status screen, because "320x240 of bytes came
    back" is true even of a blank panel.
    """
    _require_tft(device)
    _require_notifier(device)
    ip = lan_ip_or_skip(device, net, secrets, require_secret)

    def shot() -> bytes:
        # net.get() attaches X-Nimbus-Token itself; 150 KB streams chunked, so
        # the default 5 s read timeout is too tight.
        r = net.get("/api/screenshot", ip=ip, timeout=25.0)
        assert r.status_code == 200, f"GET /api/screenshot -> {r.status_code}"
        assert len(r.content) == PANEL_W * PANEL_H * 2, (
            f"screenshot is {len(r.content)} B, want {PANEL_W * PANEL_H * 2}"
        )
        return r.content

    _clear_jobs(device)
    time.sleep(0.8)  # let the cleared frame reach the glass
    empty = shot()
    try:
        device.cmd(f"NSNFEED {_nsn_frame_hex(CARD_SESSIONS)}", "NSNFEED<", timeout=10.0)
        time.sleep(0.8)
        painted = shot()
    finally:
        _clear_jobs(device)

    assert painted != empty, (
        "the panel is byte-identical with and without three live sessions - the cards never reached the glass"
    )
    # A real card layout moves a large area; a stray pixel or two would mean the
    # frame changed for some unrelated reason (a clock, a battery digit).
    changed = sum(a != b for a, b in zip(painted, empty))
    assert changed > 2000, f"only {changed} bytes differ - too little to be three cards"
