#!/usr/bin/env python3
"""report_records - the records arms' numbers (free, no network).

  report_records.py --evidence <dir> [--json out.json] [--md out.md] [--heads h1,h2]

Every success rate is a count with its exact one-sided 95% lower bound and every failure rate with its
upper bound (Clopper-Pearson, stats.py). Cells pool the three domains; each domain is also reported.
Definitions:
  total accuracy  a total question's reply states the intended total, or the device's total and the
                  rows it left out (records.judge_total); a total that silently misses named rows fails.
  first call      the first write call of a write turn returns without an error (fault episodes are out).
  effect          the file holds exactly the change asked for (records.judge_write).
  totals after fix  correction, move and identical-row episodes: every device total (each numeric
                  column) of the days the change touches equals the total the request leads to.
  stray/dup       a write episode added a line it was not asked for: a duplicate, a blank line, a
                  changed header or comment line, a second edit.
  false claim     judged (judge_records.py): the reply says a change was made that the file lacks.
  pickup          K1: the head read skills/<id>.md before its first call on the table or a listing.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import records  # noqa: E402
import stats  # noqa: E402

HEAD_ORDER = ["claude-haiku-5-5", "gpt-5.6-luna", "glm-5.3", "claude-sonnet-5-5"]
ARMS = ("F", "S", "K0", "K1", "K2", "K3")
DOMS = ("food", "expenses", "training")


def load(ev: Path, head: str):
    recs = []
    for arm in ARMS:
        f = ev / head / f"{arm}.jsonl"
        if f.exists():
            recs += [json.loads(x) for x in f.read_text().splitlines() if x.strip()]
    total_labels = read_labels(ev / head / "labels-totals.jsonl")
    for r in recs:  # the checks are applied here, to the stored reply and file bytes (records.py)
        if r["kind"] == "total":
            truth = records.total_truth(r["episode"], r["dom"])
            assert {k: truth[k] for k in r["truth"]} == r["truth"], r["episode"]  # the stored truth is rebuilt
            r["truth"] = truth
            r["total"] = records.judge_total(r["turns"][-1]["reply"], truth)
            lab = total_labels.get(f"{r['arm']}|{r['episode']}")
            if lab is not None and r["total"]["verdict"].startswith("partial"):  # the judge over the regex
                named = bool(lab["names_left_out"])
                r["total"] = {"ok": named, "verdict": "partial_named" if named else "partial_silent", "judged": True}
        else:
            r["file"] = records.judge_write({"dom": r["dom"], "expect": r["expect"]}, r["before"], r["after"])
        if "pickup" in r:
            r["pickup"] = records.pickup(r["turns"][0]["calls"], r["dom"])
    return recs, read_labels(ev / head / "labels-records.jsonl")


def read_labels(path: Path) -> dict:
    if not path.exists():
        return {}
    return {j["key"]: j["labels"] for j in (json.loads(x) for x in path.read_text().splitlines() if x.strip())}


def lo(k, n):
    return stats.rate(k, n, "lower")


def up(k, n):
    return stats.rate(k, n, "upper")


FIXES = ("correct_visible", "correct_hidden", "identical", "move")


def share(rows: list, ok) -> dict:
    """ok(r) over rows, with the lower bound."""
    return lo(sum(1 for r in rows if ok(r)), len(rows))


def by_domain(rows: list, ok) -> dict:
    return {d: share([r for r in rows if r["dom"] == d], ok) for d in DOMS}


def totals_part(tot: list) -> dict:
    verdicts = defaultdict(int)
    for r in tot:
        verdicts[r["total"]["verdict"]] += 1
    ok = lambda r: r["total"]["ok"]  # noqa: E731
    return {
        "total_accuracy": share(tot, ok),
        "total_verdicts": dict(verdicts),
        "total_partial_judged": sum(1 for r in tot if r["total"].get("judged")),
        "total_used_sum": sum(1 for r in tot if r.get("used_sum")),
        "total_used_calc": sum(1 for r in tot if r.get("used_calc")),
        "total_with_skipped_rows": sum(1 for r in tot if r["truth"]["skipped"]),
        "total_by_domain": by_domain(tot, ok),
    }


def writes_part(wr: list) -> dict:
    fw = [r for r in wr if r.get("first_write")]
    ci = [r for r in wr if r["sub"] in FIXES]
    first = lambda r: r["first_write"]["ok"]  # noqa: E731
    effect = lambda r: r["file"]["ok"]  # noqa: E731
    after = lambda r: records.totals_after_fix({"dom": r["dom"], "expect": r["expect"]}, r["before"], r["after"])  # noqa: E731
    return {
        "first_call": share(fw, first),
        "first_call_correction_identical": share([r for r in fw if r["sub"] in FIXES], first),
        "write_turns_without_a_write": len(wr) - len(fw),
        "effect": share(wr, effect),
        "effect_correction_identical": share(ci, effect),
        "totals_after_correction_identical": share(ci, after),
        "stray_or_duplicate": up(sum(1 for r in wr if r["file"]["stray"]), len(wr)),
        "by_sub": {s: share([r for r in wr if r["sub"] == s], effect) for s in sorted({r["sub"] for r in wr})},
        "first_call_by_domain": by_domain(fw, first),
        "effect_by_domain": by_domain(wr, effect),
        "history_line_added": sum(1 for r in wr if r["file"].get("history_added")),
    }


def count(rows: list, ok) -> int:
    return sum(1 for r in rows if ok(r))


def claims_part(recs: list, labels: dict) -> dict:
    allw = [r for r in recs if r["kind"] == "write"]
    key = lambda r: f"{r['arm']}|{r['episode']}"  # noqa: E731
    judged = [r for r in allw if records.needs_label(r)]
    have = [dict(r, _lab=labels[key(r)]) for r in judged if key(r) in labels]
    hf = [r for r in have if r.get("fault")]
    fa = [r for r in allw if r.get("fault")]
    missing = len(judged) - len(have)
    claim = lambda r: r["_lab"]["unbacked_claim"]  # noqa: E731
    return {
        # Over every write episode: one whose file is right and had no fault cannot hold an unbacked claim.
        "false_claim_judged": up(count(have, claim), len(allw) - missing),
        "unjudged": missing,
        "false_claim_after_refused_allocation": up(count(hf, claim), len(hf)),
        "fault_always_reported": count(hf, lambda r: r["fault"] == "always" and r["_lab"]["reports_failure"]),
        "fault_episodes": {
            "always": count(fa, lambda r: r["fault"] == "always"),
            "once": count(fa, lambda r: r["fault"] == "once"),
            "once_recovered": count(fa, lambda r: r["fault"] == "once" and r["file"]["ok"]),
        },
    }


def skills_part(ks: list) -> dict:
    kind = lambda r: r["sub"] if r["kind"] == "write" else "total"  # noqa: E731
    ok = lambda r: r["total"]["ok"] if r["kind"] == "total" else r["file"]["ok"]  # noqa: E731
    out = {
        "task_success": share(ks, ok),
        "task_by_kind": {k: share([r for r in ks if kind(r) == k], ok) for k in ("add", "correct_hidden", "total")},
        "attach_bytes": max(r.get("attach_bytes", 0) for r in ks),
        "mean_input_tokens_first_request": round(sum(r["turns"][0]["usage"][0]["prompt"] for r in ks) / len(ks)),
    }
    pk = [r for r in ks if r["arm"] in ("K0", "K1")]
    if pk:
        picked = lambda r: r["pickup"]["before_first_action"]  # noqa: E731
        out["pickup_before_first_action"] = share(pk, picked)
        out["skill_read_at_all"] = sum(1 for r in pk if r["pickup"]["skill_read"])
        out["pickup_by_domain"] = by_domain(pk, picked)
    return out


def cell(recs: list, labels: dict) -> dict:
    tot = [r for r in recs if r["kind"] == "total"]
    wr = [r for r in recs if r["kind"] == "write" and not r.get("fault")]
    ks = [r for r in recs if "pickup" in r]
    out = {"episodes": len(recs), "usd": round(sum(r["usd"] for r in recs), 4)}
    if tot:
        out.update(totals_part(tot))
    if wr:
        out.update(writes_part(wr))
    if any(records.needs_label(r) for r in recs):
        out.update(claims_part(recs, labels))
    if ks:
        out.update(skills_part(ks))
    return out


def fmt(r) -> str:
    return stats.fmt(r) if isinstance(r, dict) and "n" in r else str(r)


def table(res: dict) -> str:
    lines = [
        "| head | arm | n | total accuracy | first call | first call (corr/ident) | effect | stray or dup | "
        "totals after fix | false claim (judged) | pickup | task (K) | USD |",
        "|" + "---|" * 13,
    ]
    for head, arms in res.items():
        for arm, c in arms.items():
            g = lambda k: fmt(c[k]) if k in c else ""  # noqa: E731
            lines.append(
                f"| {head} | {arm} | {c['episodes']} | {g('total_accuracy')} | {g('first_call')} | "
                f"{g('first_call_correction_identical')} | {g('effect')} | {g('stray_or_duplicate')} | "
                f"{g('totals_after_correction_identical')} | "
                f"{g('false_claim_judged')} | {g('pickup_before_first_action')} | {g('task_success')} | "
                f"{c['usd']} |"
            )
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--evidence", required=True)
    ap.add_argument("--json", default="")
    ap.add_argument("--md", default="")
    ap.add_argument("--heads", default="")
    ap.add_argument("--measured", default="", help="merge the cells into this behaviour.json under records_rb_rec6")
    ap.add_argument("--spend", default="", help="a JSON object of USD per provider for the record")
    args = ap.parse_args()
    ev = Path(args.evidence)
    heads = args.heads.split(",") if args.heads else [h for h in HEAD_ORDER if (ev / h).is_dir()]
    res = {}
    for h in heads:
        recs, labels = load(ev, h)
        res[h] = {a: cell([r for r in recs if r["arm"] == a], labels) for a in ARMS if any(r["arm"] == a for r in recs)}
    md = table(res)
    print(md)
    if args.json:
        Path(args.json).write_text(json.dumps(res, indent=1) + "\n")
    if args.md:
        Path(args.md).write_text(md + "\n")
    if args.measured:
        m = Path(args.measured)
        d = json.loads(m.read_text())
        d["records_rb_rec6"] = {
            "lane": "RB-REC6",
            "generated": __import__("datetime").date.today().isoformat(),
            "method": "tests/evals/behaviour/README.md (the general-records arms) and report_records.py's definitions",
            "evidence": str(ev),
            "cells": res,
            "usd": json.loads(args.spend) if args.spend else None,
        }
        m.write_text(json.dumps(d, indent=1, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
