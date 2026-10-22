#!/usr/bin/env python3
"""Release gate: the storm suite ran on this release candidate, and every leg passed.

The storm suite (tests/hil/storm/) proves on hardware that a device stays usable,
never loops, never spends without a cap and tells the owner once, under eight storms
(settings store full, a re-served Telegram page, a refusing provider, a deterministic
provider error, low memory, a restart mid-turn, a full file store, a false claim).
Each leg writes one JSON evidence record per run. This check reads the evidence
directory and fails unless, for every leg:

* a run on the release candidate exists whose commit is the candidate's, that
  finished after the candidate commit was made, and that graded PASS;
* a run on the v4.5.13 control image exists (the "before" of the leg's table).

No network: the candidate commit's time comes from the local git history (or
``--candidate-time``).

Usage:
    python3 tools/release_gate/check_storm_evidence.py --evidence DIR --candidate SHA
Exit 0 = every leg has fresh passing evidence; 1 = the release is blocked.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from typing import Dict, List, Optional, Sequence

# Kept equal to tests/hil/storm/grading.py LEGS (tools/release_gate/test_release_gate.py
# pins the two lists and the leg files together).
LEGS = (
    "a_settings_full",
    "b_page_reserved",
    "c_provider_refusing",
    "d_deterministic_error",
    "e_low_heap",
    "f_reboot_mid_turn",
    "g_file_store_full",
    "h_false_claim",
)
SCHEMA = 1
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def same_commit(a: str, b: str) -> bool:
    """Two commit names agree when the shorter (at least 7 hex digits) prefixes the longer."""
    a, b = (a or "").lower(), (b or "").lower()
    if len(a) < 7 or len(b) < 7:
        return False
    return a.startswith(b) or b.startswith(a)


def commit_time(sha: str, repo: str = REPO) -> Optional[int]:
    """The committer time of ``sha`` in the local history, or None."""
    try:
        out = subprocess.run(
            ["git", "-C", repo, "show", "-s", "--format=%ct", sha], capture_output=True, text=True, timeout=20
        )
    except (OSError, subprocess.SubprocessError):
        return None
    text = out.stdout.strip()
    return int(text) if out.returncode == 0 and text.isdigit() else None


def load(directory: str) -> List[dict]:
    records = []
    for root, _dirs, files in os.walk(directory):
        for name in sorted(files):
            if not name.endswith(".json"):
                continue
            try:
                with open(os.path.join(root, name), encoding="utf-8") as f:
                    rec = json.load(f)
            except (OSError, ValueError):
                continue
            if isinstance(rec, dict) and rec.get("schema") == SCHEMA and rec.get("leg"):
                records.append(rec)
    return records


def _finished(rec: dict) -> float:
    return float(rec.get("finished") or 0)


def leg_problem(runs: Sequence[dict], candidate: str, since: float) -> str:
    """'' when one leg's runs hold fresh passing evidence on the candidate and a control
    run, else the reason the leg blocks the release."""
    on_rc = [r for r in runs if r.get("role") == "candidate" and same_commit(str(r.get("commit")), candidate)]
    if not on_rc:
        return f"no run on the release candidate {candidate[:12]}"
    fresh = [r for r in on_rc if _finished(r) > since]
    if not fresh:
        return "every run on the release candidate is older than its commit"
    newest = max(fresh, key=_finished)
    if newest.get("grade") != "PASS":
        bad = {k: v.get("status") for k, v in (newest.get("verdicts") or {}).items() if v.get("status") != "PASS"}
        return f"newest run graded {newest.get('grade')} {bad}"
    if not any(r.get("role") == "control" for r in runs):
        return "no run on the v4.5.13 control image (the before of the leg's table)"
    return ""


def check(records: Sequence[dict], candidate: str, since: float, legs: Sequence[str] = LEGS) -> Dict[str, str]:
    """Per leg, '' when it has fresh passing evidence, else the reason it blocks."""
    return {leg: leg_problem([r for r in records if r.get("leg") == leg], candidate, since) for leg in legs}


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Storm suite evidence check (release gate)")
    ap.add_argument("--evidence", required=True, help="the storm evidence directory of this release")
    ap.add_argument("--candidate", required=True, help="the release candidate's commit")
    ap.add_argument("--candidate-time", type=float, default=None, help="its commit time (default: from git)")
    ap.add_argument("--repo", default=REPO, help="the firmware repository (for the commit time)")
    args = ap.parse_args(argv)
    since = args.candidate_time if args.candidate_time is not None else commit_time(args.candidate, args.repo)
    if since is None:
        print(f"storm suite: FAIL - commit {args.candidate} is not in the local history")
        return 1
    if not os.path.isdir(args.evidence):
        print(f"storm suite: FAIL - no evidence directory {args.evidence}")
        return 1
    verdict = check(load(args.evidence), args.candidate, since)
    for leg in LEGS:
        reason = verdict[leg]
        print(f"  {'PASS' if not reason else 'FAIL'}  storm leg {leg}" + (f": {reason}" if reason else ""))
    blocked = [leg for leg, reason in verdict.items() if reason]
    if blocked:
        print(f"storm suite: FAIL - {len(blocked)} of {len(LEGS)} legs lack fresh passing evidence")
        return 1
    print(f"storm suite: PASS - every leg passed on {args.candidate[:12]} after its commit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
