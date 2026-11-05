#!/usr/bin/env python3
"""Release gate: the owner-regression suite (lane REGR, LAUNCH-5.0.0 ruling 4 Nov 20:20Z).

v5.0.0 is not tagged unless this gate passes on the exact candidate sha. Two halves:

1. Static consistency (always runs, no network, no keys). The register
   coverage/OWNER-ISSUES-REGISTER.md must not name a guard that does not exist:
   every oir_NN id it names must be a RUN_TEST in test/test_owner_regressions/main.cpp,
   every test_* id a suite directory under test/, every tools/ path a real file, and
   every row must name at least one guard (a native test, a process gate, or - for a
   row whose fix is still open - its live lc-* case). The register's EXPECTED-FAIL
   roster must equal the set of oir tests that report TEST_IGNORE_MESSAGE, and no
   EXPECTED-FAIL test may assert a non-empty RowChange::was after its ignore:
   file_append.h documents `was` as the replaced row's figure, empty when the row was
   not replaced, so an append-fix (lane UPSERT) makes it empty and such an assertion
   could never pass once the owning lane lands.

2. Live run (--candidate SHA). Refuses the release unless a full live run on the
   exact candidate sha exists on disk under
   tests/evals/owner_regressions/runs/<sha>/summary.json, every gate model has a
   leg in it, every non-xfail scenario meets its threshold on each gate model
   (thresholds.json gate_models), no scenario
   regressed against the previous candidate's run, and no xfail scenario met its
   threshold (a stale xfail must be flipped, not left skipping). With --native-log,
   an EXPECTED-FAIL skip in the candidate's native run also blocks: a landed fix
   that did not flip its xfail test must not stay green.

Usage:
    python3 tools/release_gate/check_owner_regressions.py                  # static half
    python3 tools/release_gate/check_owner_regressions.py --candidate SHA [--previous SHA]
                                                                          [--native-log PATH]
Exit 0 = pass; exit 1 = release blocked.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REGISTER = os.path.join(REPO, "coverage", "OWNER-ISSUES-REGISTER.md")
SUITE = os.path.join(REPO, "test", "test_owner_regressions", "main.cpp")
EVALS = os.path.join(REPO, "tests", "evals", "owner_regressions")
RUNS = os.path.join(EVALS, "runs")
THRESHOLDS = os.path.join(EVALS, "thresholds.json")
RUN_GATE = os.path.join(REPO, "tools", "release_gate", "run_gate.py")

OIR_ID = re.compile(r"\boir_(\d+)\b")
TEST_ID = re.compile(r"\btest_([a-z0-9_]+)\b")
TOOLS_PATH = re.compile(r"\btools/([A-Za-z0-9_./-]+)")
ROW = re.compile(r"^\|\s*(OIR-\d+)\s*\|")
RUN_TEST = re.compile(r"RUN_TEST\(\s*test_(oir_\d+)\w*\s*\)")
TEST_FN = re.compile(r"^void\s+(test_oir_\d+)_\w+\s*\(\)\s*\{", re.M)
IGNORE = "TEST_IGNORE_MESSAGE"
WAS_NONEMPTY = re.compile(r"TEST_ASSERT_(?:FALSE|NOT_NULL)\([^;]*\.was")


def read(path: str) -> str:
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def register_rows() -> "list[tuple[str, str, str, str]]":
    """(row id, fix column, guard column, live column) of every register table row."""
    rows = []
    for line in read(REGISTER).splitlines():
        m = ROW.match(line)
        if not m:
            continue
        cells = [c.strip() for c in line.split("|")[1:-1]]
        if len(cells) < 8:
            continue
        rows.append((m.group(1), cells[5], cells[6], cells[7]))
    return rows


def suite_oir_ids() -> set:
    return set(RUN_TEST.findall(read(SUITE)))


def suite_xfail_ids() -> set:
    """The oir tests that report EXPECTED-FAIL via TEST_IGNORE_MESSAGE."""
    src = read(SUITE)
    out = set()
    for m in TEST_FN.finditer(src):
        end = src.find("\n}\n", m.end())
        body = src[m.end() : end if end > 0 else len(src)]
        if IGNORE in body:
            out.add(m.group(1))
    return out


def suite_was_assertions() -> "list[str]":
    """Xfail tests asserting a non-empty RowChange::was after their ignore."""
    src = read(SUITE)
    bad = []
    for m in TEST_FN.finditer(src):
        end = src.find("\n}\n", m.end())
        body = src[m.end() : end if end > 0 else len(src)]
        if IGNORE in body and WAS_NONEMPTY.search(body):
            bad.append(m.group(1))
    return bad


def suite_exists(name: str) -> bool:
    """A test_* id resolves when a native suite dir or a python test file carries it."""
    for cand in (
        os.path.join(REPO, "test", name),
        os.path.join(REPO, "tools", "release_gate", name + ".py"),
        os.path.join(REPO, "tools", name + ".py"),
        os.path.join(REPO, "tests", "hil", name + ".py"),
    ):
        if os.path.exists(cand):
            return True
    return False


def row_guard_problems(row_id: str, fix: str, guard: str, live: str) -> "list[str]":
    """Why this register row's guard column does not hold up (empty when it does)."""
    errs = []
    oirs = {f"oir_{int(n):02d}" for n in OIR_ID.findall(guard)}
    known = suite_oir_ids()
    for o in sorted(oirs - known):
        errs.append(f"{row_id} names {o}, which no RUN_TEST in test_owner_regressions defines")
    for t in sorted(set(TEST_ID.findall(guard))):
        if not suite_exists("test_" + t):
            errs.append(f"{row_id} names test_{t}, which no suite or gate test defines")
    for p in sorted(set(TOOLS_PATH.findall(guard))):
        if not os.path.exists(os.path.join(REPO, "tools", p)):
            errs.append(f"{row_id} names tools/{p}, which does not exist")
    has_guard = bool(oirs or TEST_ID.findall(guard) or TOOLS_PATH.findall(guard) or "run_gate.py" in guard)
    has_live = bool(re.search(r"\blc-[a-z0-9-]+", guard + " " + live))
    if not has_guard and not has_live:
        errs.append(f"{row_id} names no guard that exists (fix column: {fix!r})")
    return errs


def static_problems() -> "list[str]":
    errs: list[str] = []
    rows = register_rows()
    for row_id, fix, guard, live in rows:
        errs.extend(row_guard_problems(row_id, fix, guard, live))
    reg_xfail = {f"oir_{int(r[0][4:]):02d}" for r in rows if "EXPECTED-FAIL" in r[2]}
    suite_xfail = {t[len("test_") :] for t in suite_xfail_ids()}
    if reg_xfail != suite_xfail:
        errs.append(f"EXPECTED-FAIL roster mismatch: register {sorted(reg_xfail)} vs suite {sorted(suite_xfail)}")
    for t in suite_was_assertions():
        errs.append(
            f"{t} asserts a non-empty RowChange::was after its EXPECTED-FAIL ignore; "
            "file_append.h: `was` is empty when the row was not replaced, so the assertion "
            "can never pass once the owning lane lands"
        )
    for name in ("check_owner_regressions.py", "test_owner_regressions.py"):
        if not os.path.exists(os.path.join(REPO, "tools", "release_gate", name)):
            errs.append(f"the register names tools/release_gate/{name}, which does not exist")
    if "check_owner_regressions.main" not in read(RUN_GATE):
        errs.append("run_gate.py does not wire check_owner_regressions.main into the host checks")
    return errs


def load_summary(sha: str) -> "dict | None":
    path = os.path.join(RUNS, sha, "summary.json")
    if not os.path.exists(path):
        return None
    return json.loads(read(path))


def expected_scenarios(summary: dict, thresholds: dict) -> set:
    ids = set(thresholds.get("scenarios", {}))
    for m in summary.get("models", {}).values():
        ids.update(m.get("scenarios", {}))
    return ids


def scenario_problems(model: str, sid: str, s: dict, xfail: bool, is_gate: bool, limits: dict) -> "list[str]":
    errs = []
    runs, minp = limits["runs"], limits["minp"]
    if s.get("runs", 0) < runs:
        errs.append(f"{model}/{sid}: only {s.get('runs', 0)}/{runs} reps on the candidate sha")
    if xfail and (s.get("xfail_met") or s.get("meets")):
        errs.append(f"{model}/{sid}: xfail row met its threshold - stale EXPECTED-FAIL, flip it")
    if not xfail and is_gate and not s.get("meets"):
        errs.append(f"{model}/{sid}: {s.get('passes', 0)}/{s.get('runs', 0)} passes, target {minp}/{runs} NOT MET")
    return errs


def regression_problems(summary: dict, previous: dict, scen_xfail: dict) -> "list[str]":
    errs = []
    for model, m in previous.get("models", {}).items():
        now = summary.get("models", {}).get(model, {}).get("scenarios", {})
        for sid, ps in m.get("scenarios", {}).items():
            ns = now.get(sid)
            xfail = scen_xfail.get(sid, {}).get("xfail", False)
            if ns and not xfail and ps.get("meets") and not ns.get("meets"):
                errs.append(
                    f"{model}/{sid}: regressed vs the previous candidate "
                    f"({ps.get('passes')}/{ps.get('runs')} -> {ns.get('passes')}/{ns.get('runs')})"
                )
    return errs


def live_problems(summary: dict, thresholds: dict, previous: "dict | None") -> "list[str]":
    errs: list[str] = []
    runs = thresholds.get("runs", 5)
    gate_models = thresholds.get("gate_models", [])
    scen_xfail = thresholds.get("scenarios", {})
    expected = expected_scenarios(summary, thresholds)
    for gm in sorted(gate_models):
        if gm not in summary.get("models", {}):
            errs.append(f"{gm}: gate model has no live run on the candidate sha")
    for model in sorted(summary.get("models", {})):
        m = summary["models"][model]
        is_gate = model in gate_models
        for sid in sorted(expected):
            s = m.get("scenarios", {}).get(sid)
            if s is None:
                if is_gate:
                    errs.append(f"{model}: no run of {sid} on the candidate sha")
                continue
            minp = s.get("min_passes", thresholds.get("default_min_passes", 4))
            xfail = scen_xfail.get(sid, {}).get("xfail", False)
            errs.extend(scenario_problems(model, sid, s, xfail, is_gate, {"runs": runs, "minp": minp}))
    if previous:
        errs.extend(regression_problems(summary, previous, scen_xfail))
    return errs


def previous_summary(candidate: str, explicit: "str | None") -> "dict | None":
    if explicit:
        return load_summary(explicit)
    if not os.path.isdir(RUNS):
        return None
    best, best_key = None, ""
    for name in os.listdir(RUNS):
        if name == candidate or not os.path.isdir(os.path.join(RUNS, name)):
            continue
        path = os.path.join(RUNS, name, "summary.json")
        if not os.path.exists(path):
            continue
        key = json.loads(read(path)).get("generated_utc", "")
        if key > best_key:
            best, best_key = json.loads(read(path)), key
    return best


def native_log_problems(path: str) -> "list[str]":
    if not os.path.exists(path):
        return [f"--native-log {path} does not exist"]
    hits = [ln for ln in read(path).splitlines() if "EXPECTED-FAIL" in ln]
    if hits:
        return [
            f"native run still reports {len(hits)} EXPECTED-FAIL skip(s); a landed fix must flip "
            f"its xfail test, first: {hits[0].strip()}"
        ]
    return []


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Owner-regression release gate (lane REGR)")
    ap.add_argument("--candidate", help="the release candidate's commit sha (enables the live half)")
    ap.add_argument("--previous", help="the previous candidate's sha for the regression comparison")
    ap.add_argument("--native-log", help="the candidate's native test log; EXPECTED-FAIL skips block")
    args = ap.parse_args(argv)

    errs = static_problems()
    if args.candidate:
        thresholds = json.loads(read(THRESHOLDS))
        summary = load_summary(args.candidate)
        if summary is None:
            errs.append(
                f"no live run on the candidate sha at tests/evals/owner_regressions/runs/{args.candidate}/summary.json"
            )
        else:
            if summary.get("sha") != args.candidate:
                errs.append(f"summary.json under runs/{args.candidate} records sha {summary.get('sha')}")
            errs.extend(live_problems(summary, thresholds, previous_summary(args.candidate, args.previous)))
    if args.native_log:
        errs.extend(native_log_problems(args.native_log))

    for e in errs:
        print(f"FAIL owner regressions: {e}")
    if errs:
        print(f"FAIL owner regressions: {len(errs)} problem(s), release blocked")
        return 1
    print(
        "PASS owner regressions: register, suite and gate wiring consistent"
        + (
            f"; live run on {args.candidate[:12]} meets every hard row on the gate models"
            if args.candidate
            else " (static half only; pass --candidate for the live half)"
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
