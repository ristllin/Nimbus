#!/usr/bin/env python3
"""Suite-agnostic eval runner + persistent tracking (the generic T6 layer).

This is the shared spine that tools/eval_prompt_ab.py (the N11 prompt A/B) was the
first, hand-rolled instance of. It owns the parts every eval suite needs and none
should re-implement:

  - the ~/nimbus-evals/ persistence format (per-run JSONL corpus + summary.json),
    UNCHANGED from what eval_prompt_ab.py established - reused, not redesigned;
  - a per-suite run LEDGER (`<id>.runs.jsonl`, append-only) that accumulates one
    row per run (score + cost + input hash + timestamp). This is the history the
    trend report reads;
  - CHANGE DETECTION: a content hash over each suite's declared inputs (prompts,
    goldens, models, cases). A run whose inputs hash matches the last ledger row is
    skipped unless forced, so the nightly only re-spends on changed suites;
  - a per-run BUDGET cap (max calls AND max dollars) so a runaway suite cannot
    burn the paid-API allowance;
  - a trend report over the accumulated ledgers (markdown).

A suite REGISTERS (id, inputs, models, a run callable, budget) and the runner does
the rest. The run callable is the only provider-aware part; the harness itself
makes no network calls, which is what lets the whole thing be host-tested with a
mock suite for $0 (see tools/test_eval_harness.py).

Persistence lives under ~/nimbus-evals/ (T6 rule: never into the repo).

CLI:
  # trend report over every suite that has run at least once:
  tools/eval_harness.py trend
  # trend for one suite, to a file:
  tools/eval_harness.py trend --suite n11_prompt_ab_anthropic --out ~/nimbus-evals/trend.md
"""

import argparse
import hashlib
import json
import os
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Callable, Optional

EVALS_DIR = os.path.expanduser("~/nimbus-evals")

# Rough public per-1M-token prices (USD), for the $ estimate only; the token
# counts in each row are the ground truth. Keyed by provider. "mock" is 0 so the
# host tests and any offline/tiny-model suite spend nothing. Kept in lockstep with
# eval_prompt_ab.py (which imports this table).
PRICING = {
    "anthropic": {"in": 3.0, "out": 15.0},
    "openai": {"in": 2.5, "out": 10.0},
    "mistral": {"in": 2.0, "out": 6.0},
    "mock": {"in": 0.0, "out": 0.0},
}


def price_of(provider: str) -> dict:
    return PRICING.get(provider, {"in": 0.0, "out": 0.0})


def cost_usd(provider: str, usage: dict) -> float:
    p = price_of(provider)
    return (usage.get("in", 0) * p["in"] + usage.get("out", 0) * p["out"]) / 1e6


# ---- change-detection hash --------------------------------------------------
def content_hash(inputs: Any) -> str:
    """Stable sha256 over a suite's declared inputs.

    Canonical JSON (sorted keys, no whitespace drift) so the SAME inputs always
    hash the same and ANY change (a prompt edit, a new case, a model swap) flips
    it. The suite decides what goes in `inputs`; whatever it puts there is the
    definition of "changed" for the nightly skip.
    """
    blob = json.dumps(inputs, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()


# ---- suite model ------------------------------------------------------------
@dataclass
class Budget:
    """Hard per-run caps. The run stops at whichever binds first."""

    max_calls: int = 40
    max_usd: float = 1.0


@dataclass
class CaseResult:
    """One graded call. `score` is 0..1; `ok` is the pass/fail the suite defines."""

    ok: bool
    score: float
    note: str = ""
    usage: dict = field(default_factory=lambda: {"in": 0, "out": 0})


@dataclass
class Suite:
    """A registerable eval suite.

    id:          stable slug; names the ~/nimbus-evals/ files.
    description: one line, for the trend report + nightly log.
    provider:    pricing + key env selector ("mock" for offline/host tests).
    models:      model ids to run each case against.
    cases:       opaque case objects; passed to `run` and `case_id`.
    inputs:      the dict hashed for change detection (prompts/goldens/models/...).
    run:         (model, case) -> CaseResult. The ONLY provider-aware part; the
                 harness never calls the network itself.
    budget:      hard caps for one run.
    case_id:     case -> short label for the corpus rows (defaults to str()).
    """

    id: str
    description: str
    provider: str
    models: list
    cases: list
    inputs: dict
    run: Callable[[str, Any], CaseResult]
    budget: Budget = field(default_factory=Budget)
    case_id: Callable[[Any], str] = staticmethod(str)


# ---- persistence paths ------------------------------------------------------
def corpus_path(suite_id: str) -> str:
    return os.path.join(EVALS_DIR, f"{suite_id}.corpus.jsonl")


def summary_path(suite_id: str) -> str:
    return os.path.join(EVALS_DIR, f"{suite_id}.summary.json")


def ledger_path(suite_id: str) -> str:
    return os.path.join(EVALS_DIR, f"{suite_id}.runs.jsonl")


def read_ledger(suite_id: str) -> list:
    path = ledger_path(suite_id)
    if not os.path.exists(path):
        return []
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def last_hash(suite_id: str) -> Optional[str]:
    rows = read_ledger(suite_id)
    return rows[-1].get("input_hash") if rows else None


def suite_changed(suite: Suite, force: bool = False) -> bool:
    """True if this suite should run: forced, never run, or inputs hash differs."""
    if force:
        return True
    return content_hash(suite.inputs) != last_hash(suite.id)


# ---- the runner -------------------------------------------------------------
# One-arg logger; the type alias keeps the comma out of run_suite's signature
# (a bare Callable[[str], None] there reads as an extra parameter to the
# complexity gate) and documents the shape once.
LogFn = Callable[[str], None]


def _now_iso(now: Optional[str]) -> str:
    return now or datetime.now(timezone.utc).isoformat(timespec="seconds")


def _budget_hit(budget: Budget, calls: int, spent: float) -> Optional[str]:
    """Which cap (if any) has bound. Checked BEFORE each call so neither is exceeded."""
    if calls >= budget.max_calls:
        return "max_calls"
    if spent >= budget.max_usd:
        return "max_usd"
    return None


def _grade_one(suite: Suite, model: str, case: Any, rep: int) -> tuple:
    """Run and grade one call. Returns (record, cost). A suite-run exception is a
    recorded fail, never a crash - one bad case must not sink the whole run."""
    rec = {"suite": suite.id, "provider": suite.provider, "model": model, "case": suite.case_id(case), "rep": rep}
    try:
        r = suite.run(model, case)
        c = cost_usd(suite.provider, r.usage)
        rec.update(ok=bool(r.ok), score=round(float(r.score), 4), note=r.note, usage=r.usage, cost_usd=round(c, 6))
        return rec, c
    except Exception as ex:
        rec.update(ok=False, score=0.0, note=f"error: {type(ex).__name__}: {ex}", usage={}, cost_usd=0.0)
        return rec, 0.0


def _run_cases(suite: Suite, reps: int, log: LogFn) -> tuple:
    """Run every model x case x rep under the budget, streaming rows to the corpus.
    Returns (rows, budget_hit)."""
    rows, calls, spent, hit = [], 0, 0.0, None
    with open(corpus_path(suite.id), "w", encoding="utf-8") as cf:
        for model in suite.models:
            for case in suite.cases:
                for rep in range(reps):
                    hit = _budget_hit(suite.budget, calls, spent)
                    if hit:
                        log(f"[{suite.id}] budget hit ({hit}) after {calls} calls, ${spent:.4f}; stopping")
                        break
                    calls += 1
                    rec, c = _grade_one(suite, model, case, rep)
                    spent += c
                    cf.write(json.dumps(rec) + "\n")
                    cf.flush()
                    rows.append(rec)
                    mark = "PASS" if rec.get("ok") else "FAIL"
                    log(f"  [{mark}] {suite.id} {model} {rec['case']} rep{rep}: {rec.get('note', '')[:60]}")
                if hit:
                    break
            if hit:
                break
    return rows, hit


def run_suite(suite: Suite, reps: int = 1, force: bool = False, dry_run: bool = False, now=None, log: LogFn = print):
    """Run one suite, persist its corpus + summary + ledger row, return the summary.

    Skips (no calls, no writes) when the suite's inputs are unchanged since its
    last ledger row, unless `force`. Honors the suite budget (calls AND dollars).
    `now` is injectable so tests get deterministic timestamps.
    """
    os.makedirs(EVALS_DIR, exist_ok=True)
    ihash = content_hash(suite.inputs)
    plan = len(suite.cases) * len(suite.models) * reps

    if not force and not dry_run and ihash == last_hash(suite.id):
        log(f"[{suite.id}] unchanged (hash {ihash[:8]}); skipping. Use --force to re-run.")
        return {"suite": suite.id, "skipped": True, "reason": "unchanged", "input_hash": ihash, "plan": plan}

    if dry_run:
        log(f"[{suite.id}] plan {plan} calls ({len(suite.cases)} cases x {len(suite.models)} models x {reps} reps)")
        log(f"[{suite.id}] budget: <= {suite.budget.max_calls} calls, <= ${suite.budget.max_usd}. dry-run: no calls.")
        return {"suite": suite.id, "dry_run": True, "plan": plan, "input_hash": ihash}

    rows, budget_hit = _run_cases(suite, reps, log)
    summary = _summarize(suite, rows, ihash, _now_iso(now), budget_hit)
    with open(summary_path(suite.id), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    with open(ledger_path(suite.id), "a", encoding="utf-8") as f:
        f.write(json.dumps(_ledger_row(summary)) + "\n")
    log(
        f"[{suite.id}] {summary['pass']}/{summary['n']} pass "
        f"({summary['pass_rate'] * 100:.0f}%), mean score {summary['mean_score']}, ${summary['cost_usd']}"
    )
    return summary


def _summarize(suite: Suite, rows: list, ihash: str, ts: str, budget_hit: Optional[str]) -> dict:
    n = len(rows)
    passed = sum(1 for r in rows if r.get("ok"))
    scores = [r.get("score", 0.0) for r in rows]
    tin = sum((r.get("usage") or {}).get("in", 0) for r in rows)
    tout = sum((r.get("usage") or {}).get("out", 0) for r in rows)
    cost = sum(r.get("cost_usd", 0.0) for r in rows)
    per_case = {}
    for r in rows:
        d = per_case.setdefault(r["case"], {"n": 0, "pass": 0})
        d["n"] += 1
        d["pass"] += 1 if r.get("ok") else 0
    return {
        "suite": suite.id,
        "description": suite.description,
        "provider": suite.provider,
        "models": list(suite.models),
        "generated_at": ts,
        "input_hash": ihash,
        "n": n,
        "pass": passed,
        "pass_rate": round(passed / n, 4) if n else 0.0,
        "mean_score": round(sum(scores) / n, 4) if n else 0.0,
        "tokens_in": tin,
        "tokens_out": tout,
        "cost_usd": round(cost, 4),
        "budget_hit": budget_hit,
        "per_case_pass": per_case,
    }


def _ledger_row(summary: dict) -> dict:
    """The compact, append-only history row the trend report reads."""
    return {
        "generated_at": summary["generated_at"],
        "input_hash": summary["input_hash"],
        "models": summary["models"],
        "n": summary["n"],
        "pass": summary["pass"],
        "pass_rate": summary["pass_rate"],
        "mean_score": summary["mean_score"],
        "tokens_in": summary["tokens_in"],
        "tokens_out": summary["tokens_out"],
        "cost_usd": summary["cost_usd"],
        "budget_hit": summary["budget_hit"],
    }


# ---- registry ---------------------------------------------------------------
_REGISTRY: dict = {}


def register(suite: Suite) -> Suite:
    _REGISTRY[suite.id] = suite
    return suite


def get(suite_id: str) -> Optional[Suite]:
    return _REGISTRY.get(suite_id)


def all_suites() -> list:
    return list(_REGISTRY.values())


# ---- trend report -----------------------------------------------------------
def trend_markdown(suite_ids: Optional[list] = None, now: Optional[str] = None) -> str:
    """Markdown trend report: score + cost over time per suite, from the ledgers."""
    if suite_ids is None:
        # every suite that has a ledger on disk
        suite_ids = sorted(
            fn[: -len(".runs.jsonl")]
            for fn in (os.listdir(EVALS_DIR) if os.path.isdir(EVALS_DIR) else [])
            if fn.endswith(".runs.jsonl")
        )
    out = ["# Eval trend report", "", f"Generated: {_now_iso(now)}", ""]
    if not suite_ids:
        out.append("_No suites have run yet (no `*.runs.jsonl` ledgers under ~/nimbus-evals/)._")
        return "\n".join(out) + "\n"
    for sid in suite_ids:
        rows = read_ledger(sid)
        out.append(f"## {sid}")
        out.append("")
        if not rows:
            out.append("_No runs recorded._")
            out.append("")
            continue
        out.append("| Run (UTC) | Models | Pass | Rate | Mean score | Cost $ | Inputs | Budget |")
        out.append("|---|---|---|---|---|---|---|---|")
        prev_hash = None
        for r in rows:
            changed = "new" if prev_hash is None else ("changed" if r["input_hash"] != prev_hash else "same")
            prev_hash = r["input_hash"]
            models = ",".join(r.get("models", [])) or "-"
            out.append(
                f"| {r['generated_at']} | {models} | {r['pass']}/{r['n']} | "
                f"{r['pass_rate'] * 100:.0f}% | {r['mean_score']} | {r['cost_usd']} | "
                f"{r['input_hash'][:8]} ({changed}) | {r.get('budget_hit') or '-'} |"
            )
        first, last = rows[0], rows[-1]
        d_rate = (last["pass_rate"] - first["pass_rate"]) * 100
        d_cost = last["cost_usd"] - first["cost_usd"]
        out.append("")
        out.append(
            f"Trend over {len(rows)} run(s): pass rate {d_rate:+.0f} pts, "
            f"cost {d_cost:+.4f} $/run (latest {last['pass_rate'] * 100:.0f}% at ${last['cost_usd']})."
        )
        out.append("")
    return "\n".join(out) + "\n"


# ---- CLI (trend only; running suites is the nightly's job) ------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("trend", help="write a markdown trend report over the accumulated ledgers")
    t.add_argument("--suite", action="append", help="limit to this suite id (repeatable); default: all")
    t.add_argument("--out", help="write to this path instead of stdout")
    args = ap.parse_args(argv)
    if args.cmd == "trend":
        md = trend_markdown(args.suite)
        if args.out:
            with open(os.path.expanduser(args.out), "w", encoding="utf-8") as f:
                f.write(md)
            print(f"wrote {args.out}")
        else:
            sys.stdout.write(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
