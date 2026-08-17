#!/usr/bin/env python3
"""Self-test for tcal_wizard.derive() - the load-bearing half of calibration.

Runnable directly (`python3 tools/test_tcal_wizard.py`) or under pytest; needs
no board and no pyserial.

Why this exists: the wizard's serial half can only be exercised by a finger on a
panel, but the part that can be WRONG IN A WAY NOBODY NOTICES is the arithmetic -
a mis-derived swap or flip produces a calibration that looks plausible, applies
cleanly, and then puts every tap in the wrong place. A panel can be mounted in
any of eight orientations, so all eight are checked by round-tripping: synthesise
the raw corner readings a known orientation WOULD produce, then assert derive()
recovers exactly that orientation.
"""

from __future__ import annotations

import importlib.util
import pathlib


def _load():
    p = pathlib.Path(__file__).with_name("tcal_wizard.py")
    spec = importlib.util.spec_from_file_location("tcal_wizard", p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


W = _load()


def synth(swap: bool, flip_x: bool, flip_y: bool, lo: int = 200, hi: int = 3900):
    """Raw corner readings a panel with this orientation would report.

    The inverse of the driver's transform (solide-drivers src/device/touch.cpp):
    it swaps X/Y first, then maps the raw range onto pixels, then inverts. So a
    flipped axis is one whose raw value DECREASES as the screen coordinate grows.
    """
    pts = {}
    for name, sx, sy in W.CORNERS:
        ax = (1 - sx) if flip_x else sx  # fraction along the axis carrying X
        ay = (1 - sy) if flip_y else sy
        vx = int(lo + ax * (hi - lo))
        vy = int(lo + ay * (hi - lo))
        pts[name] = (vy, vx) if swap else (vx, vy)
    return pts


def test_recovers_every_orientation():
    for swap in (False, True):
        for flip_x in (False, True):
            for flip_y in (False, True):
                want = (1 if swap else 0) | (2 if flip_x else 0) | (4 if flip_y else 0)
                min_x, max_x, min_y, max_y, flags = W.derive(synth(swap, flip_x, flip_y))
                assert flags == want, f"swap={swap} flipX={flip_x} flipY={flip_y}: got flags {flags}, want {want}"
                assert (min_x, max_x, min_y, max_y) == (200, 3900, 200, 3900), (
                    f"swap={swap} flipX={flip_x} flipY={flip_y}: ranges came out "
                    f"{min_x}-{max_x}/{min_y}-{max_y}, want 200-3900 on both axes"
                )


def test_refuses_corners_that_barely_differ():
    """Four presses in the same spot must FAIL, not yield a garbage calibration.

    This is the realistic operator error (pressing near the centre four times,
    or a shorted touch line), and the resulting near-zero span would map the
    whole panel onto a couple of pixels.
    """
    try:
        W.derive({n: (2000, 2001) for n, _, _ in W.CORNERS})
    except RuntimeError:
        return
    raise AssertionError("derive() accepted four identical corners")


def test_tolerates_a_sloppy_corner():
    """One corner off by a realistic amount must not flip the axis decision.

    Corner presses are made by a fingertip on a resistive panel, so they are
    never exact; the edges are averaged precisely so a single sloppy press
    cannot invert the mapping.
    """
    pts = synth(False, False, False)
    tl = pts["top-left"]
    pts["top-left"] = (tl[0] + 300, tl[1] + 300)  # pressed well inside the corner
    _, _, _, _, flags = W.derive(pts)
    assert flags == 0, f"a sloppy corner changed the derived orientation (flags={flags})"


if __name__ == "__main__":
    test_recovers_every_orientation()
    test_refuses_corners_that_barely_differ()
    test_tolerates_a_sloppy_corner()
    print("tcal_wizard.derive: all 8 orientations recovered; bad input refused; sloppy corner tolerated")
