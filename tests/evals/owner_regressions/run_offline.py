#!/usr/bin/env python3
"""Owner-regression offline suite: the scenarios a live model cannot replay.

A scenario with "mode": "offline" names its runner; the live suite (run_eval.py) leaves
it out. Today there is one runner, fold_replay: a hosted instance (nimbusd's NimbusdRig,
the device's fold path over the shared fold store and engine fold) with every provider
answer scripted, so the summary's own behavior is replayed with no network and no keys
(fold_replay/fold_replay.cpp says what a scenario may set). It waits out the owner-first
quiet bound in real time, about two minutes a scenario.

  run_offline.py list
  run_offline.py validate
  run_offline.py run [--scenarios a,b] [--no-build] [--out DIR]

Each run writes <out>/<scenario>.json (the replay's record plus the assertion outcomes)
and prints PASS or FAIL per scenario; the exit code is 1 when any scenario fails.
Records default to runs/offline/<git sha>/.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
SCENARIOS = HERE / "scenarios"
RUNNERS = {"fold_replay": HERE / "fold_replay" / "build" / "fold_replay"}
EM_DASH = chr(0x2014)

ASSERTIONS: dict = {}


def assertion(fn):
    ASSERTIONS[fn.__name__] = fn
    return fn


def fold_requests(rec: dict) -> list:
    return rec.get("fold_requests") or []


@assertion
def fold_requests_min(rec: dict, params: dict):
    """At least params['n'] fold requests were sent in the replay (the summary was tried)."""
    n = len(fold_requests(rec))
    return n >= int(params.get("n", 1)), f"{n} fold request(s)"


@assertion
def fold_requests_max(rec: dict, params: dict):
    """At most params['n'] fold requests were sent in the replay."""
    n = len(fold_requests(rec))
    return n <= int(params["n"]), f"{n} fold request(s), max {params['n']}"


@assertion
def no_repeat_host_model(rec: dict, params: dict):
    """No host and model was asked for a summary twice in the replay (a quiet evening, no clock advance)."""
    seen: dict = {}
    for r in fold_requests(rec):
        key = f"{r.get('host')}/{r.get('model')}"
        seen.setdefault(key, []).append(r.get("pass"))
    repeats = {k: v for k, v in seen.items() if len(v) > 1}
    if repeats:
        return False, "; ".join(f"{k} asked on passes {v}" for k, v in repeats.items())
    return True, f"{len(seen)} distinct host/model pair(s), none asked twice"


@assertion
def summary_contains(rec: dict, params: dict):
    """The chat's summary after the replay contains params['text']."""
    ok = params["text"] in (rec.get("summary_after") or "")
    return ok, "summary updated" if ok else f"summary after the replay lacks {params['text']!r}"


@assertion
def messages_forbid(rec: dict, params: dict):
    """No message the instance sent the owner contains any of params['any'] (case-insensitive)."""
    hits = [w for m in rec.get("messages") or [] for w in params.get("any", []) if w.lower() in m.lower()]
    return not hits, f"owner was told {sorted(set(hits))}" if hits else "no such message"


def grade(sc: dict, rec: dict) -> list:
    out = []
    for entry in sc.get("assertions", []):
        aid = entry if isinstance(entry, str) else entry["id"]
        params = {} if isinstance(entry, str) else entry.get("params", {})
        fn = ASSERTIONS.get(aid)
        if fn is None:
            out.append({"id": aid, "pass": False, "evidence": "unknown assertion"})
            continue
        ok, why = fn(rec, params)
        out.append({"id": aid, "pass": bool(ok), "evidence": why})
    return out


def is_offline(sc: dict) -> bool:
    return sc.get("mode") == "offline"


def load_offline(only: str | None = None) -> list:
    out = []
    for path in sorted(SCENARIOS.glob("*.json")):
        sc = json.loads(path.read_text())
        if is_offline(sc):
            sc["_path"] = str(path)
            out.append(sc)
    if only:
        want = {s.strip() for s in only.split(",") if s.strip()}
        missing = want - {s["id"] for s in out}
        if missing:
            raise SystemExit(f"unknown offline scenario(s): {', '.join(sorted(missing))}")
        out = [s for s in out if s["id"] in want]
    return out


def check_scenario(sc: dict) -> list:
    errs = [f"missing {k}" for k in ("id", "rows", "title", "runner", "assertions") if not sc.get(k)]
    if not sc.get("id", "").startswith("lc-"):
        errs.append("id must start with lc-")
    if sc.get("runner") not in RUNNERS:
        errs.append(f"unknown runner {sc.get('runner')!r}")
    for a in sc.get("assertions", []):
        aid = a if isinstance(a, str) else a.get("id")
        if aid not in ASSERTIONS:
            errs.append(f"unknown assertion {aid!r}")
    if EM_DASH in json.dumps(sc, ensure_ascii=False):
        errs.append("em dash in the scenario")
    return errs


def build() -> None:
    subprocess.run(["make", "-s", "-C", str(HERE / "fold_replay")], check=True)


def run_one(sc: dict, out_dir: Path) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    rec_path = out_dir / f"{sc['id']}.json"
    proc = subprocess.run(
        [str(RUNNERS[sc["runner"]]), "--scenario", sc["_path"], "--out", str(rec_path)],
        capture_output=True,
        text=True,
        errors="replace",
        timeout=900,
    )
    rec = json.loads(rec_path.read_text()) if rec_path.exists() else {}
    rec["runner_rc"] = proc.returncode
    rec["assertions"] = grade(sc, rec)
    rec["pass"] = proc.returncode == 0 and all(a["pass"] for a in rec["assertions"])
    rec_path.write_text(json.dumps(rec, indent=2))
    return rec


def cmd_list(args) -> int:
    for sc in load_offline():
        print(f"{sc['id']:28s} rows={','.join(sc['rows'])} runner={sc['runner']}")
    return 0


def cmd_validate(args) -> int:
    scen = load_offline()
    bad = 0
    for sc in scen:
        errs = check_scenario(sc)
        if errs:
            bad += 1
            print(f"{sc['id']}: {'; '.join(errs)}")
    print(f"{len(scen)} offline scenarios, {bad} invalid")
    return 1 if bad else 0


def cmd_run(args) -> int:
    scen = load_offline(args.scenarios)
    for sc in scen:
        errs = check_scenario(sc)
        if errs:
            raise SystemExit(f"{sc['id']}: {'; '.join(errs)}")
    if not args.no_build:
        build()
    sha = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, cwd=REPO).stdout.strip()
    out_dir = Path(args.out) if args.out else HERE / "runs" / "offline" / (sha or "unknown")
    failed = 0
    for sc in scen:
        rec = run_one(sc, out_dir)
        failed += 0 if rec["pass"] else 1
        print(f"{sc['id']}: {'PASS' if rec['pass'] else 'FAIL'} (runner rc={rec['runner_rc']})")
        for a in rec["assertions"]:
            print(f"  {'ok  ' if a['pass'] else 'FAIL'} {a['id']}: {a['evidence']}")
    print(f"offline: {len(scen) - failed}/{len(scen)} pass; records in {out_dir}")
    return 1 if failed else 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("validate")
    r = sub.add_parser("run")
    r.add_argument("--scenarios", default=None)
    r.add_argument("--no-build", action="store_true")
    r.add_argument("--out", default=None)
    args = p.parse_args(argv)
    return {"list": cmd_list, "validate": cmd_validate, "run": cmd_run}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
