#!/usr/bin/env python3
"""Touch calibration wizard - press four corners, get a calibrated panel.

Until a panel is calibrated, taps land somewhere other than where you aimed,
and that is indistinguishable from broken touch. The manual path (read raw
corners with ``TOUCH?``, work out the swap/flip flags, compose a ``TCAL``
string) is fiddly and easy to get subtly wrong, so this does it for you:

    python3 tools/tcal_wizard.py --port /dev/cu.usbserial-XXXX

You press each corner when prompted; it samples the raw ADC, derives the axis
mapping and orientation flags, applies the result and verifies it. Needs an
``[env:test]``-family build (it drives the ``TOUCH?``/``TCAL`` console).

⚠ Opening the port RESETS the board on both transports (native USB-serial-JTAG,
and a UART bridge whose DTR/RTS drive EN). That costs one reboot, so this holds
ONE session open for the whole run rather than reopening per command.

Orientation is DERIVED, never assumed. The driver applies the transform in this
order (solide-drivers src/device/touch.cpp): swap X/Y first, then map the raw
range onto the pixel range, then invert. So `minX/maxX` describe whichever RAW
axis ends up carrying screen X - which is why the swap has to be worked out
before the ranges mean anything.
"""

from __future__ import annotations

import argparse
import glob
import re
import statistics
import sys
import time

# Imported lazily-tolerantly, NOT with sys.exit(): derive() below is pure
# arithmetic and is unit-tested on machines that have no pyserial and no board.
# Exiting at import time would make the load-bearing logic untestable.
try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover
    serial = None

PANEL_W, PANEL_H = 240, 320

# Screen-space corners, in the order the owner is asked to press them.
CORNERS = [
    ("top-left", 0.0, 0.0),
    ("top-right", 1.0, 0.0),
    ("bottom-left", 0.0, 1.0),
    ("bottom-right", 1.0, 1.0),
]

PORT_GLOBS = ("/dev/cu.usbmodem*", "/dev/cu.usbserial-*", "/dev/cu.SLAB_USBtoUART*", "/dev/cu.wchusbserial*")


class Console:
    """One long-lived quiet-open serial session."""

    def __init__(self, port: str, baud: int = 115200, settle: float = 3.0):
        if serial is None:
            raise RuntimeError("pyserial is required to talk to the board: pip install pyserial")
        s = serial.Serial()
        s.port = port
        s.baudrate = baud
        # ⚠ NEVER let pySerial assert DTR/RTS: on this board those lines are
        # wired to EN/GPIO0, and strobing them is how boards get bricked.
        s.dtr = False
        s.rts = False
        s.timeout = 0.25
        s.open()
        self.s = s
        time.sleep(settle)  # the open reset the board; let it come up
        s.reset_input_buffer()

    def cmd(self, line: str, want: str, timeout: float = 3.0) -> str:
        """Send `line`, return the first reply line containing `want`.

        Drains first: the device emits unsolicited RENDER/led lines, so a bare
        read can return one of those instead of this command's answer.
        """
        self.s.reset_input_buffer()
        self.s.write((line + "\n").encode())
        self.s.flush()
        end = time.time() + timeout
        while time.time() < end:
            raw = self.s.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", "replace").strip()
            if want in text:
                return text
        raise TimeoutError(f"no reply containing {want!r} to {line!r}")

    def close(self):
        self.s.close()


def read_touch(con: Console):
    """One TOUCH? sample -> (present, rawx, rawy, z, down)."""
    line = con.cmd("TOUCH?", "TOUCH ")
    m = re.search(r"present=(\d)\s+raw=(\d+),(\d+)\s+z=(\d+)\s+down=(\d)", line)
    if not m:
        raise RuntimeError(f"unparsable TOUCH? reply: {line!r}")
    return (m.group(1) == "1", int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5) == "1")


def sample_corner(con: Console, name: str, samples: int = 7, timeout: float = 45.0):
    """Wait for a press, take the MEDIAN of several reads, wait for release.

    Median, not mean: a resistive panel emits the occasional wild outlier as
    the finger lands and lifts, and one such reading would drag a corner (and
    therefore the whole axis range) badly off.
    """
    print(f"\n  Press and HOLD the {name} corner of the screen...", flush=True)
    end = time.time() + timeout
    while True:
        if time.time() > end:
            raise TimeoutError(
                f"no {name} press within {timeout:.0f}s. Is the panel wired and "
                "is this the right board? Check `TOUCH?` reports present=1."
            )
        present, _, _, _, down = read_touch(con)
        if not present:
            raise RuntimeError(
                "the touch controller is not responding (present=0) - check T_CS "
                "(GPIO 48) and the T_CLK/T_DIN/T_DO bridges before calibrating"
            )
        if down:
            break
        time.sleep(0.05)

    xs, ys = [], []
    while len(xs) < samples:
        present, rx, ry, _z, down = read_touch(con)
        if not down:  # lifted early - take what we have
            break
        xs.append(rx)
        ys.append(ry)
        time.sleep(0.03)
    if len(xs) < 3:
        raise RuntimeError(f"{name}: press was too brief - hold it for ~1s and retry")

    mx, my = int(statistics.median(xs)), int(statistics.median(ys))
    print(f"    got raw = {mx},{my}   (release now)", flush=True)

    end = time.time() + 20.0
    while time.time() < end:
        if not read_touch(con)[4]:
            time.sleep(0.15)
            return mx, my
        time.sleep(0.05)
    raise TimeoutError(f"{name}: finger never released")


def derive(points):
    """points: {corner_name: (rawx, rawy)} -> (minX, maxX, minY, maxY, flags).

    Which raw axis carries screen X is decided by which one MOVES when only
    screen X changes - measured, not guessed, because a panel can be mounted
    in any of eight orientations.
    """
    tl, tr, bl, br = (points[n] for n, _, _ in CORNERS)

    # Average the two edges so a single sloppy corner cannot flip the decision.
    dx_rawx = ((tr[0] + br[0]) - (tl[0] + bl[0])) / 2.0  # rawX change per +screenX
    dx_rawy = ((tr[1] + br[1]) - (tl[1] + bl[1])) / 2.0  # rawY change per +screenX
    dy_rawx = ((bl[0] + br[0]) - (tl[0] + tr[0])) / 2.0
    dy_rawy = ((bl[1] + br[1]) - (tl[1] + tr[1])) / 2.0

    swap = abs(dx_rawy) > abs(dx_rawx)
    if swap:
        # screen X rides rawY, screen Y rides rawX
        x_vals = [p[1] for p in (tl, tr, bl, br)]
        y_vals = [p[0] for p in (tl, tr, bl, br)]
        x_delta, y_delta = dx_rawy, dy_rawx
    else:
        x_vals = [p[0] for p in (tl, tr, bl, br)]
        y_vals = [p[1] for p in (tl, tr, bl, br)]
        x_delta, y_delta = dx_rawx, dy_rawy

    min_x, max_x = min(x_vals), max(x_vals)
    min_y, max_y = min(y_vals), max(y_vals)

    # The driver maps lo->0 and hi->outMax, so if the raw value DECREASES as the
    # screen coordinate grows, the axis needs inverting.
    flags = (1 if swap else 0) | (2 if x_delta < 0 else 0) | (4 if y_delta < 0 else 0)

    span_x, span_y = max_x - min_x, max_y - min_y
    if span_x < 200 or span_y < 200:
        raise RuntimeError(
            f"the corner readings barely differ (spans {span_x}/{span_y}) - the "
            "presses were probably all in the same place, or a touch line is "
            "shorted. Re-run and press the actual corners."
        )
    return min_x, max_x, min_y, max_y, flags


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--dry-run", action="store_true", help="work out the calibration and print it, but do not apply it")
    args = ap.parse_args()

    port = args.port
    if not port:
        found = [p for g in PORT_GLOBS for p in glob.glob(g)]
        if len(found) != 1:
            return int(
                bool(
                    sys.stderr.write(
                        "could not pick a port automatically (found %d: %s).\n"
                        "Pass --port explicitly.\n" % (len(found), ", ".join(found) or "none")
                    )
                )
                or 2
            )
        port = found[0]

    print(f"Touch calibration - {port}")
    print("Opening the port restarts the board; waiting for it to come up...")
    con = Console(port)
    try:
        scr = con.cmd("SCREEN?", "SCREEN ")
        print(f"  {scr}")
        if "bound=tft" not in scr:
            print("\nThis board is not running the colour panel, so there is nothing to calibrate.", file=sys.stderr)
            return 2
        print(f"  before: {con.cmd('TCAL', 'TCAL')}")

        points = {}
        for name, _, _ in CORNERS:
            points[name] = sample_corner(con, name)

        min_x, max_x, min_y, max_y, flags = derive(points)
        spec = f"{min_x},{max_x},{min_y},{max_y},{flags}"
        notes = []
        if flags & 1:
            notes.append("X/Y swapped")
        if flags & 2:
            notes.append("X flipped")
        if flags & 4:
            notes.append("Y flipped")
        print(f"\n  derived: TCAL {spec}" + (f"   ({', '.join(notes)})" if notes else ""))

        if args.dry_run:
            print("\n  --dry-run: not applied. To apply it yourself:")
            print(f"      TCAL {spec}")
            return 0

        print(f"  applied: {con.cmd('TCAL ' + spec, 'TCAL', timeout=6.0)}")
        print(f"  stored : {con.cmd('TCAL', 'TCAL')}")

        print("\nVerify: tap a few places and check the pixel matches where you pressed.\nCtrl-C when satisfied.")
        try:
            last = None
            while True:
                present, rx, ry, z, down = read_touch(con)
                if down:
                    line = con.cmd("TOUCH?", "TOUCH ")
                    px = re.search(r"px=(-?\d+),(-?\d+)", line)
                    if px and px.group(0) != last:
                        last = px.group(0)
                        x, y = int(px.group(1)), int(px.group(2))
                        where = "top" if y < PANEL_H / 3 else "middle" if y < 2 * PANEL_H / 3 else "bottom"
                        side = "left" if x < PANEL_W / 3 else "centre" if x < 2 * PANEL_W / 3 else "right"
                        print(f"    tap -> pixel {x},{y}   ({where} {side})")
                time.sleep(0.05)
        except KeyboardInterrupt:
            print("\nDone. The calibration is in NVS and survives a restart.")
        return 0
    finally:
        con.close()


if __name__ == "__main__":
    raise SystemExit(main())
