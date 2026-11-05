"""Summaries and the markdown report over saved run records."""

from __future__ import annotations

import datetime as dt
import json
import re
from pathlib import Path

import fcrun
import fcworld

REC_NAME = re.compile(r"^rep\d+\.json$")


def load_records(label_dir: Path) -> list[dict]:
    recs = []
    for path in sorted(label_dir.glob("*/rep*.json")):
        if REC_NAME.match(path.name):
            recs.append(json.loads(path.read_text(encoding="utf-8")))
    return [fcrun.with_device_lines(r) for r in recs if "skipped" not in r]


def _failed_counts(mine: list[dict], key: str) -> dict:
    counts: dict = {}
    for r in mine:
        for cid in r["verdict"].get(key, []):
            counts[cid] = counts.get(cid, 0) + 1
    return dict(sorted(counts.items()))


def _count(mine: list[dict], test) -> int:
    return sum(1 for r in mine if test(r))


def _scenario_row(scn: dict, recs: list[dict], target: int) -> dict:
    mine = [r for r in recs if r["scenario"] == scn["id"]]
    agree = [r["verdict"]["judges_agree"] for r in mine if r["verdict"]["judges_agree"] is not None]
    passes = _count(mine, lambda r: r["verdict"]["pass"])
    return {
        "title": scn["title"],
        "kind": scn["kind"],
        "runs": len(mine),
        "pass": passes,
        "pass_strict": _count(mine, lambda r: r["verdict"]["pass_strict"]),
        "errors": _count(mine, lambda r: r["verdict"]["errored"]),
        "meets_target": passes >= target,
        "failed_criteria": _failed_counts(mine, "failed"),
        "advisory_failed": _failed_counts(mine, "advisory_failed"),
        "judges_agree": f"{sum(agree)}/{len(agree)}" if agree else "n/a",
        "device_lines": _count(mine, lambda r: r.get("device_lines")),
    }


def summarize(recs: list[dict], label: str, cfg: dict) -> dict:
    target = cfg["pass_target_reps"]
    scenarios = {s["id"]: _scenario_row(s, recs, target) for s in fcworld.load_scenarios()}
    scenarios = {k: v for k, v in scenarios.items() if v["runs"]}
    head = sum(r["cost_usd"]["head"]["total"] for r in recs)
    judges = sum(r["cost_usd"]["judges"] for r in recs)
    first = recs[0] if recs else {}
    return {
        "label": label,
        "generated": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%MZ"),
        "host": first.get("host", ""),
        "model": first.get("model", ""),
        "judges": [j["judge"] for j in first.get("judges", [])],
        "pass_target_reps": target,
        "scenarios": scenarios,
        "totals": {
            "runs": len(recs),
            "pass": sum(v["pass"] for v in scenarios.values()),
            "pass_strict": sum(v["pass_strict"] for v in scenarios.values()),
            "device_lines": sum(v["device_lines"] for v in scenarios.values()),
            "errors": sum(v["errors"] for v in scenarios.values()),
            "scenarios_meeting_target": sum(1 for v in scenarios.values() if v["meets_target"]),
            "scenarios": len(scenarios),
        },
        "cost_usd": {"head": round(head, 4), "judges": round(judges, 4), "total": round(head + judges, 4)},
    }


def render_markdown(summary: dict) -> str:
    t = summary["totals"]
    lines = [
        f"# Food coach eval: {summary['label']}",
        "",
        f"Head `{summary['host']}/{summary['model']}`; judges {', '.join(f'`{j}`' for j in summary['judges'])}; "
        f"target {summary['pass_target_reps']} passing runs per scenario.",
        "",
        "| scenario | kind | runs | pass | strict | errors | target | judges agree | failed criteria (runs) |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for sid, v in summary["scenarios"].items():
        failed = ", ".join(f"{k} ({n})" for k, n in v["failed_criteria"].items()) or "none"
        if v["advisory_failed"]:
            failed += "; advisory: " + ", ".join(f"{k} ({n})" for k, n in v["advisory_failed"].items())
        lines.append(
            f"| {sid} | {v['kind']} | {v['runs']} | {v['pass']} | {v['pass_strict']} | {v['errors']} | "
            f"{'met' if v['meets_target'] else 'MISSED'} | {v['judges_agree']} | {failed} |"
        )
    cost = summary["cost_usd"]
    lines += [
        "",
        f"Totals: {t['pass']} of {t['runs']} runs pass ({t['pass_strict']} strict), {t['errors']} errored; "
        f"{t['scenarios_meeting_target']} of {t['scenarios']} scenarios meet the target.",
        f"Runs whose delivered reply carries a device line: {t['device_lines']} of {t['runs']}.",
        f"Spend: head ${cost['head']:.4f}, judges ${cost['judges']:.4f}, total ${cost['total']:.4f}.",
        "",
    ]
    return "\n".join(lines)


def render_comparison(before: dict, after: dict) -> str:
    lines = [
        f"# Food coach eval: {before['label']} to {after['label']}",
        "",
        "| scenario | before | after | target |",
        "|---|---|---|---|",
    ]
    for sid, b in before["scenarios"].items():
        a = after["scenarios"].get(sid)
        if not a:
            continue
        lines.append(
            f"| {sid} | {b['pass']}/{b['runs']} | {a['pass']}/{a['runs']} | {'met' if a['meets_target'] else 'MISSED'} |"
        )
    return "\n".join(lines) + "\n"
