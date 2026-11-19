#!/usr/bin/env python3
"""Run the three validation families on the refloop rig and print the parity table (PAID).

    python3 tests/evals/families/run_families.py run --evidence <dir> --env-file <dotenv> \
        --models claude-haiku-5-5 [--arms A,B] [--reps 1] [--max-usd 3] [--domains food,expenses,training]
    python3 tests/evals/families/run_families.py table --evidence <dir> [--out FAMILIES.md]

`run` is tools/refloop/compare.py run over tests/evals/refloop/families/<domain> with the judge off
(the families are decided by their checks: file lines and replies). `table` grades every stored arm
record with the rig's checks, any arm (A the harness, B the reference loop, X the counter-test arm
whose appends write twice), and prints per head, domain and arm the checks passed and the
scenarios with every check green.

Parity (green) for a head and a domain: the harness's check pass rate is at least the loop's less
5 points, and the counter-test arm X, where it ran, is red (it fails the family: some scenario
that appends has a failed check).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
RIG = REPO / "tools" / "refloop"
FAMILIES = REPO / "tests" / "evals" / "refloop" / "families"
DOMAINS = ("food", "expenses", "training")
sys.path.insert(0, str(RIG))


def cmd_run(args) -> int:
    dirs = [str(FAMILIES / d) for d in args.domains.split(",")]
    cmd = [
        sys.executable,
        str(RIG / "compare.py"),
        "run",
        "--evidence",
        str(args.evidence),
        "--env-file",
        args.env_file,
        "--models",
        args.models,
        "--reps",
        str(args.reps),
        "--arms",
        args.arms,
        "--parallel",
        str(args.parallel),
        "--max-usd",
        str(args.max_usd),
        "--no-judge",
        "--scenarios",
        *dirs,
    ]
    if args.only:
        cmd += ["--only", args.only]
    return subprocess.call(cmd)


def collect(evidence: Path) -> list:
    import grade
    import scenario

    by_id = {s["id"]: s for s in scenario.load_all([FAMILIES / d for d in DOMAINS])}
    rows = []
    for rec_path in sorted(evidence.glob("*/*/rep*/*/turns.json")):
        arm_dir = rec_path.parent
        sid, model = arm_dir.parent.parent.name, arm_dir.parent.parent.parent.name
        if sid not in by_id:
            continue
        rec = json.loads(rec_path.read_text())
        checks = grade.run_checks(by_id[sid], rec)
        rows.append(
            {
                "model": model,
                "domain": sid.split("-", 1)[0],
                "scenario": sid,
                "rep": arm_dir.parent.name,
                "arm": arm_dir.name,
                "passed": sum(c["pass"] for c in checks),
                "total": len(checks),
                "failed": [f"{c['turn']}:{c['check']} {c['seen'][:120]}" for c in checks if not c["pass"]],
                "error": rec.get("error", ""),
                "cost": rec.get("cost_usd", 0.0),
            }
        )
    return rows


def arm_cell(rows: list) -> dict | None:
    """Checks passed and run, and how many runs had every check green, over one arm's runs."""
    if not rows:
        return None
    p, t = sum(r["passed"] for r in rows), sum(r["total"] for r in rows)
    green = sum(r["passed"] == r["total"] for r in rows)
    return {"passed": p, "total": t, "rate": p / max(t, 1), "green": green, "runs": len(rows)}


def verdicts(rows: list) -> list:
    out = []
    for model in sorted({r["model"] for r in rows}):
        for dom in DOMAINS:
            mine = [r for r in rows if r["model"] == model and r["domain"] == dom]
            cell = {arm: c for arm in ("A", "B", "X") if (c := arm_cell([r for r in mine if r["arm"] == arm]))}
            if not cell:
                continue
            parity = cell["A"]["rate"] >= cell["B"]["rate"] - 0.05 if "A" in cell and "B" in cell else None
            counter = cell["X"]["green"] < cell["X"]["runs"] if "X" in cell else None
            out.append({"model": model, "domain": dom, "cell": cell, "parity": parity, "counter_red": counter})
    return out


def table(rows: list) -> str:
    lines = [
        "| head | domain | harness A: checks, scenarios all green | loop B | counter arm X (appends twice) | parity |",
        "|---|---|---|---|---|---|",
    ]
    fmt = lambda c: f"{c['passed']}/{c['total']} ({c['rate']:.0%}), {c['green']}/{c['runs']}" if c else "not run"  # noqa: E731
    for v in verdicts(rows):
        c = v["cell"]
        par = "not run" if v["parity"] is None else ("GREEN" if v["parity"] else "RED")
        xr = (
            ""
            if v["counter_red"] is None
            else (" RED as it must be" if v["counter_red"] else " GREEN: the suite cannot see it")
        )
        lines.append(
            f"| {v['model']} | {v['domain']} | {fmt(c.get('A'))} | {fmt(c.get('B'))} | {fmt(c.get('X'))}{xr} | {par} |"
        )
    lines += ["", "Failed checks:", ""]
    for r in sorted(rows, key=lambda r: (r["model"], r["arm"], r["scenario"], r["rep"])):
        for f in r["failed"]:
            lines.append(f"- {r['model']} {r['arm']} {r['scenario']} {r['rep']}: {f}")
        if r["error"]:
            lines.append(f"- {r['model']} {r['arm']} {r['scenario']} {r['rep']}: run error {r['error'][:160]}")
    return "\n".join(lines) + "\n"


def cmd_table(args) -> int:
    rows = collect(args.evidence)
    text = table(rows)
    if args.out:
        Path(args.out).write_text(text)
    print(text)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    run = sub.add_parser("run")
    run.add_argument("--evidence", type=Path, required=True)
    run.add_argument("--env-file", required=True)
    run.add_argument("--models", default="claude-haiku-5-5")
    run.add_argument("--arms", default="A,B")
    run.add_argument("--reps", type=int, default=1)
    run.add_argument("--parallel", type=int, default=6)
    run.add_argument("--max-usd", type=float, default=3.0)
    run.add_argument("--domains", default=",".join(DOMAINS))
    run.add_argument("--only")
    tab = sub.add_parser("table")
    tab.add_argument("--evidence", type=Path, required=True)
    tab.add_argument("--out")
    args = ap.parse_args(argv)
    return cmd_run(args) if args.cmd == "run" else cmd_table(args)


if __name__ == "__main__":
    sys.exit(main())
