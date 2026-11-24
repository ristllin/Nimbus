#!/usr/bin/env python3
"""habits - count three habits in stored runs, from turns.json alone (no model call).

  unasked_writes   write calls (files, notes, memories, reminders and routines) in turns where the
                   person asked for nothing to be kept: the turns UNASKED names per scenario
  trivial_calc     calc calls on single-step arithmetic: two numbers and one operator ("12*12", "219-210")
  rounded_away     turns whose reply restates a calc result of that turn as a different nearby figure
                   ("about 560" after calc said 557): a later step may then use either figure
  failed_checks    the scenario's own deterministic checks that failed (grade.run_checks), with names

  python3 tools/refloop/habits.py --evidence <dir> [--arm A] [--json out.json]

<dir> holds <model>/<scenario>/repN/<arm>/turns.json, as compare.py stores them. One line per run,
then the totals per model and scenario.
"""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path

import grade
import scenario

SCENARIO_DIR = Path(__file__).resolve().parents[2] / "tests" / "evals" / "refloop" / "scenarios"

# Turns (first, last; 1-based owner turns) where the person asked for nothing to be saved, kept or scheduled.
UNASKED = {
    "long-chat": (1, 22),  # "No need to comment on it, just say you have it."
    "recent-compare-food": (1, 4),
    "recent-compare-plain": (1, 4),
    "memory-20-turns": (2, 24),  # turn 1 asks to remember two facts
}
# Calls that change what is kept, in either engine's names; a call whose op is a read is not a write.
WRITES = {
    "write_file", "append_file", "edit_file", "delete_file", "note", "schedule",
    "artifact_save", "files_append", "files_edit", "files_delete", "memory_write", "memory_update", "memory_pin",
    "loop_create", "loop_cancel", "prefs_edit", "openq_edit",
}
READ_OPS = {"list", "view"}
ONE_STEP = re.compile(r"^\(?-?\d+(?:\.\d+)?\)?\s*[-+*/x×÷]\s*\(?-?\d+(?:\.\d+)?\)?$")
NUMBER = re.compile(r"(?<![\w.])-?\d{1,3}(?:,\d{3})+(?:\.\d+)?|(?<![\w.])-?\d+(?:\.\d+)?")


def is_write(call: dict) -> bool:
    if call.get("name") not in WRITES:
        return False
    args = call.get("args") or {}
    return str(args.get("op") or args.get("action") or "").lower() not in READ_OPS


def is_trivial_calc(call: dict) -> bool:
    expr = str((call.get("args") or {}).get("expression", "")).replace(" ", "")
    return call.get("name") == "calc" and bool(ONE_STEP.match(expr))


def _calc_value(call: dict):
    m = re.match(r"^=\s*(-?[\d.]+)", str(call.get("result", "")).strip())
    return float(m.group(1)) if m else None


def _figures(text: str) -> list:
    """(value, decimals shown) of each figure in a text."""
    return [(float(m.replace(",", "")), len(m.split(".")[1]) if "." in m else 0) for m in NUMBER.findall(text or "")]


def rounded_away(step: dict) -> bool:
    """The reply states a figure within 3% of a calc result of the same turn that is not that result rounded
    to the decimals shown ("about 560" after calc said 557; "27.38" after 27.380952 is the result), and
    never states the result itself."""
    said = _figures(step.get("reply", ""))
    for call in step.get("tools") or []:
        value = _calc_value(call) if call.get("name") == "calc" else None
        if value is None or abs(value) < 10 or any(round(value, d) == n for n, d in said):
            continue
        if any(abs(n - value) <= 0.03 * abs(value) for n, _ in said):
            return True
    return False


def scan_run(record: dict, sc: dict | None) -> dict:
    """The three habits and the failed checks of one stored run."""
    turns = [s for s in record.get("steps", []) if s.get("kind") in ("say", "burst")]
    first, last = UNASKED.get(record.get("scenario", ""), (0, -1))
    out = {"turns": len(turns), "unasked_writes": 0, "trivial_calc": 0, "rounded_away": 0, "checks": 0,
           "failed_checks": [], "cost_usd": round(record.get("cost_usd") or 0, 4),
           "requests": sum(s.get("head_requests", 0) for s in turns), "where": defaultdict(list)}
    for n, step in ((s.get("turn", i), s) for i, s in enumerate(turns, 1)):
        every = step.get("tools") or []
        calls = [c for c in every if not c.get("via")]
        if first <= n <= last:
            w = sum(1 for c in every if is_write(c))  # with the writes an engine makes from reply fields
            out["unasked_writes"] += w
            if w:
                out["where"]["unasked_writes"].append(n)
        t = sum(1 for c in calls if is_trivial_calc(c))
        out["trivial_calc"] += t
        if t:
            out["where"]["trivial_calc"].append(n)
        if rounded_away(step):
            out["rounded_away"] += 1
            out["where"]["rounded_away"].append(n)
    if sc:
        checks = grade.run_checks(sc, record)
        out["checks"] = len(checks)
        out["failed_checks"] = [f"turn {c['turn']} {c['check']}" for c in checks if not c["pass"]]
    out["where"] = dict(out["where"])
    return out


def summarize(evidence: Path, arm: str = "A") -> dict:
    scenarios = {s["id"]: s for s in scenario.load_all([SCENARIO_DIR])}
    runs = {}
    for path in sorted(evidence.glob(f"*/*/rep*/{arm}/turns.json")):
        record = json.loads(path.read_text())
        sid = record.get("scenario") or path.parts[-4]
        record.setdefault("scenario", sid)
        runs[str(path.parent.relative_to(evidence))] = scan_run(record, scenarios.get(sid))
    return runs


def table(runs: dict) -> str:
    head = "| run | turns | requests | USD | unasked writes | trivial calc | rounded away | checks failed |"
    out = [head, "|---|---|---|---|---|---|---|---|"]
    for name, r in runs.items():
        failed = f"{len(r['failed_checks'])} of {r['checks']}" + (f" ({', '.join(r['failed_checks'])})" if r["failed_checks"] else "")
        out.append(f"| {name} | {r['turns']} | {r['requests']} | {r['cost_usd']:.4f} | {r['unasked_writes']} | "
                   f"{r['trivial_calc']} | {r['rounded_away']} | {failed} |")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--evidence", type=Path, required=True)
    ap.add_argument("--arm", default="A")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    runs = summarize(args.evidence, args.arm)
    if args.json:
        args.json.write_text(json.dumps(runs, indent=1))
    print(table(runs), end="")


if __name__ == "__main__":
    main()
