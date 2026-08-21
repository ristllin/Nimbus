"""§L27 - HIL Freenove ESP32-S3 CYD board checks (SOLIDE_BOARD=freenove_s3).

Scopes to the Freenove all-in-one board's HARDWARE DELTAS from the Solide TFT -
the pieces that only this pinout has: the capacitive touch controller (FT6336U,
not the Solide's resistive XPT2046), the SDMMC card slot (not SPI), and the fact
that there is NO physical WS2812B ring - the session ring is composed on-panel.

Board identity rides the STATUS ``board=`` field (``freenove_s3`` vs
``solide_s3``), emitted at the very END of the line by src/test_console.cpp. That
is the ONLY thing that distinguishes a Freenove from a Solide-in-TFT-mode: both
report ``scr=tft``, so ``scr=`` alone cannot tell them apart.

Board-agnostic behaviour (Wi-Fi, heap/stack health, the generic TFT/touch nav
loop) is deliberately NOT re-tested here - test_l4 (network), test_l23 (stack
health) and test_l21 (touch display) already own it. Every test in this file is a
Freenove-only assertion.

Gated behind --allow-hardware. On a device-less box every test is a LOUD,
REASONED skip - never a green-by-default. Pointed at a Solide board, every test
SKIPS loudly via ``_require_board`` (mirroring how test_l21's ``_require_tft``
skips off a non-TFT board), so running the suite against either unit exercises
the right half.
"""

from __future__ import annotations

import re

import pytest

# ⚠ Device.expect() is a SUBSTRING match, not a regex (tests/hil/device.py:422).
# Use expect_re()/cmd_re() when a pattern is genuinely needed.

# Panel geometry, mirrored from test_l21 (nimbus/tft_render/theme.h). The CYD
# panel is the same 2.8" 240x320 module mounted landscape, so the logical surface
# is 320x240 and the tap coordinates below derive from these two.
PANEL_W, PANEL_H = 320, 240

BOARD = "freenove_s3"


def _status(device) -> str:
    # cmd() drains first: the device prints unsolicited RENDER/led lines, and a
    # bare expect() can match one of those instead of this query's reply.
    return device.cmd("STATUS", "STATUS ", timeout=5.0)


def _scr_model(device) -> str:
    """The display driver that ACTUALLY bound, from STATUS scr=.

    Both boards drive the ILI9341, so this reads ``tft`` on a healthy Freenove
    AND on a healthy Solide-in-TFT-mode - it is NOT how the two are told apart
    (that is ``board=``). It still matters here as the fail-soft signal: a
    Freenove that silently fell back to e-ink would report ``scr=eink``.
    """
    line = _status(device)
    m = re.search(r"\bscr=(\w+)", line)
    assert m, f"STATUS has no scr= field - is this firmware new enough? {line!r}"
    return m.group(1)


def _board(device) -> str:
    """The compile-time board slug, from STATUS board=.

    Appended at the very END of the STATUS line (src/test_console.cpp), so the
    match is deliberately tolerant of every field that sits between heap= and it.
    A firmware old enough to lack the field fails LOUDLY here rather than being
    mistaken for the wrong board.
    """
    line = _status(device)
    m = re.search(r"\bboard=(\w+)", line)
    assert m, (
        f"STATUS carries no board= field - this firmware predates the board slug, "
        f"so a Freenove and a Solide cannot be told apart: {line!r}"
    )
    return m.group(1)


def _require_board(device, want: str = BOARD) -> str:
    """Skip LOUDLY (never silently) unless the attached board's slug matches.

    Mirrors test_l21's ``_require_tft``. Every hardware test in this file calls it
    first so that pointing the suite at a Solide board is a reasoned skip, not a
    red failure blaming Freenove-only hardware the board does not have.
    """
    got = _board(device)
    if got != want:
        pytest.skip(
            f"this board reports board={got}, not {want} - point NIMBUS_PORT at the "
            "Freenove CYD unit (the all-in-one board flashed with SOLIDE_BOARD="
            f"{want}); the Solide board has no capacitive touch / SDMMC / on-panel "
            "ring to exercise here"
        )
    return got


# ---- identity ---------------------------------------------------------------


@pytest.mark.hil
def test_reports_freenove_board_and_tft_panel(device):
    """The CYD board comes up as a TFT panel AND identifies itself as Freenove.

    scr= alone is not enough: a Solide in TFT mode reports scr=tft too. The
    board= slug is what proves this is the Freenove pinout - both must hold.
    """
    _require_board(device, BOARD)
    model = _scr_model(device)
    assert model == "tft", (
        f"a Freenove board must bind the tft driver, got scr={model!r} - the panel "
        "failed to initialise and the device fell back (check the boot log for "
        "'[tft] panel bring-up FAILED')"
    )


# ---- capacitive touch (FT6336U, not the Solide's resistive XPT2046) ---------


@pytest.mark.hil
def test_capacitive_touch_present(device):
    """The FT6336U capacitive controller reports present.

    The panel and the touch controller are separate parts, so a wiring or I2C
    fault can kill exactly the touch half while the panel still paints -
    asserting only 'it renders' would miss a dead touch controller entirely.
    """
    _require_board(device, BOARD)
    line = device.cmd("TOUCH?", "TOUCH ", timeout=5.0)
    m = re.search(r"present=(\d)", line)
    assert m, f"malformed TOUCH? reply: {line!r}"
    assert m.group(1) == "1", (
        f"capacitive touch controller absent on a Freenove board: {line!r} - check "
        "the FT6336U I2C wiring (SDA/SCL) and its shared-bus address"
    )


@pytest.mark.hil
def test_tap_inject_reaches_the_touch_layer(device):
    """The TAP/TAPUP inject seam drives the capacitive layer, knobless.

    Same seam test_l21 leans on for the Solide, re-proven here because the
    Freenove routes it through a DIFFERENT controller (capacitive FT6336U). A
    plain tap auto-releases; TAPUP is still sent in a finally to guarantee the
    panel is never left latched down for the tests that follow.
    """
    _require_board(device, BOARD)
    try:
        line = device.cmd(f"TAP {PANEL_W // 2} {PANEL_H // 2}", "TAP<", timeout=5.0)
        assert "TAP<" in line, f"the tap inject seam did not ack: {line!r}"
    finally:
        # ⚠ ALWAYS release - a synthetic press left un-lifted latches the panel and
        # real touch stops working until a reboot (see test_l21's hold test).
        device.cmd("TAPUP", "TAPUP<", timeout=5.0)


@pytest.mark.hil
def test_out_of_range_tap_is_refused(device):
    """The inject seam validates its own coordinates rather than driving the
    hit-test with nonsense (which would mask a real coordinate bug as 'works')."""
    _require_board(device, BOARD)
    line = device.cmd_re(f"TAP {PANEL_W + 50} 10", r"(ERR|TAP<)", timeout=5.0).string
    assert "ERR" in line, f"an off-panel tap was accepted: {line!r}"


# ---- SDMMC card slot (SDMMC on this board, not the Solide's SPI) ------------


@pytest.mark.hil
def test_sd_field_is_well_formed(device):
    """The SD slot reports a KNOWN state whether or not a card is inserted.

    This is a plumbing check, not a 'card present' check: an empty slot is a
    perfectly valid bench state, so asserting ``sd=present`` would fail a healthy
    board with no card in it. What must hold is that the field EXISTS and reads
    one of the two known values - proving the SDMMC probe path ran and reported,
    rather than being absent or garbage.
    """
    _require_board(device, BOARD)
    line = _status(device)
    m = re.search(r"\bsd=(\w+)", line)
    assert m, f"STATUS carries no sd= field - the SDMMC probe never reported: {line!r}"
    assert m.group(1) in ("present", "absent"), (
        f"sd= read {m.group(1)!r}, not present|absent - the SDMMC probe returned an unknown state: {line!r}"
    )


@pytest.mark.hil
def test_sdcheck_probe_responds(device):
    """SDCHECK forces one liveness probe and reports it, card or no card.

    The console command must answer with a well-formed ``SDCHECK probe=.. sdlost=..``
    line - that is the seam the degrade/promote state machine is driven through.
    Its verdict legitimately depends on whether a card is seated, so this asserts
    the SHAPE of the reply, not a particular probe value.
    """
    _require_board(device, BOARD)
    m = device.cmd_re("SDCHECK", r"SDCHECK\s+probe=(\d)\s+sdlost=(\d)", timeout=8.0)
    assert m.group(1) in ("0", "1") and m.group(2) in ("0", "1"), f"SDCHECK reply is malformed: {m.string!r}"


# ---- on-panel session ring (no physical WS2812B on this board) --------------


@pytest.mark.hil
def test_ring_is_composed_on_panel(device):
    """The session ring still composes, drawn on the panel not a WS2812B strip.

    The Freenove has no physical LED ring: the ring the Notifier mode paints is
    part of the TFT frame. RENDER? reports that composition (seg=/single=/dark=/
    bright=), so a parseable RENDER? on a Freenove board is the proof that the
    on-panel ring path is alive. Kept deliberately light - this asserts the ring
    composition is REPORTED, not any particular pattern.
    """
    _require_board(device, BOARD)
    line = device.cmd("RENDER?", "RENDER ", timeout=8.0)
    assert re.search(r"screen=(\w+)", line), f"RENDER? gave nothing useful: {line!r}"
    # The ring-intent fields must be present: they are what the panel draws in
    # place of a physical strip, so their absence would mean the ring layer never
    # reached the composed frame.
    for field in ("seg=", "single=", "dark=", "bright="):
        assert field in line, (
            f"RENDER? is missing {field!r} - the on-panel ring composition did not reach the frame: {line!r}"
        )


# ---- capacitive touch registers a real finger (human-assisted) --------------


@pytest.mark.hil
@pytest.mark.manual
def test_capacitive_touch_registers_a_finger(device, require_manual):
    """A REAL finger lands on the panel where it is placed.

    TOUCH?/TAP prove the controller answers and the inject seam is wired, but
    only a human can prove the capacitive glass actually registers a fingertip
    and maps it sensibly - the exact thing bench step 1/3 of MANUAL_freenove_cyd
    covers and firmware cannot self-verify. Baseline a quiet TOUCH?, have the
    operator press and HOLD, then assert the controller reports ``down=1``.
    """
    _require_board(device, BOARD)

    base = device.cmd("TOUCH?", "TOUCH ", timeout=5.0)
    bm = re.search(r"down=(\d)", base)
    assert bm, f"malformed TOUCH? reply: {base!r}"

    require_manual.confirm(
        "PRESS and HOLD a finger flat on the middle of the screen, then press y while still holding it down.",
        timeout=30.0,
    )

    held = device.cmd("TOUCH?", "TOUCH ", timeout=5.0)
    hm = re.search(r"down=(\d)", held)
    assert hm, f"malformed TOUCH? reply while held: {held!r}"
    assert hm.group(1) == "1", (
        f"the capacitive panel did not register a held finger: {held!r} - the "
        "FT6336U is not reporting touches even though the operator confirmed one "
        "(check the controller wiring and the touch threshold)"
    )


# ---- the Solide control -----------------------------------------------------


@pytest.mark.hil
def test_suite_excludes_the_solide_board(device):
    """NON-REGRESSION: this Freenove suite must NEVER assert against a Solide board.

    Deliberately inverted, mirroring test_l21's e-ink control: pointed at a
    non-Freenove board it SKIPS loudly (proving the ``_require_board`` guard keeps
    the whole file off the wrong hardware); on the Freenove it confirms the guard
    admits the board and the slug is stable.
    """
    got = _board(device)
    if got != BOARD:
        pytest.skip(
            f"attached board reports board={got}, not {BOARD} - the Freenove-specific "
            "checks correctly do not run here; point the suite at the CYD unit to "
            "exercise them"
        )
    # On the CYD board the guard admits us and the identity is exactly the slug.
    assert _require_board(device, BOARD) == BOARD
