#!/usr/bin/env python3
"""Release gate: a device image carries no object of the other engine (lane RB-I0a).

The engine-2 sources (lib/core orch_tx_*, lib/harness plain_*, providers/plain_wire_*) are built
for the hosts long before a device runs them. lib/core and lib/harness are not archived on the
device link: every object is loaded, --gc-sections drops its unreferenced code, and the unwind
table (.eh_frame) of an object with exception cleanups stays in the image. RB-B0a's first tip
was 280 B larger for two such objects. tools/engine_filter.py (a PlatformIO pre-script in [s3])
drops every file in tools/engine/engine2_only.txt from an engine-1 device build (and, from
RB-I0, every file in engine1_only.txt from an engine-2 one), so the object never exists. This
gate keeps that true:

  1. lists: every entry is an existing PlatformIO source under lib/ or src/, clean and given
     once, and on at most one list; engine1_only.txt must exist once an env builds engine 2;
     every engine-2 family source (a path component under lib/core/src named orch_tx or
     orch_tx_*, under lib/harness/src named plain or plain_*, at any depth) is on
     engine2_only.txt, or every device image would compile it.
  2. hooks: every device env (resolved platform not native) runs pre:tools/engine_filter.py
     and names engine 1 or 2. platformio.ini is resolved as PlatformIO resolves it: [env]
     defaults, multi-value `extends`, inline `;` comments and ${section.option} references.
  3. maps: no built firmware.map of a device env names an object of the other engine's list
     (for engine 1 also any engine-2 family object), loaded or as an archive member.
     --require-maps names envs whose map must exist (run_gate.py requires esp32s3 and esp32s3-cyd,
     which every battery and release builds; any other built map is checked too).

Usage:
    python3 tools/release_gate/check_engine_filter.py [--root REPO] [--build-dir DIR] [--require-maps ENV ...]
Exit 0 = clean; exit 1 = a list, hook or map failure.
"""

from __future__ import annotations

import argparse
import configparser
import os
import re
import sys
from typing import Dict, Iterable, Iterator, List, Optional, Set, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import engine_filter  # noqa: E402

TAG = "[gate:engine-filter]"
REPO = os.path.dirname(os.path.dirname(HERE))
HOOK = "pre:tools/engine_filter.py"
# PlatformIO's SRC_BUILD_EXT (builder/tools/piobuild.py).
SOURCE_EXTS = ("c", "cc", "cpp", "cxx", "c++", "S", "spp", "SPP", "sx", "s", "asm", "ASM")
# Engine-2 families: (library source root, path-component stem).
FAMILIES = (("lib/core/src", "orch_tx"), ("lib/harness/src", "plain"))
# The list holding each engine's own sources.
LIST_OF = {1: "engine1_only.txt", 2: "engine2_only.txt"}

_EXT = "|".join(re.escape(e) for e in SOURCE_EXTS)
MEMBER = re.compile(r"\.a\(([\w.+-]+\.(?:" + _EXT + r")\.o)\)")
LOADED = re.compile(r"(?:^|[\s(])(\S*?\.(?:" + _EXT + r")\.o)(?=[\s)]|$)")
BUILD_PREFIX = re.compile(r"^.*?\.pio/build/[^/]+/")
LIB_DIR = re.compile(r"^lib[0-9a-f]+/")


class IniError(Exception):
    pass


class PioIni:
    """platformio.ini resolved the way PlatformIO's ProjectConfig does, for the options this gate reads.

    Pure python, so the gate runs where PlatformIO is not installed (CI's gate-logic job);
    test_check_engine_filter.py compares it with ProjectConfig wherever PlatformIO is importable."""

    VAR = re.compile(r"\$\{(?:([^.}()]+)\.)?([^}]+)\}")
    INLINE = re.compile(r"\s+;.*$")

    def __init__(self, path: str):
        self.cp = configparser.ConfigParser(inline_comment_prefixes=("#", ";"), interpolation=None, strict=False)
        with open(path, "r", encoding="utf-8") as fh:
            self.cp.read_file(fh)
        if self.cp.has_option("platformio", "extra_configs"):
            raise IniError("[platformio] extra_configs is not supported by this gate")

    @classmethod
    def multi(cls, value: str) -> List[str]:
        out = []
        for item in value.split("\n" if "\n" in value else ", "):
            item = item.strip()
            if item and not item.startswith((";", "#")):
                out.append(cls.INLINE.sub("", item).strip())
        return out

    def walk(self, section: str) -> Iterator[str]:
        queue = ["env", section] if section.startswith("env:") else [section]
        while queue:
            cur = queue.pop()
            if not self.cp.has_section(cur):
                continue
            yield cur
            if self.cp.has_option(cur, "extends"):
                queue.extend(self.multi(self.cp.get(cur, "extends")))

    def get(self, section: str, option: str, depth: int = 0) -> Optional[str]:
        if depth > 20:
            raise IniError(f"[{section}] {option}: references nest too deep")
        for cur in self.walk(section):
            if self.cp.has_option(cur, option):
                return self._expand(section, option, self.cp.get(cur, option), depth)
        return None

    def _expand(self, section: str, option: str, value: str, depth: int) -> str:
        def sub(m):
            sec, opt = m.group(1), m.group(2)
            if sec is None or sec == "sysenv":
                raise IniError(f"[{section}] {option}: {m.group(0)} cannot be resolved by this gate")
            got = self.get(section if sec == "this" else sec, opt, depth + 1)
            return "" if got is None else got

        return self.VAR.sub(sub, value) if "${" in value else value

    def device_envs(self) -> List[str]:
        envs = [s[len("env:") :] for s in self.cp.sections() if s.startswith("env:")]
        return [e for e in envs if (self.get("env:" + e, "platform") or "").strip() != "native"]

    def engine(self, env: str) -> int:
        return engine_filter.engine_number(self.get("env:" + env, engine_filter.ENGINE_OPTION))

    def scripts(self, env: str) -> List[str]:
        return self.multi(self.get("env:" + env, "extra_scripts") or "")


def engine_or_none(ini: PioIni, env: str) -> Optional[int]:
    try:
        return ini.engine(env)
    except (IniError, engine_filter.EngineFilterError):
        return None


def is_source(rel: str) -> bool:
    return "." in rel and rel.rsplit(".", 1)[1] in SOURCE_EXTS


def in_family(rel: str) -> bool:
    """An engine-2 family source: a component orch_tx/orch_tx_* under lib/core/src, plain/plain_* under lib/harness/src."""
    for base, stem in FAMILIES:
        if rel.startswith(base + "/"):
            parts = rel[len(base) + 1 :].split("/")
            parts[-1] = parts[-1].split(".", 1)[0]
            return any(p == stem or p.startswith(stem + "_") for p in parts)
    return False


def family_sources(root: str) -> Iterator[str]:
    for base, _ in FAMILIES:
        for dirpath, _, files in os.walk(os.path.join(root, base)):
            for name in files:
                rel = os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/")
                if is_source(rel) and in_family(rel):
                    yield rel


def list_path(engine: int) -> str:
    return "/".join(("tools", "engine", LIST_OF[engine]))


def check_entries(root: str, name: str, entries: List[str]) -> List[str]:
    fails, seen = [], set()
    for rel in entries:
        if rel in seen:
            fails.append(f"{name}: {rel} is listed twice")
        seen.add(rel)
        clean = os.path.normpath(rel).replace(os.sep, "/")
        if os.path.isabs(rel) or clean != rel or not rel.startswith(("lib/", "src/")):
            fails.append(f"{name}: {rel} is not a clean repo path under lib/ or src/")
            continue
        if not is_source(rel):
            fails.append(f"{name}: {rel} is not a source PlatformIO compiles")
        if not os.path.isfile(os.path.join(root, rel)):
            fails.append(f"{name}: {rel} does not exist (a stale entry)")
    return fails


def check_lists(root: str, engine1_list_needed: bool) -> Tuple[Dict[int, List[str]], List[str]]:
    """({engine: its list's entries}, failures)."""
    lists: Dict[int, List[str]] = {}
    fails: List[str] = []
    for engine in (2, 1):
        name = list_path(engine)
        path = os.path.join(root, name)
        if not os.path.isfile(path):
            if engine == 2 or engine1_list_needed:
                fails.append(f"{name} is missing")
            continue
        lists[engine] = engine_filter.load_list(path)
        fails += check_entries(root, name, lists[engine])
    for rel in sorted(set(lists.get(1, [])) & set(lists.get(2, []))):
        fails.append(f"{rel} is on both engine lists")
    listed2 = set(lists.get(2, []))
    for rel in sorted(family_sources(root)):
        if rel not in listed2:
            fails.append(f"{rel} is an engine-2 source not in {list_path(2)}: every device image would compile it")
    return lists, fails


def check_hooks(ini: PioIni) -> List[str]:
    fails = []
    envs = ini.device_envs()
    if not envs:
        fails.append("platformio.ini: no device env")
    for env in envs:
        try:
            if HOOK not in ini.scripts(env):
                fails.append(f"platformio.ini env:{env}: extra_scripts lacks {HOOK}, so it compiles both engines")
            ini.engine(env)
        except (IniError, engine_filter.EngineFilterError) as exc:
            fails.append(f"platformio.ini env:{env}: {exc}")
    return fails


def object_rel(path: str) -> Optional[str]:
    """The repo source of a map object: .pio/build/<env>/lib<h>/<L>/<p>.o -> lib/<L>/src/<p>; .../src/<p>.o -> src/<p>."""
    tail = BUILD_PREFIX.sub("", path.replace("\\", "/"), count=1)
    if tail == path or not tail.endswith(".o"):
        return None
    tail = tail[: -len(".o")]
    if tail.startswith("src/"):
        return tail
    if LIB_DIR.match(tail):
        lib, _, rest = LIB_DIR.sub("", tail, count=1).partition("/")
        return f"lib/{lib}/src/{rest}" if rest else None
    return None


def member_in_family(base: str) -> bool:
    src = base[: -len(".o")]
    return any(in_family(f"{root}/{src}") for root, _ in FAMILIES)


def foreign_objects(map_text: str, entries: Iterable[str], families: bool) -> List[str]:
    """Every object of `entries` (and, with `families`, every engine-2 family object) a map names."""
    listed = set(entries)
    members = {os.path.basename(e) + ".o" for e in listed}
    found: Set[str] = set()
    for line in map_text.splitlines():
        if ".o" not in line:
            continue
        for m in MEMBER.finditer(line):
            if m.group(1) in members or (families and member_in_family(m.group(1))):
                found.add(m.group(1))
        for m in LOADED.finditer(line):
            rel = None if ".a(" in m.group(1) else object_rel(m.group(1))
            if rel and (rel in listed or (families and in_family(rel))):
                found.add(rel)
    return sorted(found)


def check_map(path: str, env: str, engine: int, lists: Dict[int, List[str]]) -> Tuple[bool, str]:
    other = 3 - engine
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        hits = foreign_objects(fh.read(), lists.get(other, []), families=engine == 1)
    if not hits:
        return True, f"env:{env}: engine {engine} firmware.map names no engine-{other} object"
    shown = ", ".join(hits[:8]) + (f" and {len(hits) - 8} more" if len(hits) > 8 else "")
    return False, f"env:{env}: engine {engine} firmware.map names {len(hits)} engine-{other} object(s): {shown}"


def check_maps(
    root: str, build_dir: str, ini: PioIni, lists: Dict[int, List[str]], require: List[str]
) -> Tuple[List[str], List[str]]:
    lines, fails = [], []
    devices = ini.device_envs()
    fails += [f"env:{env}: not a device env of platformio.ini (required)" for env in require if env not in devices]
    for env in devices:
        path = os.path.join(build_dir, env, "firmware.map")
        engine = engine_or_none(ini, env)
        if not os.path.isfile(path):
            if env in require:
                fails.append(f"env:{env}: {os.path.relpath(path, root)} is not built (required)")
        elif engine is not None:  # a bad option is check_hooks' failure
            ok, line = check_map(path, env, engine, lists)
            (lines if ok else fails).append(line)
    if not lines and not fails:
        lines.append("no device map built; the map check ran on nothing (build a device env to check it)")
    return lines, fails


def check(root: str, build_dir: str, require: Iterable[str] = ()) -> Tuple[bool, List[str], List[str]]:
    try:
        ini = PioIni(os.path.join(root, "platformio.ini"))
    except (OSError, IniError, configparser.Error) as exc:
        return False, [], [f"platformio.ini: {exc}"]
    hook_fails = check_hooks(ini)
    engine2_envs = any(engine_or_none(ini, e) == 2 for e in ini.device_envs())
    lists, fails = check_lists(root, engine2_envs)
    lines = [f"{list_path(e)}: {len(lists[e])} sources" for e in sorted(lists)]
    mlines, mfails = check_maps(root, build_dir, ini, lists, list(require))
    fails += hook_fails + mfails
    return not fails, lines + mlines, fails


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Release gate: no object of the other engine in a device image")
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
        print(f"{TAG} PASS: every built device image links one engine")
        return 0
    print(f"{TAG} FAIL: see above")
    return 1


if __name__ == "__main__":
    sys.exit(main())
