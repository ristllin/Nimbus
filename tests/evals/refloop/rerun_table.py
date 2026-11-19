#!/usr/bin/env python3
"""The before and after table of the reference comparison (lane RB-REC7, orchestrator note 5).

Reads two summary.json files written by tools/refloop/report.py (the 17 Nov baseline and a rerun) and prints,
per head and in total, the harness's wins, ties and losses, composite, checks passed, judge mean, the judge's
two-pass preferences, latency, head requests and prompt tokens per turn, over the scenarios both runs hold
(or a named subset), beside each other. No model call.

    python3 tests/evals/refloop/rerun_table.py --before <baseline summary.json> --after <rerun summary.json> \
        [--only id,id] [--without id,id] [--per-scenario]

"checks passed" is every deterministic check of those repetitions, passed over run, read from each run's
grade.json beside its summary.json (the figure RBREF-REPORT.md gives).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HEADS = ("claude-sonnet-5-5", "gpt-5.6-luna", "claude-haiku-5-5")


def _wmean(cells: list, side: str, key: str, weight: str) -> float | None:
    pairs = [(c[side][key], c[weight]) for c in cells if c[side].get(key) is not None and c[weight]]
    total = sum(w for _, w in pairs)
    return sum(v * w for v, w in pairs) / total if total else None


def _stat(cells: list, side: str, key: str) -> float | None:
    pairs = [(c["stats"][side][key], c["stats"][side]["turns"]) for c in cells if c["stats"][side].get("turns")]
    total = sum(w for _, w in pairs)
    return sum(v * w for v, w in pairs) / total if total else None


def aggregate(cells: list) -> dict:
    """Scenario cells (summary.json models[m].scenarios[id]) summed and rep- or turn-weighted."""
    out = {k: sum(c[k] for c in cells) for k in ("reps", "win", "tie", "loss")}
    for side in ("A", "B"):
        out[side] = {k: _wmean(cells, side, k, "reps") for k in ("composite", "check_rate", "judge_mean")}
        if all("checks" in c[side] for c in cells):
            passed, total = (sum(c[side]["checks"][n] for c in cells) for n in (0, 1))
            out[side]["check_rate"] = passed / total if total else None
        out[side].update(
            {k: _stat(cells, side, k) for k in ("latency_mean_s", "requests_per_turn", "input_tokens_per_turn")}
        )
    out["prefers"] = {k: sum(c["judge_prefers"].get(k, 0) for c in cells) for k in ("A", "B", "split")}
    return out


def cells_of(summary: dict, head: str, ids: list) -> list:
    """The scenario cells of a head, each with its checks passed and run from the grade.json files."""
    sc = summary["models"].get(head, {}).get("scenarios", {})
    root = summary.get("_dir")
    out = []
    for i in ids:
        if i not in sc:
            continue
        cell = dict(sc[i])
        if root:
            for side in ("A", "B"):
                passed = total = 0
                for g in (root / head / i).glob("rep*/grade.json"):
                    arm = json.loads(g.read_text())["arms"].get(side, {})
                    passed, total = passed + arm.get("passed", 0), total + arm.get("total", 0)
                cell[side] = {**cell[side], "checks": (passed, total)}
        out.append(cell)
    return out


def _f(v, fmt: str) -> str:
    return "-" if v is None else format(v, fmt)


def row(label: str, a: dict) -> str:
    A, B, p = a["A"], a["B"], a["prefers"]
    return (
        f"| {label} | {a['reps']} | {a['win']} / {a['tie']} / {a['loss']} | {_f(A['composite'], '.2f')} / {_f(B['composite'], '.2f')} "
        f"| {_f(A['check_rate'], '.1%')} / {_f(B['check_rate'], '.1%')} | {_f(A['judge_mean'], '.2f')} / {_f(B['judge_mean'], '.2f')} "
        f"| {p['A']} / {p['B']}, {p['split']} | {_f(A['latency_mean_s'], '.2f')} / {_f(B['latency_mean_s'], '.2f')} "
        f"| {_f(A['requests_per_turn'], '.2f')} / {_f(B['requests_per_turn'], '.2f')} "
        f"| {_f(A['input_tokens_per_turn'], ',.0f')} / {_f(B['input_tokens_per_turn'], ',.0f')} |"
    )


HEADER = (
    "| head, run | reps | harness W / T / L | composite A / B | checks passed A / B | judge mean A / B "
    "| both passes prefer A / B, split | latency s/turn A / B | head requests/turn A / B | prompt tokens/turn A / B |\n"
    "|---|---|---|---|---|---|---|---|---|---|"
)


def table(before: dict, after: dict, ids: list) -> list:
    lines = [HEADER]
    tot = {"before": [], "after": []}
    for head in HEADS:
        for name, summary in (("before", before), ("after", after)):
            cells = cells_of(summary, head, ids)
            if cells:
                lines.append(row(f"{head}, {'17 Nov' if name == 'before' else 'rerun'}", aggregate(cells)))
                tot[name] += cells
    for name in ("before", "after"):
        if tot[name]:
            lines.append(row(f"**all three, {'17 Nov' if name == 'before' else 'rerun'}**", aggregate(tot[name])))
    return lines


def per_scenario(before: dict, after: dict, ids: list) -> list:
    head_cols = " | ".join(f"{h}: W/T/L before -> after, composite A/B after" for h in HEADS)
    lines = [f"| scenario | {head_cols} |", "|---|" + "---|" * len(HEADS)]
    for sid in ids:
        cells = []
        for h in HEADS:
            b, a = cells_of(before, h, [sid]), cells_of(after, h, [sid])
            fb = f"{b[0]['win']}/{b[0]['tie']}/{b[0]['loss']}" if b else "-"
            fa = (
                f"{a[0]['win']}/{a[0]['tie']}/{a[0]['loss']}, {_f(a[0]['A']['composite'], '.2f')} / {_f(a[0]['B']['composite'], '.2f')}"
                if a
                else "not run"
            )
            cells.append(f"{fb} -> {fa}")
        lines.append(f"| {sid} | " + " | ".join(cells) + " |")
    return lines


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--before", type=Path, required=True)
    ap.add_argument("--after", type=Path, required=True)
    ap.add_argument("--only")
    ap.add_argument("--without")
    ap.add_argument("--per-scenario", action="store_true")
    args = ap.parse_args(argv)
    before, after = json.loads(args.before.read_text()), json.loads(args.after.read_text())
    before["_dir"], after["_dir"] = args.before.parent, args.after.parent
    ids = args.only.split(",") if args.only else list(before.get("common_scenarios", []))
    if args.without:
        ids = [i for i in ids if i not in args.without.split(",")]
    print("\n".join(per_scenario(before, after, ids) if args.per_scenario else table(before, after, ids)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
