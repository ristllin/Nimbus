#!/usr/bin/env python3
"""Release gate: an engine-1 device image carries no engine-2 object (lane RB-I0a).

The engine-2 sources (lib/core orch_tx_*, lib/harness plain_*, providers/plain_wire_*) are built
for the hosts long before a device runs them. lib/core and lib/harness are not archived on the
device link: every object is loaded, --gc-sections drops its unreferenced code, and the unwind
table (.eh_frame) of an object with exception cleanups stays in the image. RB-B0a's first tip
was 280 B larger for two such objects. tools/engine_filter.py (a PlatformIO pre-script in [s3])
drops every file in tools/engine/engine2_only.txt from an engine-1 device build, so the object
never exists. This gate keeps that true:

  1. list: every entry of engine2_only.txt is an existing .c/.cc/.cpp file under lib/ or src/,
     given once; every source matching ENGINE2_GLOBS is on the list (a new engine-2 file that
     nobody listed would be compiled into every device image).
  2. hook: every env that extends [s3] runs pre:tools/engine_filter.py (an env that overrides
     extra_scripts without it would build both engines).
  3. maps: no built firmware.map of an engine-1 device env (custom_nimbus_engine absent or 1)
     names an engine-2 object, listed or glob-matched, as a loaded object or archive member.
     A map that names one has its code, its .eh_frame or at least its load in the link.

Usage:
    python3 tools/release_gate/check_engine_filter.py [--root REPO] [--build-dir DIR] [--require-maps ENV ...]
Exit 0 = clean; exit 1 = a missing or stale list entry, a missing hook, or an engine-2 object in a device map.
"""

from __future__ import annotations

import argparse
import fnmatch
import glob
import os
import re
import sys
from typing import Iterable, List, Set, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

import engine_filter  # noqa: E402
from check_image_size import merged_options, parse_ini  # noqa: E402

TAG = "[gate:engine-filter]"
REPO = os.path.dirname(os.path.dirname(HERE))
LIST = os.path.join("tools", "engine", "engine2_only.txt")
HOOK = "pre:tools/engine_filter.py"
SOURCE_EXTS = (".c", ".cc", ".cpp")
# Every engine-2 source family. A file matching one of these must be on the list.
ENGINE2_GLOBS = (
    "lib/core/src/orch_tx_*",
    "lib/harness/src/plain_*",
    "lib/harness/src/providers/plain_wire_*",
)
# An object path in a map: .pio/build/<env>/lib<hash>/<lib>/<rel>.cpp.o, .pio/build/<env>/src/<rel>.cpp.o,
# or an archive member lib<name>.a(<base>.cpp.o).
MEMBER = re.compile(r"\.a\(([\w.+-]+\.(?:c|cc|cpp)\.o)\)")
BUILD_PREFIX = re.compile(r"^.*?\.pio/build/[^/]+/(?:lib[0-9a-f]+/)?")
LOADED = re.compile(r"(?:^|[\s(])(\S*?\.(?:c|cc|cpp)\.o)(?=[\s)]|$)")


def object_suffix(rel: str) -> "str | None":
    """The map path tail of a repo source: lib/<L>/src/<p> -> <L>/<p>.o, src/<p> -> src/<p>.o."""
    parts = rel.replace(os.sep, "/").split("/")
    if len(parts) >= 4 and parts[0] == "lib" and parts[2] == "src":
        return "/".join([parts[1]] + parts[3:]) + ".o"
    if len(parts) >= 2 and parts[0] == "src":
        return rel.replace(os.sep, "/") + ".o"
    return None


def glob_suffixes() -> List[str]:
    out = []
    for g in ENGINE2_GLOBS:
        suffix = object_suffix(g)
        out.append(suffix[: -len(".o")] + "*.o")  # plain_* -> harness/plain_*.o matches plain_x.cpp.o
    return out


def check_list(root: str) -> Tuple[List[str], List[str]]:
    """(entries, failures) for engine2_only.txt against the tree."""
    path = os.path.join(root, LIST)
    if not os.path.isfile(path):
        return [], [f"{LIST} is missing"]
    entries = engine_filter.load_list(path)
    fails = []
    seen: Set[str] = set()
    for rel in entries:
        clean = os.path.normpath(rel).replace(os.sep, "/")
        if rel in seen:
            fails.append(f"{LIST}: {rel} is listed twice")
        seen.add(rel)
        if os.path.isabs(rel) or clean != rel or not (rel.startswith("lib/") or rel.startswith("src/")):
            fails.append(f"{LIST}: {rel} is not a clean repo path under lib/ or src/")
            continue
        if not rel.endswith(SOURCE_EXTS):
            fails.append(f"{LIST}: {rel} is not a .c/.cc/.cpp source")
        if not os.path.isfile(os.path.join(root, rel)):
            fails.append(f"{LIST}: {rel} does not exist (a stale entry)")
    for pattern in ENGINE2_GLOBS:
        for hit in sorted(glob.glob(os.path.join(root, pattern))):
            rel = os.path.relpath(hit, root).replace(os.sep, "/")
            if os.path.isfile(hit) and rel.endswith(SOURCE_EXTS) and rel not in seen:
                fails.append(f"{rel} matches {pattern} but is not in {LIST}: every device image would compile it")
    return entries, fails


def device_envs(cp) -> List[str]:
    """Envs whose extends chain reaches [s3] (the device builds)."""
    out = []
    for section in cp.sections():
        if not section.startswith("env:"):
            continue
        cur, seen = section, set()
        while cur and cur not in seen and cp.has_section(cur):
            seen.add(cur)
            if cur == "s3":
                out.append(section[len("env:") :])
                break
            cur = cp[cur].get("extends")
    return out


def env_engine(cp, env: str) -> int:
    return engine_filter.engine_number(merged_options(cp, "env:" + env).get(engine_filter.ENGINE_OPTION))


def check_hooks(cp) -> List[str]:
    fails = []
    envs = device_envs(cp)
    if not envs:
        fails.append("platformio.ini: no env extends [s3]")
    for env in envs:
        scripts = merged_options(cp, "env:" + env).get("extra_scripts", "")
        if HOOK not in scripts.split():
            fails.append(f"platformio.ini env:{env}: extra_scripts lacks {HOOK}, so it compiles both engines")
        try:
            env_engine(cp, env)
        except engine_filter.EngineFilterError as exc:
            fails.append(f"platformio.ini env:{env}: {exc}")
    return fails


def engine2_objects(map_text: str, entries: Iterable[str]) -> List[str]:
    """Every engine-2 object (listed or glob-matched) a linker map names, sorted, once each.

    A loaded object is reported by its tail (harness/plain_engine.cpp.o), an archive member by
    its name (plain_engine.cpp.o)."""
    tails = [s for s in (object_suffix(e) for e in entries) if s] + glob_suffixes()
    members = [os.path.basename(t) for t in tails]
    found: Set[str] = set()
    for line in map_text.splitlines():
        if ".o" not in line:
            continue
        for m in MEMBER.finditer(line):
            if any(fnmatch.fnmatchcase(m.group(1), g) for g in members):
                found.add(m.group(1))
        for m in LOADED.finditer(line):
            path = m.group(1).replace("\\", "/")
            if ".a(" in path:
                continue
            for t in tails:
                if fnmatch.fnmatchcase(path, t) or fnmatch.fnmatchcase(path, "*/" + t):
                    found.add(BUILD_PREFIX.sub("", path))
    return sorted(found)


def check_maps(
    root: str, build_dir: str, cp, entries: List[str], require: Iterable[str]
) -> Tuple[List[str], List[str]]:
    lines, fails = [], []
    require = list(require)
    for env in device_envs(cp):
        path = os.path.join(build_dir, env, "firmware.map")
        if not os.path.isfile(path):
            if env in require:
                fails.append(f"env:{env}: {os.path.relpath(path, root)} is not built (required)")
            continue
        if env_engine(cp, env) != 1:
            lines.append(f"env:{env}: engine 2, not an engine-1 image (skipped)")
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            hits = engine2_objects(fh.read(), entries)
        if hits:
            shown = ", ".join(hits[:8]) + (f" and {len(hits) - 8} more" if len(hits) > 8 else "")
            fails.append(f"env:{env}: firmware.map names {len(hits)} engine-2 object(s): {shown}")
        else:
            lines.append(f"env:{env}: firmware.map names no engine-2 object")
    for env in require:
        if env not in device_envs(cp):
            fails.append(f"env:{env}: not a device env of platformio.ini (required)")
    if not lines and not fails:
        lines.append("no device map built; the map check ran on nothing (build a device env to check it)")
    return lines, fails


def check(root: str, build_dir: str, require: Iterable[str] = ()) -> Tuple[bool, List[str], List[str]]:
    entries, fails = check_list(root)
    lines = [f"{LIST}: {len(entries)} engine-2 sources"]
    cp = parse_ini(os.path.join(root, "platformio.ini"))
    fails += check_hooks(cp)
    mlines, mfails = check_maps(root, build_dir, cp, entries, require)
    return not (fails + mfails), lines + mlines, fails + mfails


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Release gate: no engine-2 object in an engine-1 device image")
    ap.add_argument("--root", default=REPO, help="repo root (default: this file's repo)")
    ap.add_argument("--build-dir", default=None, help="build dir (default: <root>/.pio/build)")
    ap.add_argument("--require-maps", nargs="*", default=[], metavar="ENV", help="envs whose map must exist")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    build_dir = os.path.abspath(args.build_dir) if args.build_dir else os.path.join(root, ".pio", "build")
    ok, lines, fails = check(root, build_dir, args.require_maps)
    for line in lines:
        print(f"{TAG} {line}")
    for line in fails:
        print(f"{TAG} FAIL: {line}")
    if ok:
        print(f"{TAG} PASS: engine-1 device images compile no engine-2 source")
        return 0
    print(f"{TAG} FAIL: see above")
    return 1


if __name__ == "__main__":
    sys.exit(main())
