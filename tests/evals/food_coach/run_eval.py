#!/usr/bin/env python3
"""Food coach eval: replay the owner's scenarios, judge the transcripts, report.

  run_eval.py list
  run_eval.py run --label base-sonnet --model <id> [--reps 5] [--scenarios a,b]
                  [--judge anthropic/<id>] [--judge2 openai/<id>[,anthropic/<id>]|none] [--workers 3]
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
import signal
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import fcjudge
import fcreport
import fcrun
import fcworld


def _scenarios(arg: str | None) -> list[dict]:
    return fcworld.load_scenarios([s for s in arg.split(",") if s] if arg else None)


def _judges(args, cfg: dict) -> list[str]:
    """The first judge decides; the rest (a comma list in --judge2) are second opinions."""
    first = args.judge or cfg["models"]["judge"]
    rest = args.judge2 if args.judge2 is not None else cfg["models"]["judge2"]
    return [first] + [j for j in rest.split(",") if j and j != "none"]


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
    common = _common(args, cfg, keys, outdir)
    if args.backend == "device":
        return _run_device(args, cfg, scenarios, common, budget)
    tasks = [fcrun.Task(scn=s, rep=rep, **common) for rep in range(1, args.reps + 1) for s in scenarios]
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for rec in pool.map(lambda t: fcrun.execute(t, budget), tasks):
            _print_rec(rec)
    return _finish(args, cfg, outdir, budget)


def _common(args, cfg: dict, keys: dict, outdir: Path) -> dict:
    """The Task fields every repetition of one invocation shares."""
    return {
        "label": args.label,
        "outdir": outdir,
        "host": args.host,
        "model": args.model,
        "cfg": cfg,
        "keys": keys,
        "judges": _judges(args, cfg),
        "effort": args.effort,
        "env_path": args.env or "",
        "retries": args.retries,
        "backend": args.backend,
        "extra": {"second_on_pass": args.judge2_on_pass},
    }


def _run_device(args, cfg: dict, scenarios: list[dict], common: dict, budget: fcrun.Budget) -> int:
    """The device leg: one repetition at a time on a bench unit held under its lock."""
    import device_leg

    if not args.unit:
        print("--backend device needs --unit A or L", file=sys.stderr)
        return 2
    directive = (fcworld.HERE / "profiles" / "stored_directive.txt").read_text(encoding="utf-8").strip()
    deadline = time.time() + args.max_minutes * 60
    try:
        with device_leg.leg_session(
            args.unit,
            args.model,
            directive,
            device_leg.Bench(who="QW7 food coach device leg", held=args.lock_held),
        ) as (
            unit,
            info,
        ):
            print(f"unit {args.unit} firmware {info.get('fw')} build {info.get('build')}; lock held")
            extra = dict(common["extra"], unit=unit, unit_id=args.unit, fw=info.get("fw"))
            for rep in range(1, args.reps + 1):
                for scn in scenarios:
                    if time.time() > deadline:
                        print("time limit reached; the lock is released now")
                        return _finish(args, cfg, common["outdir"], budget)
                    task = fcrun.Task(scn=scn, rep=rep, **dict(common, extra=extra, effort="device"))
                    _print_rec(fcrun.execute(task, budget))
    except device_leg.UnitRefused as exc:
        print(f"device leg refused: {exc}", file=sys.stderr)
        return 2
    return _finish(args, cfg, common["outdir"], budget)


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
        if args.only_ungraded and not fcrun.ungraded(rec):
            continue
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
        rec["judges"] = fcrun.judge_run(task, rec["turns"], rec.get("history"))
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
    run.add_argument("--judge2-on-pass", action="store_true", help="run the second judge only on runs the first passes")
    run.add_argument("--max-usd", type=float, default=3.0)
    run.add_argument("--effort", default="adapter", help="adapter = what the device sends (default)")
    run.add_argument("--retries", type=int, default=2)
    run.add_argument("--backend", choices=["lab", "device"], default="lab")
    run.add_argument("--unit", choices=["A", "L"], help="bench unit for --backend device")
    run.add_argument("--max-minutes", type=int, default=40, help="device leg: stop starting runs after this")
    run.add_argument("--lock-held", action="store_true", help="device leg: the caller already holds the unit's lock")
    sub.choices["report"].add_argument("--against")
    sub.choices["rejudge"].add_argument(
        "--only-ungraded", action="store_true", help="re-grade only runs a judge returned no verdict for"
    )
    return p


def _interrupt(signum, frame) -> None:
    raise KeyboardInterrupt


def install_signal_handlers() -> None:
    """Ctrl-C and a plain kill both unwind through the `finally` blocks, so a stopped device
    leg restores the unit. (A background shell job starts with SIGINT ignored, and Python then
    leaves it ignored: the default handler is set explicitly.)"""
    signal.signal(signal.SIGINT, signal.default_int_handler)
    signal.signal(signal.SIGTERM, _interrupt)


def main(argv: list[str] | None = None) -> int:
    install_signal_handlers()
    args = build_parser().parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
