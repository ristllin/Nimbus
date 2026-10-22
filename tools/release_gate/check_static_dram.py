#!/usr/bin/env python3
"""Release gate: a shipped image's static internal RAM stays under its recorded ceiling.

Static DRAM (the ELF's .dram0.bss plus .dram0.data) is internal SRAM the heap never
gets back: every byte it grows is a byte less for a turn's TLS handshake. v4.5.14 grew
it by about 4.5 KB over v4.5.13 without anyone noticing until a bench A/B; v4.5.15
recovered 6.3 KB. This gate makes growth a visible, deliberate decision: each shipped
variant has a recorded baseline (the v4.5.15 ELF) and a ceiling (baseline plus a small
allowance) in static_dram_ceilings.json, and an image over its ceiling fails.

Raising a ceiling is an owner-visible change: it needs the bench heap A/B (idle and
request-start free heap, before and after) in the PR, never just "it grew".

The ELF section table is read directly (ELF32, little-endian, as the xtensa toolchain
writes it): no toolchain, no third-party module.

Usage:
    python3 tools/release_gate/check_static_dram.py
        every variant in the ceilings file whose .pio/build/<env>/firmware.elf exists
        (at least one must exist)
    python3 tools/release_gate/check_static_dram.py --elf <path> --variant <name>
Exit 0 = every checked image is at or under its ceiling; exit 1 = over a ceiling, an
unknown variant, a missing section or file, or nothing to check.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CEILINGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static_dram_ceilings.json")
SECTIONS = (".dram0.bss", ".dram0.data")


def section_sizes(blob: bytes) -> dict:
    """Section name -> size from an ELF32 little-endian image. Raises ValueError."""
    if len(blob) < 52 or blob[:4] != b"\x7fELF":
        raise ValueError("not an ELF file")
    if blob[4] != 1 or blob[5] != 1:
        raise ValueError("not a 32-bit little-endian ELF")
    shoff = struct.unpack_from("<I", blob, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", blob, 0x2E)
    if shentsize < 40 or shnum == 0 or shstrndx >= shnum:
        raise ValueError("no section table")
    if shoff + shnum * shentsize > len(blob):
        raise ValueError("section table past the end of the file")

    def header(i: int) -> tuple:
        # name, type, flags, addr, offset, size
        return struct.unpack_from("<IIIIII", blob, shoff + i * shentsize)

    strtab = header(shstrndx)
    str_off, str_size = strtab[4], strtab[5]
    if str_off + str_size > len(blob):
        raise ValueError("section names past the end of the file")
    names = blob[str_off : str_off + str_size]
    out = {}
    for i in range(shnum):
        name_off, _type, _flags, _addr, _off, size = header(i)
        end = names.find(b"\x00", name_off)
        if name_off >= len(names) or end < 0:
            continue
        out[names[name_off:end].decode("ascii", "replace")] = size
    return out


def static_dram(blob: bytes) -> int:
    sizes = section_sizes(blob)
    missing = [s for s in SECTIONS if s not in sizes]
    if missing:
        raise ValueError("missing section(s): " + ", ".join(missing))
    return sum(sizes[s] for s in SECTIONS)


def load_ceilings(path: str = CEILINGS) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)["variants"]


def judge(variant: str, used: int, ceilings: dict) -> tuple:
    """(ok, message) for one image's static DRAM against its recorded ceiling."""
    row = ceilings.get(variant)
    if row is None:
        return False, f"{variant}: no recorded ceiling (add it to static_dram_ceilings.json)"
    base, ceil = row["baseline"], row["ceiling"]
    delta = used - base
    msg = f"{variant}: static DRAM {used} B (baseline {base} B, {delta:+d} B; ceiling {ceil} B)"
    if used > ceil:
        return False, msg + f" - OVER by {used - ceil} B: recover it, or raise the ceiling with a bench heap A/B"
    return True, msg


def check_file(path: str, variant: str, ceilings: dict) -> bool:
    try:
        with open(path, "rb") as f:
            used = static_dram(f.read())
    except (OSError, ValueError) as e:
        print(f"FAIL {variant}: {path}: {e}")
        return False
    ok, msg = judge(variant, used, ceilings)
    print(("PASS " if ok else "FAIL ") + msg)
    return ok


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Static DRAM ceiling per shipped variant")
    ap.add_argument("--elf", help="the firmware.elf to check (with --variant)")
    ap.add_argument("--variant", help="the release variant name (nimbus-tft, freenove-28, ...)")
    ap.add_argument("--ceilings", default=CEILINGS, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    try:
        ceilings = load_ceilings(args.ceilings)
    except (OSError, ValueError, KeyError) as e:
        print(f"FAIL static DRAM: cannot read {args.ceilings}: {e}")
        return 1
    if args.elf or args.variant:
        if not (args.elf and args.variant):
            print("FAIL static DRAM: --elf and --variant go together")
            return 1
        return 0 if check_file(args.elf, args.variant, ceilings) else 1
    checked, ok = 0, True
    for variant, row in ceilings.items():
        path = os.path.join(REPO, ".pio", "build", row["env"], "firmware.elf")
        if not os.path.exists(path):
            continue
        checked += 1
        ok = check_file(path, variant, ceilings) and ok
    if checked == 0:
        print("FAIL static DRAM: no shipped image is built (pio run -e esp32s3 first)")
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
