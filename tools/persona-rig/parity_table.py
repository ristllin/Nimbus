#!/usr/bin/env python3
"""parity_table - the parity eval's table, read from a persona rig evidence directory.

One row per persona, one column per scenario. A cell is PASS or FAIL from the rig's hard
assertions, with the judge's lowest score when the run had one, and it links to the step
evidence. The rig decides every verdict: this only renders summary.json files.

Usage: parity_table.py EVIDENCE_DIR [--link-prefix PATH] > table.md
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def load_summaries(root: Path) -> list[dict]:
    out = []
    for path in sorted(root.glob("*/summary.json")):
        data = json.loads(path.read_text())
        data["_dir"] = path.parent.name
        out.append(data)
    return out


def scenario_rows(summary: dict) -> dict:
    scenarios = summary.get("scenarios", {})
    if isinstance(scenarios, list):
        return {s.get("id", "?"): s for s in scenarios}
    return scenarios


def judge_low(scenario: dict):
    score = (scenario.get("judge") or {}).get("min_score")
    return score if isinstance(score, (int, float)) else None


def cell(scenario: dict | None, link: str) -> str:
    if not scenario:
        return "-"
    verdict = "PASS" if scenario.get("pass") else "FAIL"
    score = judge_low(scenario)
    text = f"{verdict} {score:.2f}" if score is not None else verdict
    return f"[{text}]({link})"


def render(summaries: list[dict], prefix: str) -> str:
    columns: list[str] = []
    for summary in summaries:
        for sid in scenario_rows(summary):
            if sid not in columns:
                columns.append(sid)
    lines = ["| Model | " + " | ".join(columns) + " |", "|---|" + "---|" * len(columns)]
    for summary in summaries:
        rows = scenario_rows(summary)
        cells = [cell(rows.get(c), f"{prefix}{summary['_dir']}/{c}/") for c in columns]
        lines.append(f"| {summary['_dir']} | " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence")
    parser.add_argument("--link-prefix", default="")
    args = parser.parse_args(argv)
    summaries = load_summaries(Path(args.evidence))
    if not summaries:
        print("no summary.json under " + args.evidence, file=sys.stderr)
        return 2
    sys.stdout.write(render(summaries, args.link_prefix))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
