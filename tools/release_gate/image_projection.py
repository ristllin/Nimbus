#!/usr/bin/env python3
"""Image projection: flash bytes per object and per group, from two linker map files.

Why: the owner's fear is that engine 2 runs out of image space once every feature the
released engine has is back, and that this is found late. The analysis of 24 Nov 2026
(ram-flash.md, section 1) measured it once by hand from the two maps; this tool makes the
same measurement repeatable, so a parity lane can say before it starts what its feature
weighs in the released engine, and after it lands what it really added.

Two maps, A and B (GNU ld firmware.map of a device build):
- per object: text, rodata, merged strings, data and bss placed in the image, from the
  input-section lines after "Linker script and memory map" (sections at address 0 are
  discarded and not counted; fill is not counted);
- per group: the objects summed by the groups of parity_groups.json (first match wins;
  the rest are "other");
- the objects only in A, and the objects in both that are larger in A (the linker drops
  what B never calls), each with its bytes: with A = engine 1 and B = engine 2, this is the
  list of engine-1 feature code not yet in the engine-2 image;
- the objects only in B and the objects larger in B: with A = engine 2 before a lane and
  B = after, this is what the lane added.

Flash bytes of an object = text + rodata + strings + data (data initialisers are stored in
the image; bss is not). Merged string sections (.rodata.str1.1 and the like) are put by the
linker on the first object that contributes them, so per-object string bytes are
approximate: they are shown in their own column and the "flash" figure carries them, while
the projection uses text + rodata only when --no-strings is given.

The projection (A = engine 1, B = engine 2): the sum of the engine-1 bytes of every group
whose "returns" is "yes" or "partly", added to B's firmware.bin size, against the slot. It
is the "engine-1 size" bound of ram-flash.md section 1.5 (no engine-2 overhead, no new
wires); the analysis' likely and upper figures add judgement this tool does not make.

Usage:
    python3 tools/release_gate/image_projection.py A.map B.map [--bin B.bin] [--slot N]
        [--groups parity_groups.json] [--top N] [--group NAME] [--json out.json]
    python3 tools/release_gate/image_projection.py \\
        .pio/build/esp32s3-cyd/firmware.map .pio/build/esp32s3-cyd-e2/firmware.map \\
        --bin .pio/build/esp32s3-cyd-e2/firmware.bin
Exit 0 = report printed; 1 = a map cannot be read or has no memory map; 2 = bad arguments.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
GROUPS = os.path.join(HERE, "parity_groups.json")
SLOT_BYTES = 6_553_600  # app0/app1 of default_16MB.csv, the production envs' slot
KINDS = ("text", "rodata", "strings", "data", "bss")
FLASH_KINDS = ("text", "rodata", "strings", "data")

# Output sections of the ESP32-S3 linker script, by what their input sections are.
OUTPUT_KIND = {
    ".iram0.vectors": "text",
    ".iram0.text": "text",
    ".flash.text": "text",
    ".rtc.text": "text",
    ".flash.rodata": "rodata",
    ".flash.appdesc": "rodata",
    ".dram0.data": "data",
    ".rtc.data": "data",
    ".rtc.force_fast": "data",
    ".rtc.force_slow": "data",
    ".iram0.data": "data",
    ".dram0.bss": "bss",
    ".rtc.bss": "bss",
    ".noinit": "bss",
    ".rtc_noinit": "bss",
    ".iram0.bss": "bss",
    ".ext_ram.bss": "bss",
}
STRING_SECTION = re.compile(r"\.str\d|\.rodata\.str")
OUTPUT_RE = re.compile(r"^(\.[^\s]+)")
ONE_LINE_RE = re.compile(r"^ (\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$")
NAME_ONLY_RE = re.compile(r"^ (\S+)\s*$")
CONT_RE = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$")
BUILD_PREFIX_RE = re.compile(r"^(?:.*/)?\.pio/build/[^/]+/")
HASH_LIB_RE = re.compile(r"^lib[0-9a-f]{3}/")
MARKER = "Linker script and memory map"


def object_name(path: str) -> str:
    """The object with the build folder stripped, so two envs' maps name it alike.

    `.pio/build/<env>/lib19d/harness/plain_engine.cpp.o` -> `harness/plain_engine.cpp.o`;
    `.pio/build/<env>/src/agent/x.cpp.o` -> `src/agent/x.cpp.o`;
    `/abs/path/libspi_flash.a(spi_flash_wrap.c.obj)` -> `libspi_flash.a(spi_flash_wrap.c.obj)`.
    """
    path = path.strip()
    m = re.match(r"^(.*?)([^/]+\.a)\((.+)\)$", path)
    if m:
        return f"{m.group(2)}({m.group(3)})"
    path = BUILD_PREFIX_RE.sub("", path)
    return HASH_LIB_RE.sub("", path)


def input_line(line: str, pending: "str | None") -> "tuple[tuple | None, str | None]":
    """(name, addr, size, object) of one input-section line, or None; and the section name a
    wrapped line leaves for the next line (the linker puts a long name on its own line)."""
    m = ONE_LINE_RE.match(line)
    if m:
        return m.groups(), None
    m = CONT_RE.match(line) if pending else None
    if m:
        return (pending, *m.groups()), None
    nm = NAME_ONLY_RE.match(line)
    return None, (nm.group(1) if nm and not line.startswith(" *") else None)


def placed_sections(text: str) -> "dict[str, list[tuple[int, int, str, str]]]":
    """Output section -> [(addr, size, object, kind)] of every input section placed in the image."""
    start = text.find(MARKER)
    if start < 0:
        raise ValueError(f"no '{MARKER}' line: not a GNU ld map file")
    rows: "dict[str, list[tuple[int, int, str, str]]]" = defaultdict(list)
    output, kind, pending = "", None, None
    for line in text[start + len(MARKER) :].splitlines():
        if line and not line[0].isspace():  # an output section, /DISCARD/, OUTPUT(...), the cross references
            out = OUTPUT_RE.match(line)
            output = out.group(1) if out else ""
            kind, pending = OUTPUT_KIND.get(output), None
            continue
        if kind is None:
            continue
        rec, pending = input_line(line, pending)
        if rec is None or rec[0].startswith("*") or int(rec[1], 16) == 0 or int(rec[2], 16) == 0:
            continue
        k = "strings" if kind == "rodata" and STRING_SECTION.search(rec[0]) else kind
        rows[output].append((int(rec[1], 16), int(rec[2], 16), object_name(rec[3]), k))
    return rows


def parse_map(text: str) -> "dict[str, dict[str, int]]":
    """Object -> {kind: bytes} for every input section placed in the image."""
    objs: "dict[str, dict[str, int]]" = defaultdict(lambda: dict.fromkeys(KINDS, 0))
    for section in placed_sections(text).values():
        for (obj, k), n in attribute(section).items():
            objs[obj][k] += n
    if not objs:
        raise ValueError("no input sections placed in the image")
    return dict(objs)


def attribute(rows: "list[tuple[int, int, str, str]]") -> "dict[tuple[str, str], int]":
    """(object, kind) -> bytes for one output section, every image byte counted once.

    The linker merges equal string literals across objects into one blob. The map lists the
    blob on the first object that contributes, with a size that spans the whole blob, and lists
    the other objects' sections again at addresses inside it (their string sections at the
    pre-merge size). So each run of overlapping lines (a cluster) is counted at the bytes it
    really covers (the union of its ranges): the sections inside a spanning line keep their
    size (string sections share what is left in proportion), and the spanning line keeps the
    rest. Lines that overlap nothing are counted as listed. The sum is the image's bytes; one
    object's string figure is an estimate. Strings linked before the first listed object show
    as size 0 lines and stay on the blob's owner (an "apportion" group in parity_groups.json)."""
    out: "dict[tuple[str, str], int]" = defaultdict(int)
    for cluster in _clusters(sorted(rows, key=lambda r: (r[0], -r[1]))):
        attribute_cluster(cluster, out)
    return dict(out)


def attribute_cluster(cluster: "list[tuple[int, int, str, str]]", out: "dict[tuple[str, str], int]") -> None:
    if len(cluster) == 1:
        _a, n, obj, k = cluster[0]
        out[(obj, k)] += n
        return
    lo, hi = cluster[0][0], max(r[0] + r[1] for r in cluster)
    owner = next((r for r in cluster if r[0] == lo and r[0] + r[1] == hi), None)
    rest = [r for r in cluster if r is not owner]
    left = _share(rest, "fixed", _union(cluster), out)
    left = _share(rest, "strings", left, out)
    target = owner or cluster[0]
    out[(target[2], target[3])] += left


def _share(rows: "list[tuple[int, int, str, str]]", which: str, left: int, out: dict) -> int:
    """Give the rows of `which` ("fixed": every kind but strings, at their size while bytes are
    left; "strings": in proportion to their listed size) their bytes; return what is left."""
    picked = [r for r in rows if (r[3] == "strings") == (which == "strings")]
    listed = sum(r[1] for r in picked)
    scale = 1.0 if which == "fixed" else min(1.0, left / listed) if listed else 0.0
    for _a, n, obj, k in picked:
        share = min(int(n * scale), left)
        out[(obj, k)] += share
        left -= share
    return left


def _clusters(rows: "list[tuple]") -> "list[list[tuple]]":
    """Runs of rows whose ranges overlap, in address order."""
    out: "list[list[tuple]]" = []
    end = -1
    for r in rows:
        if out and r[0] < end:
            out[-1].append(r)
            end = max(end, r[0] + r[1])
        else:
            out.append([r])
            end = r[0] + r[1]
    return out


def _union(rows: "list[tuple]") -> int:
    total, end = 0, -1
    for a, n, *_ in rows:
        if a + n > end:
            total += a + n - max(a, end)
            end = a + n
    return total


def read_map(path: str) -> "dict[str, dict[str, int]]":
    with open(path, encoding="utf-8", errors="replace") as fh:
        return parse_map(fh.read())


def flash(row: "dict[str, int]", strings: bool = True) -> int:
    return sum(row[k] for k in FLASH_KINDS if strings or k != "strings")


def load_groups(path: str = GROUPS) -> "list[dict]":
    """[{name, returns, patterns (compiled), why}] in file order. Raises ValueError."""
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    out = []
    for g in data["groups"]:
        if g.get("returns") not in ("yes", "no", "partly") or not isinstance(g.get("apportion", False), bool):
            raise ValueError(f"group {g.get('name')!r}: returns must be yes, no or partly; apportion true or false")
        if not g.get("patterns"):
            raise ValueError(f"group {g.get('name')!r}: no patterns")
        out.append({**g, "compiled": [re.compile(p) for p in g["patterns"]]})
    return out


def group_of(obj: str, groups: "list[dict]") -> str:
    for g in groups:
        if any(p.search(obj) for p in g["compiled"]):
            return g["name"]
    return "other"


def library_of(obj: str) -> str:
    """A coarse folder: the archive, `src/<dir>`, or the library folder."""
    if "(" in obj:
        return obj.split("(", 1)[0]
    parts = obj.split("/")
    if parts[0] == "src" and len(parts) > 2:
        return "/".join(parts[:2])
    return parts[0] if len(parts) > 1 else "(top)"


def compare(a: dict, b: dict, strings: bool = True) -> dict:
    """The four lists, each [(obj, bytes in A, bytes in B)], largest difference first."""
    only_a = [(o, flash(a[o], strings), 0) for o in a if o not in b]
    only_b = [(o, 0, flash(b[o], strings)) for o in b if o not in a]
    both = [(o, flash(a[o], strings), flash(b[o], strings)) for o in a if o in b]
    larger_a = [r for r in both if r[1] > r[2]]
    larger_b = [r for r in both if r[2] > r[1]]

    def key(r):
        return -abs(r[1] - r[2])

    return {
        "only_a": sorted(only_a, key=key),
        "larger_a": sorted(larger_a, key=key),
        "only_b": sorted(only_b, key=key),
        "larger_b": sorted(larger_b, key=key),
    }


def missing_by_group(cmp: dict, groups: "list[dict]") -> "dict[str, dict]":
    """Group -> {bytes, objects}: what A has that B does not (only in A, plus A's surplus)."""
    out: "dict[str, dict]" = {}
    for obj, in_a, in_b in cmp["only_a"] + cmp["larger_a"]:
        g = out.setdefault(group_of(obj, groups), {"bytes": 0, "objects": []})
        g["bytes"] += in_a - in_b
        g["objects"].append((obj, in_a, in_b))
    return out


def projection(missing: dict, groups: "list[dict]", image_b: "int | None", slot: int) -> dict:
    """What returns at parity: the yes and partly groups at engine-1 size. An "apportion" group
    (bytes the map cannot place on their real object) returns in the share the others do."""
    returns = {g["name"]: g["returns"] for g in groups}
    shared = {g["name"] for g in groups if g.get("apportion")}
    plain = {k: v["bytes"] for k, v in missing.items() if k not in shared}
    back = sum(n for k, n in plain.items() if returns.get(k) in ("yes", "partly"))
    share = back / sum(plain.values()) if sum(plain.values()) > 0 else 0.0
    back += int(sum(missing[k]["bytes"] for k in shared if k in missing) * share)
    row = {"returns_bytes": back}
    if image_b is not None:
        row.update({"image_b": image_b, "projected": image_b + back, "slot": slot, "free": slot - image_b - back})
    return row


def totals(objs: dict) -> "dict[str, int]":
    return {k: sum(r[k] for r in objs.values()) for k in KINDS}


def fmt(n: int) -> str:
    return f"{n:,}"


def totals_table(a: dict, b: dict) -> "list[str]":
    lines = [
        "| map | text B | rodata B | strings B (approx.) | data B | bss B | flash B |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for name, t in (("A", totals(a)), ("B", totals(b))):
        lines.append(
            f"| {name} | {fmt(t['text'])} | {fmt(t['rodata'])} | {fmt(t['strings'])} | {fmt(t['data'])} "
            f"| {fmt(t['bss'])} | {fmt(flash(t))} |"
        )
    return lines


def group_table(missing: dict, groups: "list[dict]", proj: dict, slot: int) -> "list[str]":
    returns = {g["name"]: g["returns"] for g in groups}
    lines = [
        "## What A has that B does not, by group (objects only in A, plus A's surplus in shared objects)",
        "",
        "| group | returns at parity | bytes | objects |",
        "|---|---|---:|---:|",
    ]
    for name, v in sorted(missing.items(), key=lambda kv: -kv[1]["bytes"]):
        lines.append(f"| {name} | {returns.get(name, 'unknown')} | {fmt(v['bytes'])} | {len(v['objects'])} |")
    lines.append(f"| all | | {fmt(sum(v['bytes'] for v in missing.values()))} | |")
    lines += ["", f"Groups that return (yes or partly), at engine-1 size: {fmt(proj['returns_bytes'])} B."]
    if "image_b" in proj:
        lines.append(
            f"Projected B image: {fmt(proj['image_b'])} B + {fmt(proj['returns_bytes'])} B = {fmt(proj['projected'])} B; "
            f"free in the {fmt(slot)} B slot: {fmt(proj['free'])} B (engine-1 size, no engine-2 overhead, no new wires)."
        )
    return lines


OBJECT_LISTS = (
    ("only_a", "Objects only in A"),
    ("larger_a", "Objects larger in A"),
    ("only_b", "Objects only in B"),
    ("larger_b", "Objects larger in B"),
)


def object_tables(cmp: dict, groups: "list[dict]", top: int, only_group: "str | None") -> "list[str]":
    lines: "list[str]" = []
    for key, title in OBJECT_LISTS:
        rows = [r for r in cmp[key] if only_group is None or group_of(r[0], groups) == only_group]
        total = sum(abs(r[1] - r[2]) for r in rows)
        lines += ["", f"## {title}: {len(rows)} objects, {fmt(total)} B difference", ""]
        lines += ["| object | group | A B | B B | difference B |", "|---|---|---:|---:|---:|"]
        for obj, in_a, in_b in rows[: top if top > 0 else None]:
            lines.append(f"| {obj} | {group_of(obj, groups)} | {fmt(in_a)} | {fmt(in_b)} | {fmt(in_b - in_a)} |")
        if top > 0 and len(rows) > top:
            lines.append(f"| ... {len(rows) - top} more | | | | |")
    return lines


def folder_table(a: dict, b: dict, top: int, strings: bool) -> "list[str]":
    by_lib: "dict[str, list[int]]" = defaultdict(lambda: [0, 0])
    for side, objs in enumerate((a, b)):
        for o, r in objs.items():
            by_lib[library_of(o)][side] += flash(r, strings)
    lines = ["", "## Flash bytes per folder or archive", "", "| folder | A B | B B | B - A |", "|---|---:|---:|---:|"]
    for lib, (x, y) in sorted(by_lib.items(), key=lambda kv: -max(kv[1]))[: top if top > 0 else None]:
        lines.append(f"| {lib} | {fmt(x)} | {fmt(y)} | {fmt(y - x)} |")
    return lines


def report(args, a: dict, b: dict, groups: "list[dict]", image_b: "int | None") -> "tuple[list[str], dict]":
    """The printed report and the figures for --json. args: map_a, map_b, slot, top, group, no_strings."""
    strings = not args.no_strings
    cmp = compare(a, b, strings)
    missing = missing_by_group(cmp, groups)
    proj = projection(missing, groups, image_b, args.slot)
    lines = [f"A: {args.map_a} ({len(a)} objects)", f"B: {args.map_b} ({len(b)} objects)", ""]
    lines += totals_table(a, b) + [""] + group_table(missing, groups, proj, args.slot)
    lines += object_tables(cmp, groups, args.top, args.group) + folder_table(a, b, args.top, strings)
    returns = {g["name"]: g["returns"] for g in groups}
    data = {
        "a": args.map_a,
        "b": args.map_b,
        "totals": {"a": totals(a), "b": totals(b)},
        "missing_by_group": {
            k: {"bytes": v["bytes"], "returns": returns.get(k, "unknown")} for k, v in missing.items()
        },
        "projection": proj,
        "objects": {"a": a, "b": b},
    }
    return lines, data


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Flash bytes per object and group from two firmware.map files")
    ap.add_argument("map_a", help="A: the reference map (engine 1, or engine 2 before a lane)")
    ap.add_argument("map_b", help="B: the map to compare (engine 2, or engine 2 after a lane)")
    ap.add_argument("--bin", help="B's firmware.bin, for the projected image and its free bytes")
    ap.add_argument("--slot", type=int, default=SLOT_BYTES, help=f"app slot bytes (default {SLOT_BYTES})")
    ap.add_argument("--groups", default=GROUPS, help="group file (default parity_groups.json)")
    ap.add_argument("--top", type=int, default=40, help="rows per object list (0: all; default 40)")
    ap.add_argument("--group", help="list only the objects of this group")
    ap.add_argument("--no-strings", action="store_true", help="leave merged string bytes out of every figure")
    ap.add_argument("--json", help="also write the figures and every object's bytes here")
    args = ap.parse_args(argv)
    try:
        groups = load_groups(args.groups)
        a, b = read_map(args.map_a), read_map(args.map_b)
        image_b = os.path.getsize(args.bin) if args.bin else None
    except (OSError, ValueError, KeyError, re.error) as e:
        print(f"image projection: FAIL: {e}")
        return 1
    if args.group and args.group != "other" and args.group not in {g["name"] for g in groups}:
        print(f"image projection: unknown group {args.group!r}")
        return 2
    lines, data = report(args, a, b, groups, image_b)
    print("\n".join(lines))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=1, sort_keys=True)
            fh.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
