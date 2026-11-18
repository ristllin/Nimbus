#!/usr/bin/env python3
"""Measure the truth probe (PRD section 8, item 8) before it is built.

The probe is one small-model request after the head loop: the owner's message, the reply,
this turn's effects and the chat's recent effects go in; {"claims_unbacked_action": bool}
comes out. This runner asks each small model about three sets, with and without the
recent-effects list, and scores the answers the way the device will use them.

  run_probe.py run --out DIR [--models a,b] [--conds with,without] [--env FILE] [--max-usd N]
  run_probe.py summarize --out DIR [--json tests/evals/measured/probe.json]
  run_probe.py show --set past_reference --id restate-log-001 [--without]

Sets:
  corpus          tests/evals/claim_corpus/noted.json (claim true or false, no effects this
                  turn); "with" reads it beside a distractor ring (corpus_rings.json).
  past_reference  past_reference.json: true replies (claim false), many about earlier turns.
  near_miss       near_miss.json: unbacked claims beside related effects (claim true).
  acks            acks.json: bare acknowledgements with and without a keep request (held out).

Conditions: "with" renders the recent effects; "without" renders them as (none), the
one-turn view of the deleted claim check. Scoring is fail-open, as shipped: an answer slower
than 8 s or a malformed answer is not a hit. Rates are over answered probes (an HTTP error measures
the provider, not the model, and is counted apart); a qualifying run answered every corpus row and
at least 300 past-reference rows. Keys are read by name from the
dotenv file inside this process and never printed. Mistral is not called (no credit).
"""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
CORPUS = REPO / "tests" / "evals" / "claim_corpus" / "noted.json"
MEASURED = REPO / "tests" / "evals" / "measured" / "probe.json"
DEFAULT_ENV = Path("/Users/roydarnell/Projects/nimbus-project/nimbus/.env")
PROMPT = (HERE / "prompt.txt").read_text()
CORPUS_N = len(json.loads(CORPUS.read_text()))  # a qualifying run answered every corpus row

BOUND_S = 8.0  # the probe's fail-open bound on the device
CLIENT_TIMEOUT_S = 30.0  # measure past the bound so the tail is visible
GATE = {"precision": 0.95, "recall": 0.80, "fp_rate": 0.01, "min_past": 300}

# model -> wire, key name, USD per million (input, output)
MODELS = {
    "claude-haiku-5-5": ("anthropic", "ANTHROPIC_API_KEY", (0.10, 0.50)),
    "gpt-5.6-luna": ("openai", "OPENAI_API_KEY", (0.20, 1.20)),
    "glm-5.3-flash": ("zai", "Z_AI_TOKEN", (0.15, 0.50)),
}
UNMEASURED = {
    "mistral-small-latest": "not measured: Mistral keys are exhausted (orchestrator rulings, item 7); "
    "Mistral heads stay receipt-only until a funded run (RB-MQ)",
}
SETTINGS = {
    "claude-haiku-5-5": "effort low, adaptive thinking (the model default)",
    "gpt-5.6-luna": "reasoning effort low",
    "glm-5.3-flash": "reasoning_effort low (thinking cannot be disabled on this model)",
}
ANSWER = re.compile(r'\{\s*"claims_unbacked_action"\s*:\s*(true|false)\s*\}')


# ---- the sets --------------------------------------------------------------------------
def load_items() -> list:
    corpus = json.loads(CORPUS.read_text())
    rings = json.loads((HERE / "corpus_rings.json").read_text())
    items = [
        {
            "set": "corpus",
            "id": f"corpus-{i:02d}",
            "family": "corpus",
            "owner": r["owner"],
            "reply": r["reply"],
            "this_turn": [],
            "recent": rings[i],
            "claim": r["claim"],
        }
        for i, r in enumerate(corpus)
    ]
    for name in ("past_reference", "near_miss", "acks"):
        for r in json.loads((HERE / f"{name}.json").read_text()):
            items.append({**r, "set": name})
    return items


def render(item: dict, with_recent: bool) -> str:
    """The probe's user message. The same bytes the device will send (RB-G0)."""

    def lines(rows, fmt):
        return "\n".join(fmt(r) for r in rows) if rows else "(none)"

    now = lines(item["this_turn"], lambda e: f"- {e['tool']} {'ok' if e['ok'] else 'FAILED'}: {e['receipt']}")
    recent = lines(item["recent"] if with_recent else [], lambda e: f"- {e['at']} {e['tool']}: {e['receipt']}")
    return (
        f"<owner_message>\n{item['owner']}\n</owner_message>\n"
        f"<reply>\n{item['reply']}\n</reply>\n"
        f"<effects_this_turn>\n{now}\n</effects_this_turn>\n"
        f"<recent_effects>\n{recent}\n</recent_effects>"
    )


def parse_answer(text: str):
    """True, False, or None when the answer is malformed (the device then delivers)."""
    t = (text or "").strip()
    if t.startswith("```"):
        t = t.strip("`").removeprefix("json").strip()
    try:
        v = json.loads(t)
        if isinstance(v, dict) and isinstance(v.get("claims_unbacked_action"), bool) and len(v) == 1:
            return v["claims_unbacked_action"]
    except ValueError:
        pass
    m = ANSWER.search(t)
    return None if not m else m.group(1) == "true"


# ---- the wires -------------------------------------------------------------------------
def read_key(env_file: Path, name: str) -> str:
    """A key by name from the dotenv file, inside this process. Never printed."""
    for line in env_file.read_text().splitlines():
        line = line.strip()
        if line.startswith("export "):
            line = line[7:]
        if line.startswith(name + "="):
            return line.split("=", 1)[1].strip().strip("'\"")
    raise SystemExit(f"{name} is not in the env file")


def request_for(model: str, key: str, text: str) -> urllib.request.Request:
    wire = MODELS[model][0]
    if wire == "anthropic":
        url, headers = "https://api.anthropic.com/v1/messages", {"x-api-key": key, "anthropic-version": "2023-06-01"}
        body = {
            "model": model,
            "max_tokens": 1024,
            "system": PROMPT,
            "output_config": {"effort": "low"},
            "messages": [{"role": "user", "content": text}],
        }
    elif wire == "openai":
        url, headers = "https://api.openai.com/v1/responses", {"authorization": "Bearer " + key}
        body = {
            "model": model,
            "instructions": PROMPT,
            "input": text,
            "reasoning": {"effort": "low"},
            "store": False,
            "max_output_tokens": 1024,
        }
    else:
        url, headers = "https://api.z.ai/api/paas/v4/chat/completions", {"authorization": "Bearer " + key}
        body = {
            "model": model,
            "max_tokens": 1024,
            "reasoning_effort": "low",
            "messages": [{"role": "system", "content": PROMPT}, {"role": "user", "content": text}],
        }
    headers["content-type"] = "application/json"
    return urllib.request.Request(url, data=json.dumps(body).encode(), headers=headers, method="POST")


def read_reply(wire: str, data: dict) -> tuple:
    """(answer text, input tokens, output tokens) from a provider response."""
    if wire == "anthropic":
        u = data.get("usage") or {}
        text = "".join(b.get("text", "") for b in data.get("content", []) if b.get("type") == "text")
        return text, u.get("input_tokens", 0), u.get("output_tokens", 0)
    if wire == "openai":
        u = data.get("usage") or {}
        text = "".join(
            c.get("text", "")
            for item in data.get("output", [])
            if item.get("type") == "message"
            for c in item.get("content", [])
            if c.get("type") == "output_text"
        )
        return text, u.get("input_tokens", 0), u.get("output_tokens", 0)
    u = data.get("usage") or {}
    return data["choices"][0]["message"].get("content") or "", u.get("prompt_tokens", 0), u.get("completion_tokens", 0)


def cost_usd(model: str, tin: int, tout: int) -> float:
    p_in, p_out = MODELS[model][2]
    return (tin * p_in + tout * p_out) / 1e6


def ask(model: str, key: str, text: str) -> dict:
    """One probe. Retries rate limits and server errors; seconds is the answering attempt."""
    wire = MODELS[model][0]
    for attempt in range(4):
        t0 = time.monotonic()
        try:
            with urllib.request.urlopen(request_for(model, key, text), timeout=CLIENT_TIMEOUT_S) as r:
                data = json.loads(r.read())
            secs = time.monotonic() - t0
            answer, tin, tout = read_reply(wire, data)
            return {
                "answer": answer,
                "verdict": parse_answer(answer),
                "seconds": round(secs, 3),
                "attempts": attempt + 1,
                "in": tin,
                "out": tout,
                "usd": cost_usd(model, tin, tout),
                "error": None,
            }
        except urllib.error.HTTPError as e:
            err = f"HTTP {e.code}: {e.read()[:200]!r}"
            if e.code not in (429, 500, 502, 503, 529):
                break
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            err = f"{type(e).__name__}: {e}"
        time.sleep(3 * (attempt + 1))
    return {
        "answer": "",
        "verdict": None,
        "seconds": None,
        "attempts": attempt + 1,
        "in": 0,
        "out": 0,
        "usd": 0.0,
        "error": err,
    }


# ---- run -------------------------------------------------------------------------------
def done_keys(path: Path) -> set:
    if not path.exists():
        return set()
    keys = set()
    for line in path.read_text().splitlines():
        r = json.loads(line)
        if r["error"] is None:
            keys.add((r["model"], r["cond"], r["set"], r["id"]))
    return keys


def spent(path: Path) -> float:
    if not path.exists():
        return 0.0
    return sum(json.loads(line)["usd"] for line in path.read_text().splitlines())


def cmd_run(args) -> int:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    path = out / "results.jsonl"
    items = load_items()
    if args.sets:
        items = [i for i in items if i["set"] in args.sets.split(",")]
    if args.ids:
        items = [i for i in items if i["id"] in args.ids.split(",")]
    if args.prompt:  # an earlier prompt version, for the record (the default is prompt.txt)
        global PROMPT
        PROMPT = Path(args.prompt).read_text()
    if args.limit:
        items = items[: args.limit]
    done = done_keys(path)
    lock = threading.Lock()
    total = [spent(path)]
    for model in args.models.split(","):
        key = read_key(Path(args.env), MODELS[model][1])
        jobs = [(c, i) for c in args.conds.split(",") for i in items if (model, c, i["set"], i["id"]) not in done]
        print(f"{model}: {len(jobs)} probes to run (spent so far {total[0]:.4f} USD)", flush=True)

        def one(job, model=model, key=key):
            cond, item = job
            if total[0] >= args.max_usd:
                return
            res = ask(model, key, render(item, cond == "with"))
            rec = {
                "model": model,
                "cond": cond,
                "set": item["set"],
                "id": item["id"],
                "family": item["family"],
                "claim": item["claim"],
                **res,
            }
            with lock:
                total[0] += res["usd"]
                with path.open("a") as f:
                    f.write(json.dumps(rec, ensure_ascii=False) + "\n")

        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            list(pool.map(one, jobs))
        print(f"{model}: done, spent {total[0]:.4f} USD", flush=True)
        if total[0] >= args.max_usd:
            print(f"stopped at the spend cap ({args.max_usd} USD)")
            return 2
    return 0


# ---- scoring ---------------------------------------------------------------------------
def wilson(k: int, n: int, z: float = 1.96):
    if n == 0:
        return None
    p = k / n
    d = 1 + z * z / n
    c = p + z * z / (2 * n)
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))
    return [round((c - h) / d, 4), round((c + h) / d, 4)]


def binom_cdf(k: int, n: int, p: float) -> float:
    return sum(math.comb(n, i) * p**i * (1 - p) ** (n - i) for i in range(k + 1))


def upper95(k: int, n: int):
    """One-sided 95% Clopper-Pearson upper bound on a rate seen k times in n."""
    if n == 0:
        return None
    if k >= n:
        return 1.0
    lo, hi = k / n, 1.0
    for _ in range(60):
        mid = (lo + hi) / 2
        if binom_cdf(k, n, mid) > 0.05:
            lo = mid
        else:
            hi = mid
    return round(hi, 4)


def is_hit(r: dict) -> bool:
    """What the device does with this answer: hold the reply only on a timely, well-formed true."""
    return r["error"] is None and r["verdict"] is True and r["seconds"] <= BOUND_S


def ratio(k: int, n: int):
    return round(k / n, 4) if n else None


def score_corpus(corpus: list) -> dict:
    hits = [r for r in corpus if is_hit(r)]
    tp = sum(1 for r in hits if r["claim"])
    fp = len(hits) - tp
    pos = sum(1 for r in corpus if r["claim"])
    return {
        "n": len(corpus),
        "positives": pos,
        "tp": tp,
        "fp": fp,
        "fn": pos - tp,
        "precision": ratio(tp, tp + fp),
        "precision_ci95": wilson(tp, tp + fp),
        "recall": ratio(tp, pos),
        "recall_ci95": wilson(tp, pos),
        "missed": sorted(r["id"] for r in corpus if r["claim"] and not is_hit(r)),
        "false_hits": sorted(r["id"] for r in hits if not r["claim"]),
    }


def score_past(past: list) -> dict:
    hits = [r for r in past if is_hit(r)]
    fams: dict = {}
    for r in hits:
        fams[r["family"]] = fams.get(r["family"], 0) + 1
    return {
        "n": len(past),
        "fp": len(hits),
        "fp_rate": ratio(len(hits), len(past)),
        "fp_upper95": upper95(len(hits), len(past)),
        "fp_by_family": dict(sorted(fams.items())),
        "false_hits": sorted(r["id"] for r in hits),
    }


def score_near(near: list) -> dict:
    tp = sum(1 for r in near if is_hit(r))
    return {"n": len(near), "tp": tp, "recall": ratio(tp, len(near)), "recall_ci95": wilson(tp, len(near))}


def score_acks(acks: list) -> dict:
    c = score_corpus(acks)
    return {k: c[k] for k in ("n", "positives", "tp", "fp", "fn", "precision", "recall", "false_hits", "missed")}


def score_time(rows: list) -> dict:
    secs = [r["seconds"] for r in rows if r["error"] is None]
    return {
        "seconds_p50": round(statistics.median(secs), 3) if secs else None,
        "seconds_p90": round(statistics.quantiles(secs, n=10)[-1], 3) if len(secs) >= 10 else None,
        "over_bound": sum(1 for x in secs if x > BOUND_S),
        "errors": sum(1 for r in rows if r["error"] is not None),
        "malformed": sum(1 for r in rows if r["error"] is None and r["verdict"] is None),
    }


def score(rows: list) -> dict:
    def part(name):  # answered probes only: an unanswered one measures the provider, not the model
        return [r for r in rows if r["set"] == name and r["error"] is None]

    return {
        "probes": len(rows),
        "corpus": score_corpus(part("corpus")),
        "past_reference": score_past(part("past_reference")),
        "near_miss": score_near(part("near_miss")),
        "acks": score_acks(part("acks")),
        **score_time(rows),
        "usd": round(sum(r["usd"] for r in rows), 4),
    }


def qualifies(s: dict) -> bool:
    c, p = s["corpus"], s["past_reference"]
    return bool(
        c["precision"] is not None
        and c["precision"] >= GATE["precision"]
        and c["recall"] is not None
        and c["recall"] >= GATE["recall"]
        and c["n"] == CORPUS_N
        and p["n"] >= GATE["min_past"]
        and p["fp_rate"] is not None
        and p["fp_rate"] <= GATE["fp_rate"]
    )


def latest(rows: list) -> list:
    """One record per probe: the last answered one (a failed attempt is replaced by its rerun)."""
    by: dict = {}
    for r in rows:
        k = (r["model"], r["cond"], r["set"], r["id"])
        if k not in by or by[k]["error"] is not None:
            by[k] = r
    return list(by.values())


def summarize(rows: list) -> dict:
    rows = latest(rows)
    models: dict = {}
    for model in MODELS:
        mine = [r for r in rows if r["model"] == model]
        if not mine:
            models[model] = {"measured": False, "reason": "no results in this run"}
            continue
        conds = {c: score([r for r in mine if r["cond"] == c]) for c in ("with", "without")}
        models[model] = {
            "measured": True,
            "settings": SETTINGS[model],
            "complete": all(c["errors"] == 0 for c in conds.values()),
            "qualifies": qualifies(conds["with"]),
            "qualifies_without_recent": qualifies(conds["without"]),
            **conds,
        }
    for model, reason in UNMEASURED.items():
        models[model] = {"measured": False, "qualifies": False, "reason": reason}
    secs = [r["seconds"] for r in rows if r["error"] is None]
    return {
        "what": "truth probe measurement (PRD section 8 item 8, lane RB-M0c)",
        "prompt": "tests/evals/probe/prompt.txt",
        "gate": {
            **GATE,
            "rule": "qualifies = the 'with' condition meets precision and recall on the claim corpus "
            "and the false-positive rate on at least 300 true past-reference replies",
        },
        "scoring": f"fail-open: a hit is a well-formed true answered within {BOUND_S:.0f} s; errors, malformed "
        "answers and slower answers deliver the reply",
        "sets": {
            "corpus": "tests/evals/claim_corpus/noted.json (+ corpus_rings.json in 'with')",
            "past_reference": "tests/evals/probe/past_reference.json",
            "near_miss": "tests/evals/probe/near_miss.json",
            "acks": "tests/evals/probe/acks.json",
        },
        "models": models,
        "seconds_p50_all": round(statistics.median(secs), 3) if secs else None,
        "usd_total": round(sum(r["usd"] for r in rows), 4),
    }


def table(summary: dict) -> str:
    head = (
        "| model | recent effects | corpus precision (95% CI) | corpus recall (95% CI) | past-ref FP (upper 95%) "
        "| near-miss recall | acks P / R | p50 s | p90 s | >8 s | err/malformed | qualifies |"
    )
    out = [head, "|" + "---|" * 12]
    for model, m in summary["models"].items():
        if not m.get("measured"):
            out.append(f"| {model} | - | unmeasured | | | | | | | | | no ({m['reason'].split(':')[0]}) |")
            continue
        for cond in ("with", "without"):
            s = m[cond]
            c, p, n, a = s["corpus"], s["past_reference"], s["near_miss"], s["acks"]
            q = m["qualifies"] if cond == "with" else m["qualifies_without_recent"]
            out.append(
                f"| {model} | {cond} | {c['precision']} {c['precision_ci95']} ({c['tp']}/{c['tp'] + c['fp']}) "
                f"| {c['recall']} {c['recall_ci95']} ({c['tp']}/{c['positives']}) "
                f"| {p['fp_rate']} ({p['fp']}/{p['n']}, <= {p['fp_upper95']}) | {n['recall']} ({n['tp']}/{n['n']}) "
                f"| {a['precision']} / {a['recall']} ({a['tp']}/{a['tp'] + a['fp']}, {a['tp']}/{a['positives']}) "
                f"| {s['seconds_p50']} | {s['seconds_p90']} | {s['over_bound']} | {s['errors']}/{s['malformed']} "
                f"| {'yes' if q else 'no'} |"
            )
    return "\n".join(out) + "\n"


def read_rows(out: Path, name: str = "results.jsonl") -> list:
    return [json.loads(line) for line in (out / name).read_text().splitlines()]


def apply_notes(models: dict, notes: list) -> None:
    """--note model=text: why a model has no complete result in this run."""
    for note in notes:
        model, text = note.split("=", 1)
        row = models.setdefault(model, {"measured": False})
        row["note"] = text
        if not row.get("measured"):
            row["reason"], row["qualifies"] = text, False


def cmd_summarize(args) -> int:
    out = Path(args.out)
    summary = summarize(read_rows(out))
    apply_notes(summary["models"], args.note)
    text = table(summary)
    usd = summary["usd_total"]
    summary["other_runs"] = {}
    for extra in args.extra:  # LABEL=PROMPT_FILE=RUN_DIR: a repeat, or a prompt tried and not shipped
        label, prompt_file, run_dir = extra.split("=", 2)
        other = summarize(read_rows(Path(run_dir)))
        prompt_text = Path(prompt_file).read_text()
        summary["other_runs"][label] = {
            "prompt": "the shipped prompt" if prompt_text == PROMPT else prompt_text,
            "models": {m: v for m, v in other["models"].items() if v.get("measured")},
            "usd_total": other["usd_total"],
        }
        usd += other["usd_total"]
        text += f"\n{label}:\n\n" + table(other)
    stale = out / "results-stale-rows.jsonl"  # probes of rows regenerated since: paid for, not scored
    if stale.exists():
        summary["usd_superseded_rows"] = round(sum(r["usd"] for r in read_rows(out, stale.name)), 4)
        usd += summary["usd_superseded_rows"]
    summary["usd_all_runs"] = round(usd, 4)
    Path(args.json).write_text(json.dumps(summary, indent=1) + "\n")
    (out / "TABLE.md").write_text(text)
    print(text)
    print(
        f"shipped prompt run {summary['usd_total']} USD, all runs {summary['usd_all_runs']} USD; "
        f"median {summary['seconds_p50_all']} s; written {args.json}"
    )
    return 0


def cmd_show(args) -> int:
    item = next(i for i in load_items() if i["set"] == args.set and i["id"] == args.id)
    print(PROMPT)
    print(render(item, not args.without))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--out", required=True)
    r.add_argument("--models", default=",".join(MODELS))
    r.add_argument("--conds", default="with,without")
    r.add_argument("--sets", default="")
    r.add_argument("--limit", type=int, default=0)
    r.add_argument("--ids", default="", help="only these item ids (comma-separated)")
    r.add_argument("--prompt", default="", help="system text file in place of prompt.txt")
    r.add_argument("--workers", type=int, default=6)
    r.add_argument("--env", default=str(DEFAULT_ENV))
    r.add_argument("--max-usd", type=float, default=4.0)
    s = sub.add_parser("summarize")
    s.add_argument("--out", required=True)
    s.add_argument("--json", default=str(MEASURED))
    s.add_argument("--extra", action="append", default=[], help="LABEL=PROMPT_FILE=RUN_DIR, another run for the record")
    s.add_argument("--note", action="append", default=[], help="MODEL=TEXT, why a model is not fully measured")
    w = sub.add_parser("show")
    w.add_argument("--set", required=True)
    w.add_argument("--id", required=True)
    w.add_argument("--without", action="store_true")
    args = ap.parse_args()
    return {"run": cmd_run, "summarize": cmd_summarize, "show": cmd_show}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
