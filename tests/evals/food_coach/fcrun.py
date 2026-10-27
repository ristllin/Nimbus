"""Run scenarios through a backend, judge the transcripts, price the spend."""

from __future__ import annotations

import datetime as dt
import json
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

import fcjudge
import fcworld

REPO = fcworld.HERE.parents[2]
DRIVER = fcworld.HERE / "driver" / "build" / "fc_driver"
RETRY_WAIT_S = 20


def spend_ledger():
    """The lab's price table (provider-reported usage, never a device ledger)."""
    lab = str(REPO / "tools" / "harness-lab")
    if lab not in sys.path:
        sys.path.insert(0, lab)
    import spend_ledger as ledger

    return ledger


@dataclass
class Task:
    scn: dict
    rep: int
    label: str
    outdir: Path
    host: str
    model: str
    cfg: dict
    keys: dict
    judges: list[str]
    effort: str = "low"
    env_path: str = ""
    retries: int = 2
    timeout_s: int = 900
    backend: str = "lab"
    extra: dict = field(default_factory=dict)


def _git(*args: str) -> str:
    proc = subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True)
    return proc.stdout.strip()


def provenance(args) -> dict:
    """What a run measured: the commit, local edits, the driver build and the run settings."""
    dirty = [ln for ln in _git("status", "--porcelain").splitlines() if "tests/evals/" not in ln]
    built = DRIVER.stat().st_mtime if DRIVER.exists() else 0
    return {
        "label": args.label,
        "model": args.model,
        "host": args.host,
        "effort": args.effort,
        "reps": args.reps,
        "scenarios": args.scenarios or "all",
        "backend": args.backend,
        "git_head": _git("rev-parse", "HEAD"),
        "git_dirty_outside_evals": dirty,
        "driver_built": dt.datetime.fromtimestamp(built, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "started": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }


class Budget:
    """A thread-safe running total; once it passes the cap, new runs are skipped."""

    def __init__(self, cap_usd: float):
        self.cap, self.spent, self._lock = cap_usd, 0.0, threading.Lock()

    def add(self, usd: float) -> None:
        with self._lock:
            self.spent += usd

    def exhausted(self) -> bool:
        with self._lock:
            return self.spent >= self.cap


def head_cost(rec_dir: Path) -> dict:
    """Priced provider usage of one run: {'total': usd, 'by': {'provider model': usd}}."""
    ledger = spend_ledger()
    by: dict = {}
    for path in sorted(rec_dir.glob("usage*.jsonl")):
        for row in ledger.price_file(path):
            key = f"{row['provider']} {row['model']}"
            by[key] = by.get(key, 0.0) + row["cost_usd"]
    return {"total": round(sum(by.values()), 6), "by": {k: round(v, 6) for k, v in by.items()}}


def judge_cost(spec: str, usage: dict) -> float:
    provider, _, model = spec.partition("/")
    usd, _ = spend_ledger().cost(provider, model, usage)
    return round(usd, 6)


def _driver_cmd(t: Task, files: dict) -> list[str]:
    cmd = [
        str(DRIVER),
        "--scenario",
        str(files["run"]),
        "--out",
        str(files["out"]),
        "--host",
        t.host,
        "--model",
        t.model,
        "--effort",
        t.effort,
        "--record",
        str(files["rec"]),
        "--dev-name",
        "Pixel",
    ]
    if t.env_path:
        cmd += ["--env", t.env_path]
    if t.rep == 1:
        cmd += ["--system-out", str(files["system"])]
    return cmd


def _task_files(t: Task) -> dict:
    rdir = t.outdir / t.scn["id"]
    rdir.mkdir(parents=True, exist_ok=True)
    stem = f"rep{t.rep}"
    return {
        "dir": rdir,
        "run": rdir / f"{stem}.run.json",
        "out": rdir / f"{stem}.driver.json",
        "system": rdir / f"{stem}.system.json",
        "rec": rdir / f"{stem}.rec",
        "final": rdir / f"{stem}.json",
    }


def run_lab(t: Task, files: dict) -> dict:
    """One scenario run on the host lab; retries a failed or empty run."""
    payload = fcworld.run_payload(t.scn, t.cfg, dt.date.today())
    files["run"].write_text(json.dumps(payload, indent=1), encoding="utf-8")
    files["rec"].mkdir(exist_ok=True)
    attempts, stderr = 0, ""
    while attempts <= t.retries:
        attempts += 1
        files["out"].unlink(missing_ok=True)
        try:
            proc = subprocess.run(_driver_cmd(t, files), capture_output=True, text=True, timeout=t.timeout_s)
            stderr = proc.stderr[-600:]
            if proc.returncode == 0 and files["out"].exists():
                return {"attempts": attempts, "driver": json.loads(files["out"].read_text()), "error": ""}
        except subprocess.TimeoutExpired:
            stderr = "driver timed out"
        time.sleep(RETRY_WAIT_S * attempts)
    return {"attempts": attempts, "driver": None, "error": stderr or "driver failed"}


def run_device(t: Task, files: dict) -> dict:
    import device_leg

    return device_leg.run_task(t, files)


JUDGE_ATTEMPTS = 3


def _judge_once(spec: str, t: Task, user: str, sha: str) -> dict:
    """One judge's verdict. A reply with no verdict for any criterion is asked again (up to
    JUDGE_ATTEMPTS calls, their cost summed); still nothing is a judge error, so the run
    counts as errored, never as a graded failure."""
    cost = 0.0
    for _ in range(JUDGE_ATTEMPTS):
        verdict, usage = fcjudge.call_judge(spec, t.keys, user)
        cost += judge_cost(spec, usage)
        norm = fcjudge.normalize(verdict, t.scn)
        if not norm.pop("empty"):
            return {"judge": spec, "rubric_sha": sha, "cost_usd": round(cost, 6), **norm}
    raise RuntimeError(f"judge returned no verdict in {JUDGE_ATTEMPTS} calls (cost {cost:.4f} USD)")


def judge_run(t: Task, turns: list[dict], history: list[dict] | None = None) -> list[dict]:
    """Judge a transcript. `history` is the earlier chat as it happened (the device leg's
    live replies); the scenario's seeded history otherwise."""
    rubric = (fcworld.HERE / "rubric.md").read_text(encoding="utf-8")
    facts = fcworld.facts_for(t.scn, t.cfg)
    history = t.scn["world"].get("history", []) if history is None else history
    user = fcjudge.build_user_prompt(rubric, t.scn, facts, fcjudge.render_transcript(history, turns))
    sha = fcjudge.rubric_sha(rubric, t.scn, facts)
    out = []
    for spec in t.judges:
        if out and t.extra.get("second_on_pass") and not out[0]["pass"]:
            break  # the second judge only re-checks a pass: a failure needs no second opinion
        try:
            out.append(_judge_once(spec, t, user, sha))
        except (RuntimeError, ValueError, KeyError) as exc:
            out.append(
                {"judge": spec, "rubric_sha": sha, "error": str(exc), "pass": False, "criteria": [], "cost_usd": 0.0}
            )
    return out


def _failed_ids(judges: list[dict], required: bool) -> list[str]:
    return sorted({c["id"] for j in judges for c in j["criteria"] if c["required"] == required and not c["pass"]})


def ungraded(rec: dict) -> bool:
    """A saved run some judge did not grade: an error, or no verdict for any criterion."""
    for j in rec.get("judges", []):
        crits = j.get("criteria") or []
        if "error" in j or (crits and all(c.get("evidence") == "no verdict returned" for c in crits)):
            return True
    return False


def decide(judges: list[dict], errored: bool) -> dict:
    """Primary = first judge. Strict = every judge passes. An errored run never passes, and a
    run the first judge could not grade is errored (shown as such, not as a failure)."""
    errored = errored or bool(judges and "error" in judges[0])
    usable = [j for j in judges if "error" not in j]
    complete = bool(usable) and len(usable) == len(judges)
    return {
        "pass": bool(usable) and usable[0]["pass"] and not errored,
        "pass_strict": complete and all(j["pass"] for j in usable) and not errored,
        "failed": _failed_ids(usable, True),
        "advisory_failed": _failed_ids(usable, False),
        "judges_agree": None if len(usable) < 2 else len({j["pass"] for j in usable}) == 1,
        "errored": errored,
    }


def execute(t: Task, budget: Budget) -> dict:
    """Run, judge and save one scenario repetition; returns the saved record."""
    files = _task_files(t)
    if files["final"].exists():
        return json.loads(files["final"].read_text())
    if budget.exhausted():
        return {"scenario": t.scn["id"], "rep": t.rep, "skipped": "budget"}
    ran = run_lab(t, files) if t.backend == "lab" else run_device(t, files)
    drv = ran["driver"] or {}
    turns = drv.get("turns", [])
    errored = not turns or not all(x.get("ok") for x in turns)
    judges = judge_run(t, turns, drv.get("history")) if turns else []
    head = ran.get("cost") or (head_cost(files["rec"]) if files["rec"].exists() else {"total": 0.0, "by": {}})
    rec = {
        "scenario": t.scn["id"],
        "rep": t.rep,
        "label": t.label,
        "backend": t.backend,
        "host": t.host,
        "model": t.model,
        "effort": t.effort,
        "attempts": ran["attempts"],
        "error": ran["error"],
        "turns": turns,
        "history": drv.get("history"),
        "banner": drv.get("banner", ""),
        "judges": judges,
        "verdict": decide(judges, errored),
        "cost_usd": {"head": head, "judges": round(sum(j["cost_usd"] for j in judges), 6)},
    }
    budget.add(head["total"] + rec["cost_usd"]["judges"])
    files["final"].write_text(json.dumps(rec, indent=1), encoding="utf-8")
    return rec
