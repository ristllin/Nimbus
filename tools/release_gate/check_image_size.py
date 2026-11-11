#!/usr/bin/env python3
"""Release gate: every built firmware image must fit its env's app partition.

Lane TESTFIT (bench-501 evidence): the test-cyd image was 6,553,936 B against
the 6,553,600 B app0/app1 slot of default_16MB.csv. PlatformIO's own report
counts sections ("used 6553407 bytes from 6553600") and stayed green, but the
flashed image file carries headers and padding, and the ROM refuses to boot it
("Image length 6553936 doesn't fit in partition length 6553600"). This gate
compares the real firmware.bin size of every built env against the app
partition(s) of THAT env's partition table (resolved from platformio.ini,
following `extends`), so the overflow class fails the build instead of the
bench.

FAIL: any built image larger than any of its env's app partitions, a table
without an app partition, an unresolvable table, or no built images at all.
WARN (still exit 0): headroom under --warn-bytes (default 64 KB). Every env is
printed with its partition table, app slot size, image size and headroom,
production envs included.

Usage:
    python3 tools/release_gate/check_image_size.py [--root REPO] [--build-dir DIR]
        [--warn-bytes N] [--framework-partitions DIR ...]
Exit 0 = every built image fits; exit 1 = overflow / nothing built / bad table.
"""

from __future__ import annotations

import argparse
import configparser
import glob
import os
import sys

WARN_BYTES = 64 * 1024
DEFAULT_BUILD_DIR = os.path.join(".pio", "build")


def parse_ini(path: str) -> configparser.ConfigParser:
    cp = configparser.ConfigParser(interpolation=None, strict=False, comment_prefixes=(";", "#"), delimiters=("=",))
    with open(path, "r", encoding="utf-8") as fh:
        cp.read_file(fh)
    return cp


def env_names(cp: configparser.ConfigParser) -> "list[str]":
    return [s[len("env:") :] for s in cp.sections() if s.startswith("env:")]


def merged_options(cp: configparser.ConfigParser, section: str) -> "dict[str, str]":
    """The section's options with its `extends` chain applied (child wins)."""
    chain: "list[str]" = []
    cur: "str | None" = section
    seen: "set[str]" = set()
    while cur and cur not in seen and cp.has_section(cur):
        seen.add(cur)
        chain.append(cur)
        cur = cp[cur].get("extends")
    opts: "dict[str, str]" = {}
    for name in reversed(chain):
        opts.update(dict(cp[name]))
    return opts


def default_framework_dirs() -> "list[str]":
    home = os.path.expanduser("~")
    pat = os.path.join(home, ".platformio", "packages", "framework-arduinoespressif32*", "tools", "partitions")
    return sorted(glob.glob(pat))


def resolve_partitions_csv(root: str, value: str, framework_dirs: "list[str]") -> "str | None":
    """PlatformIO resolves a partitions value as a project-relative path first,
    then inside the framework package's partitions directory."""
    for cand in (os.path.join(root, value), value if os.path.isabs(value) else None):
        if cand and os.path.isfile(cand):
            return cand
    for d in framework_dirs:
        cand = os.path.join(d, value)
        if os.path.isfile(cand):
            return cand
    return None


def parse_int(token: str) -> int:
    token = token.strip()
    return int(token, 16) if token.lower().startswith("0x") else int(token, 10)


def parse_partitions_csv(path: str) -> "list[dict[str, object]]":
    rows: "list[dict[str, object]]" = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 5 or parts[1] == "Type":
                continue
            rows.append(
                {
                    "name": parts[0],
                    "type": parts[1],
                    "subtype": parts[2],
                    "offset": parse_int(parts[3]),
                    "size": parse_int(parts[4]),
                }
            )
    return rows


def app_rows(rows: "list[dict[str, object]]") -> "list[dict[str, object]]":
    return [r for r in rows if r["type"] == "app"]


def check_env(
    root: str, build_dir: str, env: str, opts: "dict[str, str]", framework_dirs: "list[str]", warn_bytes: int
) -> "tuple[str, str, str]":
    """One env: (status, table row, detail). status in OK/WARN/FAIL/NOTBUILT."""
    image = os.path.join(build_dir, env, "firmware.bin")
    if not os.path.isfile(image):
        return "NOTBUILT", f"{env:<18} {'-':<24} {'-':<12} {'-':>8} {'-':>8} {'-':>9}  not built", ""
    pval = opts.get("board_build.partitions", "default.csv")
    csv = resolve_partitions_csv(root, pval, framework_dirs)
    if csv is None:
        return "FAIL", f"{env:<18} {pval:<24} table not found", f"{env}: partitions table '{pval}' not found"
    rows = app_rows(parse_partitions_csv(csv))
    if not rows:
        return "FAIL", f"{env:<18} {os.path.basename(csv):<24} no app row", f"{env}: {pval} has no app partition"
    img = os.path.getsize(image)
    slot = min(int(r["size"]) for r in rows)  # type: ignore[arg-type]
    names = "/".join(str(r["name"]) for r in rows)
    headroom = slot - img
    row = f"{env:<18} {os.path.basename(csv):<24} {names:<12} {slot:>8} {img:>8} {headroom:>9}  "
    if img > slot:
        over = img - slot
        detail = (
            f"{env}: firmware.bin {img} B is {over} B over app partition {names} "
            f"({slot} B, {os.path.basename(csv)}) - the ROM refuses to boot it"
        )
        return "FAIL", row + "FAIL", detail
    if headroom < warn_bytes:
        return "WARN", row + "WARN", f"{env}: headroom {headroom} B is under {warn_bytes} B ({warn_bytes // 1024} KB)"
    return "OK", row + "OK", ""


def check(
    root: str, build_dir: str, warn_bytes: int, framework_dirs: "list[str]"
) -> "tuple[bool, list[str], list[str], list[str]]":
    """(ok, table lines, fail details, warn details) over every env in platformio.ini."""
    cp = parse_ini(os.path.join(root, "platformio.ini"))
    lines = [f"{'env':<18} {'partitions':<24} {'app slot(s)':<12} {'slot B':>8} {'image B':>8} {'headroom':>9}  status"]
    fails: "list[str]" = []
    warns: "list[str]" = []
    built = 0
    for env in env_names(cp):
        opts = merged_options(cp, f"env:{env}")
        status, row, detail = check_env(root, build_dir, env, opts, framework_dirs, warn_bytes)
        lines.append(row)
        if status == "NOTBUILT":
            continue
        built += 1
        if status == "FAIL":
            fails.append(detail)
        elif status == "WARN":
            warns.append(detail)
    if built == 0:
        fails.append(f"no built firmware images under {build_dir} - run `pio run` first; nothing was checked")
    return (not fails), lines, fails, warns


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Release gate: built firmware.bin must fit its env's app partition")
    default_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--root", default=default_root, help="repo root (default: this file's repo)")
    ap.add_argument("--build-dir", default=None, help="build dir (default: <root>/.pio/build)")
    ap.add_argument("--warn-bytes", type=int, default=WARN_BYTES, help="WARN threshold on headroom (default 64 KB)")
    ap.add_argument(
        "--framework-partitions",
        action="append",
        default=None,
        help="extra dir to resolve named partition tables (repeatable; default: ~/.platformio framework packages)",
    )
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    build_dir = os.path.abspath(args.build_dir) if args.build_dir else os.path.join(root, DEFAULT_BUILD_DIR)
    framework_dirs = args.framework_partitions or default_framework_dirs()
    ok, lines, fails, warns = check(root, build_dir, args.warn_bytes, framework_dirs)
    for line in lines:
        print(f"[gate:image-size] {line}")
    for detail in fails:
        print(f"[gate:image-size] FAIL: {detail}")
    for detail in warns:
        print(f"[gate:image-size] WARN: {detail}")
    if ok:
        print(f"[gate:image-size] PASS: every built image fits its app partition (WARN threshold {args.warn_bytes} B)")
        return 0
    print("[gate:image-size] FAIL: see above")
    return 1


if __name__ == "__main__":
    sys.exit(main())
