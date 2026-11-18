#!/usr/bin/env python3
"""e1_arm - the same synthetic day through engine 1 (nimbusd's own harness) at a given build.

  e1_arm.py --daemon <nimbusd binary> --model claude-sonnet-5-5 --shape days-2026-11-15-17
            --evidence <dir> --env-file <dotenv> [--limit N] [--max-usd 6]

PAID. Rig arm A (tools/refloop/arms.py) on the given daemon: every owner turn of the shape posted
to the web chat, every provider exchange captured. The daemon need not have the test clock (main
does not), so the shape's gaps are applied in pricing only: the first head request of a turn
whose shape gap is over 300 s (and of the first turn) is priced cold, every cached token at the
cache-write price; on OpenAI, where writes are free, at the input price.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools" / "refloop"))

import arms  # noqa: E402
import fixtures  # noqa: E402
import nimbusd_ctl  # noqa: E402
import synth  # noqa: E402
from costs import rows_cost  # noqa: E402
from PIL import Image  # noqa: E402

MODELS = {
    "claude-sonnet-5-5": {"head": "anthropic", "effort": "medium", "sub_model": "claude-haiku-5-5"},
    "claude-haiku-5-5": {"head": "anthropic", "effort": "medium", "sub_model": "claude-haiku-5-5"},
    "gpt-5.6": {"head": "openai", "effort": None, "sub_model": "gpt-5.6-luna"},
}
# model -> (input, output, cache read, cache write) USD per million tokens. gpt-5.6 is the PRD's
# 5.00 / 30.00 with OpenAI's cached-input rate; OpenAI bills no cache write.
PRICES = {
    "claude-sonnet-5-5": (2.00, 10.00, 0.10, 2.50),
    "claude-haiku-5-5": (0.10, 0.50, 0.01, 0.125),
    "gpt-5.6": (5.00, 30.00, 0.50, 5.00),
    "gpt-5.6-luna": (0.20, 1.20, 0.02, 0.20),
    "text-embedding-3-small": (0.02, 0.0, 0.02, 0.02),
}
COLD_GAP_S = 300
CEILING_CENTS = 10_000  # the daemon's own monthly ceiling must not clamp the measurement


def usd(model: str, u: dict | None, cold: bool = False) -> float:
    if not u:
        return 0.0
    row = PRICES.get(model or "")
    if row is None:
        best = max((m for m in PRICES if (model or "").startswith(m)), key=len, default=None)
        row = PRICES[best] if best else (3.00, 15.00, 0.30, 3.75)
    p_in, p_out, p_read, p_write = row
    cached, written = u.get("cached", 0), u.get("cache_write", 0)
    fresh = max(u.get("input", 0) - cached - written, 0)
    if cold:
        cached, written = 0, written + cached
    return (fresh * p_in + cached * p_read + written * p_write + u.get("output", 0) * p_out) / 1e6


def png(kind: str, i: int) -> bytes:
    import base64

    _, b64, _ = synth.photo(kind, i)
    out = io.BytesIO()
    Image.open(io.BytesIO(base64.b64decode(b64))).save(out, format="PNG")
    return out.getvalue()


def scenario(shape_name: str, limit: int | None) -> tuple:
    shape = synth.load_shape(shape_name)
    turns = synth.turns_for(shape, seed=shape_name)[: limit or None]
    steps = []
    for t in turns:
        names = []
        for kind, i in t["photos"]:
            name = f"rbm0b-{kind}{i}"
            fixtures.PHOTOS[name] = {"make": (lambda k=kind, j=i: png(k, j)), "truth": f"synthetic {kind} {i}"}
            names.append(name)
        steps.append({"say": t["text"], **({"photos": names} if names else {})})
    return {"id": f"e1-{shape_name}", "turns": steps}, turns


class E1Arm(arms.ArmA):
    def start(self):
        super().start()
        self.d.configure(self.job.head, self.job.model, self.job.sub_model, ceiling_cents=CEILING_CENTS)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--daemon", required=True, type=Path)
    ap.add_argument("--model", required=True, choices=sorted(MODELS))
    ap.add_argument("--shape", default="days-2026-11-15-17")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--evidence", required=True, type=Path)
    ap.add_argument("--env-file", required=True)
    ap.add_argument("--max-usd", type=float, default=6.0, help="stop starting turns once this run has billed this much")
    a = ap.parse_args()
    nimbusd_ctl.DAEMON = a.daemon.resolve()
    env = nimbusd_ctl.load_env(a.env_file)
    sc, turns = scenario(a.shape, a.limit)
    out_dir = a.evidence / a.model / a.shape
    job = arms.Job(model=a.model, scenario=sc, rep=1, dir=out_dir, env=env, **MODELS[a.model])
    arm = E1Arm(job)
    rec = {"model": a.model, "shape": a.shape, "daemon": str(a.daemon), "steps": [], "started": time.time()}
    try:
        arm.start()
        for n, step in enumerate(sc["turns"]):
            if rows_cost(arm.proxy.records) >= a.max_usd:
                rec["held"] = f"stopped before turn {n + 1}: run billed {a.max_usd} USD"
                break
            s = arm.step(step)
            s.pop("state", None)
            s["kind"] = turns[n]["kind"]
            s["shape_gap_s"] = None if n == 0 else turns[n]["at_s"] - turns[n - 1]["at_s"]
            rec["steps"].append(s)
            out_dir.mkdir(parents=True, exist_ok=True)
            (out_dir / "e1.json").write_text(json.dumps(rec, indent=1, ensure_ascii=False))
    except Exception as e:
        rec["error"] = f"{type(e).__name__}: {e}"[:600]
    finally:
        rows = arm.proxy.rows()
        rec["requests"] = [
            {k: r.get(k) for k in ("seq", "ctx", "path", "model", "status", "ms", "usage")} for r in rows
        ]
        arm.close()
    # Price on the shape's gaps: a turn's first head request after a gap over 300 s is cold.
    for n, s in enumerate(rec["steps"]):
        mine = [r for r in rec["requests"] if (r.get("ctx") or {}).get("turn") == n + 1]
        head = [r for r in mine if r["model"] == a.model and r["usage"]]
        cold = n == 0 or (s["shape_gap_s"] or 0) > COLD_GAP_S
        first = head[0]["seq"] if head else None
        s["usd_billed"] = round(sum(usd(r["model"], r["usage"]) for r in mine), 6)
        s["usd_shape"] = round(sum(usd(r["model"], r["usage"], cold and r["seq"] == first) for r in mine), 6)
        s["usd_all_warm"] = s["usd_billed"]
        s["usd_cold"] = round(sum(usd(r["model"], r["usage"], r["seq"] == first) for r in mine), 6)
        s["cold_open"] = cold
    rec["ended"] = time.time()
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "e1.json").write_text(json.dumps(rec, indent=1, ensure_ascii=False))
    billed = sum(usd(r["model"], r["usage"]) for r in rec["requests"])
    print(f"done e1 {a.model} {a.shape}: {len(rec['steps'])} turns, billed {billed:.3f} USD {rec.get('error', '')}")


if __name__ == "__main__":
    main()
