#!/usr/bin/env python3
"""Render a tool-count summary as a text report.

Prints, per model, a condition x metric table (full / curated / lazy) plus the
core-subset selection accuracy that is the fair cross-condition comparison, and
the coverage-subset accuracy that exposes over-curation. Reads a summary.json
(from `score.py`) or a corpus (`*.jsonl`, summarized on the fly).

Usage:
    python3 evals/toolcount/report.py <summary.json | corpus.jsonl>
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import score as S

_ORDER = ["full", "curated", "lazy"]


def _fmt(v, pct=False):
    if v is None:
        return "  -  "
    if pct:
        return f"{100 * v:4.0f}%"
    return f"{v:5.1f}"


def render(summary: dict) -> str:
    out: list[str] = []
    out.append("# Tool-count benchmark report")
    out.append(f"run={summary.get('run') or '-'}  date={summary.get('date') or '-'}"
               f"  rows={summary['rows']}  spend=${summary['usd_total']:.4f}")
    out.append("")
    conds = [c for c in _ORDER if c in summary["conditions"]]
    for model in summary["models"]:
        out.append(f"## {model}")
        header = f"{'metric':<26}" + "".join(f"{c:>10}" for c in conds)
        out.append(header)
        out.append("-" * len(header))

        def row(label, key, sub="overall", pct=False):
            cells = [summary["cells"].get(f"{model}|{c}", {}).get(sub, {}) for c in conds]
            vals = "".join(f"{_fmt(cell.get(key), pct):>10}" for cell in cells)
            out.append(f"{label:<26}{vals}")

        row("selection acc (all)", "selection_accuracy", pct=True)
        row("selection acc (core)", "selection_accuracy", sub="subset:core", pct=True)
        row("selection acc (coverage)", "selection_accuracy", sub="subset:coverage", pct=True)
        row("overtrigger (no-tool)", "overtrigger_rate", sub="subset:none", pct=True)
        row("wrong-call rate", "wrong_call_rate", pct=True)
        row("miss rate", "miss_rate", pct=True)
        row("mean prompt tokens", "mean_prompt_tokens")
        row("mean search hops", "mean_search_hops")
        row("mean latency ms", "mean_latency_ms")
        row("judge score", "judge_mean_score", pct=True)
        row("errors", "errors")
        row("spend usd", "usd_total")
        out.append("")

    out.append("Legend: core = expected tool is in the curated set (fair "
               "cross-condition compare); coverage = expected tool was cut from "
               "curated (so curated cannot answer); overtrigger = called a tool "
               "when none was needed. Lower is better for wrong-call, miss, "
               "overtrigger, tokens, hops, latency, spend.")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Report a tool-count summary")
    ap.add_argument("path", help="summary.json or corpus.jsonl")
    args = ap.parse_args(argv)
    p = Path(args.path)
    text = p.read_text()
    if p.suffix == ".jsonl" or "\n{" in text.strip()[:200] or text.lstrip().startswith("{\"task"):
        summary = S.summarize(S.read_corpus(p))
    else:
        obj = json.loads(text)
        summary = obj if "cells" in obj else S.summarize(S.read_corpus(p))
    print(render(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
