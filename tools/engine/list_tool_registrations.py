#!/usr/bin/env python3
"""No old-engine tool registers on an engine-2 device build (lane RB-I0).

The device keeps one shared tool registry (memory::registry(), nimbus/orch/tool_registry.h). The
old engine's device code registers its tools there from neutral subsystems (memory::begin,
files::begin, main.cpp, the music player, the connectors). An engine-2 image compiles those
subsystems too, so every such registration must be absent from it. A registration site is:

  - a call of add() on a registry: a receiver named like one (`reg`, `g_reg`, `registry`, `toolReg`,
    any identifier containing "reg") or `registry()`, through `.` or `->`; or any `.add("x.y", ...)`
    whose first argument is a dotted tool-name literal;
  - a call of a registration helper `register...Tool(` / `register...Tools(` (registerCalcTool,
    registerResultTools, files' registerTools, pdf_tool::registerTool, ...); a declaration or a
    definition (`void registerTools() {`) is not a call.

Each site in a device file (src/, include/) must be one of:
  engine1_only  in a file listed in tools/engine/engine1_only.txt (never compiled on engine 2);
  guarded       inside the engine-1 guard  #if !defined(NIMBUS_ENGINE) || NIMBUS_ENGINE == 1
                (its first branch; an #else branch is engine 2);
  engine2       in a file of ENGINE2_REGISTRATION_FILES below: the engine-2 bindings, which
                register engine 2's own tools (the hooks of src/agent/engine_tools.h live there).

The engine-1 tool families reached through the engine_tools.h hooks (registerMemoryFamilies,
registerModelsFamily) are not sites: tools_e1.cpp defines them (engine1_only) and tools_plain.cpp
defines them for engine 2.

Usage (from the repo root):
    python3 tools/engine/list_tool_registrations.py           # every site with its status
    python3 tools/engine/list_tool_registrations.py --check   # exit 1 on an unguarded site
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from typing import List, Optional, Sequence, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import list_engine1_includes as lister  # noqa: E402  (the guard-aware scanner, RB-SEAM2)

REPO = os.path.dirname(os.path.dirname(HERE))
TAG = "[tool-registrations]"

# The engine-2 binding files that may register tools on an engine-2 build. An engine-2 lane that
# binds a tool adds its *_device.cpp here in the same commit.
ENGINE2_REGISTRATION_FILES: Tuple[str, ...] = (
    "src/agent/tools_plain.cpp",
    "src/agent/notes_device.cpp",
    "src/agent/file_tools_device.cpp",
    "src/agent/schedule_device.cpp",
    "src/agent/history_tools_device.cpp",
    "src/agent/tools_plain_send.h",
    "src/agent/tools_plain_send.cpp",
    "src/agent/tools_plain_files.cpp",
    "src/agent/tools_plain_skills.cpp",  # lane E2-P-SKILL: skill_list, skill_save, skill_delete
    "src/agent/tools_plain_device.cpp",  # E2-P-TOOLS
    "src/agent/tools_plain_device.h",
    "src/agent/orchestrator_plain.cpp",  # lane E2-P-SUB: registerAgentTools (agent_start, agent_list, agent_stop)
)

# A registry receiver: any identifier with "reg" in it (reg, g_reg, registry, toolReg, ...) or a
# registry() call; the fallback below also catches any receiver given a dotted tool-name literal.
RECEIVER = re.compile(r"(?:\b\w*[Rr]eg\w*|\bregistry\(\s*\))\s*$")
ANY_ADD = re.compile(r"(?:\.|->)\s*add\s*\(")
TOOL_NAME_LITERAL = re.compile(r'\s*"[a-z][a-z0-9_]*\.[a-z0-9_.]+"')
HELPER_CALL = re.compile(r"\b(register\w*Tools?)\s*\(")
DECL_PREFIX = re.compile(r"\bvoid\s*(?:[\w:]+::)?\s*$")


def guarded_flags(stripped: str) -> List[bool]:
    """For each line (1-based index), True when it sits in the engine-1 guard's first branch."""
    flags = [False]
    stack: List[bool] = []
    for line in stripped.split("\n"):
        m = lister.DIRECTIVE.match(line)
        if m:
            word, rest = m.group(1), m.group(2)
            if word in ("if", "ifdef", "ifndef"):
                stack.append(word == "if" and lister.is_guard(rest))
            elif word in ("elif", "else") and stack:
                stack[-1] = False
            elif word == "endif" and stack:
                stack.pop()
        flags.append(any(stack))
    return flags


def sites(text: str) -> List[Tuple[int, str, bool]]:
    """(line, what, guarded) for every registration site in a source text."""
    stripped = lister.strip_comments(text)
    flags = guarded_flags(stripped)
    found = {}
    for m in ANY_ADD.finditer(stripped):
        named = RECEIVER.search(stripped[max(0, m.start() - 40) : m.start()])
        literal = TOOL_NAME_LITERAL.match(text, m.end())
        if named or literal:
            line = stripped.count("\n", 0, m.start()) + 1
            found.setdefault((line, "add()"), flags[line])
    for m in HELPER_CALL.finditer(stripped):
        start_of_line = stripped.rfind("\n", 0, m.start()) + 1
        if DECL_PREFIX.search(stripped[start_of_line : m.start()]):
            continue  # a declaration or definition, not a call
        line = stripped.count("\n", 0, m.start()) + 1
        found.setdefault((line, m.group(1) + "()"), flags[line])
    return sorted((line, what, g) for (line, what), g in found.items())


def classify(root: str, engine1_only: set) -> List[Tuple[str, int, str, str]]:
    """(file, line, what, status) for every registration site under src/ and include/."""
    out = []
    for rel in lister.device_files(root):
        with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for line, what, guarded in sites(text):
            if rel in engine1_only:
                status = "engine1_only"
            elif rel in ENGINE2_REGISTRATION_FILES:
                status = "engine2"
            elif guarded:
                status = "guarded"
            else:
                status = "UNGUARDED"
            out.append((rel, line, what, status))
    return out


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="tool registrations on an engine-2 device build")
    ap.add_argument("--root", default=REPO)
    ap.add_argument("--check", action="store_true", help="exit 1 on a registration an engine-2 build would run")
    args = ap.parse_args(argv)
    engine1_only = lister.read_engine1_only(args.root)
    if args.check and not engine1_only:
        print(f"{TAG} FAIL: {lister.ENGINE1_ONLY_LIST} is missing or empty")
        return 1
    rows = classify(args.root, engine1_only)
    bad = [r for r in rows if r[3] == "UNGUARDED"]
    for rel, line, what, status in rows:
        if not args.check or status == "UNGUARDED":
            print(f"{TAG} {rel}:{line}: {what} {status}")
    if args.check:
        if bad:
            print(
                f"{TAG} RED: {len(bad)} registration(s) would run on an engine-2 build: put each inside "
                "the engine-1 guard, or register an engine-2 tool from a file in ENGINE2_REGISTRATION_FILES"
            )
            return 1
        print(f"{TAG} GREEN: {len(rows)} registration sites, none runs on an engine-2 build")
    return 0


if __name__ == "__main__":
    sys.exit(main())
