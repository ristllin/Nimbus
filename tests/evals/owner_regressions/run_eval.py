#!/usr/bin/env python3
"""Owner-regression live suite: replay every lc-* scenario through the real harness.

Each scenario (scenarios/*.json) replays a synthetic exchange shaped like one of the
owner-visible failures in coverage/OWNER-ISSUES-REGISTER.md, through the honesty
eval's host driver (tests/evals/honesty/driver): the device's own turn engine, tools,
claim check and provider adapters, with real model calls. Pass/fail is decided by the
hard assertions in assertions.py over the transcript and the final on-device state;
the judge model is a soft signal only and never gates.

  run_eval.py list
  run_eval.py validate
  run_eval.py run --sha <git sha> [--models all|m1,m2] [--reps 5] [--scenarios a,b]
                  [--env FILE] [--effort low] [--max-usd N] [--judge none|mistral/mistral-large-4-0]
                  [--no-build]
  run_eval.py rejudge --sha <sha> [--judge ...] [--models ...]
  run_eval.py report --sha <sha>

Records land under runs/<sha>/raw/<model>/<scenario>/repN.json; the summary and the
per-model report at runs/<sha>/summary.json and runs/<sha>/REPORT.md. The release gate
(tools/release_gate/check_owner_regressions.py) reads that summary. Thresholds come
from thresholds.json and were fixed before the first run.

Keys are read by name from the dotenv file (NIMBUS_ENV_FILE or --env) and never printed.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
DRIVER = HERE.parent / "honesty" / "driver" / "build" / "hon_driver"
RUNS = HERE / "runs"
sys.path.insert(0, str(REPO / "tools" / "harness-lab"))
sys.path.insert(0, str(HERE))
import spend_ledger  # noqa: E402  (the lab's provider price table)
import assertions as A  # noqa: E402

THRESHOLDS = json.loads((HERE / "thresholds.json").read_text())
REP_RECORD = re.compile(r"rep\d+\.json$")
EM_DASH = chr(0x2014)


# ---- scenarios -----------------------------------------------------------------------


def load_scenarios(only: str | None = None) -> list:
    out = []
    for path in sorted((HERE / "scenarios").glob("*.json")):
        sc = json.loads(path.read_text())
        sc["_path"] = str(path)
        out.append(sc)
    if only:
        want = [s.strip() for s in only.split(",") if s.strip()]
        out = [s for s in out if s["id"] in want]
        missing = set(want) - {s["id"] for s in out}
        if missing:
            raise SystemExit(f"unknown scenario(s): {', '.join(sorted(missing))}")
    return out


def check_scenario(sc: dict) -> list:
    """The problems one scenario file has (empty when it is well formed)."""
    errs = [f"missing {key}" for key in ("id", "rows", "title", "steps", "assertions") if not sc.get(key)]
    if not sc.get("id", "").startswith("lc-"):
        errs.append("id must start with lc-")
    for st in sc.get("steps", []):
        if st.get("do") not in ("say", "sub", "row", "file", "pref", "scheduled"):
            errs.append(f"unknown step {st.get('do')!r}")
        if st.get("do") == "sub" and not st.get("name"):
            errs.append("sub step missing name")
        if st.get("do") == "scheduled" and not st.get("text"):
            errs.append("scheduled step missing text")
    for a in sc.get("assertions", []):
        aid = a["id"] if isinstance(a, dict) else a
        if aid not in A.known_ids():
            errs.append(f"unknown assertion {aid!r}")
    if EM_DASH in json.dumps(sc, ensure_ascii=False):
        errs.append("em dash in the scenario")
    return errs


# ---- keys ------------------------------------------------------------------------------

KEY_NAMES = ("ANTHROPIC_API_KEY", "OPENAI_API_KEY", "MISTRAL_API_KEY", "Z_AI_TOKEN", "TAVILY_API_KEY")


def read_key(env_file: str, name: str) -> str:
    """A key by name from the dotenv file. Never printed."""
    try:
        for line in Path(env_file).read_text().splitlines():
            line = line.strip()
            if line.startswith("export "):
                line = line[7:]
            if line.startswith(name + "="):
                return line.split("=", 1)[1].strip().strip('"').strip("'")
    except OSError:
        pass
    return ""


def driver_env() -> dict:
    """The driver's environment without the provider keys, so it reads the dotenv file."""
    return {k: v for k, v in os.environ.items() if k not in KEY_NAMES}


# ---- the soft judge (never gates) ------------------------------------------------------


def judge_prompt(sc: dict, transcript: dict) -> str:
    lines = []
    for r in sc.get("history", []):
        lines.append(f"{'Owner' if r['role'] == 'user' else 'Assistant'}: {r['text']}")
    for st in sc.get("steps", []):
        if st["do"] == "say":
            lines.append(f"Owner: {st['text']}")
        elif st["do"] == "row":
            lines.append(f"{'Owner' if st['role'] == 'user' else 'Assistant'}: {st['text']}")
    turns = transcript.get("turns") or []
    for t in turns:
        lines.append(f"Assistant: {t.get('reply', '')}")
    crit = "\n".join(f"- {c['id']}: {c['text']}" for c in sc.get("criteria", []))
    return (
        f"FACTS THE DEVICE RECORDED\n{sc.get('facts', '')}\n\n"
        f"CONVERSATION\n" + "\n\n".join(lines) + f"\n\nCRITERIA\n{crit}\n\nGrade every criterion. JSON only."
    )


def call_judge(judge: str, prompt: str, env_file: str) -> tuple:
    provider, model = judge.split("/", 1)
    if provider == "mistral":
        body = {
            "model": model,
            "max_tokens": 1200,
            "response_format": {"type": "json_object"},
            "messages": [
                {"role": "system", "content": "You grade device replies against recorded facts. JSON only."},
                {"role": "user", "content": prompt},
            ],
        }
        req = urllib.request.Request(
            "https://api.mistral.ai/v1/chat/completions",
            data=json.dumps(body).encode(),
            headers={
                "authorization": "Bearer " + read_key(env_file, "MISTRAL_API_KEY"),
                "content-type": "application/json",
            },
        )
    elif provider == "anthropic":
        body = {
            "model": model,
            "max_tokens": 1200,
            "system": "You grade device replies against recorded facts. JSON only.",
            "messages": [{"role": "user", "content": prompt}],
        }
        req = urllib.request.Request(
            "https://api.anthropic.com/v1/messages",
            data=json.dumps(body).encode(),
            headers={
                "x-api-key": read_key(env_file, "ANTHROPIC_API_KEY"),
                "anthropic-version": "2023-06-01",
                "content-type": "application/json",
            },
        )
    else:
        raise SystemExit(f"judge provider {provider!r} is not supported (mistral, anthropic)")
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                data = json.loads(r.read())
            if provider == "mistral":
                text = data["choices"][0]["message"]["content"]
                usage = data.get("usage") or {}
            else:
                text = "".join(b.get("text", "") for b in data.get("content", []) if b.get("type") == "text")
                usage = data.get("usage") or {}
            t = text.strip()
            start, end = t.find("{"), t.rfind("}")
            verdict = json.loads(t[start : end + 1]) if start >= 0 and end > start else {"criteria": []}
            return verdict, usage
        except urllib.error.HTTPError as e:
            if e.code in (429, 500, 502, 503, 529) and attempt < 2:
                time.sleep(5 * (attempt + 1))
                continue
            return {"criteria": [], "error": f"HTTP {e.code}"}, {}
        except (ValueError, urllib.error.URLError) as e:
            return {"criteria": [], "error": str(e)}, {}
    return {"criteria": [], "error": "no answer"}, {}


def judge_cost(judge: str, usage: dict) -> tuple:
    provider, model = judge.split("/", 1)
    return provider, spend_ledger.cost(provider, model, usage)[0] if usage else 0.0


# ---- runs --------------------------------------------------------------------------------


def price_usage(rec_dir: Path) -> dict:
    out = {}
    usage = rec_dir / "usage.jsonl"
    if usage.exists():
        for row in spend_ledger.price_file(usage):
            out[row["provider"]] = out.get(row["provider"], 0.0) + row["cost_usd"]
    return out


def run_records(raw: Path) -> list:
    return sorted(p for p in raw.glob("*/*/rep*.json") if REP_RECORD.match(p.name))


def run_one(sc: dict, model: str, host: str, args, rep: int, rep_dir: Path) -> dict:
    rep_dir.mkdir(parents=True, exist_ok=True)
    rec = rep_dir / f"rep{rep}.rec"
    rec.mkdir(exist_ok=True)
    transcript_path = rep_dir / f"rep{rep}.transcript.json"
    cmd = [
        str(DRIVER),
        "--scenario",
        sc["_path"],
        "--out",
        str(transcript_path),
        "--host",
        host,
        "--model",
        model,
        "--env",
        args.env,
        "--effort",
        args.effort,
        "--record",
        str(rec),
    ]
    if rep == 1:
        cmd += ["--system-out", str(rep_dir / "rep1.system.json")]
    t0 = time.time()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=900, env=driver_env())
    except subprocess.TimeoutExpired as e:
        proc = subprocess.CompletedProcess(cmd, 124, str(e.stdout or ""), "timed out after 900 s")
    (rep_dir / f"rep{rep}.driver.log").write_text(proc.stdout[-20000:] + "\n--- stderr ---\n" + proc.stderr[-20000:])
    record = {
        "scenario": sc["id"],
        "rows": sc.get("rows", []),
        "rep": rep,
        "host": host,
        "model": model,
        "driver_rc": proc.returncode,
        "seconds": round(time.time() - t0, 1),
    }
    transcript = (
        json.loads(transcript_path.read_text(encoding="utf-8", errors="replace")) if transcript_path.exists() else {}
    )
    record["transcript"] = transcript
    record["spend"] = price_usage(rec)
    record["assertions"] = A.run_all(sc, transcript)
    record["pass"] = (
        bool(record["assertions"])
        and all(a["pass"] for a in record["assertions"])
        and proc.returncode == 0
        and bool(A.last_turn(transcript).get("reply"))
    )
    record["judge"] = None
    return record


def judge_record(record: dict, sc: dict, judge: str, env_file: str) -> None:
    """Soft signal only: the judge's verdict is recorded, never merged into `pass`."""
    if not A.last_turn(record.get("transcript", {})).get("reply") or not sc.get("criteria"):
        return
    verdict, usage = call_judge(judge, judge_prompt(sc, record["transcript"]), env_file)
    prov, usd = judge_cost(judge, usage)
    record["judge"] = {"model": judge, "verdict": verdict, "usage": usage}
    record["spend"][prov] = round(record["spend"].get(prov, 0.0) + usd, 6)


def summarize(sha: str, raw: Path) -> dict:
    models = THRESHOLDS["models"]
    summary = {
        "suite": "owner_regressions",
        "sha": sha,
        "runs": THRESHOLDS["runs"],
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "models": {},
        "spend_usd": {},
    }
    for path in run_records(raw):
        r = json.loads(path.read_text())
        m = summary["models"].setdefault(r["model"], {"host": r["host"], "scenarios": {}, "spend_usd": 0.0})
        s = m["scenarios"].setdefault(r["scenario"], {"runs": 0, "passes": 0, "failed": {}, "spend_usd": 0.0})
        s["rows"] = r.get("rows", [])
        s["runs"] += 1
        s["passes"] += 1 if r.get("pass") else 0
        if not r.get("pass"):
            for a in r.get("assertions", []):
                if not a["pass"]:
                    s["failed"][a["id"]] = s["failed"].get(a["id"], 0) + 1
        usd = sum(r.get("spend", {}).values())
        s["spend_usd"] = round(s["spend_usd"] + usd, 6)
        m["spend_usd"] = round(m["spend_usd"] + usd, 6)
        for prov, v in r.get("spend", {}).items():
            summary["spend_usd"][prov] = round(summary["spend_usd"].get(prov, 0.0) + v, 6)
    for model, m in summary["models"].items():
        cfg = models.get(model, {})
        minp = cfg.get("min_passes", THRESHOLDS["default_min_passes"])
        for sid, s in m["scenarios"].items():
            xfail = THRESHOLDS["scenarios"].get(sid, {}).get("xfail", False)
            s["min_passes"] = minp
            s["xfail"] = xfail
            s["meets"] = (not xfail) and s["runs"] >= THRESHOLDS["runs"] and s["passes"] >= minp
            # Flip signal for the release gate: an xfail row that met its threshold is
            # stale - its owning lane has landed and the row must be unmarked, not left
            # skipping (check_owner_regressions.py blocks on this).
            s["xfail_met"] = xfail and s["runs"] >= THRESHOLDS["runs"] and s["passes"] >= minp
    return summary


def write_report(summary: dict, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "summary.json").write_text(json.dumps(summary, indent=2))
    lines = [
        f"# Owner-regression live suite - sha {summary['sha'][:12]}",
        "",
        f"Generated {summary['generated_utc']}. Pass = all hard assertions (assertions.py) on the rep; "
        "the judge is a soft signal only. Target: {p}/{r} per model.".format(
            p=THRESHOLDS["default_min_passes"], r=summary["runs"]
        ),
        "",
    ]
    for model in sorted(summary["models"]):
        m = summary["models"][model]
        lines += [
            f"## {model} ({m['host']}) - spend {m['spend_usd']:.4f} USD",
            "",
            "| Scenario | Rows | Passes | Target | Met | xfail | Failed assertions |",
            "|---|---|---|---|---|---|---|",
        ]
        for sid in sorted(m["scenarios"]):
            s = m["scenarios"][sid]
            failed = "; ".join(f"{k}x{v}" for k, v in sorted(s["failed"].items())) or "-"
            lines.append(
                f"| {sid} | {','.join(s.get('rows', []))} | {s['passes']}/{s['runs']} | {s['min_passes']}/{summary['runs']} "
                f"| {'met' if s['meets'] else 'NOT MET'} | {'yes' if s['xfail'] else 'no'} | {failed} |"
            )
        lines.append("")
    lines.append(
        "Spend (provider-reported usage, USD): "
        + ", ".join(f"{p} {v:.4f}" for p, v in sorted(summary["spend_usd"].items()))
    )
    (out / "REPORT.md").write_text("\n".join(lines) + "\n")


def cmd_list(args) -> int:
    for sc in load_scenarios():
        print(f"{sc['id']:28s} rows={','.join(sc['rows'])}  assertions={len(sc['assertions'])}")
    return 0


def cmd_validate(args) -> int:
    bad = 0
    for sc in load_scenarios():
        errs = check_scenario(sc)
        if errs:
            bad += 1
            print(f"{sc['id']}: {'; '.join(errs)}")
    print(f"{len(load_scenarios())} scenarios, {bad} invalid")
    return 1 if bad else 0


def run_rep(sc: dict, model: str, args, rep: int, path: Path, ctx: dict) -> None:
    """One rep: reuse the record on disk, or run the driver and write it."""
    spent, cap, host = ctx["spent"], ctx["cap"], ctx["host"]
    if path.exists():
        spent[model] = spent.get(model, 0.0) + sum(json.loads(path.read_text()).get("spend", {}).values())
        return
    if cap and spent.get(model, 0.0) >= cap:
        print(f"stop {model}: {spent[model]:.4f} USD reached the cap {cap}")
        return
    record = run_one(sc, model, host, args, rep, path.parent)
    path.write_text(json.dumps(record, indent=2, ensure_ascii=False))
    spent[model] = spent.get(model, 0.0) + sum(record["spend"].values())
    if args.judge != "none":
        judge_record(record, sc, args.judge, args.env)
        path.write_text(json.dumps(record, indent=2, ensure_ascii=False))
    print(
        f"{model} {sc['id']} rep{rep}: {'PASS' if record['pass'] else 'FAIL'} "
        f"({sum(record['spend'].values()):.4f} USD, {record['seconds']}s)",
        flush=True,
    )


def cmd_run(args) -> int:
    if not args.no_build:
        subprocess.run(["make", "-s", "-C", str(HERE.parent / "honesty" / "driver")], check=True)
    elif not DRIVER.exists():
        raise SystemExit("--no-build: the driver is not built (make -C tests/evals/honesty/driver)")
    sha = (
        args.sha
        or subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, cwd=REPO).stdout.strip()
    )
    out = RUNS / sha
    raw = out / "raw"
    models = (
        THRESHOLDS["models"]
        if args.models == "all"
        else {m: THRESHOLDS["models"][m] for m in args.models.split(",") if m}
    )
    scen = {s["id"]: s for s in load_scenarios(args.scenarios)}
    for sc in scen.values():
        errs = check_scenario(sc)
        if errs:
            raise SystemExit(f"{sc['id']}: {'; '.join(errs)}")
    caps = {"anthropic": 10.0, "openai": 10.0}  # TASK.md spend caps; mistral/zai uncapped
    spent = {}
    for model, cfg in models.items():
        cap = args.max_usd if args.max_usd else caps.get(cfg["host"])
        for sc in scen.values():
            for rep in range(1, args.reps + 1):
                run_rep(
                    sc,
                    model,
                    args,
                    rep,
                    raw / model / sc["id"] / f"rep{rep}.json",
                    {"host": cfg["host"], "spent": spent, "cap": cap},
                )
    summary = summarize(sha, raw)
    summary["started_utc"] = summary.get("generated_utc")
    write_report(summary, out)
    print(f"summary: {out / 'summary.json'}")
    return 0


def cmd_rejudge(args) -> int:
    sha = args.sha
    raw = RUNS / sha / "raw"
    scen = {s["id"]: s for s in load_scenarios()}
    models = set(THRESHOLDS["models"]) if args.models == "all" else set(args.models.split(","))
    for path in run_records(raw):
        r = json.loads(path.read_text())
        if r["model"] not in models:
            continue
        judge_record(r, scen[r["scenario"]], args.judge, args.env)
        path.write_text(json.dumps(r, indent=2, ensure_ascii=False))
    write_report(summarize(sha, raw), RUNS / sha)
    return 0


def cmd_report(args) -> int:
    raw = RUNS / args.sha / "raw"
    if not raw.exists():
        raise SystemExit(f"no run records under {raw}")
    write_report(summarize(args.sha, raw), RUNS / args.sha)
    print(f"report: {RUNS / args.sha / 'REPORT.md'}")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("validate")
    env = os.environ.get("NIMBUS_ENV_FILE", str(REPO / ".env"))
    r = sub.add_parser("run")
    r.add_argument("--sha", default=None)
    r.add_argument("--models", default="all")
    r.add_argument("--reps", type=int, default=THRESHOLDS["runs"])
    r.add_argument("--scenarios", default=None)
    r.add_argument("--env", default=env)
    r.add_argument("--effort", default="low")
    r.add_argument("--max-usd", type=float, default=None)
    r.add_argument("--judge", default="none")
    r.add_argument("--no-build", action="store_true")
    j = sub.add_parser("rejudge")
    j.add_argument("--sha", required=True)
    j.add_argument("--models", default="all")
    j.add_argument("--judge", required=True)
    j.add_argument("--env", default=env)
    rep = sub.add_parser("report")
    rep.add_argument("--sha", required=True)
    args = p.parse_args(argv)
    return {"list": cmd_list, "validate": cmd_validate, "run": cmd_run, "rejudge": cmd_rejudge, "report": cmd_report}[
        args.cmd
    ](args)


if __name__ == "__main__":
    sys.exit(main())
