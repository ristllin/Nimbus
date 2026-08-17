#!/usr/bin/env python3
"""Status-language doc <-> code gate - fail the build when they drift.

The status->animation mapping is the single most-drifted piece of this codebase:
docs/notifier-status-language.md claimed "AwaitingApproval blinks, Error solid, Idle
breathes" while the code did otherwise - TWICE (the audit caught it, and again after
the ambient-grammar change). statusStyle.cpp (the presentation single-source) and the
doc table must agree.

This asserts every Status's animation + brightness in lib/core/src/status_style.cpp
matches its row in docs/notifier-status-language.md. Edit the code, run this, fix the
doc. (Full JSON-generation across the driver + broker is a larger follow-up; this gates
the device<->docs slice, which is where the drift actually bit users.)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "lib" / "core" / "src" / "status_style.cpp"
DOC = ROOT / "docs" / "notifier-status-language.md"

# Anim enum -> the keyword(s) that MUST appear in the doc's Pattern column.
ANIM_DOC = {
    "Comet": ("comet",),
    "Breathe": ("breathe",),
    "Fade": ("fade",),
    "Solid": ("static", "solid"),  # doc says "Static dim"; code Anim::Solid
    "Off": ("off",),
}


def parse_code() -> dict[str, tuple[str, int]]:
    """status -> (anim, brightPct) from the statusStyle() switch."""
    out = {}
    pat = re.compile(r"case\s+Status::(\w+):\s*return\s*\{[^,]*,[^,]*,\s*Anim::(\w+),\s*(\d+)")
    for m in pat.finditer(SRC.read_text()):
        out[m.group(1)] = (m.group(2), int(m.group(3)))
    if not out:
        sys.exit("check_status_doc: could not parse status_style.cpp")
    return out


def parse_doc() -> dict[str, tuple[str, str]]:
    """status -> (pattern_cell_lower, bright_cell) from the markdown table."""
    out = {}
    for line in DOC.read_text().splitlines():
        if not line.startswith("| "):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 4:
            continue
        state = cells[0]
        if state in ("Running", "WaitingInput", "AwaitingApproval", "Done", "Error", "Idle", "Offline"):
            out[state] = (cells[2].lower(), cells[3])  # Pattern, Bright columns
    return out


def main() -> int:
    code = parse_code()
    doc = parse_doc()
    errors = []
    for status, (anim, bright) in code.items():
        if status not in doc:
            errors.append(f"{status}: in code, MISSING from the doc table")
            continue
        pattern_cell, bright_cell = doc[status]
        want = ANIM_DOC.get(anim, (anim.lower(),))
        if not any(w in pattern_cell for w in want):
            errors.append(
                f"{status}: code says Anim::{anim}, doc Pattern column is '{pattern_cell}' (want one of {want})"
            )
        # Brightness: Offline is "0%" only in code; the doc shows 0% too. Idle 20 etc.
        if status != "Offline" and f"{bright}%" not in bright_cell:
            errors.append(f"{status}: code brightPct {bright}%, doc Bright column '{bright_cell}'")

    if errors:
        print("check_status_doc: FAIL - status_style.cpp and notifier-status-language.md disagree:", file=sys.stderr)
        for e in errors:
            print("  -", e, file=sys.stderr)
        return 1
    print(f"check_status_doc: OK - all {len(code)} statuses match between code + docs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
