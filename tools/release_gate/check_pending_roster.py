#!/usr/bin/env python3
"""Gate: the tests a native run ignores are exactly the pending roster, and the roster only shrinks.

The rebuild writes tests before their implementation (PRD section 10). Such a test reports
TEST_IGNORE_MESSAGE("PENDING RB-<lane>") and is named in test/pending_roster.txt, so
`pio test -e native` keeps its exit status and its 100% while the work is open. This gate
keeps the roster honest in both directions:

- every test the native run ignored is on the roster with the lane its message names, and
  every roster line is a test the run ignored (a test that now passes leaves the roster);
- an ignore whose message is not "PENDING RB-<lane>" fails: a native run ignores nothing else;
- the log is a whole run: PlatformIO's "N test cases: ..." summary is present and its skipped
  count equals the ignored cases read (a case line in an unknown shape cannot hide a skip);
- against the merge base with the integration branch the roster only shrinks: a line that is
  not in the base roster fails. The first holder may create the file when the base has none;
- static leg: each roster line names a suite under test/ whose sources hold the test name and
  its "PENDING RB-<lane>" message.

Roster format, one test per line, '#' starts a comment:
    test_tx_prefix::test_prefix_is_stable RB-T3

Usage:
    python3 tools/release_gate/check_pending_roster.py --native-log <log> [--base-ref rb-main]
    python3 tools/release_gate/check_pending_roster.py --static-only     # no native run at hand
    ... --require-empty        # the candidate gate: no roster line may remain
Exit 0 with "PENDING ROSTER GREEN", exit 1 with "PENDING ROSTER RED".
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

ROSTER_REL = os.path.join("test", "pending_roster.txt")
ABSENT = object()  # the base commit has no roster file
TAG = "[gate:pending-roster]"

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
# PlatformIO's line for a case: "test/<suite>/<file>:<line>: <name>[: <message>]\t[STATUS]"
PIO_CASE_RE = re.compile(
    r"^test/(?P<suite>[^/\s]+)/\S+?:\d+: (?P<name>[^\s:]+)(?:: (?P<msg>.*?))?\s*\[(?P<status>[A-Z]+)\]\s*$"
)
# Unity's raw line (verbose runs): "test/<suite>/<file>:<line>:<name>:IGNORE[: <message>]"
UNITY_CASE_RE = re.compile(r"^test/(?P<suite>[^/\s]+)/\S+?:\d+:(?P<name>[^\s:]+):IGNORE(?::\s*(?P<msg>.*))?$")
SUMMARY_RE = re.compile(r"(?P<total>\d+) test cases: (?:(?P<failed>\d+) failed, )?(?:(?P<skipped>\d+) skipped, )?")
PENDING_RE = re.compile(r"^PENDING (?P<lane>RB-[A-Za-z0-9]+)\b")
ROSTER_LINE_RE = re.compile(
    r"^(?P<suite>test_[A-Za-z0-9_]+)::(?P<name>[A-Za-z_][A-Za-z0-9_]*) (?P<lane>RB-[A-Za-z0-9]+)$"
)


def parse_native_log(text: str) -> "tuple[dict[str, str], int | None]":
    """(ignored test key -> message, skipped count from the summary or None when absent)."""
    ignored: "dict[str, str]" = {}
    skipped: "int | None" = None
    for raw in text.splitlines():
        line = ANSI_RE.sub("", raw).strip()
        m = PIO_CASE_RE.match(line)
        if m and m.group("status") == "SKIPPED":
            ignored[f"{m.group('suite')}::{m.group('name')}"] = (m.group("msg") or "").strip()
            continue
        m = UNITY_CASE_RE.match(line)
        if m:
            ignored[f"{m.group('suite')}::{m.group('name')}"] = (m.group("msg") or "").strip()
            continue
        m = SUMMARY_RE.search(line)
        if m:
            skipped = (skipped or 0) + int(m.group("skipped") or 0)
    return ignored, skipped


def parse_roster(text: "str | None") -> "tuple[dict[str, str], list[str]]":
    """(test key -> lane, problems). A missing file is an empty roster."""
    entries: "dict[str, str]" = {}
    errs: "list[str]" = []
    for n, raw in enumerate((text or "").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = ROSTER_LINE_RE.match(line)
        if not m:
            errs.append(f"roster line {n} is not '<suite>::<test> RB-<lane>': {raw.strip()}")
            continue
        key = f"{m.group('suite')}::{m.group('name')}"
        if key in entries:
            errs.append(f"roster line {n}: {key} is listed twice")
        entries[key] = m.group("lane")
    return entries, errs


def equality_problems(ignored: "dict[str, str]", skipped: "int | None", roster: "dict[str, str]") -> "list[str]":
    errs: "list[str]" = []
    if skipped is None:
        errs.append("the native log has no 'N test cases:' summary: not a whole `pio test -e native` run")
    elif skipped != len(ignored):
        errs.append(f"the summary reports {skipped} skipped but {len(ignored)} ignored case line(s) were read")
    for key in sorted(ignored):
        m = PENDING_RE.match(ignored[key])
        if not m:
            errs.append(f"{key} is ignored with '{ignored[key]}': a native run ignores only 'PENDING RB-<lane>' tests")
        elif key not in roster:
            errs.append(f"{key} is ignored (PENDING {m.group('lane')}) but not on the roster")
        elif roster[key] != m.group("lane"):
            errs.append(f"{key}: the roster says {roster[key]}, the test says PENDING {m.group('lane')}")
    for key in sorted(set(roster) - set(ignored)):
        errs.append(f"{key} is on the roster but not ignored in this run: remove its roster line")
    return errs


def shrink_problems(roster: "dict[str, str]", base: object) -> "tuple[list[str], str]":
    """(problems, one report line) for the shrink-only rule against the base roster text."""
    if base is ABSENT:
        return [], f"base has no roster: this tree creates it with {len(roster)} line(s)"
    base_entries, _ = parse_roster(base)  # type: ignore[arg-type]
    grown = sorted(f"{k} {v}" for k, v in roster.items() if base_entries.get(k) != v)
    errs = [f"roster line '{g}' is not in the base roster: the roster can only shrink" for g in grown]
    return (
        errs,
        f"roster shrank by {len(set(base_entries) - set(roster))} against the base ({len(base_entries)} line(s))",
    )


def check(
    log_text: "str | None", roster_text: "str | None", base: object, require_empty: bool = False
) -> "tuple[bool, list[str], list[str]]":
    """(ok, report lines, failures). log_text None skips the equality leg (static-only runs)."""
    roster, fails = parse_roster(roster_text)
    lines: "list[str]" = []
    if log_text is not None:
        ignored, skipped = parse_native_log(log_text)
        fails += equality_problems(ignored, skipped, roster)
        lines.append(f"native run: {len(ignored)} ignored, {len(roster)} on the roster")
    errs, line = shrink_problems(roster, base)
    fails += errs
    lines.append(line)
    if require_empty and roster:
        fails.append(f"the roster must be empty at this gate; {len(roster)} line(s) remain, first {sorted(roster)[0]}")
    return not fails, lines, fails


def static_problems(root: str, roster: "dict[str, str]") -> "list[str]":
    """Each roster line names an existing suite whose sources hold the test and its PENDING message."""
    errs: "list[str]" = []
    for key, lane in sorted(roster.items()):
        suite, name = key.split("::", 1)
        text = suite_sources(os.path.join(root, "test", suite))
        if text is None:
            errs.append(f"{key}: no suite test/{suite}")
        elif not re.search(rf"\b{re.escape(name)}\b", text) or f"PENDING {lane}" not in text:
            errs.append(f"{key}: test/{suite} does not hold {name} with 'PENDING {lane}'")
    return errs


def suite_sources(path: str) -> "str | None":
    if not os.path.isdir(path):
        return None
    chunks = []
    for dirpath, _dirs, files in os.walk(path):
        for f in sorted(files):
            if f.endswith((".c", ".cpp", ".h")):
                with open(os.path.join(dirpath, f), encoding="utf-8", errors="replace") as fh:
                    chunks.append(fh.read())
    return "\n".join(chunks)


def git(root: str, *args: str) -> "subprocess.CompletedProcess[str]":
    return subprocess.run(["git", "-C", root, *args], capture_output=True, text=True)


def default_base_ref(root: str) -> str:
    """The rebuild integrates on rb-main; elsewhere the integration branch is main."""
    return "rb-main" if git(root, "rev-parse", "--verify", "-q", "rb-main^{commit}").returncode == 0 else "main"


def base_roster(root: str, ref: str) -> "tuple[object, str | None]":
    """(roster text at merge-base(HEAD, ref) or ABSENT, error)."""
    mb = git(root, "merge-base", "HEAD", ref)
    if mb.returncode != 0:
        return ABSENT, f"cannot find the merge base of HEAD and '{ref}': {mb.stderr.strip()}"
    sha = mb.stdout.strip()
    shown = git(root, "show", f"{sha}:{ROSTER_REL.replace(os.sep, '/')}")
    return (shown.stdout if shown.returncode == 0 else ABSENT), None


def read_optional(path: str) -> "str | None":
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Pending-test roster gate (lane RB-GATE0)")
    default_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--root", default=default_root, help="repo root (default: this file's repo)")
    ap.add_argument("--native-log", help="the log of a whole `pio test -e native` run")
    ap.add_argument(
        "--static-only", action="store_true", help="no native run: only the roster's shape, base and sources"
    )
    ap.add_argument("--base-ref", help="integration branch to compare against (default rb-main, else main)")
    ap.add_argument("--require-empty", action="store_true", help="fail while any roster line remains")
    ap.add_argument("--no-static", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    fails: "list[str]" = []
    log_text = None
    if args.native_log:
        log_text = read_optional(args.native_log)
        if log_text is None:
            fails.append(f"--native-log {args.native_log} does not exist")
    elif not args.static_only:
        fails.append("no --native-log given: the equality leg needs a native run (or pass --static-only)")
    roster_text = read_optional(os.path.join(root, ROSTER_REL))
    ref = args.base_ref or default_base_ref(root)
    base, err = base_roster(root, ref)
    if err:
        fails.append(err)
    _, lines, more = check(log_text, roster_text, base, args.require_empty)
    fails += more
    if not args.no_static:
        fails += static_problems(root, parse_roster(roster_text)[0])
    for line in lines:
        print(f"{TAG} {line}")
    for f in fails:
        print(f"{TAG} FAIL: {f}")
    if fails:
        print(f"{TAG} PENDING ROSTER RED ({len(fails)} problem(s); base {ref})")
        return 1
    print(f"{TAG} PENDING ROSTER GREEN (base {ref}{', static only' if log_text is None else ''})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
