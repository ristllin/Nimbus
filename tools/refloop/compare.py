#!/usr/bin/env python3
"""compare - run the same scripted scenarios through the Nimbus harness and the reference loop.

  arm A  nimbusd's own harness (its chat API), head model X
  arm B  refloop.py with model X and nimbusd's tools, on a separate data dir seeded identically

One command runs everything, grades it and writes the tables:

  python3 tools/refloop/compare.py run --evidence <dir> --env-file <dotenv>
      [--models claude-sonnet-5-5,gpt-5.6-luna] [--reps 3] [--only id,id] [--arms A,B]
      [--parallel 4] [--max-usd 40] [--no-judge] [--force]

  compare.py grade  --evidence <dir> --env-file <dotenv>   grade stored runs again (checks + judge)
  compare.py report --evidence <dir>                       rebuild RESULTS.md and summary.json
  compare.py show   --run <dir>/<model>/<scenario>/rep1/A --turn 4
                                                           what the model was shown on that turn

A run is PAID: real provider turns on both arms. --max-usd stops new runs once the ledger
(<evidence>/spend.jsonl) reaches it. Finished runs are skipped on a rerun unless --force.
"""

from __future__ import annotations

import argparse
import gzip
import json
import sys
import time
import traceback
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import grade  # noqa: E402
import report  # noqa: E402
import scenario  # noqa: E402
from arms import ARMS, HEAD_PATHS, Job  # noqa: E402
from costs import add_spend, rows_cost, total_spend  # noqa: E402
from nimbusd_ctl import load_env  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SCENARIO_DIR = REPO / "tests" / "evals" / "refloop" / "scenarios"
# Per model: the head provider, and the effort the harness itself sends for it (read off the
# wire, see README.md), which the reference loop mirrors. None = no effort field.
MODELS = {
    "claude-sonnet-5-5": {"head": "anthropic", "effort": "medium", "sub_model": "claude-haiku-5-5"},
    "claude-haiku-5-5": {"head": "anthropic", "effort": "medium", "sub_model": "claude-haiku-5-5"},
    "gpt-5.6-luna": {"head": "openai", "effort": None, "sub_model": ""},
}


def utc() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def run_dir(evidence: Path, model: str, scenario_id: str, rep: int) -> Path:
    return evidence / model / scenario_id / f"rep{rep}"


def make_jobs(args, scenarios: list, env: dict) -> list:
    jobs = []
    for model in args.models.split(","):
        if model not in MODELS:
            raise SystemExit(f"unknown model {model}: add it to MODELS in compare.py")
        for sc in scenarios:
            if sc.get("models") and model not in sc["models"]:
                continue
            for rep in range(1, min(args.reps, sc.get("reps", args.reps)) + 1):
                jobs.append(Job(model=model, scenario=sc, rep=rep, env=env,
                                dir=run_dir(args.evidence, model, sc["id"], rep), **MODELS[model]))
    return jobs


def finished(path: Path) -> bool:
    try:
        return "error" not in json.loads(path.read_text())
    except (OSError, ValueError):
        return False


def run_task(job: Job, arm_name: str, args) -> str:
    """One arm of one job, start to teardown. Returns a one-line status."""
    tag = f"{job.model} {job.scenario['id']} rep{job.rep} {arm_name}"
    out = job.dir / arm_name / "turns.json"
    ledger = args.evidence / "spend.jsonl"
    if finished(out) and not args.force:
        return f"skip  {tag} (already run)"
    if total_spend(ledger) >= args.max_usd:
        return f"HELD  {tag}: the spend ledger reached --max-usd {args.max_usd}"
    arm = ARMS[arm_name](job)
    rec = {"arm": arm_name, "model": job.model, "scenario": job.scenario["id"], "rep": job.rep,
           "effort": job.effort, "started": utc(), "steps": []}
    try:
        arm.start()
        arm.seed()
        for step in job.scenario["turns"]:
            rec["steps"].append(arm.step(step))
        rec["final_state"] = arm.final_state()
    except Exception as e:
        rec["error"] = f"{type(e).__name__}: {e}"[:600]
        rec["traceback"] = traceback.format_exc()[-3000:]
    finally:
        rec["cost_usd"] = round(rows_cost(arm.proxy.records), 6)
        rec["provider_exchanges"] = len(arm.proxy.records)
        arm.close()
    rec["ended"] = utc()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(rec, indent=1, ensure_ascii=False))
    add_spend(ledger, f"run {tag}", rec["cost_usd"])
    turns = [s for s in rec["steps"] if s.get("kind") in ("say", "burst")]
    errors = sum(1 for s in turns if s.get("error") or s.get("timed_out"))
    state = "FAIL " if "error" in rec else "done "
    return (f"{state} {tag}: {len(turns)} turns, {sum(s['latency_s'] for s in turns):.0f} s, "
            f"${rec['cost_usd']:.3f}, {errors} failed turns {rec.get('error', '')}")


def secret_scan(evidence: Path, env: dict) -> int:
    """How many evidence files hold a provider key. Must be zero."""
    secrets = [v.encode() for k, v in env.items() if v and len(v) >= 20 and ("KEY" in k or "TOKEN" in k)]
    hits = 0
    for path in evidence.rglob("*"):
        if not path.is_file():
            continue
        data = gzip.open(path, "rb").read() if path.suffix == ".gz" else path.read_bytes()
        if any(s in data for s in secrets):
            hits += 1
            print(f"SECRET FOUND in {path}", flush=True)
    return hits


def cmd_run(args):
    env = load_env(args.env_file)
    scenarios = scenario.load_all(args.scenarios or [SCENARIO_DIR])
    if args.only:
        wanted = set(args.only.split(","))
        scenarios = [s for s in scenarios if s["id"] in wanted]
    jobs = make_jobs(args, scenarios, env)
    tasks = [(job, arm) for job in jobs for arm in args.arms.split(",")]
    print(f"{len(tasks)} runs ({len(jobs)} scenario repetitions x {args.arms}), "
          f"spent so far ${total_spend(args.evidence / 'spend.jsonl'):.2f} of ${args.max_usd}", flush=True)
    with ThreadPoolExecutor(max_workers=args.parallel) as pool:
        for line in pool.map(lambda t: run_task(t[0], t[1], args), tasks):
            print(line, flush=True)
    grade.grade_all(args.evidence, scenarios, env, judge=not args.no_judge, force=args.force)
    report.write(args.evidence)
    leaked = secret_scan(args.evidence, env)
    print(f"secret scan: {leaked} evidence files hold a key. Spend ${total_spend(args.evidence / 'spend.jsonl'):.2f}.")
    return 1 if leaked else 0


def cmd_grade(args):
    env = load_env(args.env_file)
    scenarios = scenario.load_all(args.scenarios or [SCENARIO_DIR])
    grade.grade_all(args.evidence, scenarios, env, judge=not args.no_judge, force=args.force)
    report.write(args.evidence)
    return 0


def cmd_report(args):
    report.write(args.evidence)
    return 0


def _brief(value, width: int) -> str:
    text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False)
    return text if len(text) <= width else f"{text[:width]} ...[{len(text)} chars]"


def _show_items(items: list, width: int):
    for item in items:
        role = item.get("role") or item.get("type")
        content = item.get("content")
        if isinstance(content, list):
            for block in content:
                kind = block.get("type")
                body = block.get("text") if kind in ("text", "input_text", "output_text") else block
                if kind in ("image", "input_image"):
                    body = f"<{kind} {len(json.dumps(block))} bytes>"
                print(f"  [{role}/{kind}] {_brief(body, width)}")
        else:
            print(f"  [{role}] {_brief(content if content is not None else item, width)}")


def cmd_show(args):
    """Print what the model was shown on one turn: the last head request of that turn, and its answer."""
    rows = [json.loads(line) for line in gzip.open(Path(args.run) / "requests.jsonl.gz", "rt")]
    rows = [r for r in rows if r["ctx"].get("turn") == args.turn and r["path"] in HEAD_PATHS]
    if not rows:
        print("no head request recorded for that turn")
        return 1
    print(f"{len(rows)} head request(s) on turn {args.turn}; showing #{args.request if args.request else len(rows)}")
    row = rows[(args.request or len(rows)) - 1]
    req = row["request"]
    print(f"model {req.get('model')}  request {row['req_bytes']} bytes  usage {row['usage']}  {row['ms']} ms")
    print("params:", {k: v for k, v in req.items() if k not in ("system", "messages", "tools", "input", "instructions")})
    system = req.get("system") or req.get("instructions") or ""
    blocks = system if isinstance(system, list) else [{"text": system}]
    print(f"system: {[len(b.get('text', '')) for b in blocks]} chars; tools: {len(req.get('tools') or [])}"
          f" ({len(json.dumps(req.get('tools') or []))} bytes)")
    if args.system:
        for b in blocks:
            print(b.get("text", ""))
    print("--- messages")
    _show_items(req.get("messages") or req.get("input") or [], args.width)
    print("--- response")
    resp = row["response"] if isinstance(row["response"], dict) else {"raw": row["response"]}
    _show_items([{"role": "assistant", "content": resp.get("content") or resp.get("output") or [resp]}], args.width)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("run", "grade", "report"):
        p = sub.add_parser(name)
        p.add_argument("--evidence", type=Path, required=True)
        if name != "report":
            p.add_argument("--env-file", required=True, help="dotenv file with ANTHROPIC_API_KEY and OPENAI_API_KEY")
            p.add_argument("--scenarios", nargs="*", help=f"scenario files or directories (default {SCENARIO_DIR})")
            p.add_argument("--no-judge", action="store_true")
            p.add_argument("--force", action="store_true", help="run or judge again what is already stored")
    run = sub.choices["run"]
    run.add_argument("--models", default="claude-sonnet-5-5,gpt-5.6-luna")
    run.add_argument("--reps", type=int, default=3)
    run.add_argument("--only", help="comma-separated scenario ids")
    run.add_argument("--arms", default="A,B")
    run.add_argument("--parallel", type=int, default=4)
    run.add_argument("--max-usd", type=float, default=40.0)
    show = sub.add_parser("show")
    show.add_argument("--run", required=True, help="an arm directory: <evidence>/<model>/<scenario>/repN/A")
    show.add_argument("--turn", type=int, required=True)
    show.add_argument("--request", type=int, help="which head request of the turn (default: the last)")
    show.add_argument("--width", type=int, default=1200)
    show.add_argument("--system", action="store_true", help="print the system prompt in full")
    args = ap.parse_args()
    return {"run": cmd_run, "grade": cmd_grade, "report": cmd_report, "show": cmd_show}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
