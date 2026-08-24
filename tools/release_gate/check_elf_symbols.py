#!/usr/bin/env python3
"""Release gate: the TFT firmware must link ZERO e-paper (GxEPD2) symbols.

CUM-167 verification bar: the ~14.5 KB internal-SRAM win from compiling the
e-paper driver out must survive any display fix. If a change accidentally relinks
GxEPD2 into the color-TFT image, internal SRAM shrinks and the panel/relay heap
pressure that caused earlier field failures returns. This check inspects the
built ELF's symbol table and fails if any e-paper symbol is present.

It also asserts the color-TFT panel plumbing IS linked (a real defined symbol),
so an empty/half-built ELF cannot pass silently.

Usage:
    pio run -e esp32s3            # build first
    python3 tools/release_gate/check_elf_symbols.py [--elf <path>]
Exit 0 = clean; exit 1 = e-paper linked, plumbing missing, or ELF/nm unavailable.
"""

from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_ELF = os.path.join(REPO, ".pio", "build", "esp32s3", "firmware.elf")

# Symbols that must NOT appear (the e-paper footprint).
EPAPER_MARKERS = ("GxEPD2", "SSD1680")
# A symbol that MUST appear (proves the color-TFT driver is actually linked).
REQUIRED_TFT = "solide11display_tft5begin"  # mangled solide::display_tft::begin


def find_nm() -> "str | None":
    hits = glob.glob(os.path.expanduser("~/.platformio/packages/toolchain-*/bin/xtensa-esp32s3-elf-nm"))
    return hits[0] if hits else None


def read_symbols(nm: str, elf: str) -> str:
    out = subprocess.run([nm, elf], capture_output=True, text=True)
    return out.stdout


def check(symbols: str) -> "tuple[bool, list[str]]":
    """(ok, messages)."""
    msgs = []
    ok = True
    epaper = [m for m in EPAPER_MARKERS if m.lower() in symbols.lower()]
    if epaper:
        ok = False
        msgs.append(f"e-paper symbols present in the TFT image: {epaper} (the 14.5 KB win regressed)")
    else:
        msgs.append("no GxEPD2 / e-paper symbols (size win intact)")
    if REQUIRED_TFT not in symbols:
        ok = False
        msgs.append(f"missing color-TFT plumbing symbol ({REQUIRED_TFT}) - ELF may be empty/half-built")
    else:
        msgs.append("color-TFT panel plumbing is linked")
    return ok, msgs


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Release gate: e-paper symbol check (CUM-167)")
    ap.add_argument("--elf", default=DEFAULT_ELF, help="path to the built esp32s3 firmware.elf")
    args = ap.parse_args(argv)

    if not os.path.exists(args.elf):
        print(f"[gate:elf-symbols] FAIL: ELF not found at {args.elf} - run `pio run -e esp32s3` first")
        return 1
    nm = find_nm()
    if not nm:
        print("[gate:elf-symbols] FAIL: xtensa-esp32s3-elf-nm not found (build the firmware once to fetch the toolchain)")
        return 1

    ok, msgs = check(read_symbols(nm, args.elf))
    for m in msgs:
        print(f"[gate:elf-symbols] {'ok' if ok else '!!'} {m}")
    print(f"[gate:elf-symbols] {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
