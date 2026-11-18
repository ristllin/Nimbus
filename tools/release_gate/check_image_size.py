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

Per-lane allowance (lane RB-GATE0, PRD section 10): with --lane, every built env that has
a row in image_baseline.json is also measured as code bytes plus docs pack bytes. The docs
pack is read from firmware.elf (the pack's tables and every string they point at, merged
literals counted once) and counted apart; code bytes are firmware.bin minus the pack. The
code delta against the baseline must be at most the lane's allowance in
image_budget.d/<lane>.json (no file: 0 B; a negative allowance is a promised deletion).
FAIL: over the allowance, a malformed budget file, a bin without its ELF, no built image
with a baseline row, or a baseline whose sha is not an ancestor of HEAD. The baseline is
re-recorded with --record-baseline after each merge into the integration branch.

Usage:
    python3 tools/release_gate/check_image_size.py [--root REPO] [--build-dir DIR]
        [--warn-bytes N] [--framework-partitions DIR ...]
    python3 tools/release_gate/check_image_size.py --lane RB-X        # slot fit + allowance
    python3 tools/release_gate/check_image_size.py --record-baseline  # after a merge, at its sha
Exit 0 = every built image fits (and, with --lane, is inside the allowance: "IMAGE ALLOWANCE
GREEN"); exit 1 = overflow / over the allowance / nothing built / bad table or file.
"""

from __future__ import annotations

import argparse
import configparser
import glob
import json
import mmap
import os
import struct
import subprocess
import sys

WARN_BYTES = 64 * 1024
DEFAULT_BUILD_DIR = os.path.join(".pio", "build")
HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, "image_baseline.json")
BUDGET_DIR = os.path.join(HERE, "image_budget.d")
ATAG = "[gate:image-allowance]"


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


# --- per-lane allowance: code bytes and docs pack bytes, measured apart -----------------

SHT_SYMTAB, SHT_NOBITS, SHF_ALLOC = 2, 8, 0x2


def elf_sections(blob: bytes) -> "list[dict[str, int]]":
    """Section headers of an ELF32 little-endian image (the xtensa toolchain's output)."""
    if len(blob) < 52 or blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 1:
        raise ValueError("not a 32-bit little-endian ELF")
    shoff = struct.unpack_from("<I", blob, 0x20)[0]
    shentsize, shnum = struct.unpack_from("<HH", blob, 0x2E)
    if shentsize < 40 or shoff + shnum * shentsize > len(blob):
        raise ValueError("bad section table")
    keys = ("name", "type", "flags", "addr", "offset", "size", "link")
    return [dict(zip(keys, struct.unpack_from("<7I", blob, shoff + i * shentsize))) for i in range(shnum)]


def elf_symbols(blob: bytes, sections: "list[dict[str, int]]", wanted: "set[str]") -> "dict[str, tuple[int, int]]":
    """Wanted symbol name -> (address, size), from every SHT_SYMTAB."""
    out: "dict[str, tuple[int, int]]" = {}
    keys = {w.encode(): w for w in wanted}
    for sec in sections:
        if sec["type"] != SHT_SYMTAB or sec["link"] >= len(sections):
            continue
        strs = sections[sec["link"]]
        names = blob[strs["offset"] : strs["offset"] + strs["size"]]
        table = blob[sec["offset"] : sec["offset"] + sec["size"] - sec["size"] % 16]
        for name_off, value, size, _info, _other, _shndx in struct.iter_unpack("<IIIBBH", table):
            end = names.find(b"\x00", name_off)
            key = keys.get(names[name_off:end]) if end > name_off else None
            if key:
                out[key] = (value, size)
    return out


def file_offset(sections: "list[dict[str, int]]", addr: int) -> "int | None":
    for sec in sections:
        loaded = sec["flags"] & SHF_ALLOC and sec["type"] != SHT_NOBITS
        if loaded and sec["addr"] <= addr < sec["addr"] + sec["size"]:
            return sec["offset"] + addr - sec["addr"]
    return None


def docs_pack_bytes(blob: bytes, tables: "list[dict]") -> "tuple[int, list[str]]":
    """(bytes the docs pack occupies in the image, notes): table bytes plus unique strings."""
    sections = elf_sections(blob)
    syms = elf_symbols(blob, sections, {t["symbol"] for t in tables})
    total, notes, seen = 0, [], set()
    for t in tables:
        if t["symbol"] not in syms:
            notes.append(f"{t['symbol']} is not in the image: counted as 0 docs pack bytes")
            continue
        addr, size = syms[t["symbol"]]
        base = file_offset(sections, addr)
        if base is None:
            raise ValueError(f"{t['symbol']} is not in a loaded section")
        total += size
        for row in range(0, size - size % t["entry_bytes"], t["entry_bytes"]):
            for off in t["string_offsets"]:
                ptr = struct.unpack_from("<I", blob, base + row + off)[0]
                total += string_bytes(blob, sections, ptr, seen)
    return total, notes


def string_bytes(blob: bytes, sections: "list[dict[str, int]]", ptr: int, seen: "set[int]") -> int:
    """Bytes of the NUL-terminated string at ptr, once per address."""
    if ptr == 0 or ptr in seen:
        return 0
    seen.add(ptr)
    off = file_offset(sections, ptr)
    if off is None:
        raise ValueError(f"docs pack string at {ptr:#x} is not in a loaded section")
    end = blob.find(b"\x00", off)
    if end < 0:
        raise ValueError(f"docs pack string at {ptr:#x} has no terminating NUL")
    return end - off + 1


def measure(env_dir: str, tables: "list[dict]") -> "dict[str, int]":
    """image, docs pack and code bytes of one built env. Raises OSError / ValueError."""
    image = os.path.getsize(os.path.join(env_dir, "firmware.bin"))
    elf = os.path.join(env_dir, "firmware.elf")
    if not os.path.isfile(elf):
        raise ValueError(f"{elf} is missing: the docs pack is measured from the ELF beside firmware.bin")
    with open(elf, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as blob:
        docs, _ = docs_pack_bytes(blob, tables)  # type: ignore[arg-type]
    return {"image_bytes": image, "docs_pack_bytes": docs, "code_bytes": image - docs}


def record_baseline(build_dir: str, sha: str, path: str, tables: "list[dict]", about: str = "") -> "dict":
    envs = {env: measure(os.path.join(build_dir, env), tables) for env in built_envs(build_dir)}
    row = {"about": about, "sha": sha, "docs_pack_tables": tables, "envs": envs}
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(row, fh, indent=2)
        fh.write("\n")
    return row


def load_budget(budget_dir: str, lane: str) -> "tuple[dict | None, list[str]]":
    """(budget row or None when the lane has no file, problems)."""
    path = os.path.join(budget_dir, f"{lane}.json")
    if not os.path.exists(path):
        return None, []
    name = os.path.basename(path)
    try:
        with open(path, encoding="utf-8") as fh:
            row = json.load(fh)
    except (OSError, ValueError) as e:
        return None, [f"{name}: {e}"]
    errs = []
    if not isinstance(row, dict) or row.get("lane") != lane:
        errs.append(f"{name}: 'lane' must be {lane}")
        row = row if isinstance(row, dict) else {}
    envs = row.get("envs", {})
    if not isinstance(envs, dict):
        errs.append(f"{name}: 'envs' must map env names to integers")
        envs = {}
    values = [row.get("allowance_bytes"), *envs.values()]
    if not all(isinstance(v, int) and not isinstance(v, bool) for v in values):
        errs.append(f"{name}: allowance_bytes and every envs value must be integers (bytes)")
    if not str(row.get("why") or "").strip():
        errs.append(f"{name}: 'why' is required (where the figure comes from)")
    return row, errs


def env_allowance(env: str, budget: "dict | None") -> int:
    if budget is None:
        return 0
    return int(dict(budget.get("envs") or {}).get(env, budget["allowance_bytes"]))


def judge_env(env: str, now: "dict[str, int]", base: "dict[str, int]", allow: int) -> "tuple[str, str | None]":
    delta = now["code_bytes"] - base["code_bytes"]
    ddelta = now["docs_pack_bytes"] - base["docs_pack_bytes"]
    line = (
        f"{env}: code {now['code_bytes']} B ({delta:+d} B; allowance {allow} B), "
        f"docs pack {now['docs_pack_bytes']} B ({ddelta:+d} B, counted apart), image {now['image_bytes']} B"
    )
    if delta <= allow:
        return line, None
    return line, f"{env}: code grew {delta:+d} B against the baseline, {delta - allow} B over the allowance {allow} B"


def built_from(env_dir: str, build_id: "str | None") -> "str | None":
    """A problem when firmware.bin or firmware.elf does not carry this commit's clean build id
    (a stale .pio, a stale ELF beside a fresh bin, or a build of an uncommitted tree: '-dirty')."""
    if build_id is None:
        return None
    want = build_id.encode() + b"\x00"
    for name in ("firmware.bin", "firmware.elf"):
        path = os.path.join(env_dir, name)
        if not os.path.isfile(path):
            return f"{name} is missing"
        with open(path, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as blob:
            if blob.find(want) < 0:
                return f"{name} is not this commit's clean build (no '{build_id}' inside): commit, then build it"
    return None


def check_allowance(
    build_dir: str, lane: str, baseline_path: str, budget_dir: str, baseline_problems=None, build_id=None
):
    """(ok, report lines, failures) for one lane's images against the recorded baseline.

    baseline_problems(sha) -> list of problems with the recorded baseline (git: ancestry, staleness);
    build_id, when given, must be inside every measured firmware.bin.
    """
    with open(baseline_path, encoding="utf-8") as fh:
        baseline = json.load(fh)
    budget, fails = load_budget(budget_dir, lane)
    usable = None if fails else budget  # a malformed file grants nothing
    lines = [f"{lane}: " + ("no budget file: allowance 0 B" if budget is None else f"budget {lane}.json")]
    if baseline_problems is not None:
        fails += baseline_problems(baseline["sha"])
    for env, base in sorted(baseline["envs"].items()):
        env_dir = os.path.join(build_dir, env)
        if not os.path.isfile(os.path.join(env_dir, "firmware.bin")):
            fails.append(f"{env} has a baseline row but is not built: build every baseline env (pio run -e {env})")
            continue
        try:
            stale = built_from(env_dir, build_id)
            if stale:
                raise ValueError(stale)
            now = measure(env_dir, baseline["docs_pack_tables"])
        except (OSError, ValueError) as e:
            fails.append(f"{env}: {e}")
            continue
        line, fail = judge_env(env, now, base, env_allowance(env, usable))
        lines.append(line)
        if fail:
            fails.append(fail)
    lines += [f"{env}: built, no baseline row: NOT GATED (record one)" for env in unbaselined(build_dir, baseline)]
    return not fails, lines, fails


def built_envs(build_dir: str) -> "list[str]":
    if not os.path.isdir(build_dir):
        return []
    return sorted(e for e in os.listdir(build_dir) if os.path.isfile(os.path.join(build_dir, e, "firmware.bin")))


def unbaselined(build_dir: str, baseline: dict) -> "list[str]":
    return [e for e in built_envs(build_dir) if e not in baseline["envs"]]


# Paths whose change moves code bytes. The generated docs packs only move docs bytes.
FIRMWARE_PATHS = (
    "src/",
    "lib/",
    "include/",
    "patches/",
    "boards/",
    "platformio.ini",
    "partitions",
    "tools/git_version.py",
    "tools/engine/engine",
    "tools/engine_filter.py",
)
DOCS_PACK_HEADERS = ("lib/core/include/nimbus/docs_pack_data.h", "lib/core/include/nimbus/docs_pack_e2_data.h")


def git(root: str, *args: str) -> "tuple[int, str]":
    r = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True)
    return r.returncode, (r.stdout if r.returncode == 0 else r.stderr).strip()


def git_baseline_problems(root: str, base_ref: str):
    """The baseline must be on this line and current at the lane's merge base: no firmware
    source may change between the recorded sha and merge-base(HEAD, base_ref)."""

    def problems(sha: str) -> "list[str]":
        if git(root, "merge-base", "--is-ancestor", sha, "HEAD")[0] != 0:
            return [f"baseline {sha[:12]} is not an ancestor of HEAD: record it on this line"]
        rc, mb = git(root, "merge-base", "HEAD", base_ref)
        if rc != 0:
            return [f"cannot find the merge base of HEAD and '{base_ref}': {mb}"]
        rc, out = git(root, "diff", "--name-only", sha, mb)
        if rc != 0:
            return [f"git diff {sha[:12]} {mb[:12]} failed: {out}"]
        moved = [f for f in out.splitlines() if f.startswith(FIRMWARE_PATHS) and f not in DOCS_PACK_HEADERS]
        if moved:
            return [
                f"baseline {sha[:12]} is stale: {len(moved)} firmware file(s) changed before this lane's merge base "
                f"(first {moved[0]}); re-record it on {base_ref} with --record-baseline"
            ]
        return []

    return problems


def git_build_id(root: str) -> "str | None":
    """The id tools/git_version.py writes into the image (git describe, no -dirty)."""
    rc, out = git(root, "describe", "--tags", "--always", "--match", "v[0-9]*")
    return out if rc == 0 and out else None


def run_allowance(args, root: str, build_dir: str) -> int:
    probe = None if args.no_ancestry else git_baseline_problems(root, args.base_ref)
    want = None if args.no_ancestry else git_build_id(root)
    try:
        if not args.no_ancestry and want is None:
            raise ValueError("no git build id for HEAD (git describe failed): cannot tell this commit's build")
        ok, lines, fails = check_allowance(build_dir, args.lane, args.baseline, args.budget_dir, probe, want)
    except (OSError, ValueError, KeyError, TypeError, struct.error) as e:
        ok, lines, fails = False, [], [f"{type(e).__name__}: {e}"]
    for line in lines:
        print(f"{ATAG} {line}")
    for detail in fails:
        print(f"{ATAG} FAIL: {detail}")
    off = " (provenance checks off)" if args.no_ancestry else ""
    print(f"{ATAG} IMAGE ALLOWANCE {'GREEN' if ok else 'RED'} {args.lane}{off}")
    return 0 if ok else 1


def run_record(args, root: str, build_dir: str) -> int:
    rc, sha = git(root, "rev-parse", "HEAD")
    want = git_build_id(root)
    try:
        with open(args.baseline, encoding="utf-8") as fh:
            old = json.load(fh)
        missing = [env for env in old["envs"] if env not in built_envs(build_dir)]
        if missing:
            raise ValueError(f"baseline env(s) not built: {', '.join(missing)}; a baseline never drops an env")
        stale = [env for env in built_envs(build_dir) if built_from(os.path.join(build_dir, env), want)]
        if rc != 0 or not want or stale:
            raise ValueError(f"not every built image is this commit's ({', '.join(stale) or 'no git id'}): build first")
        row = record_baseline(build_dir, sha, args.baseline, old["docs_pack_tables"], old.get("about", ""))
    except (OSError, ValueError, KeyError, struct.error) as e:
        print(f"{ATAG} FAIL: cannot record the baseline: {e}")
        return 1
    for env, e in row["envs"].items():
        print(f"{ATAG} recorded {env}: {e} at {sha[:12]}")
    return 0


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
    ap.add_argument("--lane", help="also check this lane's image allowance (RB-<id>)")
    ap.add_argument("--baseline", default=BASELINE, help="recorded baseline (default: image_baseline.json)")
    ap.add_argument("--budget-dir", default=BUDGET_DIR, help="lane budget files (default: image_budget.d)")
    ap.add_argument("--record-baseline", action="store_true", help="re-record the baseline from the built images")
    ap.add_argument("--base-ref", default="rb-main", help="integration branch for the baseline freshness check")
    ap.add_argument("--no-ancestry", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    build_dir = os.path.abspath(args.build_dir) if args.build_dir else os.path.join(root, DEFAULT_BUILD_DIR)
    if args.record_baseline:
        return run_record(args, root, build_dir)
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
    else:
        print("[gate:image-size] FAIL: see above")
    if args.lane:
        return max(0 if ok else 1, run_allowance(args, root, build_dir))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
