#!/usr/bin/env python3
"""Gate: the tests a native run ignores are exactly the pending roster, and the roster only shrinks.

The rebuild writes tests before their implementation (PRD section 10). Such a test reports
TEST_IGNORE_MESSAGE("PENDING RB-<lane>") and is named in test/pending_roster.txt, so
`pio test -e native` keeps its exit status and its 100% while the work is open. This gate
keeps the roster honest in both directions:

- every test the native run ignored is on the roster with the lane its message names, and
  every roster line is a test the run ignored (a test that now passes leaves the roster);
- an ignore whose message is not "PENDING RB-<lane>" fails: a native run ignores nothing else;
- the log is a whole, green run: PlatformIO's "N test cases: ..." summary is present, reports
  nothing failed or errored, and its skipped count equals the ignored cases read (a case line
  in an unknown shape cannot hide a skip, and a crashed suite cannot hide its ignores);
- against merge-base(HEAD, rb-main) the roster only shrinks: a line that is not in the base
  roster fails (a missing ref fails too: there is no fallback), unless it names the
  orchestrator ruling that put it there (` ruling:<n>`, ruling 56) AND that ruling lists the
  test in tools/release_gate/roster_rulings.txt (ruling 59 item 8: a well-formed number alone
  is not enough; the orchestrator commits the list). The first holder may create the file when
  the base has none;
- static leg: each roster line names a suite under test/ whose sources hold the test name and
  its "PENDING RB-<lane>" message.

Roster format, one test per line, '#' starts a comment:
    test_tx_prefix::test_prefix_is_stable RB-T3
    test_plain_conformance::test_a_full_arena_frees_its_oldest_turn RB-E1b ruling:56

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
RULINGS_REL = os.path.join("tools", "release_gate", "roster_rulings.txt")
ABSENT = object()  # the base commit has no roster file
TAG = "[gate:pending-roster]"

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
# PlatformIO's line for a case: "test/<suite>/<file>:<line>: <name>[: <message>]\t[STATUS]"
PIO_CASE_RE = re.compile(
    r"^test/(?P<suite>[^/\s]+)/\S+?:\d+: (?P<name>[^\s:]+)(?:: (?P<msg>.*?))?\s*\[(?P<status>[A-Z]+)\]\s*$"
)
# Unity's raw line (verbose runs): "test/<suite>/<file>:<line>:<name>:IGNORE[: <message>]"
UNITY_CASE_RE = re.compile(r"^test/(?P<suite>[^/\s]+)/\S+?:\d+:(?P<name>[^\s:]+):IGNORE(?::\s*(?P<msg>.*))?$")
SUMMARY_RE = re.compile(r"\b\d+ test cases: (?P<counts>.*?) in \d")
COUNT_RE = re.compile(r"(?P<n>\d+) (?P<what>failed|errored|skipped|succeeded)\b")
NOT_GREEN_RE = re.compile(r"\[(FAILED|ERRORED)\]")
LANE_END = r"(?![A-Za-z0-9-])"  # RB-T3 is not RB-T30 nor RB-T3-x
PENDING_RE = re.compile(r"^PENDING (?P<lane>RB-[A-Za-z0-9]+)" + LANE_END)
ROSTER_LINE_RE = re.compile(
    r"^(?P<suite>test_[A-Za-z0-9_]+)::(?P<name>[A-Za-z_][A-Za-z0-9_]*) (?P<lane>RB-[A-Za-z0-9]+)"
    r"(?: ruling:(?P<ruling>[1-9][0-9]*))?$"
)
# roster_rulings.txt: "<ruling> <suite>::<test>", one allowed test per line, '#' starts a comment.
RULINGS_LINE_RE = re.compile(r"^(?P<ruling>[1-9][0-9]*) (?P<key>test_[A-Za-z0-9_]+::[A-Za-z_][A-Za-z0-9_]*)$")


def parse_native_log(text: str) -> "tuple[dict[str, str], int | None, list[str]]":
    """(ignored test key -> message, skipped count from the summary or None when absent,
    reasons the run is not green: failed or errored cases or suites hide what they would ignore)."""
    ignored: "dict[str, str]" = {}
    skipped: "int | None" = None
    red: "list[str]" = []
    for raw in text.splitlines():
        line = ANSI_RE.sub("", raw).strip()
        m = PIO_CASE_RE.match(line) or UNITY_CASE_RE.match(line)
        if m and (m.re is UNITY_CASE_RE or m.group("status") == "SKIPPED"):
            ignored[f"{m.group('suite')}::{m.group('name')}"] = (m.group("msg") or "").strip()
            continue
        m = SUMMARY_RE.search(line)
        if m:
            counts = {c.group("what"): int(c.group("n")) for c in COUNT_RE.finditer(m.group("counts"))}
            skipped = (skipped or 0) + counts.get("skipped", 0)
            red += [f"the summary reports {counts[k]} {k}" for k in ("failed", "errored") if counts.get(k)]
        elif NOT_GREEN_RE.search(line):
            red.append(f"not green: {line[:160]}")
    return ignored, skipped, red


def parse_roster(text: "str | None") -> "tuple[dict[str, str], list[str]]":
    """(test key -> lane, problems). A missing file is an empty roster."""
    entries, _ruled, errs = parse_roster_rulings(text)
    return entries, errs


def parse_roster_rulings(text: "str | None") -> "tuple[dict[str, str], dict[str, int], list[str]]":
    """(test key -> lane, key -> the orchestrator ruling its line names, problems)."""
    entries: "dict[str, str]" = {}
    ruled: "dict[str, int]" = {}
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
        if m.group("ruling"):
            ruled[key] = int(m.group("ruling"))
    return entries, ruled, errs


def parse_rulings(text: "str | None") -> "tuple[dict[int, set[str]], list[str]]":
    """(ruling -> the test keys it allows, problems) from roster_rulings.txt."""
    allowed: "dict[int, set[str]]" = {}
    errs: "list[str]" = []
    for n, raw in enumerate((text or "").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = RULINGS_LINE_RE.match(line)
        if not m:
            errs.append(f"{RULINGS_REL} line {n} is not '<ruling> <suite>::<test>': {raw.strip()}")
            continue
        allowed.setdefault(int(m.group("ruling")), set()).add(m.group("key"))
    return allowed, errs


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


def shrink_problems(
    roster: "dict[str, str]",
    base: object,
    ruled: "dict[str, int] | None" = None,
    allowed: "dict[int, set[str]] | None" = None,
) -> "tuple[list[str], str]":
    """(problems, one report line) for the shrink-only rule against the base roster text. A
    line that is new against the base passes only when it names an orchestrator ruling that
    lists it in roster_rulings.txt (`allowed`; None: the tree has no list)."""
    if base is ABSENT:
        return [], f"base has no roster: this tree creates it with {len(roster)} line(s)"
    base_entries, _ = parse_roster(base)  # type: ignore[arg-type]
    ruled = ruled or {}
    new = [k for k, v in roster.items() if base_entries.get(k) != v]
    grown, unlisted, added = split_new(new, roster, ruled, allowed or {})
    errs = [
        f"roster line '{g}' is not in the base roster: the roster can only shrink, or grow by a line that names an "
        "orchestrator ruling (' ruling:<n>')"
        for g in grown
    ]
    errs += [unlisted_problem(k, roster[k], ruled[k], allowed) for k in unlisted]
    line = f"roster shrank by {len(set(base_entries) - set(roster))} against the base ({len(base_entries)} line(s))"
    if added:
        line += f"; {len(added)} line(s) added under a ruling: " + ", ".join(added)
    return errs, line


def split_new(
    new: "list[str]", roster: "dict[str, str]", ruled: "dict[str, int]", allowed: "dict[int, set[str]]"
) -> "tuple[list[str], list[str], list[str]]":
    """(untagged new lines, tagged keys their ruling does not list, tagged lines it lists)."""
    grown = sorted(f"{k} {roster[k]}" for k in new if k not in ruled)
    unlisted = sorted(k for k in new if k in ruled and k not in allowed.get(ruled[k], set()))
    added = sorted(f"{k} {roster[k]}" for k in new if k in ruled and k not in unlisted)
    return grown, unlisted, added


def stale_listing_problems(ruled: "dict[str, int]", allowed: "dict[int, set[str]] | None") -> "list[str]":
    """Ruling 79 item 3: every entry of roster_rulings.txt is a current roster line under that ruling, so
    a list line whose test left the roster cannot let a later lane park the test again."""
    return [
        f"{RULINGS_REL} lists '{n} {key}', which is not a roster line under ruling {n}: remove the list line"
        for n, keys in sorted((allowed or {}).items())
        for key in sorted(keys)
        if ruled.get(key) != n
    ]


def unlisted_problem(key: str, lane: str, ruling: int, allowed: "dict[int, set[str]] | None") -> str:
    where = "does not exist in this tree" if allowed is None else f"does not list {key} under ruling {ruling}"
    return f"roster line '{key} {lane} ruling:{ruling}' names ruling {ruling}, but {RULINGS_REL} {where}"


def check(
    log_text: "str | None",
    roster_text: "str | None",
    base: object,
    require_empty: bool = False,
    allowed: "dict[int, set[str]] | None" = None,
) -> "tuple[bool, list[str], list[str]]":
    """(ok, report lines, failures). log_text None skips the equality leg (static-only runs);
    `allowed` is roster_rulings.txt read (None: the tree has none)."""
    roster, ruled, fails = parse_roster_rulings(roster_text)
    lines: "list[str]" = []
    if log_text is not None:
        ignored, skipped, red = parse_native_log(log_text)
        fails += [f"the native run is not green, so its ignored set is not known: {r}" for r in red[:5]]
        fails += equality_problems(ignored, skipped, roster)
        lines.append(f"native run: {len(ignored)} ignored, {len(roster)} on the roster")
    errs, line = shrink_problems(roster, base, ruled, allowed)
    errs += stale_listing_problems(ruled, allowed)
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
        elif not re.search(rf"\b{re.escape(name)}\b", text) or not re.search(
            f"PENDING {re.escape(lane)}" + LANE_END, text
        ):
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


def base_roster(root: str, ref: str) -> "tuple[object, str | None]":
    """(roster text at merge-base(HEAD, ref) or ABSENT, error)."""
    mb = git(root, "merge-base", "HEAD", ref)
    if mb.returncode != 0:
        return ABSENT, f"cannot find the merge base of HEAD and '{ref}': {mb.stderr.strip()}"
    sha = mb.stdout.strip()
    shown = git(root, "show", f"{sha}:{ROSTER_REL.replace(os.sep, '/')}")
    return (shown.stdout if shown.returncode == 0 else ABSENT), None


def read_optional(path: str) -> "str | None":
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def main(argv: "list[str] | None" = None) -> int:
    try:
        return run(argv)
    except Exception as e:  # a gate never ends in a traceback without its marker
        print(f"{TAG} FAIL: {type(e).__name__}: {e}")
        print(f"{TAG} PENDING ROSTER RED (the gate itself failed)")
        return 1


def run(argv: "list[str] | None") -> int:
    ap = argparse.ArgumentParser(description="Pending-test roster gate (lane RB-GATE0)")
    default_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--root", default=default_root, help="repo root (default: this file's repo)")
    ap.add_argument("--native-log", help="the log of a whole `pio test -e native` run")
    ap.add_argument(
        "--static-only", action="store_true", help="no native run: only the roster's shape, base and sources"
    )
    ap.add_argument("--base-ref", default="rb-main", help="integration branch to compare against (default rb-main)")
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
    ref = args.base_ref
    base, err = base_roster(root, ref)
    if err:
        fails.append(err)
    rulings_text = read_optional(os.path.join(root, RULINGS_REL))
    allowed, list_errs = parse_rulings(rulings_text)
    fails += list_errs
    _, lines, more = check(log_text, roster_text, base, args.require_empty, None if rulings_text is None else allowed)
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
    skipped = [n for n, off in (("equality leg", log_text is None), ("static leg", args.no_static)) if off]
    print(f"{TAG} PENDING ROSTER GREEN (base {ref}{''.join(f', {n} not run' for n in skipped)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
