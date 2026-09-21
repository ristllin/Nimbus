"""L35 - button-press feedback on real hardware (CUM-309).

Lane tf-btn added a host-tested outcome -> cue map (sound + ring swell + screen line)
for the actionable device-menu controls, but the actual tone on the speaker and the
green/red ring on a real unit were never exercised. This drives that seam on the
board.

The automatable class (asserted here over serial)
-------------------------------------------------
The ``ACTFB <action> <outcome>`` seam fires the REAL device feedback path
(emitMenuActionFeedback) for a named MenuAction + Outcome - the "serial TAP injection
on the test build" the issue itself calls out, since the owner's nimbus-light has dead
touch. ``FEEDBACK?`` then reports the cue that fired (tone id + ring swell + toast).

  * a SUCCESS row (Reset -> Ok): confirm tone = agent_done, ring = success (green),
    toast = "Settings reset to defaults.",
  * a FAILURE row (Rescan SD -> Failed): flag tone = error, ring = failure (red),
    toast = "No SD card found. Reseat it and rescan.",
  * the ring swell ENDS - no lit arc outlives its confirm window (the frozen CUM-134/
    CUM-11 rule "no lit arc outlives its job"): the ring returns to dark after each
    cue.

What stays a human leg
----------------------
Whether the tone is actually AUDIBLE and the ring is actually GREEN/RED to the eye
needs ears and eyes on the device - the manual test at the bottom. The seam proves
the right cue was SELECTED and cleaned up; the owner confirms it was heard and seen.

Gated ``@pytest.mark.hil``. The pure parser is host-tested in
test_l35_button_feedback_host.py.
"""

from __future__ import annotations

import re
import time

import pytest

# MenuAction enum indices (lib/core/include/nimbus/action_feedback.h, enum order) and
# Outcome indices (Ok=0, Acknowledged=1, Failed=2). Positionally mirrored like the
# other HIL enums - do not renumber without updating the firmware enum.
ACTION_RESCAN_SD = 1
ACTION_RESET = 6
OUTCOME_OK = 0
OUTCOME_FAILED = 2

# emitMenuActionFeedback holds the ring swell for kActionConfirmMs (1200 ms); allow a
# generous margin for render scheduling before asserting the ring is back to dark.
RING_CLEAR_WINDOW_S = 4.0


def parse_feedback(raw: str) -> dict:
    """Parse ``FEEDBACK seen=.. action=.. outcome=.. sfx=.. ring=.. toast=<rest>``.
    ``toast`` runs to end of line (it contains spaces). Raises ValueError if the line
    is missing or unparseable."""
    idx = raw.find("FEEDBACK ")
    if idx < 0:
        raise ValueError(f"no FEEDBACK line in: {raw!r}")
    line = raw[idx:].splitlines()[0]
    m = re.search(
        r"FEEDBACK\s+seen=(?P<seen>\d)\s+action=(?P<action>-?\d+)\s+"
        r"outcome=(?P<outcome>-?\d+)\s+sfx=(?P<sfx>\S+)\s+ring=(?P<ring>\S+)\s+"
        r"toast=(?P<toast>.*)$",
        line,
    )
    if not m:
        raise ValueError(f"unparseable FEEDBACK line: {line!r}")
    return {
        "seen": m.group("seen") == "1",
        "action": int(m.group("action")),
        "outcome": int(m.group("outcome")),
        "sfx": m.group("sfx"),
        "ring": m.group("ring"),
        "toast": m.group("toast"),
    }


def _fire(device, action: int, outcome: int) -> dict:
    device.cmd(f"ACTFB {action} {outcome}", "ACTFB fired", timeout=6.0)
    return parse_feedback(device.cmd("FEEDBACK?", "FEEDBACK ", timeout=6.0))


def _ring_ends_dark(device, window_s: float = RING_CLEAR_WINDOW_S) -> "tuple[bool, bool]":
    """Poll RENDER? until the ring is dark, up to ``window_s``. Returns
    (ended_dark, ever_lit): ended_dark is the CUM-134/CUM-11 invariant (no lit arc
    outlives its window); ever_lit records whether a swell was physically observed
    (it is suppressed on a Dark-posture / pack-less board, where dark is expected
    throughout and the colour is the owner's eyes leg)."""
    ever_lit = False
    ended_dark = False
    deadline = time.time() + window_s
    while time.time() < deadline:
        r = device.render(timeout=4.0)
        if not r.dark:
            ever_lit = True
        if r.dark:
            ended_dark = True
            break
        time.sleep(0.25)
    else:
        # One last read after the window in case the last sample caught the swell.
        ended_dark = device.render(timeout=4.0).dark
    return ended_dark, ever_lit


@pytest.mark.hil
def test_success_row_confirms(device, wake_home):
    """Reset -> Ok fires the confirm cue (agent_done + green swell + the reset line),
    and the swell ends (ring returns to dark)."""
    wake_home()  # menu closed, ring at its idle baseline
    fb = _fire(device, ACTION_RESET, OUTCOME_OK)
    assert fb["seen"] and fb["action"] == ACTION_RESET and fb["outcome"] == OUTCOME_OK
    assert fb["sfx"] == "agent_done", f"success tone should be agent_done, got {fb['sfx']!r}"
    assert fb["ring"] == "success", f"success ring should be a green swell, got {fb['ring']!r}"
    assert fb["toast"] == "Settings reset to defaults.", f"unexpected toast {fb['toast']!r}"
    ended_dark, ever_lit = _ring_ends_dark(device)
    assert ended_dark, "ring did not return to dark after the confirm swell (a lit arc outlived its window)"
    print(f"[feedback] Reset->Ok: cue ok; ring swell {'observed then cleared' if ever_lit else 'suppressed (posture)'}")


@pytest.mark.hil
def test_failure_row_flags(device, wake_home):
    """Rescan SD -> Failed fires the flag cue (error + red swell + the no-card line),
    and the swell ends."""
    wake_home()
    fb = _fire(device, ACTION_RESCAN_SD, OUTCOME_FAILED)
    assert fb["seen"] and fb["action"] == ACTION_RESCAN_SD and fb["outcome"] == OUTCOME_FAILED
    assert fb["sfx"] == "error", f"failure tone should be error, got {fb['sfx']!r}"
    assert fb["ring"] == "failure", f"failure ring should be a red swell, got {fb['ring']!r}"
    assert fb["toast"] == "No SD card found. Reseat it and rescan.", f"unexpected toast {fb['toast']!r}"
    ended_dark, ever_lit = _ring_ends_dark(device)
    assert ended_dark, "ring did not return to dark after the flag swell (a lit arc outlived its window)"
    print(
        f"[feedback] RescanSd->Failed: cue ok; ring swell {'observed then cleared' if ever_lit else 'suppressed (posture)'}"
    )


@pytest.mark.hil
@pytest.mark.manual
def test_feedback_is_audible_and_visible(device, require_manual, wake_home):
    """Owner leg: the tone must be HEARD and the ring colour SEEN. The seam proves the
    right cue was chosen and cleared; only a human can confirm it reached the speaker
    and the eye. Uses the real menu row where the operator can navigate to it."""
    wake_home()
    require_manual.confirm(
        "BUTTON FEEDBACK (owner leg):\n"
        "  1. Open Settings and press 'Reset to defaults'.\n"
        "     Confirm: a rising confirm tone AND a brief GREEN ring swell that then goes dark.\n"
        "  2. With NO SD card inserted, press 'Rescan SD card'.\n"
        "     Confirm: a distinct error tone, a brief RED ring swell that then goes dark,\n"
        "     and the screen line 'No SD card found. Reseat it and rescan.'\n"
        "Did both the sound and the ring colour match, and did the ring go dark after each?"
    )
