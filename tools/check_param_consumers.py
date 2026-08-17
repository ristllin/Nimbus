#!/usr/bin/env python3
"""Param->consumer CI gate - fail the build on a knob that lies.

Born from the 2026-07 UX audit: profile.cpp's Param table is a beautiful menu/web
CATALOG (label, help, range, per-profile preset for every knob) that LOOKS
authoritative and is shown to the user - but NOTHING links a Param to the code that
reads effective(Param::X). RingFps shipped fully wired to the menu, the web UI, and
NVS, read by zero code, with green CI (the anim cadence was a hardcoded constant).

This gate asserts every editable Param has at least one FUNCTIONAL consumer - an
`effective(Param::NAME)` reference outside the profile definition + the pure UI
catalog surfaces. An orphan fails CI. Add an ALLOWLIST entry (with a reason) only
for a param consumed via genuine indirection.

NOTE: this catches the ZERO-consumer class (dead knobs). It does NOT catch a param
that reaches a Plan field which a lower layer then drops (AttnLedIndex, AttnPeriodMs
in Dark/Calm, FullRefreshEveryN) - that seam-drop class is guarded by the LED/e-ink
golden-frame contract tests instead. Two mechanisms, two failure modes.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROFILE_H = ROOT / "lib" / "core" / "include" / "nimbus" / "profile.h"

# Params whose consumer is genuine but not a literal effective(Param::X) call.
# Each MUST carry a reason. Empty today - keep it that way if you can.
ALLOWLIST: dict[str, str] = {}

# Only this script is excluded. We match `effective(Param::X)` specifically, and the
# catalog surfaces (the enum def, profile.cpp's meta/preset/label tables, webui kParams,
# menu rows) reference params by `Param::X` but NEVER via `effective(Param::X)` - so
# the accessor methods in profile.h (e.g. posture() -> effective(Param::Posture)) still
# count as the real consumer interface without a per-file exclusion.
CATALOG_HINTS = ("check_param_consumers.py",)


def param_names() -> list[str]:
    text = PROFILE_H.read_text()
    m = re.search(r"enum class Param\s*:\s*\w+\s*\{(.*?)\};", text, re.S)
    if not m:
        sys.exit("check_param_consumers: could not find the Param enum in profile.h")
    names = []
    for line in m.group(1).splitlines():
        line = line.split("//")[0]  # drop trailing comment
        name = re.split(r"[=,]", line)[0].strip()  # token before '=' initializer / ','
        if name and name != "COUNT":
            names.append(name)
    return names


def has_functional_consumer(name: str) -> bool:
    """True if effective(Param::name) is referenced outside the catalog surfaces."""
    pat = re.compile(r"effective\s*\(\s*Param::" + re.escape(name) + r"\b")
    for base in (ROOT / "src", ROOT / "lib"):
        for f in base.rglob("*"):
            if f.suffix not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            if any(h in f.name for h in CATALOG_HINTS):
                continue
            try:
                if pat.search(f.read_text(errors="ignore")):
                    return True
            except OSError:
                continue
    return False


def main() -> int:
    orphans = []
    for name in param_names():
        if name in ALLOWLIST:
            continue
        if not has_functional_consumer(name):
            orphans.append(name)

    if orphans:
        print(
            "check_param_consumers: FAIL - knob(s) with NO functional consumer (shown to the user, read by nothing):",
            file=sys.stderr,
        )
        for n in orphans:
            print(
                f"  - Param::{n}: no effective(Param::{n}) outside the catalog. "
                f"WIRE it to a consumer, DELETE it from the enum, or ALLOWLIST "
                f"it with a reason.",
                file=sys.stderr,
            )
        return 1
    print(f"check_param_consumers: OK - all {len(param_names())} params have a functional consumer.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
