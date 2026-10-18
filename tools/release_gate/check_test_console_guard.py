#!/usr/bin/env python3
"""Source guard: the serial test console is NIMBUS_TEST-only.

``src/test_console.cpp`` carries commands that must never exist in a shipped
image - they fire panics, fill NVS, inject faults, feed raw Telegram pages and
drive the voice path. The module is invisible to production by construction:

* ``src/test_console.cpp``: everything but its includes sits inside
  ``#if defined(NIMBUS_TEST)``. Checked STRUCTURALLY: outside the guarded region
  only preprocessor lines, comments and blank lines may appear, so a verb added
  anywhere outside the guard fails here whether or not anyone listed it.
* ``src/test_console.h``: every public symbol has a no-op ``#else`` inline, so the
  ``tc::`` calls in ``main.cpp`` compile to nothing in production.
* ``src/main.cpp``: the test-only helpers it defines for the console (today the
  AUDIOINJECT injector) and the ``tc::Hooks`` wiring sit inside a NIMBUS_TEST
  region.

Pure source check (no compiler, no ELF, no board), so it runs in CI. The bench
additionally scans the built production ELF for code-only markers (see the PR body).

  check_test_console_guard.py           # exit 1 blocks the release
  python3 -m pytest tools/release_gate  # tests this gate's own logic
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
CPP = REPO / "src/test_console.cpp"
HDR = REPO / "src/test_console.h"
MAIN = REPO / "src/main.cpp"

# main.cpp symbols that exist only for the test console and must be guarded.
MAIN_TEST_ONLY = ("static size_t copyClipPcm(", "static String audioInjectTurn(", "h.audioInject =", "tc::Hooks h;")
# Public tc:: functions whose production side must be a no-op inline.
HEADER_STUBS = ("inline void pumpOrch() {}", "inline bool ttsMuted() { return false; }")

_OPEN = re.compile(r"^\s*#\s*(if\s+defined\s*\(\s*NIMBUS_TEST\s*\)|ifdef\s+NIMBUS_TEST\b)")
_COND = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b")
_ENDIF = re.compile(r"^\s*#\s*endif\b")


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments (string literals are left alone; a comment
    marker inside a string only makes this check stricter, never laxer)."""
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def test_regions(text: str) -> "list[tuple[int, int]]":
    """Line spans [first, last] (1-based, inclusive of the #if/#endif lines) of every
    top-level NIMBUS_TEST region, with nested conditionals tracked."""
    regions, depth, start = [], 0, None
    for n, line in enumerate(text.splitlines(), 1):
        if start is None:
            if _OPEN.match(line):
                start, depth = n, 1
            continue
        if _COND.match(line):
            depth += 1
        elif _ENDIF.match(line):
            depth -= 1
            if depth == 0:
                regions.append((start, n))
                start = None
    return regions


def _inside(line_no: int, regions) -> bool:
    return any(a <= line_no <= b for a, b in regions)


def judge_console_cpp(text: str) -> "list[str]":
    regions = test_regions(text)
    if not regions:
        return ["src/test_console.cpp has no NIMBUS_TEST region"]
    problems = []
    for n, line in enumerate(strip_comments(text).splitlines(), 1):
        s = line.strip()
        if not s or s.startswith("#") or _inside(n, regions):
            continue
        problems.append(f"src/test_console.cpp:{n} has code outside the NIMBUS_TEST guard: {s[:60]!r}")
    return problems


def judge_header(text: str) -> "list[str]":
    if "#else" not in text:
        return ["src/test_console.h has no '#else' production side"]
    return [f"src/test_console.h lacks the production no-op stub {stub!r}" for stub in HEADER_STUBS if stub not in text]


def judge_main(text: str) -> "list[str]":
    regions = test_regions(text)
    problems = []
    lines = text.splitlines()
    for needle in MAIN_TEST_ONLY:
        hits = [n for n, line in enumerate(lines, 1) if needle in line]
        if not hits:
            problems.append(f"src/main.cpp: expected test-only {needle!r} not found (update MAIN_TEST_ONLY)")
        problems += [
            f"src/main.cpp:{n} {needle!r} is OUTSIDE a NIMBUS_TEST region" for n in hits if not _inside(n, regions)
        ]
    return problems


def judge(cpp_text: str, hdr_text: str, main_text: str) -> "tuple[bool, list[str]]":
    problems = judge_console_cpp(cpp_text) + judge_header(hdr_text) + judge_main(main_text)
    return (not problems), problems


def main(argv=None) -> int:
    ok, problems = judge(CPP.read_text(), HDR.read_text(), MAIN.read_text())
    for p in problems:
        print(f"FAIL {p}")
    if ok:
        print("test-console guard: the console and its main.cpp helpers are NIMBUS_TEST-only")
        return 0
    print("test-console guard: BLOCKED - test-only code could ship in production")
    return 1


if __name__ == "__main__":
    sys.exit(main())
