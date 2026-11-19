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
    for r in recs:  # the checks are applied here, to the stored reply and file bytes (records.py)
        if r["kind"] == "total":
            r["total"] = records.judge_total(r["turns"][-1]["reply"], r["truth"])
        else:
            r["file"] = records.judge_write({"dom": r["dom"], "expect": r["expect"]}, r["before"], r["after"])
    labels = {}
    lp = ev / head / "labels-records.jsonl"
    if lp.exists():
        for x in lp.read_text().splitlines():
            if x.strip():
                j = json.loads(x)
                labels[j["key"]] = j["labels"]
    return recs, labels


def lo(k, n):
    return stats.rate(k, n, "lower")


def up(k, n):
    return stats.rate(k, n, "upper")


def cell(recs: list, labels: dict) -> dict:
    tot = [r for r in recs if r["kind"] == "total"]
    wr = [r for r in recs if r["kind"] == "write" and not r.get("fault")]
    fa = [r for r in recs if r["kind"] == "write" and r.get("fault")]
    fw = [r for r in wr if r.get("first_write")]
    cor = [r for r in fw if r["sub"] in ("correct_visible", "correct_hidden", "identical", "move")]
    out = {"episodes": len(recs), "usd": round(sum(r["usd"] for r in recs), 4)}
    if tot:
        verdicts = defaultdict(int)
        for r in tot:
            verdicts[r["total"]["verdict"]] += 1
        out["total_accuracy"] = lo(sum(r["total"]["ok"] for r in tot), len(tot))
        out["total_verdicts"] = dict(verdicts)
        out["total_used_sum"] = sum(1 for r in tot if r.get("used_sum"))
        out["total_used_calc"] = sum(1 for r in tot if r.get("used_calc"))
        out["total_with_skipped_rows"] = sum(1 for r in tot if r["truth"]["skipped"])
        out["total_by_domain"] = {d: lo(sum(r["total"]["ok"] for r in tot if r["dom"] == d),
                                        sum(1 for r in tot if r["dom"] == d)) for d in DOMS}
    if wr:
        out["first_call"] = lo(sum(r["first_write"]["ok"] for r in fw), len(fw))
        out["first_call_correction_identical"] = lo(sum(r["first_write"]["ok"] for r in cor), len(cor))
        out["write_turns_without_a_write"] = len(wr) - len(fw)
        out["effect"] = lo(sum(r["file"]["ok"] for r in wr), len(wr))
        ci = [r for r in wr if r["sub"] in ("correct_visible", "correct_hidden", "identical", "move")]
        out["effect_correction_identical"] = lo(sum(r["file"]["ok"] for r in ci), len(ci))
        out["stray_or_duplicate"] = up(sum(1 for r in wr if r["file"]["stray"]), len(wr))
        out["by_sub"] = {s: lo(sum(r["file"]["ok"] for r in wr if r["sub"] == s), sum(1 for r in wr if r["sub"] == s))
                         for s in sorted({r["sub"] for r in wr})}
        out["first_call_by_domain"] = {d: lo(sum(r["first_write"]["ok"] for r in fw if r["dom"] == d),
                                             sum(1 for r in fw if r["dom"] == d)) for d in DOMS}
        out["effect_by_domain"] = {d: lo(sum(r["file"]["ok"] for r in wr if r["dom"] == d),
                                         sum(1 for r in wr if r["dom"] == d)) for d in DOMS}
    judged = [r for r in recs if r["kind"] == "write" and (r.get("fault") or not r["file"]["effect_ok"])]
    if judged:
        lab = [labels.get(f"{r['arm']}|{r['episode']}") for r in judged]
        have = [(r, x) for r, x in zip(judged, lab) if x is not None]
        out["false_claim_judged"] = up(sum(1 for _, x in have if x["unbacked_claim"]), len(have))
        out["unjudged"] = len(judged) - len(have)
        hf = [(r, x) for r, x in have if r.get("fault")]
        out["false_claim_after_refused_allocation"] = up(sum(1 for _, x in hf if x["unbacked_claim"]), len(hf))
        out["fault_always_reported"] = sum(1 for r, x in hf if r["fault"] == "always" and x["reports_failure"])
        out["fault_episodes"] = {"always": sum(1 for r in fa if r["fault"] == "always"),
                                 "once": sum(1 for r in fa if r["fault"] == "once"),
                                 "once_recovered": sum(1 for r in fa if r["fault"] == "once" and r["file"]["ok"])}
    if wr:
        out["history_line_added"] = sum(1 for r in wr if r["file"].get("history_added"))
    pk = [r for r in recs if "pickup" in r and r["arm"] in ("K0", "K1")]
    ks = [r for r in recs if "pickup" in r]
    if ks:
        ok = [r["total"]["ok"] if r["kind"] == "total" else r["file"]["ok"] for r in ks]
        out["task_success"] = lo(sum(ok), len(ok))
        out["task_by_kind"] = {k: lo(sum(o for r, o in zip(ks, ok) if (r["sub"] if r["kind"] == "write" else "total") == k),
                                     sum(1 for r in ks if (r["sub"] if r["kind"] == "write" else "total") == k))
                               for k in ("add", "correct_hidden", "total")}
        out["attach_bytes"] = max(r.get("attach_bytes", 0) for r in ks)
        out["mean_input_tokens_first_request"] = round(sum(r["turns"][0]["usage"][0]["prompt"] for r in ks) / len(ks))
    if pk:
        out["pickup_before_first_action"] = lo(sum(r["pickup"]["before_first_action"] for r in pk), len(pk))
        out["skill_read_at_all"] = sum(1 for r in pk if r["pickup"]["skill_read"])
        out["pickup_by_domain"] = {d: lo(sum(r["pickup"]["before_first_action"] for r in pk if r["dom"] == d),
                                         sum(1 for r in pk if r["dom"] == d)) for d in DOMS}
    return out


def fmt(r) -> str:
    return stats.fmt(r) if isinstance(r, dict) and "n" in r else str(r)


def table(res: dict) -> str:
    lines = ["| head | arm | n | total accuracy | first call | first call (corr/ident) | effect | stray or dup | "
             "false claim (judged) | pickup | task (K) | USD |", "|" + "---|" * 12]
    for head, arms in res.items():
        for arm, c in arms.items():
            g = lambda k: fmt(c[k]) if k in c else ""  # noqa: E731
            lines.append(f"| {head} | {arm} | {c['episodes']} | {g('total_accuracy')} | {g('first_call')} | "
                         f"{g('first_call_correction_identical')} | {g('effect')} | {g('stray_or_duplicate')} | "
                         f"{g('false_claim_judged')} | {g('pickup_before_first_action')} | {g('task_success')} | "
                         f"{c['usd']} |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--evidence", required=True)
    ap.add_argument("--json", default="")
    ap.add_argument("--md", default="")
    ap.add_argument("--heads", default="")
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


if __name__ == "__main__":
    main()
