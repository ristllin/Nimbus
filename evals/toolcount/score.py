#!/usr/bin/env python3
"""Reduce a tool-count corpus (`*.jsonl`) to a summary.

Groups rows by (model, condition) and by subset, and computes the headline
metrics: tool-selection accuracy (the hard signal), wrong-call rate, no-tool
rate, mean prompt tokens (the context-bloat proxy), mean search hops (the lazy
latency tax), spend, and the soft judge score. Writes `summary.json`.

Usage:
    python3 evals/toolcount/score.py <corpus.jsonl> [--out summary.json] \
        [--run <name>] [--date YYYY-MM-DD]
"""

from __future__ import annotations

import argparse
import json
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

SCHEMA_VERSION = 1


def read_corpus(path: str | Path) -> list[dict[str, Any]]:
    rows = []
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if line:
            rows.append(json.loads(line))
    return rows


def _mean(xs: Iterable[float]) -> float:
    xs = list(xs)
    return round(statistics.fmean(xs), 4) if xs else 0.0


def _rate(rows: list[dict[str, Any]], flag: str) -> float:
    return _mean(1.0 if r[flag] else 0.0 for r in rows)


def _missed(r: dict[str, Any]) -> bool:
    """Expected a tool, called none, and did not error out."""
    return r["expected_tool"] is not None and r["selected_tool"] is None and not r["error"]


def _judges(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [r["judge"] for r in rows if r.get("judge") and "score" in r["judge"]]


def _cost_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "mean_prompt_tokens": round(_mean(r["prompt_tokens"] for r in rows), 1),
        "mean_total_tokens": round(_mean(r["total_tokens"] for r in rows), 1),
        "mean_search_hops": _mean(r["search_hops"] for r in rows),
        "mean_latency_ms": round(_mean(r["latency_ms"] for r in rows), 1),
        "usd_total": round(sum(r["usd_est"] for r in rows), 6),
        "errors": sum(1 for r in rows if r["error"]),
    }


def _judge_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    judged = _judges(rows)
    if not judged:
        return {"judge_mean_score": None, "judge_appropriate_rate": None}
    return {
        "judge_mean_score": _mean(j["score"] for j in judged),
        "judge_appropriate_rate": _mean(1.0 if j.get("appropriate") else 0.0 for j in judged),
    }


def _cell(rows: list[dict[str, Any]]) -> dict[str, Any]:
    n = len(rows)
    if n == 0:
        return {"n": 0}
    tool_expected = [r for r in rows if r["expected_tool"] is not None]
    no_tool = [r for r in rows if r["expected_tool"] is None]
    cell = {
        "n": n,
        "selection_accuracy": _rate(rows, "selected_correct"),
        "wrong_call_rate": _rate(rows, "wrong_call"),
        "miss_rate": _mean(1.0 if _missed(r) else 0.0 for r in tool_expected),
        "overtrigger_rate": _mean(1.0 if r["selected_tool"] is not None else 0.0 for r in no_tool),
    }
    cell.update(_cost_metrics(rows))
    cell.update(_judge_metrics(rows))
    return cell


def summarize(rows: list[dict[str, Any]], run: str = "", date: str = "") -> dict[str, Any]:
    by_mc: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for r in rows:
        by_mc[(r["model"], r["condition"])].append(r)

    models = sorted({r["model"] for r in rows})
    conditions = sorted({r["condition"] for r in rows}, key=lambda c: {"full": 0, "curated": 1, "lazy": 2}.get(c, 9))
    subsets = sorted({r.get("subset", "core") for r in rows})

    cells: dict[str, dict[str, Any]] = {}
    for (model, cond), rs in by_mc.items():
        key = f"{model}|{cond}"
        entry = {"overall": _cell(rs)}
        for sub in subsets:
            entry[f"subset:{sub}"] = _cell([r for r in rs if r.get("subset", "core") == sub])
        cells[key] = entry

    return {
        "schema_version": SCHEMA_VERSION,
        "run": run,
        "date": date,
        "models": models,
        "conditions": conditions,
        "subsets": subsets,
        "rows": len(rows),
        "usd_total": round(sum(r["usd_est"] for r in rows), 6),
        "cells": cells,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Summarize a tool-count corpus")
    ap.add_argument("corpus")
    ap.add_argument("--out", default="")
    ap.add_argument("--run", default="")
    ap.add_argument("--date", default="")
    args = ap.parse_args(argv)
    rows = read_corpus(args.corpus)
    summary = summarize(rows, args.run, args.date)
    out = args.out or str(Path(args.corpus).with_suffix(".summary.json"))
    Path(out).write_text(json.dumps(summary, indent=2) + "\n")
    print(f"wrote summary -> {out} ({summary['rows']} rows, ${summary['usd_total']:.4f})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
