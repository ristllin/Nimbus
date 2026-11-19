#!/usr/bin/env python3
"""Gate data check: the v5.2.0 release rows that replaced the ledger rows stay whole (lane RB-REC7).

tools/release_gate/v520_gate.json lists the rows RB-R1 runs on the v5.2.0 candidate (owner rulings 41 and
81). This check fails when a required row is missing, a row names a path the tree does not have, a removed
gate's path is back, or a gate script or workflow still runs the food coach eval.

    python3 tools/release_gate/check_v520_gate.py
Exit 0 with "V520 GATE ROWS GREEN", exit 1 with "V520 GATE ROWS RED".
"""

from __future__ import annotations

import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(REPO, "tools", "release_gate", "v520_gate.json")
REQUIRED = (
    "file-first-call",
    "file-totals",
    "no-stray-or-duplicate-lines",
    "refloop-day-scenarios",
    "families-parity",
    "owner-regressions",
    "file-replay",
    "no-false-claims",
    "hedge-rate",
    "domain-words-lint",
    "free-flash",
    "cut-during-append",
    "cut-during-edit",
    "round-trip-one-row",
    "rollback-up-down-up",
    "refused-allocation",
    "history-never-reduced",
)
# Where a gate is run from: none of them may run the food coach eval again.
GATE_SOURCES = (".github/workflows", "tools/release_gate")


def row_problems(rows: list, repo: str) -> list:
    ids = [r.get("id") for r in rows]
    out = [f"required row {r} is missing" for r in REQUIRED if r not in ids]
    out += [f"row {i} is listed twice" for i in sorted({i for i in ids if ids.count(i) > 1})]
    out += [f"row {r.get('id')} has no what or no measure" for r in rows if not r.get("what") or not r.get("measure")]
    out += [
        f"row {r.get('id')} names {p}, which is not in the tree"
        for r in rows
        for p in r.get("paths", [])
        if not os.path.exists(os.path.join(repo, p))
    ]
    return out


def removed_problems(removed: list, repo: str) -> list:
    """A removed gate's path back in the tree (the food coach eval stays, as a user-content example)."""
    return [
        f"removed gate {g['gate']} is back at {g['path']}"
        for g in removed
        if g.get("path") and g["path"] != "tests/evals/food_coach" and os.path.exists(os.path.join(repo, g["path"]))
    ]


def gate_sources(repo: str):
    """The scripts and workflows a gate runs from (their tests and this check's own data left out)."""
    for top in GATE_SOURCES:
        for root, _, names in os.walk(os.path.join(repo, top)):
            for name in names:
                if name.endswith((".py", ".yml", ".yaml", ".sh")) and not name.startswith("test_"):
                    if name != os.path.basename(__file__):
                        yield os.path.join(root, name)


def problems(data: dict, repo: str = REPO) -> list:
    out = row_problems(data.get("rows", []), repo) + removed_problems(data.get("removed", []), repo)
    for path in gate_sources(repo):
        with open(path, encoding="utf-8", errors="replace") as fh:
            if "evals/food_coach" in fh.read():
                out.append(f"{os.path.relpath(path, repo)} still runs the food coach eval")
    return out


def main(argv=None) -> int:
    with open(DATA, encoding="utf-8") as fh:
        found = problems(json.load(fh))
    for p in found:
        print(f"  {p}")
    print("V520 GATE ROWS RED" if found else "V520 GATE ROWS GREEN")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
