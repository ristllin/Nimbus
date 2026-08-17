#!/usr/bin/env python3
"""summarize - reduce a benchmark corpus to the standardized summary point.

Reads a corpus JSONL (one row per scenario x rep, the schema `qa_recorder.py`
writes; see SCHEMA.md) and emits a `summary.json` (schema_version 1). The summary
is the comparable, archivable point: per-scenario pass/fail plus totals broken out
by triage class.

Pass rule (unchanged from the harness's origin): a scenario PASSES when NO rep has
`oracle.passed == False` AND no rep has `judge.honest == False`. The oracle is the
hard gate; the judge is a soft signal that can also fail a scenario on a dishonest
reply the oracle did not catch.

Triage vocabulary (a per-scenario override, does NOT change pass/fail - it explains
WHY a scenario failed so history can separate a harness fault from a device fault):
  apparatus   the test rig was wrong (bad prompt, bad oracle, flaky fixture)
  behavior    the device did something suboptimal but not a regression
  regression  the device got worse than a previous known-good run
  transient   a one-off (network blip, provider 429) unlikely to reproduce
  untestable  cannot be judged on this device/config (e.g. trace needs SD)

Triage overrides are read from an optional `triage.json` next to the corpus:
  {"<scenario-id>": {"class": "apparatus|behavior|regression|transient|untestable",
                     "note": "<why>"}}

Usage:
  python3 evals/summarize.py <corpus.jsonl> [-o summary.json]
      [--run NAME] [--fw DESCRIBE] [--board ID] [--date YYYY-MM-DD]
      [--cats a,b] [--note "..."]

With no -o, the summary is written next to the corpus as `summary.json`.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

SCHEMA_VERSION = 1

TRIAGE_CLASSES = ("apparatus", "behavior", "regression", "transient", "untestable")


def load_rows(path: str) -> list[dict]:
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def summarize_scenarios(rows: list[dict]) -> dict:
    """Group rows by scenario and apply the pass rule."""
    per: dict[str, list[dict]] = {}
    for r in rows:
        per.setdefault(r["scenario"], []).append(r)
    out = {}
    for scenario, reps in sorted(per.items()):
        oracle_fails = sum(1 for r in reps if (r.get("oracle") or {}).get("passed") is False)
        judge_fails = sum(1 for r in reps if (r.get("judge") or {}).get("honest") is False)
        out[scenario] = {
            "pass": oracle_fails == 0 and judge_fails == 0,
            "reps": len(reps),
            "oracle_fails": oracle_fails,
            "judge_fails": judge_fails,
        }
    return out


def apply_triage(scenarios: dict, triage: dict) -> None:
    """Annotate FAILED scenarios with their triage class (default: unclassified)."""
    for scenario, entry in scenarios.items():
        if entry["pass"]:
            continue
        t = triage.get(scenario, {})
        entry["triage"] = t.get("class", "unclassified")
        if t.get("note"):
            entry["note"] = t["note"]


def build_summary(rows: list[dict], triage: dict, meta: dict) -> dict:
    scenarios = summarize_scenarios(rows)
    apply_triage(scenarios, triage)

    fails = {s: e for s, e in scenarios.items() if not e["pass"]}
    by_class: dict[str, int] = {}
    for e in fails.values():
        cls = e.get("triage", "unclassified")
        by_class[cls] = by_class.get(cls, 0) + 1

    return {
        "schema_version": SCHEMA_VERSION,
        "run": meta.get("run", ""),
        "date": meta.get("date", ""),
        "fw": meta.get("fw", ""),
        "board": meta.get("board", ""),
        "categories": meta.get("categories", []),
        "totals": {
            "ran": len(scenarios),
            "pass": len(scenarios) - len(fails),
            "fail": len(fails),
            "fail_by_class": by_class,
        },
        "scenarios": scenarios,
        "note": meta.get("note", ""),
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Summarize a benchmark corpus into summary.json.")
    ap.add_argument("corpus", help="path to a corpus .jsonl")
    ap.add_argument("-o", "--out", default="", help="output path (default: summary.json next to corpus)")
    ap.add_argument("--run", default="", help="experiment id")
    ap.add_argument("--fw", default="", help="firmware describe/sha the run tested")
    ap.add_argument("--board", default="", help="board id (e.g. b1, n3)")
    ap.add_argument("--date", default="", help="run date YYYY-MM-DD")
    ap.add_argument("--cats", default="", help="comma-separated categories covered")
    ap.add_argument("--note", default="")
    a = ap.parse_args(argv)

    rows = load_rows(a.corpus)

    corpus_dir = os.path.dirname(os.path.abspath(a.corpus))
    triage_path = os.path.join(corpus_dir, "triage.json")
    triage = json.load(open(triage_path)) if os.path.exists(triage_path) else {}

    meta = {
        "run": a.run,
        "date": a.date,
        "fw": a.fw,
        "board": a.board,
        "categories": [c for c in a.cats.split(",") if c],
        "note": a.note,
    }
    summary = build_summary(rows, triage, meta)

    out = a.out or os.path.join(corpus_dir, "summary.json")
    with open(out, "w") as f:
        json.dump(summary, f, indent=1, sort_keys=True)
        f.write("\n")

    t = summary["totals"]
    print(
        f"{a.run or os.path.basename(a.corpus)}: "
        f"ran={t['ran']} pass={t['pass']} fail={t['fail']} {t['fail_by_class']} -> {out}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
