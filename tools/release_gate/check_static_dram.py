#!/usr/bin/env python3
"""Release gate: a shipped image's static internal RAM stays under its recorded ceiling.

Static DRAM (the ELF's .dram0.bss plus .dram0.data) is internal SRAM the heap never
gets back: every byte it grows is a byte less for a turn's TLS handshake. v4.5.14 grew
it by about 4.5 KB over v4.5.13 without anyone noticing until a bench A/B; v4.5.15
recovered 6.3 KB. This gate makes growth a visible, deliberate decision: each shipped
variant has a recorded baseline (the last measured tree, re-based at release boundaries)
and a ceiling (the v4.5.15 ELF plus a small allowance) in static_dram_ceilings.json, and
an image over its ceiling fails.

The engine-2 images (the *-e2 envs, lane RB-I0) have their own table, "engine2", in the same
file: release.yml ships none of them yet, so they are not shipped variants, but every check here
covers them the same way (a built e2 image over its ceiling fails).

Raising a ceiling is an owner-visible change: it needs the bench heap A/B (idle and
request-start free heap, before and after) in the PR, never just "it grew".

Declared allowances (lane E2-GATE, orchestrator ruling 121, 24 Nov 2026). An engine-2 row's
ceiling is its measured baseline plus 2,048 B of declared room: a policy line, far from the
physical limit. Growth above the baseline must be declared by the lane that makes it, in the
same file as its image allowance: "static_dram_bytes" in image_budget.d/<lane>.json
(static_dram_ceilings.json, allowance_how, says it in one place). With --lane, or on a
lane/<LANE> branch whose baseline is fresh (no firmware change between engine2_baseline_sha
and the merge base with rb-main), an engine-2 image whose growth exceeds the declared bytes
fails, ceiling or not; with a stale baseline it warns. The shipped engine-1 rows keep their
ceiling-only check.

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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_image_size as image  # noqa: E402  (the lane allowance files and the baseline freshness)

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
    """The shipped variants (release.yml's loop)."""
    with open(path, encoding="utf-8") as f:
        return json.load(f)["variants"]


def load_all_ceilings(path: str = CEILINGS) -> dict:
    """The shipped variants and the engine-2 images ("engine2"), one table; a name in both fails."""
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    rows = dict(data["variants"])
    for name, row in data.get("engine2", {}).items():
        if name in rows:
            raise ValueError(f"{name} is both a shipped variant and an engine-2 row")
        rows[name] = row
    return rows


def engine2_variants(path: str = CEILINGS) -> "set[str]":
    with open(path, encoding="utf-8") as f:
        return set(json.load(f).get("engine2", {}))


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


def declared_bytes(budget_dir: str, lane: str) -> "tuple[int, list[str]]":
    """(static DRAM bytes the lane declares in image_budget.d/<lane>.json, problems): 0 without a file."""
    row, errs = image.load_budget(budget_dir, lane)
    if row is None or errs:
        return 0, errs
    n = row.get("static_dram_bytes", 0)
    if not isinstance(n, int) or isinstance(n, bool) or n < 0:
        return 0, [f"{lane}.json: static_dram_bytes must be a non-negative integer (bytes)"]
    return n, []


def judge_declared(variant: str, used: int, row: dict, lane: str, allow: int) -> "tuple[bool, str]":
    """(ok, message): the growth over the baseline is inside what the lane declared."""
    growth = used - row["baseline"]
    if growth <= allow:
        return True, f"{variant}: growth {growth:+d} B inside {lane}'s declared {allow} B"
    return False, (
        f"{variant}: static DRAM grew {growth:+d} B over the baseline, {growth - allow} B more than {lane} declared "
        f"({allow} B): declare it as static_dram_bytes in image_budget.d/{lane}.json (ruling 121), or recover it"
    )


def check_file(path: str, variant: str, ceilings: dict, declared: "tuple[str, int] | None" = None) -> bool:
    """One image against its ceiling and, with declared = (lane, bytes) on an engine-2 row,
    its growth against what the lane declared."""
    try:
        with open(path, "rb") as f:
            used = static_dram(f.read())
    except (OSError, ValueError) as e:
        print(f"FAIL {variant}: {path}: {e}")
        return False
    ok, msg = judge(variant, used, ceilings)
    print(("PASS " if ok else "FAIL ") + msg)
    if declared is not None and variant in ceilings:
        dok, dmsg = judge_declared(variant, used, ceilings[variant], *declared)
        print(("PASS " if dok else "FAIL ") + dmsg)
        ok = ok and dok
    return ok


def lane_view(args) -> "tuple[str | None, int, list[str], list[str]]":
    """(lane to judge, its declared bytes, fails, warns): --lane, else a lane/<LANE> branch whose
    engine-2 baseline is fresh; a stale one only warns, any other branch is not judged."""
    lane = args.lane or (None if args.no_infer_lane else image.infer_lane(REPO))
    if lane is None:
        return None, 0, [], []
    allow, errs = declared_bytes(args.budget_dir, lane)
    if errs:
        return lane, 0, [f"FAIL static DRAM: {e}" for e in errs], []
    if args.lane:
        return lane, allow, [], []
    with open(args.ceilings, encoding="utf-8") as f:
        sha = json.load(f).get("engine2_baseline_sha")
    stale = image.git_baseline_problems(REPO, args.base_ref)(sha) if sha else ["no engine2_baseline_sha"]
    if stale:
        return (
            None,
            0,
            [],
            [f"WARN static DRAM: {lane}'s growth not judged, the engine-2 baseline is stale: {stale[0]}"],
        )
    return lane, allow, [], []


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Static DRAM ceiling per shipped variant")
    ap.add_argument("--elf", help="the firmware.elf to check (with --variant)")
    ap.add_argument("--variant", help="the release variant name (nimbus-tft, freenove-28, ...)")
    ap.add_argument("--ceilings", default=CEILINGS, help=argparse.SUPPRESS)
    ap.add_argument("--lane", help="judge engine-2 growth against this lane's declared static_dram_bytes")
    ap.add_argument("--budget-dir", default=image.BUDGET_DIR, help=argparse.SUPPRESS)
    ap.add_argument("--base-ref", default="rb-main", help="integration branch for the baseline freshness check")
    ap.add_argument("--no-infer-lane", action="store_true", help="do not read the lane from a lane/<LANE> branch")
    args = ap.parse_args(argv)
    try:
        ceilings = load_all_ceilings(args.ceilings)
    except (OSError, ValueError, KeyError) as e:
        print(f"FAIL static DRAM: cannot read {args.ceilings}: {e}")
        return 1
    try:
        lane, allow, lfails, lwarns = lane_view(args)
    except (OSError, ValueError) as e:
        lane, allow, lfails, lwarns = None, 0, [f"FAIL static DRAM: {e}"], []
    for line in lfails + lwarns:
        print(line)
    e2 = engine2_variants(args.ceilings)

    def declared(variant: str) -> "tuple[str, int] | None":
        return (lane, allow) if lane is not None and variant in e2 else None

    if args.elf or args.variant:
        if not (args.elf and args.variant):
            print("FAIL static DRAM: --elf and --variant go together")
            return 1
        lane_ok = not (lfails and args.variant in e2)  # a bad declaration fails the engine-2 images only
        return 0 if check_file(args.elf, args.variant, ceilings, declared(args.variant)) and lane_ok else 1
    return check_built(ceilings, declared, {v for v in e2 if lfails})


def check_built(ceilings: dict, declared, failed: "set[str]") -> int:
    """Every variant whose image is built in this tree; `failed`: variants already failed (a bad declaration)."""
    checked, ok = 0, True
    for variant, row in ceilings.items():
        path = os.path.join(REPO, ".pio", "build", row["env"], "firmware.elf")
        if not os.path.exists(path):
            continue
        checked += 1
        ok = check_file(path, variant, ceilings, declared(variant)) and variant not in failed and ok
    if checked == 0:
        print("FAIL static DRAM: no shipped image is built (pio run -e esp32s3 first)")
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
