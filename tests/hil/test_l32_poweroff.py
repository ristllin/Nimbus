"""L32 - software power-off (deep sleep, wake on touch), CUM-224.

Two things this proves, and one it hands to a human.

The device's ``POWEROFF?`` seam reports the wake decision WITHOUT sleeping
(``POWEROFF tapWakes=<0|1> pin=<gpio>``): whether a screen tap can wake this board
from deep sleep, and on which GPIO. That decision is the whole per-variant story,
so it is worth an automated oracle:

- **Coherence (any board):** ``tapWakes`` is 1 exactly when the reported pin is an
  RTC-capable GPIO (0-21 on the ESP32-S3) - the same rule ``boardCanWakeOnTouch()``
  applies. A board can never claim a tap wakes it while pointing at an unwired or
  non-RTC pin.
- **Per variant (against ``board=`` in STATUS):** the Freenove CYD wires the FT6336U
  INT to GPIO17, so it wakes on a tap; the Solide S3 leaves the XPT2046 T_IRQ
  unconnected (pin -1), so a tap cannot wake it and it returns on a power-cycle.

Actually entering deep sleep (``POWEROFF``) makes the panel go dark and kills the
USB console, and coming back needs a physical tap or a power-cycle - so the full
"sleep entered -> wake -> state intact" loop is a MANUAL step, below.
"""

from __future__ import annotations

import re

import pytest

# Compile-time board slug (STATUS board=) -> expected wake-on-touch capability.
# Freenove CYD: FT6336U INT on GPIO17 (RTC-capable) -> a tap wakes it.
# Solide S3: XPT2046 T_IRQ not routed (pin -1) -> only a power-cycle wakes it.
_WAKES_ON_TOUCH = {"freenove_s3": True, "solide_s3": False}


def _status_board(device) -> str:
    m = re.search(r"board=(\S+)", device.cmd("STATUS", "STATUS ", timeout=8.0))
    return m.group(1) if m else "unknown"


def _poweroff_info(device) -> tuple[int, int]:
    """Parse ``POWEROFF tapWakes=<0|1> pin=<gpio>`` -> (tapWakes, pin)."""
    raw = device.cmd("POWEROFF?", "POWEROFF ", timeout=8.0)
    line = raw[raw.index("POWEROFF ") :].splitlines()[0]
    tap = int(re.search(r"tapWakes=(\d)", line).group(1))
    pin = int(re.search(r"pin=(-?\d+)", line).group(1))
    return tap, pin


@pytest.mark.hil
def test_poweroff_wake_decision_is_coherent(device):
    """tapWakes is true iff the touch INT pin is an RTC-capable GPIO (0-21)."""
    tap, pin = _poweroff_info(device)
    rtc_capable = 0 <= pin <= 21
    assert bool(tap) == rtc_capable, (
        f"POWEROFF? tapWakes={tap} but pin={pin} "
        f"({'is' if rtc_capable else 'is not'} an RTC GPIO) - the wake decision is incoherent"
    )


@pytest.mark.hil
def test_poweroff_matches_the_board_variant(device):
    """The wake capability matches how THIS board wires its touch interrupt."""
    board = _status_board(device)
    if board not in _WAKES_ON_TOUCH:
        pytest.skip(f"unknown board slug {board!r} - extend _WAKES_ON_TOUCH")
    tap, pin = _poweroff_info(device)
    expected = _WAKES_ON_TOUCH[board]
    assert bool(tap) == expected, f"board={board} expected tapWakes={int(expected)} but got {tap} (pin={pin})"
    if board == "freenove_s3":
        assert pin == 17, f"Freenove FT6336U INT should be GPIO17, got {pin}"
    if board == "solide_s3":
        assert pin == -1, f"Solide XPT2046 T_IRQ is unwired, expected pin -1, got {pin}"


@pytest.mark.hil
@pytest.mark.manual
def test_poweroff_enters_sleep_and_wakes_state_intact(device, manual):
    """Human-in-the-loop: power off, confirm the panel notice, wake, verify state.

    Steps (the harness prints these and waits for confirmation):
      1. Note the current mode/profile via STATUS (recorded below as `before`).
      2. Send POWEROFF. The panel shows "Powered off." with the wake instruction
         this board can actually honor (tap the screen on Freenove, reconnect power
         on Solide); the ring and backlight go dark; the USB console goes silent.
      3. Wake it: tap the screen (Freenove) or power-cycle (Solide).
      4. After it boots, STATUS reports the SAME mode/profile as `before` - the
         power-off saved config and did not reset anything.
    """
    before = device.cmd("STATUS", "STATUS ", timeout=8.0)
    b_mode = re.search(r"mode=(\d)", before).group(1)
    b_scr = re.search(r"scr=(\w+)", before).group(1)
    tap, _pin = _poweroff_info(device)
    wake = "tap the screen" if tap else "reconnect power"
    manual.confirm(
        "POWER-OFF + WAKE:\n"
        "  1. The board is about to deep-sleep on POWEROFF.\n"
        f"  2. Verify the panel reads 'Powered off.' and tells you to {wake}.\n"
        f"  3. Wake it ({wake}); let it boot.\n"
        "  4. Confirm it came back (screen shows the normal UI).\n"
        "Was the notice correct and did it wake cleanly?"
    )
    # NOTE: POWEROFF itself is not sent automatically - the operator triggers it at
    # the console so they control the wake. After wake, re-read STATUS out-of-band
    # and confirm the persisted state is unchanged.
    after = device.cmd("STATUS", "STATUS ", timeout=12.0)
    assert re.search(r"mode=(\d)", after).group(1) == b_mode, "mode changed across power-off"
    assert re.search(r"scr=(\w+)", after).group(1) == b_scr, "screen model changed across power-off"
