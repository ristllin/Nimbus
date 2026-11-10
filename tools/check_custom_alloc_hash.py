#!/usr/bin/env python3
"""Custom-allocator-string hasher lint: no unordered container keyed by a
basic_string with a non-default allocator may rely on std::hash.

Why this exists (lane LINUXFIX, release run 42832344608 job nimbusd-image):
libstdc++ (g++ 12, the debian:12-slim nimbusd image) provides std::hash only
for basic_string with the DEFAULT allocator; libc++ (macOS clang, the battery
and the device build) provides it for ANY allocator. So
`std::unordered_map<WorkingString, V, std::hash<WorkingString>, ...>` compiles
on every Mac and CI-native job and fails to compile in the hosted image.

The class rule, enforced here over every C++ source in the tree:
  1. No `std::hash<X>` where X is a basic_string with a custom allocator
     (WorkingString or any basic_string<..., ..., Alloc>).
  2. No unordered_map/unordered_set whose KEY is such a string and whose
     template arguments omit an explicit hasher (the default is std::hash<Key>).

A hasher that hashes the string CONTENT through std::string_view (provided by
every standard library) is the accepted fix - see WorkingStringHash in
lib/core/include/nimbus/orch/psram_alloc.h.

Usage:
  python3 tools/check_custom_alloc_hash.py            # lint the tree (exit 1 on findings)
  python3 tools/check_custom_alloc_hash.py --list     # every container found, with its verdict
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SCAN_ROOTS = ["lib", "src", "include", "nimbusd", "test", "tools"]
SCAN_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".cxx"}

# A key type that is a basic_string with a custom allocator: WorkingString, or
# basic_string<char, traits, SomeAllocator> (three template args).
CUSTOM_ALLOC_STRING = re.compile(r"(?:\bWorkingString\b|\bbasic_string\s*<[^;{}()]*?,\s*[^;{}()]*?,\s*[^;{}()]*?>)")
HASH_TMPL = re.compile(r"\bstd::hash\s*<")
CONTAINER_TMPL = re.compile(r"\bstd::unordered_(?:map|set)\s*<")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def angle_args(text: str, start: int) -> str | None:
    """Return the balanced <> argument list starting at the '<' at `start`."""
    assert text[start] == "<"
    depth = 0
    for i in range(start, len(text)):
        c = text[i]
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
            if depth == 0:
                return text[start + 1 : i]
    return None


def split_top_level(args: str) -> list[str]:
    parts, depth, cur = [], 0, []
    for c in args:
        if c in "<([":
            depth += 1
        elif c in ">)]":
            depth -= 1
        if c == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
    if cur:
        parts.append("".join(cur))
    return [p.strip() for p in parts]


def is_custom_alloc_key(key: str) -> bool:
    return bool(CUSTOM_ALLOC_STRING.search(key))


def scan_file(path: Path) -> list[str]:
    """Return offence strings for one file."""
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    text = strip_comments(raw)
    offences: list[str] = []

    def line_of(pos: int) -> int:
        return text.count("\n", 0, pos) + 1

    # Rule 1: std::hash<custom-allocator string> used as a hasher. A mention
    # inside a static_assert is the rule's own guard test, not a use - skip it.
    for m in HASH_TMPL.finditer(text):
        args = angle_args(text, m.end() - 1)
        if args is None:
            continue
        first = split_top_level(args)[0]
        if not is_custom_alloc_key(first):
            continue
        line_start = text.rfind("\n", 0, m.start()) + 1
        line_end = text.find("\n", m.start())
        line = text[line_start : line_end if line_end != -1 else len(text)]
        if "static_assert" in line:
            continue
        offences.append(
            f"{path}:{line_of(m.start())}: std::hash<{first}> - libstdc++ has no "
            f"std::hash for a basic_string with a custom allocator; name an "
            f"explicit content hasher (WorkingStringHash)"
        )

    # Rule 2: unordered container keyed by a custom-allocator string with no
    # explicit hasher argument (map needs >= 3 args, set needs >= 2).
    for m in CONTAINER_TMPL.finditer(text):
        args = angle_args(text, m.end() - 1)
        if args is None:
            continue
        parts = split_top_level(args)
        kind = "map" if "map" in m.group(0) else "set"
        min_args = 3 if kind == "map" else 2
        if parts and is_custom_alloc_key(parts[0]) and len(parts) < min_args:
            offences.append(
                f"{path}:{line_of(m.start())}: std::unordered_{kind}<{parts[0]}, ...> "
                f"omits the hasher argument, so it defaults to std::hash<Key>, which "
                f"libstdc++ cannot invoke for a custom-allocator string"
            )
    return offences


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", action="store_true", help="print every container found")
    args = ap.parse_args()

    files: list[Path] = []
    for root in SCAN_ROOTS:
        base = REPO / root
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if p.is_file() and p.suffix in SCAN_SUFFIXES:
                files.append(p)

    all_offences: list[str] = []
    for f in files:
        all_offences.extend(scan_file(f))

    if args.list:
        for f in files:
            raw = strip_comments(f.read_text(encoding="utf-8", errors="replace"))
            for m in CONTAINER_TMPL.finditer(raw):
                args2 = angle_args(raw, m.end() - 1)
                if args2:
                    print(f"{f}: {m.group(0)}{args2[:80]}")

    if all_offences:
        for o in all_offences:
            print(o, file=sys.stderr)
        print(
            f"\n{len(all_offences)} custom-allocator-string hasher offence(s). "
            "libstdc++ (g++ 12, the nimbusd image) cannot compile these; see "
            "tools/check_custom_alloc_hash.py",
            file=sys.stderr,
        )
        return 1
    print(f"custom-allocator-string hasher lint clean ({len(files)} files scanned)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
