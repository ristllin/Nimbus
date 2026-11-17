#!/usr/bin/env python3
"""report - turn the stored runs and grades into tables.

Reads <evidence>/<model>/<scenario>/repN/{A,B}/turns.json and grade.json, writes
<evidence>/summary.json (every number) and <evidence>/RESULTS.md (the tables):
overall per model, per scenario per model, and one line per repetition the harness
lost, with the checks it failed and where its evidence is.

Arm A is the Nimbus harness, arm B the reference loop. Win, tie and loss are from A's side.
"""

from __future__ import annotations

import json
import statistics
from pathlib import Path

from grade import RUBRIC


def _turns(rec: dict) -> list:
    return [s for s in rec.get("steps", []) if s.get("kind") in ("say", "burst")]


def _per_turn(turns: list, key: str, digits: int = 2):
    return round(sum(t.get(key) or 0 for t in turns) / len(turns), digits)


def _failed(turn: dict) -> bool:
    return bool(turn.get("error") or turn.get("timed_out") or not turn.get("replies"))


def arm_stats(recs: list) -> dict:
    """Per-turn averages over the runs of one arm."""
    turns = [t for r in recs for t in _turns(r)]
    if not turns:
        return {}
    n = len(turns)
    tok = {k: sum(t.get("tokens", {}).get(k, 0) for t in turns) for k in ("input", "cached", "output")}
    latency = sorted(t["latency_s"] for t in turns)
    return {
        "turns": n,
        "latency_mean_s": round(statistics.mean(latency), 2),
        "latency_median_s": round(statistics.median(latency), 2),
        "latency_p90_s": round(latency[int(0.9 * (n - 1))], 2),
        "requests_per_turn": _per_turn(turns, "head_requests"),
        "other_requests_per_turn": _per_turn(turns, "other_requests"),
        "input_tokens_per_turn": round(tok["input"] / n),
        "output_tokens_per_turn": round(tok["output"] / n),
        "cache_share": round(tok["cached"] / tok["input"], 3) if tok["input"] else 0.0,
        "cost_per_turn_usd": _per_turn(turns, "cost_usd", 5),
        "cold_cost_per_turn_usd": _per_turn(turns, "cold_cost_usd", 5),
        "cost_total_usd": round(sum(r.get("cost_usd", 0) for r in recs), 4),
        "failed_turns": sum(1 for t in turns if _failed(t)),
        "tool_calls_per_turn": round(sum(len(t.get("tools") or []) for t in turns) / n, 2),
    }


def _arm_scores(grades: list, arm: str) -> dict:
    rows = [g["arms"][arm] for g in grades]
    judged = [g["judge"][arm] for g in grades if g.get("judge")]
    out = {
        "composite": round(statistics.mean(r["composite"] for r in rows), 3),
        "check_rate": round(sum(r["passed"] for r in rows) / max(sum(r["total"] for r in rows), 1), 3),
        "judge_mean": None,
        **{k: None for k in RUBRIC},
    }
    if judged:
        out["judge_mean"] = round(statistics.mean(sum(j[k] for k in RUBRIC) / len(RUBRIC) for j in judged), 2)
        out.update({k: round(statistics.mean(j[k] for j in judged), 2) for k in RUBRIC})
    return out


def grade_stats(grades: list) -> dict:
    """Outcome counts and score means over a set of graded repetitions."""
    if not grades:
        return {}
    prefers = [(g.get("judge") or {}).get("better") for g in grades]
    return {
        "reps": len(grades),
        **{k: sum(1 for g in grades if g["outcome_for_A"] == k) for k in ("win", "tie", "loss")},
        "A": _arm_scores(grades, "A"),
        "B": _arm_scores(grades, "B"),
        "judge_prefers": {k: prefers.count(k) for k in ("A", "B", "tie")},
    }


def collect(evidence: Path) -> dict:
    """{model: {scenario: {"grades": [...], "A": [records], "B": [records]}}} for every graded repetition."""
    data = {}
    for gpath in sorted(evidence.glob("*/*/rep*/grade.json")):
        g = json.loads(gpath.read_text())
        cell = data.setdefault(g["model"], {}).setdefault(g["scenario"], {"grades": [], "A": [], "B": [], "dirs": []})
        cell["grades"].append(g)
        cell["dirs"].append(str(gpath.parent))
        for arm in ("A", "B"):
            cell[arm].append(json.loads((gpath.parent / arm / "turns.json").read_text()))
    return data


def summarize(evidence: Path) -> dict:
    data = collect(evidence)
    summary = {"models": {}}
    for model, cells in data.items():
        per = {}
        for sid, cell in cells.items():
            per[sid] = {**grade_stats(cell["grades"]), "stats": {arm: arm_stats(cell[arm]) for arm in ("A", "B")}}
        every = [g for cell in cells.values() for g in cell["grades"]]
        overall = {
            **grade_stats(every),
            "stats": {arm: arm_stats([r for cell in cells.values() for r in cell[arm]]) for arm in ("A", "B")},
        }
        losses = []
        for sid, cell in cells.items():
            for g, d in zip(cell["grades"], cell["dirs"]):
                if g["outcome_for_A"] == "loss":
                    losses.append(_loss_row(g, d, evidence))
        summary["models"][model] = {"overall": overall, "scenarios": per, "losses": losses}
    return summary


def _loss_row(g: dict, job_dir: str, evidence: Path) -> dict:
    failed = {
        arm: [f"turn {c['turn']} {c['check']}" for c in g["arms"][arm]["checks"] if not c["pass"]] for arm in ("A", "B")
    }
    judge = g.get("judge") or {}
    return {
        "scenario": g["scenario"],
        "rep": g["rep"],
        "evidence": str(Path(job_dir).relative_to(evidence)),
        "A_composite": g["arms"]["A"]["composite"],
        "B_composite": g["arms"]["B"]["composite"],
        "A_failed_checks": failed["A"],
        "B_failed_checks": failed["B"],
        "judge_on_A": (judge.get("A") or {}).get("notes", ""),
        "judge_why": judge.get("why", ""),
    }


def _pair(a, b, fmt="{}") -> str:
    def one(v):
        return "-" if v is None else fmt.format(v)

    return f"{one(a)} / {one(b)}"


HEAD = (
    "| {first} | W/T/L (A) | composite A / B | checks A / B | judge A / B | latency s/turn A / B | "
    "requests/turn A / B | input tok/turn A / B | cache share A / B | output tok/turn A / B | cost $/turn A / B | "
    "no-cache cost $/turn A / B |"
)


def _row(label: str, s: dict) -> str:
    a, b, sa, sb = s["A"], s["B"], s["stats"]["A"], s["stats"]["B"]
    cells = [
        label,
        f"{s['win']}/{s['tie']}/{s['loss']}",
        _pair(a["composite"], b["composite"], "{:.2f}"),
        _pair(a["check_rate"], b["check_rate"], "{:.0%}"),
        _pair(a["judge_mean"], b["judge_mean"], "{:.2f}"),
        _pair(sa["latency_mean_s"], sb["latency_mean_s"], "{:.1f}"),
        _pair(sa["requests_per_turn"], sb["requests_per_turn"], "{:.2f}"),
        _pair(sa["input_tokens_per_turn"], sb["input_tokens_per_turn"], "{:,}"),
        _pair(sa["cache_share"], sb["cache_share"], "{:.0%}"),
        _pair(sa["output_tokens_per_turn"], sb["output_tokens_per_turn"], "{:,}"),
        _pair(sa["cost_per_turn_usd"], sb["cost_per_turn_usd"], "{:.4f}"),
        _pair(sa["cold_cost_per_turn_usd"], sb["cold_cost_per_turn_usd"], "{:.4f}"),
    ]
    return "| " + " | ".join(cells) + " |"


def markdown(summary: dict) -> str:
    out = [
        "# Reference loop comparison: results",
        "",
        "Arm A is the Nimbus harness (nimbusd), arm B the reference loop (refloop.py), same model and same tools.",
        "W/T/L counts repetitions from A's side. composite = half the share of deterministic checks passed, half the",
        "blind judge's mean score scaled to 0..1. Latency, requests, tokens and cost are per owner turn. Cost is as",
        "billed (runs made close together share the provider's cache of the fixed prefix); the no-cache cost is the",
        "same turns priced with every prompt token at the input rate, the bound a cold conversation moves toward.",
        "",
    ]
    for model, m in summary["models"].items():
        o = m["overall"]
        out += [f"## {model}", "", HEAD.format(first="scope"), "|" + "---|" * 12, _row("all scenarios", o), ""]
        ja, jb = o["A"], o["B"]
        out += [
            "Judge criteria (1 to 5), A / B: "
            + ", ".join(f"{k} {_pair(ja[k], jb[k], '{:.2f}')}" for k in RUBRIC)
            + f". Judge preferred A {o['judge_prefers']['A']}, B {o['judge_prefers']['B']}, tie {o['judge_prefers']['tie']}"
            + f" of {o['reps']} repetitions. Failed turns A / B: {o['stats']['A']['failed_turns']} / {o['stats']['B']['failed_turns']}.",
            "",
        ]
        out += [HEAD.format(first="scenario"), "|" + "---|" * 12]
        out += [_row(sid, s) for sid, s in m["scenarios"].items()]
        out += ["", f"### Repetitions the harness lost on {model}", ""]
        if not m["losses"]:
            out.append("None.")
        for row in m["losses"]:
            out.append(
                f"- **{row['scenario']} rep{row['rep']}** (composite A {row['A_composite']:.2f}, B {row['B_composite']:.2f}; "
                f"evidence `{row['evidence']}/`). A failed: {', '.join(row['A_failed_checks']) or 'no check'}. "
                f"B failed: {', '.join(row['B_failed_checks']) or 'no check'}. Judge: {row['judge_why']} "
                f"On A: {row['judge_on_A']}"
            )
        out.append("")
    return "\n".join(out)


def write(evidence: Path) -> dict:
    summary = summarize(evidence)
    (evidence / "summary.json").write_text(json.dumps(summary, indent=1, ensure_ascii=False))
    (evidence / "RESULTS.md").write_text(markdown(summary))
    for model, m in summary["models"].items():
        o = m["overall"]
        if o:
            print(
                f"{model}: {o['reps']} reps, A W/T/L {o['win']}/{o['tie']}/{o['loss']}, "
                f"composite A {o['A']['composite']} B {o['B']['composite']}",
                flush=True,
            )
    return summary
