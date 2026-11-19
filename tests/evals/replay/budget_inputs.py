#!/usr/bin/env python3
"""budget_inputs - the measured inputs of engine 2's per-head budget table (lane RB-T4).

  budget_inputs.py --m0b <RB-M0b evidence dir> --arm <full-arm evidence dir> --out tests/evals/measured/transcript_budget.json

Reads only numbers: the usage and body sizes of RB-M0b's replays (e2-aged/, e2/) and of the measured
full arm (run_replay.py --profile full --aged), the link and card timings of RB-M2
(tests/evals/measured/link.json) and the cold share and turns a day of RB-M0b
(tests/evals/measured/cost.json). test_tx_budget renders docs/reference/transcript-budgets.md from
the file this writes and fails when a measured figure contradicts its row by more than 15%.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import report

HERE = Path(__file__).resolve().parent
MEASURED = HERE.parent / "measured"


def runs_in(d: Path) -> list:
    return [json.loads(p.read_text()) for p in sorted(d.glob("run-*.json"))]


def photo_turns(run: dict) -> set:
    """Turns whose requests may carry a photo: a photo turn and the two after it (photos stay in view
    for two owner turns)."""
    out = set()
    for t in run["turns"]:
        if t.get("photos"):
            out |= {t["n"], t["n"] + 1, t["n"] + 2}
    return out


def least_squares(xs: list, ys: list) -> tuple:
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return slope, my - slope * mx


def wire_fit(runs: list) -> dict:
    """Body bytes against prompt tokens on the Anthropic Messages wire, photo-free requests."""
    xs, ys = [], []
    for r in runs:
        skip = photo_turns(r)
        for q in r["requests"]:
            if q["turn"] not in skip and q["usage"]["prompt"]:
                xs.append(q["usage"]["prompt"])
                ys.append(q["body_bytes"])
    slope, icpt = least_squares(xs, ys)
    return {
        "wire": "anthropic-messages",
        "model": "claude-sonnet-5-5",
        "requests": len(xs),
        "milli_bytes_per_token": round(slope * 1000),
        "intercept_bytes": round(icpt),
        "tokens_range": [min(xs), max(xs)],
    }


def growth(runs: list) -> dict:
    """The next request's growth: inside a turn (its largest request over its first) and from one
    turn's first request to the next turn's first, in prompt tokens."""
    inside, across = [], []
    for r in runs:
        by = {}
        for q in r["requests"]:
            by.setdefault(q["turn"], []).append(q["usage"]["prompt"])
        turns = sorted(by)
        inside += [max(by[t]) - by[t][0] for t in turns]
        across += [by[b][0] - by[a][0] for a, b in zip(turns, turns[1:])]
    return {"turns": len(inside), "in_turn_max": max(inside), "turn_to_turn_max": max(across)}


def turn_after(run: dict, at: float) -> int:
    """The turn a compaction ran before: run_replay compacts in the idle gap before a turn opens, at
    the previous request's end plus IDLE_COMPACT_S, so the turn is the first whose shape time is past it."""
    import run_replay
    import synth

    shape = synth.load_shape(run["shape"])
    start = run_replay.epoch_of(shape["first_local"])
    for n, t in enumerate(synth.turns_for(shape, seed=run["shape"]), start=1):
        if start + t["at_s"] >= at:
            return n
    return 0


def due_and_after(run: dict) -> list:
    """Per compaction: the last request before it and the first request after it, in prompt tokens."""
    out = []
    for c in run["compactions"]:
        turn = c.get("before_turn") or turn_after(run, c["at"])
        prev = [q["usage"]["prompt"] for q in run["requests"] if q["turn"] < turn]
        nxt = [q["usage"]["prompt"] for q in run["requests"] if q["turn"] == turn]
        if prev and nxt:
            out.append(
                {
                    "shape": run["shape"],
                    "turn": turn,
                    "before": prev[-1],
                    "after": nxt[0],
                    "turns": c["turns"],
                    "digests": c["digests"],
                }
            )
    return out


def blended_classes(runs: list, c5: float) -> dict:
    """Mean tokens a turn by price class, a cold and a warm opening blended at the device's cold share:
    fresh input, cache reads, cache writes, output (thinking included)."""

    def classes(u: dict, cold: bool) -> tuple:
        cached, written = u.get("cached", 0), u.get("written", 0)
        if cold:
            cached, written = 0, written + cached
        fresh = max(u["prompt"] - u.get("cached", 0) - u.get("written", 0), 0)
        return fresh, cached, written, u.get("output", 0)

    sums, turns = {"cold": [0, 0, 0, 0], "warm": [0, 0, 0, 0]}, 0
    for r in runs:
        for t in r["turns"]:
            turns += 1
            mine = [q for q in r["requests"] if q["turn"] == t["n"]]
            for mode in ("cold", "warm"):
                for i, q in enumerate(mine):
                    c = classes(q["usage"], mode == "cold" and i == 0)
                    sums[mode] = [a + b for a, b in zip(sums[mode], c)]
    out = {}
    for k, name in enumerate(("fresh", "read", "write", "output")):
        out[name] = round((c5 * sums["cold"][k] + (1 - c5) * sums["warm"][k]) / turns)
    return out


def full_arm(arm: Path, c5: float, per_day: float) -> dict:
    runs = [r for r in runs_in(arm) if r["profile"] == "full"]
    prof = report.e2_profile(runs, c5, per_day)
    reqs = [q["usage"]["prompt"] for r in runs for q in r["requests"]]
    points = [p for r in runs for p in due_and_after(r)]
    return {
        "model": runs[0]["model"],
        "shapes": [r["shape"] for r in runs],
        "turns": prof["turns"],
        "requests": prof["requests"],
        "requests_per_turn": prof["requests_per_turn"],
        "prompt_tokens_per_request_mean": round(sum(reqs) / len(reqs)),
        "prompt_tokens_per_request_max": max(reqs),
        "compactions": points,
        "turn_classes_blended": blended_classes(runs, c5),
        "cold_turn_usd": prof["cold_turn_usd"],
        "warm_turn_usd": prof["warm_turn_usd"],
        "blended_turn_usd": prof["blended_turn_usd_at_measured_cold_share"],
        "background_usd_per_turn": prof["background_usd_per_turn"],
        "warm_first_request_cached_share_mean": prof["warm_first_request_cached_share_mean"],
        "checks_passed": prof["checks_passed"],
        "seconds": prof["seconds"],
        "request_seconds": prof["request_seconds"],
        "billed_usd": prof["billed_usd"],
    }


def link(lk: dict) -> dict:
    d = lk["derived"]
    read = lk["store"]["read_130k_8k"]["total"]["median"]
    t = lk["turns"]
    return {
        "bench": {"connect_ms": round(d["connect_ms_anthropic"]), "upload_Bps": d["upload_Bps"]},
        "production": {"connect_ms": d["prd_constants"]["connect_ms"], "upload_Bps": d["prd_constants"]["upload_Bps"]},
        "card_read_ms_per_130kb": read,
        "bench_arrival_over_hold_ms": t["hold_release_to_start_ms"]["median"],
        "bench_result_to_send_ms": t["result_to_send_ms"]["median"],
    }


def build(a) -> dict:
    cost = json.loads((MEASURED / "cost.json").read_text())
    c5, per_day = cost["cold_share"]["cold_5m"]["share"], cost["turns_per_day"]["mean"]
    m0b = runs_in(a.m0b / "e2-aged") + runs_in(a.m0b / "e2")
    return {
        "lane": "RB-T4",
        "about": "Measured inputs of engine 2's per-head budget (tx/budget.h); test_tx_budget renders the table.",
        "cold_share_5m": c5,
        "turns_per_day": per_day,
        "other_services_usd_month": report.OTHER_SERVICES_USD,
        "wire_fit": wire_fit(m0b),
        "next_request_growth": growth(runs_in(a.m0b / "e2-aged")),
        "full_arm": full_arm(a.arm, c5, per_day),
        "link": link(json.loads((MEASURED / "link.json").read_text())),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--m0b", required=True, type=Path)
    ap.add_argument("--arm", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()
    a.out.write_text(json.dumps(build(a), indent=1) + "\n")


if __name__ == "__main__":
    main()
