"""Source guard: the test-build crash switch never reaches a shipped image.

The test console's ``PANIC <kind> arm=turn [round=N]`` arms a panic inside the next turn
(src/sys/crash_report.cpp, lib/core nimbus/sys/panic_arm.h). Every use of it under src/
must sit inside an ``#ifdef NIMBUS_TEST`` (or ``#if defined(NIMBUS_TEST)``) block, so a
production build cannot even compile a call. The release gate's ELF check
(check_elf_symbols.py) is the second line: no shipped image may link the symbols.

Run: python3 -m pytest tools/release_gate
"""

from __future__ import annotations

import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SWITCH = re.compile(r"\b(armTurnPanic|testPanicPoint|dropUnfiredArm|parsePanicArm|panicArmFires|PanicArm)\b")
TEST_IF = re.compile(r"^\s*#\s*(ifdef\s+NIMBUS_TEST\b|if\s+defined\s*\(\s*NIMBUS_TEST\s*\))")


def unguarded_uses(text: str) -> "list[int]":
    """Line numbers that mention the switch outside a NIMBUS_TEST block."""
    stack: "list[bool]" = []  # per open #if: is this branch a NIMBUS_TEST branch?
    bad = []
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if s.startswith("#"):
            directive = s[1:].strip()
            if TEST_IF.match(line):
                stack.append(True)
                continue
            if directive.startswith(("if", "ifdef", "ifndef")):
                stack.append(False)
                continue
            if directive.startswith(("else", "elif")) and stack:
                stack[-1] = False  # the other branch of a NIMBUS_TEST test is production
                continue
            if directive.startswith("endif") and stack:
                stack.pop()
                continue
        if SWITCH.search(line) and not any(stack) and not s.startswith("//"):
            bad.append(n)
    return bad


def test_guard_logic():
    good = "#ifdef NIMBUS_TEST\nnimbus::crash::testPanicPoint();\n#endif\n"
    assert unguarded_uses(good) == []
    nested = "#if defined(NIMBUS_TEST)\n#if 1\narmTurnPanic(x);\n#endif\n#endif\n"
    assert unguarded_uses(nested) == []
    bare = "void f() { testPanicPoint(); }\n"
    assert unguarded_uses(bare) == [1]
    else_branch = "#ifdef NIMBUS_TEST\n#else\ntestPanicPoint();\n#endif\n"
    assert unguarded_uses(else_branch) == [3]
    other = "#ifdef SOMETHING\ntestPanicPoint();\n#endif\n"
    assert unguarded_uses(other) == [2]


def test_every_use_under_src_is_test_only():
    offenders = []
    seen = 0
    for root, _, files in os.walk(os.path.join(REPO, "src")):
        for name in files:
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(root, name)
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
            if SWITCH.search(text):
                seen += 1
            offenders += [f"{os.path.relpath(path, REPO)}:{n}" for n in unguarded_uses(text)]
    assert seen >= 2, "the guard found no uses at all: the switch moved, update the guard"
    assert not offenders, f"test-build crash switch used outside #ifdef NIMBUS_TEST: {offenders}"


def test_console_hook_is_bound_inside_the_test_block():
    """The console's ``PANIC <kind> arm=turn`` reaches the arm only through main.cpp's
    ``h.panicArmTurn`` binding; without it the verb answers "not wired" and the armed L39
    legs skip. Pin the binding (the guard above already keeps it test-only)."""
    with open(os.path.join(REPO, "src", "main.cpp"), encoding="utf-8") as f:
        main = f.read()
    hits = len(re.findall(r"\bh\.panicArmTurn\s*=[^\n]*\barmTurnPanic\b", main))
    assert hits == 1, f"main.cpp binds h.panicArmTurn to armTurnPanic {hits} times, want once"
    assert unguarded_uses(main) == [], "the binding sits outside #ifdef NIMBUS_TEST"
