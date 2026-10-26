#!/usr/bin/env python3
"""Food coach eval: replay the owner's scenarios, judge the transcripts, report.

  run_eval.py list
  run_eval.py run --label base-sonnet --model <id> [--reps 5] [--scenarios a,b]
                  [--judge anthropic/<id>] [--judge2 openai/<id>|none] [--workers 3]
                  [--max-usd 3] [--out DIR] [--env FILE]
  run_eval.py rejudge --label base-sonnet [--judge ...] [--judge2 ...]
  run_eval.py report --label base-sonnet [--against other-label]

Real, paid API calls (the head under test and the judges). Keys are read by name
from the dotenv file (--env or NIMBUS_ENV_FILE) and never printed. Build the host
driver first: make -C tests/evals/food_coach/driver
"""

from __future__ import annotations

import argparse
import json
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import fcjudge
import fcreport
import fcrun
import fcworld


def _scenarios(arg: str | None) -> list[dict]:
    return fcworld.load_scenarios([s for s in arg.split(",") if s] if arg else None)


def _judges(args, cfg: dict) -> list[str]:
    first = args.judge or cfg["models"]["judge"]
    second = args.judge2 if args.judge2 is not None else cfg["models"]["judge2"]
    return [first] + ([] if second in ("", "none") else [second])


def _outdir(args) -> Path:
    base = Path(args.out) if args.out else fcworld.HERE / "runs"
    return base / args.label


def cmd_list(_args) -> int:
    cfg = fcworld.load_config()
    for s in fcworld.load_scenarios():
        bad = fcworld.validate_scenario(s, cfg)
        print(
            f"{s['id']:20s} {s['kind']:7s} {len(s['turns'])} turn(s)  {s['title']}"
            + (f"  PROBLEMS {bad}" if bad else "")
        )
    return 0


def cmd_run(args) -> int:
    cfg = fcworld.load_config()
    keys = fcjudge.load_keys(args.env)
    scenarios = _scenarios(args.scenarios)
    for s in scenarios:
        bad = fcworld.validate_scenario(s, cfg)
        if bad:
            print(f"scenario {s['id']} invalid: {bad}", file=sys.stderr)
            return 2
    if args.backend == "lab" and not fcrun.DRIVER.exists():
        print("build the driver first: make -C tests/evals/food_coach/driver", file=sys.stderr)
        return 2
    outdir = _outdir(args)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "provenance.json").write_text(json.dumps(fcrun.provenance(args), indent=1), encoding="utf-8")
    budget = fcrun.Budget(args.max_usd)
    tasks = [
        fcrun.Task(
            scn=s,
            rep=rep,
            label=args.label,
            outdir=outdir,
            host=args.host,
            model=args.model,
            cfg=cfg,
            keys=keys,
            judges=_judges(args, cfg),
            effort=args.effort,
            env_path=args.env or "",
            retries=args.retries,
            backend=args.backend,
        )
        for rep in range(1, args.reps + 1)
        for s in scenarios
    ]
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for rec in pool.map(lambda t: fcrun.execute(t, budget), tasks):
            _print_rec(rec)
    return _finish(args, cfg, outdir, budget)


def _print_rec(rec: dict) -> None:
    if "skipped" in rec:
        print(f"{rec['scenario']:20s} rep{rec['rep']} skipped ({rec['skipped']})")
        return
    v = rec["verdict"]
    state = "ERROR" if v["errored"] else ("PASS " if v["pass"] else "FAIL ")
    print(f"{rec['scenario']:20s} rep{rec['rep']} {state} failed={v['failed']} agree={v['judges_agree']}")


def _finish(args, cfg: dict, outdir: Path, budget: fcrun.Budget) -> int:
    recs = fcreport.load_records(outdir)
    summary = fcreport.summarize(recs, args.label, cfg)
    (outdir / "summary.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
    (outdir / "results.md").write_text(fcreport.render_markdown(summary), encoding="utf-8")
    print(fcreport.render_markdown(summary))
    print(f"spend this invocation: ${budget.spent:.4f}")
    return 0


def cmd_rejudge(args) -> int:
    cfg = fcworld.load_config()
    keys = fcjudge.load_keys(args.env)
    outdir = _outdir(args)
    by_id = {s["id"]: s for s in fcworld.load_scenarios()}
    for rec in fcreport.load_records(outdir):
        task = fcrun.Task(
            scn=by_id[rec["scenario"]],
            rep=rec["rep"],
            label=args.label,
            outdir=outdir,
            host=rec["host"],
            model=rec["model"],
            cfg=cfg,
            keys=keys,
            judges=_judges(args, cfg),
        )
        rec["judges"] = fcrun.judge_run(task, rec["turns"])
        rec["verdict"] = fcrun.decide(rec["judges"], rec["verdict"]["errored"])
        rec["cost_usd"]["judges"] = round(rec["cost_usd"]["judges"] + sum(j["cost_usd"] for j in rec["judges"]), 6)
        (outdir / rec["scenario"] / f"rep{rec['rep']}.json").write_text(json.dumps(rec, indent=1), encoding="utf-8")
        _print_rec(rec)
    return _finish(args, cfg, outdir, fcrun.Budget(0))


def cmd_report(args) -> int:
    cfg = fcworld.load_config()
    base = Path(args.out) if args.out else fcworld.HERE / "runs"
    first = fcreport.summarize(fcreport.load_records(base / args.label), args.label, cfg)
    print(fcreport.render_markdown(first))
    if args.against:
        second = fcreport.summarize(fcreport.load_records(base / args.against), args.against, cfg)
        print(fcreport.render_comparison(first, second))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list").set_defaults(fn=cmd_list)
    for name, fn in (("run", cmd_run), ("rejudge", cmd_rejudge), ("report", cmd_report)):
        sp = sub.add_parser(name)
        sp.set_defaults(fn=fn)
        sp.add_argument("--label", required=True)
        sp.add_argument("--out")
        sp.add_argument("--env")
        sp.add_argument("--judge")
        sp.add_argument("--judge2")
    run = sub.choices["run"]
    run.add_argument("--model", required=True)
    run.add_argument("--host", default="anthropic")
    run.add_argument("--reps", type=int, default=5)
    run.add_argument("--scenarios")
    run.add_argument("--workers", type=int, default=3)
    run.add_argument("--max-usd", type=float, default=3.0)
    run.add_argument("--effort", default="adapter", help="adapter = what the device sends (default)")
    run.add_argument("--retries", type=int, default=2)
    run.add_argument("--backend", choices=["lab"], default="lab")
    sub.choices["report"].add_argument("--against")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
