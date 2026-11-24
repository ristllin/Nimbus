#!/usr/bin/env python3
"""List every device file that includes an old-engine (engine-1) header (lane RB-SEAM2).

An engine-2 device image (RB-I0) does not compile the old engine's library sources, so a
device file that still includes one of their headers either does not compile there or
references a symbol that is not linked. Every such file is one of:

  guard         the include and every use of what it declares sit inside the engine-1 guard
                    #if !defined(NIMBUS_ENGINE) || NIMBUS_ENGINE == 1
                and the file compiles without them under -DNIMBUS_ENGINE=2;
  engine1_only  the whole file is engine 1 (src/agent/orchestrator_e1.cpp, tools_e1.cpp, ...);
                RB-I0 lists it in tools/engine/engine1_only.txt and an engine-2 env drops it;
  neutral       the file included an old-engine header at RB-SEAM2's base and now reaches what it
                needed through src/agent/engine_api.h instead (each engine seam defines the call),
                so it includes none, guarded or not.

tools/engine/guard_files.txt records the decision per file ('<path>  # <decision>: why'). A device
header listed there as engine1_only declares what only an engine-1 file defines (src/agent/skills.h
for skills.cpp), so it counts as an old-engine header for its includers too; an engine1_only
source's own header (foo.cpp, foo.h) must be engine1_only as well.

A device file is a source or header under src/ or include/. It "includes" an old-engine header
when one of its #include lines that is NOT inside the engine-1 guard names the header, or names a
device header that (transitively, by the same rule) does. A device header that guards its own
include therefore stops the chain for its includers.

Usage (from the repo root):
    python3 tools/engine/list_engine1_includes.py             # the list: '<file>: <header> [via <h>], ...'
    python3 tools/engine/list_engine1_includes.py --all       # also the guarded includes
    python3 tools/engine/list_engine1_includes.py --check     # every listed file is engine1_only in
                                                              # guard_files.txt or engine1_only.txt
Exit 0: the list (or the check) is clean or printed; 1: --check found an unguarded includer, a
guard_files.txt line is malformed or names a missing file.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from typing import Dict, Iterable, List, Optional, Set, Tuple

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TAG = "[engine1-includes]"

# The old engine's headers: the turn engine and its prompt machinery, the built-in skills, the
# dream turn, and the two engine-1 tool headers that declare nothing else (PRD sections 4 and 10).
# A header engine 2 itself includes (jobs.h, providers.h, turn_memory.h, tool_registry.h,
# compact.h, ...) is shared, not listed. So is a tool header that also declares what the device
# keeps under engine 2: memory_tools.h (embedding failure words, recall, the embed drain),
# prefs_tools.h (prefsCommand, prefsJson for the web page), openq_tools.h (the context the prefs
# subsystem builds); only their register*Tools calls are engine 1, and those sit in
# src/agent/tools_e1.cpp. Each line: path, why.
ENGINE1_HEADERS: Tuple[Tuple[str, str], ...] = (
    ("lib/harness/include/nimbus/harness/apply.h", "the old turn's action applier"),
    ("lib/harness/include/nimbus/harness/board_identity.h", "built-in skills' board text"),
    ("lib/harness/include/nimbus/harness/builtin_skills.h", "built-in skill bodies"),
    ("lib/harness/include/nimbus/harness/compose.h", "the old prompt composer"),
    ("lib/harness/include/nimbus/harness/conv_cache.h", "the old engine's conversation cache"),
    ("lib/harness/include/nimbus/harness/conversation.h", "the old conversation store"),
    ("lib/harness/include/nimbus/harness/dream.h", "the dream turn"),
    ("lib/harness/include/nimbus/harness/engine.h", "the old turn engine"),
    ("lib/harness/include/nimbus/harness/fold_driver.h", "the old engine's fold driver"),
    ("lib/harness/include/nimbus/harness/fold_ladder.h", "the old engine's fold ladder"),
    ("lib/harness/include/nimbus/harness/prompt_layout.h", "the old prompt layout"),
    ("lib/harness/include/nimbus/harness/router_refusal.h", "the old engine's router refusal turn"),
    ("lib/harness/include/nimbus/harness/skill_attach.h", "built-in skill attach"),
    ("lib/harness/include/nimbus/harness/turn_contract.h", "the old turn contract"),
    ("lib/harness/include/nimbus/harness/turn_fit.h", "the old request fitter"),
    ("lib/harness/include/nimbus/harness/turn_inbox.h", "the old engine's turn inbox"),
    ("lib/harness/include/nimbus/harness/turn_progress.h", "the old engine's turn progress"),
    ("lib/core/include/nimbus/orch/file_aids_e1.h", "files.read and files.edit of engine 1"),
    ("lib/core/include/nimbus/orch/session_tools.h", "session.* (the agents group in engine 2)"),
)

# Where an #include "..." or <...> is looked up after the including file's own folder.
INCLUDE_ROOTS = ("src", "include", "lib/harness/include", "lib/core/include", "lib/harness/src")
DEVICE_DIRS = ("src", "include")
LIBRARY_INCLUDE_DIRS = ("lib/harness/include", "lib/core/include")
SOURCE_EXTS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp")
GUARD_LIST = "tools/engine/guard_files.txt"
ENGINE1_ONLY_LIST = "tools/engine/engine1_only.txt"
DECISIONS = ("guard", "engine1_only", "neutral")

GUARD_EXPR = "!defined(NIMBUS_ENGINE)||NIMBUS_ENGINE==1"
DIRECTIVE = re.compile(r"^\s*#\s*(\w+)\s*(.*?)\s*$")
MACRO_INCLUDE = "<macro> "  # an #include whose name a macro supplies: it cannot be followed
INCLUDE_ARG = re.compile(r'^[<"]([^>"]+)[>"]')
RAW_OPEN = re.compile(r'R"([^()\\\s]{0,16})\(')
INCLUDE_LINE = re.compile(r"^\s*#\s*include\s*$")


def engine1_headers() -> Set[str]:
    return {path for path, _why in ENGINE1_HEADERS}


def strip_comments(text: str) -> str:
    """Comments and the contents of string, character and raw-string literals become spaces;
    every newline stays, so line numbers do not move. A '/*' or '//' inside a literal ("image/*",
    "http://") is not a comment, and a '#include' inside a raw string is not a directive."""
    out: List[str] = []
    i, n = 0, len(text)
    while i < n:
        j, keep = token_end(text, i, out)
        if j is None:
            out.append(text[i])
            i += 1
            continue
        out.append(text[i:j] if keep else blank(text[i:j]))
        i = j
    return "".join(out)


def token_end(text: str, i: int, out: List[str]) -> Tuple[Optional[int], bool]:
    """(end, keep) of a comment or literal starting at i; (None, True) for plain code."""
    two = text[i : i + 2]
    if two == "//":
        j = text.find("\n", i)
        return (len(text) if j < 0 else j), False
    if two == "/*":
        j = text.find("*/", i + 2)
        return (len(text) if j < 0 else j + 2), False
    if is_raw_open(text, i):
        return raw_end(text, i), False
    c = text[i]
    if c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
        j = quoted_end(text, i)
        # An include's own "file.h" is kept: the directive is read from this output.
        chunk = text[i:j]
        return j, chunk.endswith(c) and "\n" not in chunk and is_include_tail(out)
    return None, True


def is_raw_open(text: str, i: int) -> bool:
    if text[i : i + 2] != 'R"':
        return False
    return i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")


def raw_end(text: str, i: int) -> int:
    m = RAW_OPEN.match(text, i)
    if not m:
        return len(text)
    k = text.find(")" + m.group(1) + '"', m.end())
    return len(text) if k < 0 else k + len(m.group(1)) + 2


def quoted_end(text: str, i: int) -> int:
    """One past the closing quote of the literal opening at i (or the end of its line)."""
    c, j, n = text[i], i + 1, len(text)
    while j < n and text[j] != c and text[j] != "\n":
        j += 2 if text[j] == "\\" else 1
    return min(j + 1, n)


def blank(chunk: str) -> str:
    return "".join("\n" if ch == "\n" else " " for ch in chunk)


def is_include_tail(out: List[str]) -> bool:
    """True when the text since the last newline is '#include' (the quoted name follows)."""
    tail: List[str] = []
    for piece in reversed(out):
        k = piece.rfind("\n")
        if k >= 0:
            tail.append(piece[k + 1 :])
            break
        tail.append(piece)
    return bool(INCLUDE_LINE.match("".join(reversed(tail))))


def is_guard(expr: str) -> bool:
    return re.sub(r"\s+", "", expr) == GUARD_EXPR


def scan_includes(text: str) -> List[Tuple[str, bool, int]]:
    """(included name, inside the engine-1 guard, line number) for every #include line.

    Tracks #if/#ifdef/#ifndef ... #elif/#else ... #endif. An include is guarded when an enclosing
    conditional is the engine-1 guard and the include is in its first (true) branch."""
    out: List[Tuple[str, bool, int]] = []
    # One entry per open conditional: True while inside the guard's own branch.
    stack: List[bool] = []
    for num, line in enumerate(strip_comments(text).split("\n"), 1):
        m = DIRECTIVE.match(line)
        if not m:
            continue
        word, rest = m.group(1), m.group(2)
        if word in ("if", "ifdef", "ifndef"):
            stack.append(word == "if" and is_guard(rest))
        elif word in ("elif", "else"):
            if stack:
                stack[-1] = False
        elif word == "endif":
            if stack:
                stack.pop()
        elif word == "include":
            inc = INCLUDE_ARG.match(rest)
            out.append((inc.group(1) if inc else MACRO_INCLUDE + rest, any(stack), num))
    return out


def device_files(root: str) -> List[str]:
    out = []
    for top in DEVICE_DIRS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, top)):
            dirnames.sort()
            for name in sorted(filenames):
                if name.endswith(SOURCE_EXTS):
                    out.append(os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/"))
    return out


def library_headers(root: str) -> List[str]:
    out = []
    for top in LIBRARY_INCLUDE_DIRS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, top)):
            dirnames.sort()
            for name in sorted(filenames):
                if name.endswith((".h", ".hh", ".hpp")):
                    out.append(os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/"))
    return out


def resolve(root: str, includer: str, name: str) -> Optional[str]:
    for base in (os.path.dirname(includer),) + INCLUDE_ROOTS:
        cand = os.path.normpath(os.path.join(base, name)).replace(os.sep, "/")
        if os.path.isfile(os.path.join(root, cand)):
            return cand
    return None


class Scan:
    """Each device file's include lines, resolved to repo paths, and the library headers' own, so
    a chain through a shared library header is followed too (only device files are listed).
    An unguarded include of a device file that cannot be followed (a macro name, or a nimbus/
    header that does not resolve) is kept in `unfollowed`: the check fails on it."""

    def __init__(self, root: str, headers: Iterable[str]):
        self.root = root
        self.headers = set(headers)
        self.files = device_files(root)
        self.includes: Dict[str, List[Tuple[str, bool]]] = {}
        self.unfollowed: Dict[str, List[str]] = {}
        for rel in self.files + library_headers(root):
            with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            rows = []
            for name, guarded, num in scan_includes(text):
                path = None if name.startswith(MACRO_INCLUDE) else resolve(root, rel, name)
                if path:
                    rows.append((path, guarded))
                elif not guarded and (name.startswith((MACRO_INCLUDE, "nimbus/"))) and rel in self.files:
                    self.unfollowed.setdefault(rel, []).append(f"line {num}: {name}")
            self.includes[rel] = rows

    def reached(self, rel: str, guarded_too: bool = False) -> Dict[str, str]:
        """old-engine header -> how: '' (direct), 'guarded', or the device header it comes through.

        A breadth-first walk over the unguarded includes of device headers (guarded_too widens only
        the file's own first step), so an include cycle ends and the nearest path is named."""
        out: Dict[str, str] = {}
        seen = {rel}
        queue: List[Tuple[str, str]] = []
        for path, guarded in self.includes.get(rel, []):
            if guarded and not guarded_too:
                continue
            if path in self.headers:
                out.setdefault(path, "guarded" if guarded else "")
            elif path in self.includes and path not in seen:
                seen.add(path)
                queue.append((path, path))
        while queue:
            cur, via = queue.pop(0)
            for path, guarded in self.includes.get(cur, []):
                if guarded:
                    continue
                if path in self.headers:
                    out.setdefault(path, via)
                elif path in self.includes and path not in seen:
                    seen.add(path)
                    queue.append((path, via))
        return out

    def listing(self, guarded_too: bool = False) -> Dict[str, Dict[str, str]]:
        rows = {}
        for rel in self.files:
            hit = self.reached(rel, guarded_too)
            if hit:
                rows[rel] = hit
        return rows


def read_decisions(root: str) -> Tuple[Dict[str, str], List[str]]:
    """guard_files.txt: path -> decision; plus the problems found in it."""
    path = os.path.join(root, GUARD_LIST)
    decisions: Dict[str, str] = {}
    problems: List[str] = []
    if not os.path.isfile(path):
        return decisions, problems
    with open(path, encoding="utf-8") as fh:
        for num, raw in enumerate(fh, 1):
            body, _, comment = raw.partition("#")
            rel = body.strip()
            if not rel:
                continue
            word = comment.strip().split(":", 1)[0].strip()
            if word not in DECISIONS:
                problems.append(f"{GUARD_LIST}:{num}: {rel}: the comment must start with one of {', '.join(DECISIONS)}")
            elif not os.path.isfile(os.path.join(root, rel)):
                problems.append(f"{GUARD_LIST}:{num}: {rel} does not exist")
            elif rel in decisions:
                problems.append(f"{GUARD_LIST}:{num}: {rel} is listed twice")
            else:
                decisions[rel] = word
    return decisions, problems


def read_engine1_only(root: str) -> Set[str]:
    path = os.path.join(root, ENGINE1_ONLY_LIST)
    if not os.path.isfile(path):
        return set()
    with open(path, encoding="utf-8") as fh:
        return {ln.split("#", 1)[0].strip() for ln in fh if ln.split("#", 1)[0].strip()}


def describe(hit: Dict[str, str]) -> str:
    parts = []
    for hdr in sorted(hit):
        how = hit[hdr]
        short = hdr.split("/include/", 1)[-1]
        parts.append(short if not how else f"{short} ({how})" if how == "guarded" else f"{short} via {how}")
    return ", ".join(parts)


def device_engine1_headers(decisions: Dict[str, str]) -> Set[str]:
    return {p for p, d in decisions.items() if d == "engine1_only" and p.endswith((".h", ".hh", ".hpp"))}


def check(root: str, scan: Scan, decisions: Dict[str, str], problems: List[str]) -> List[str]:
    problems = list(problems)
    e1_only = read_engine1_only(root) | {p for p, d in decisions.items() if d == "engine1_only"}
    for rel in sorted(e1_only):
        stem, ext = os.path.splitext(rel)
        if (
            ext in (".cpp", ".cc", ".c")
            and os.path.isfile(os.path.join(root, stem + ".h"))
            and stem + ".h" not in e1_only
        ):
            problems.append(f"{rel} is engine1_only but its header {stem}.h is not")
    for rel, names in sorted(scan.unfollowed.items()):
        problems.append(f"{rel}: cannot follow {', '.join(names)}")
    for rel, hit in sorted(scan.listing().items()):
        if rel in e1_only:
            continue
        problems.append(f"{rel}: includes {describe(hit)} outside the engine-1 guard and is not engine1_only")
    guarded = scan.listing(guarded_too=True)
    for rel, decision in sorted(decisions.items()):
        if decision == "guard" and rel not in guarded:
            problems.append(f"{rel}: listed as guard but includes no old-engine header, guarded or not")
        if decision == "neutral" and rel in guarded:
            problems.append(f"{rel}: listed as neutral but includes {describe(guarded[rel])}")
    return problems


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", default=REPO)
    ap.add_argument("--all", action="store_true", help="also list includes inside the engine-1 guard")
    ap.add_argument("--check", action="store_true", help="fail on an includer that is neither guarded nor engine1_only")
    args = ap.parse_args(argv)
    for hdr in sorted(engine1_headers()):
        if not os.path.isfile(os.path.join(args.root, hdr)):
            print(f"{TAG} RED: {hdr} (ENGINE1_HEADERS) does not exist")
            return 1
    decisions, problems = read_decisions(args.root)
    scan = Scan(args.root, engine1_headers() | device_engine1_headers(decisions))
    if args.check:
        problems = check(args.root, scan, decisions, problems)
        for line in problems:
            print(f"{TAG} {line}")
        print(f"{TAG} {'RED' if problems else 'GREEN'}: {len(problems)} problem(s)")
        return 1 if problems else 0
    for rel, hit in sorted(scan.listing(guarded_too=args.all).items()):
        print(f"{rel}: {describe(hit)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
