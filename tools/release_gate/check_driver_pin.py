#!/usr/bin/env python3
"""Release gate: refuse a firmware release pinned to a known-white-screen driver.

CUM-167: the solide_s3 white screen shipped because platformio.ini pinned
solide-drivers v0.6.0, whose display compile-gate broke the color-TFT init path
(the panel logged 'up' while pixels never reached the glass). The fix was pinning
v0.6.1. This host check is the fast backstop for the on-hardware render-to-glass
gate: it FAILS while the firmware still points at a denylisted driver version, so
a release candidate on the bad pin cannot pass the gate.

Usage:
    python3 tools/release_gate/check_driver_pin.py [--ini platformio.ini]
Exit 0 = pin is clean; exit 1 = pin is denylisted or unreadable.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# Driver versions known to ship a release-blocking display defect.
WHITE_SCREEN_DENYLIST = {"v0.6.0"}
# The earliest version with the fix, for the human-readable hint.
FIXED_AT = "v0.6.1"

PIN_RE = re.compile(r"solide-drivers\.git#(?P<ref>[^\s]+)")


def find_pin(ini_text: str) -> "str | None":
    """Return the solide-drivers git ref pinned in a platformio.ini, ignoring
    commented-out lines (a leading ';' is a PlatformIO/ini comment)."""
    for raw in ini_text.splitlines():
        line = raw.strip()
        if line.startswith(";"):
            continue
        m = PIN_RE.search(line)
        if m:
            return m.group("ref")
    return None


def judge(ref: "str | None") -> "tuple[bool, str]":
    """(ok, message). ok=False when the pin is missing or denylisted."""
    if ref is None:
        return False, "no solide-drivers pin found in platformio.ini (cannot verify the driver)"
    if ref in WHITE_SCREEN_DENYLIST:
        return False, (
            f"solide-drivers is pinned to {ref}, a known white-screen version "
            f"(CUM-167). Bump the pin to {FIXED_AT} or later before releasing."
        )
    return True, f"solide-drivers pin {ref} is not denylisted"


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Release gate: solide-drivers pin check (CUM-167)")
    default_ini = os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "platformio.ini",
    )
    ap.add_argument("--ini", default=default_ini, help="path to platformio.ini")
    args = ap.parse_args(argv)

    try:
        with open(args.ini, "r", encoding="utf-8") as fh:
            text = fh.read()
    except OSError as exc:
        print(f"[gate:driver-pin] FAIL: cannot read {args.ini}: {exc}")
        return 1

    ok, msg = judge(find_pin(text))
    print(f"[gate:driver-pin] {'PASS' if ok else 'FAIL'}: {msg}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
