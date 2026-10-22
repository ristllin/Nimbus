#!/usr/bin/env python3
"""Print the storm suite's before/after tables from an evidence directory: for each leg,
the newest v4.5.13 control run against the newest release-candidate run (optionally the
newest on one commit), with the candidate's bounds and verdicts.

Usage: python3 tests/hil/storm/report.py <evidence dir> [--candidate SHA]
Exit 0 when every leg's newest candidate run passed, 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import List, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import grading as g  # noqa: E402


def newest_candidate(records: List[dict], leg: str, candidate: Optional[str]) -> Optional[dict]:
    runs = [r for r in records if r.get("leg") == leg and r.get("role") == g.CANDIDATE]
    if candidate:
        runs = [r for r in runs if str(r.get("commit") or "")[:7] == candidate[:7]]
    return max(runs, key=lambda r: r.get("finished", 0)) if runs else None


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Storm suite before/after tables")
    ap.add_argument("evidence")
    ap.add_argument("--candidate", help="only candidate runs on this commit")
    args = ap.parse_args(argv)
    records = g.load_evidence(args.evidence)
    ok = True
    for leg in g.LEGS:
        control = g.latest(records, leg, g.CONTROL)
        cand = newest_candidate(records, leg, args.candidate)
        bounds = g.bounds_of(cand) or g.bounds_of(control)
        print(g.table(leg, g.metrics_of(control), g.metrics_of(cand), bounds))
        where = (
            f"candidate {cand['build']} on unit {cand['unit']} at {cand['finishedUtc']}" if cand else "no candidate run"
        )
        print(f"({where}; grade {cand['grade'] if cand else 'missing'})\n")
        ok = ok and bool(cand) and cand.get("grade") == g.PASS
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
