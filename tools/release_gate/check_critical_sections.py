#!/usr/bin/env python3
"""Release gate: nothing that logs, touches a file or blocks runs inside a spinlock.

A portMUX critical section runs with the CPU's interrupt level raised. Newlib's recursive
FILE lock aborts at a raised level, so one durable log line under a spinlock restarts the
device. The 29 Oct 2026 crash (fingerprint 2419bf4a) aborted in that lock. Its frames, solved
against a build that does not match by call-graph alignment, point at orchDeviceSink's "lights"
line inside net::ConfigLockGuard. That reading awaits the matching ELF and a board check, but the
shape is fatal whichever line was the trigger.

The scan reads src/ and lib/ for the three critical-section shapes:
    portENTER_CRITICAL(&m) ... portEXIT_CRITICAL(&m)    (any _ISR or _SAFE spelling, same mux)
    lockConfig() ... unlockConfig()
    net::ConfigLockGuard lk;   to the end of its scope
and fails on a forbidden call written inside one. It reads the region's own text: it does not
follow calls, so a helper that logs is a reviewer's catch, not this gate's.

Usage:
    python3 tools/release_gate/check_critical_sections.py [paths...]
Exit 0 = clean; exit 1 = a forbidden call inside a critical section.
"""

from __future__ import annotations

import os
import re
import sys
from typing import Iterator, List, NamedTuple, Optional

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_ROOTS = ("src", "lib")
EXTENSIONS = (".cpp", ".cc", ".c", ".h", ".hpp")

# What must never run with interrupts masked: a log line's durable write, stdio, the card and
# flash file systems, NVS, the blocking primitives, and the subsystems that do those inside.
FORBIDDEN = re.compile(
    r"\b("
    r"alog\w*|logPersist|logSerial|emitRedacted|log_[evwid]|ESP_LOG[EWIDV]|printf|fprintf|fwrite|fread|fopen|fclose|fflush|"
    r"captureEvent|xSemaphoreTake\w*|xQueueSend\w*|xQueueReceive\w*|vTaskDelay\w*|delay"
    r")\s*\("
    r"|\b(Serial|SD|LittleFS|Preferences)\s*\."
    r"|\b(errlog|store|memory|telegram|sfx|nvs)\s*::\s*\w+\s*\("
    r"|\bnvs_\w+\s*\("
)

ENTER = re.compile(r"\b(?:portENTER_CRITICAL|taskENTER_CRITICAL)(?:_ISR|_SAFE)?\s*\(\s*&?\s*([\w:.>-]+)\s*\)")
EXIT = re.compile(r"\b(?:portEXIT_CRITICAL|taskEXIT_CRITICAL)(?:_ISR|_SAFE)?\s*\(\s*&?\s*([\w:.>-]+)\s*\)")
LOCK_CALL = re.compile(r"(?<![\w:])(?:net::)?lockConfig\s*\(\s*\)\s*;")
UNLOCK_CALL = re.compile(r"(?<![\w:])(?:net::)?unlockConfig\s*\(\s*\)\s*;")
GUARD = re.compile(r"\bConfigLockGuard\s+\w+\s*;")


class Finding(NamedTuple):
    path: str
    line: int
    text: str


def blank_comments_and_strings(src: str) -> str:
    """The source with comments, strings and char literals blanked, line numbers kept."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        two = src[i : i + 2]
        if two == "//":
            while i < n and src[i] != "\n":
                i += 1
        elif two == "/*":
            end = src.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(ch if ch == "\n" else " " for ch in src[i:end]))
            i = end
        elif c in "\"'":
            quote = c
            out.append(" ")
            i += 1
            while i < n and src[i] != quote and src[i] != "\n":
                i += 2 if src[i] == "\\" else 1
            i += 1
            out.append(" ")
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _scope_end(text: str, start: int) -> int:
    """Where the scope around `start` closes (or the end of the text)."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth < 0:
                return i
    return len(text)


def _pair_end(text: str, start: int, closes) -> int:
    """The closing call at the brace depth of the opening one: an early `{ exit; return; }` in a
    nested block is not the end of the section. The end of the text when it never closes."""
    depth = 0
    i = start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth < 0:
                return i
        elif depth == 0:
            m = closes(i)
            if m:
                return i
        i += 1
    return len(text)


def regions(text: str) -> Iterator["tuple[int, int]"]:
    """(start, end) of every critical section in comment-free source."""
    for m in ENTER.finditer(text):
        mux = m.group(1)

        def closes(i: int, mux: str = mux) -> Optional[re.Match]:
            e = EXIT.match(text, i)
            return e if e and e.group(1) == mux else None

        yield m.end(), _pair_end(text, m.end(), closes)
    for m in LOCK_CALL.finditer(text):
        yield m.end(), _pair_end(text, m.end(), lambda i: UNLOCK_CALL.match(text, i))
    for m in GUARD.finditer(text):
        yield m.end(), _scope_end(text, m.end())


def scan_source(path: str, src: str) -> List[Finding]:
    text = blank_comments_and_strings(src)
    lines = src.split("\n")
    seen = set()
    found: List[Finding] = []
    for start, end in regions(text):
        for bad in FORBIDDEN.finditer(text, start, end):
            line = text.count("\n", 0, bad.start()) + 1
            if line in seen:
                continue
            seen.add(line)
            found.append(Finding(path, line, lines[line - 1].strip()))
    return sorted(found)


def source_files(roots: "tuple[str, ...]") -> Iterator[str]:
    for root in roots:
        path = root if os.path.isabs(root) else os.path.join(REPO, root)
        if os.path.isfile(path):
            yield path
            continue
        for dirpath, _dirs, names in os.walk(path):
            for name in sorted(names):
                if name.endswith(EXTENSIONS):
                    yield os.path.join(dirpath, name)


def main(argv: "list[str] | None" = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    roots = tuple(args) if args else DEFAULT_ROOTS
    found: List[Finding] = []
    files = sections = 0
    for path in source_files(roots):
        files += 1
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read()
        sections += sum(1 for _ in regions(blank_comments_and_strings(src)))
        found.extend(scan_source(os.path.relpath(path, REPO), src))
    for fd in found:
        print(f"FAIL {fd.path}:{fd.line}: inside a critical section: {fd.text}")
    if files == 0:
        print("FAIL critical sections: no source read")
        return 1
    if found:
        print(f"FAIL critical sections: {len(found)} call(s) that can abort or block with interrupts masked")
        return 1
    print(f"PASS critical sections: {files} files, {sections} sections, nothing logs or blocks inside a spinlock")
    return 0


if __name__ == "__main__":
    sys.exit(main())
