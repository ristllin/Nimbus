#!/usr/bin/env python3
"""report - turn the replay evidence into tests/evals/measured/cost.json and a RESULTS.md.

  report.py --e2 <dir> --e1 <dir> --thinking <dir> --gaps <gaps.json> --out cost.json --md RESULTS.md

Inputs: the engine-2 replays (run_replay.py), the engine-1 arm (e1_arm.py), the warm follow-up
thinking arm (run_replay.py --warm) and the timing figures of tools/context_gaps.py gaps. Every
figure carries its count. The month is 30 days at the given turns a day, cold turns at the
measured cold share, plus the measured background (digests) and the PRD's 1.2 USD for search,
speech and transcription (not measured here).
"""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

OTHER_SERVICES_USD = 1.2  # PRD section 2: search, speech and transcription (assumed, not measured here)
PRD = {
    "requests_per_turn": 2.2,
    "cold_share": 0.55,
    "month_usd": {"tight": 14.5, "lean": 17.7, "standard": 21.4, "wide": 28.1},
    "cold_turn_usd": {"tight": 0.053, "lean": 0.068, "standard": 0.086, "wide": 0.117},
    "warm_turn_usd": {"tight": 0.015, "lean": 0.017, "standard": 0.018, "wide": 0.021},
    "uncached_tokens_per_turn": {"tight": 10_500, "lean": 13_600, "standard": 17_300, "wide": 23_900},
    "v512_month_usd": "32 to 36",
    "cold_request_s": 8.6,
    "warm_round_s": 7.0,
}
PROFILES = ("tight", "lean", "standard", "wide")


def pct(vals, q):
    vals = sorted(vals)
    if not vals:
        return None
    k = (len(vals) - 1) * q
    lo, hi = int(k), min(int(k) + 1, len(vals) - 1)
    return round(vals[lo] + (vals[hi] - vals[lo]) * (k - lo), 2)


def mean(vals):
    return round(statistics.mean(vals), 6) if vals else None


def uncached(u: dict, cold: bool) -> int:
    fresh = max(u["prompt"] - u["cached"] - u["written"], 0)
    return fresh + u["written"] + (u["cached"] if cold else 0)


def turn_class(t: dict) -> str:
    if t["photos"]:
        return "photo"
    return "chat" if not t["tools"] else "tools"


def e2_runs(d: Path) -> list:
    return [json.loads(p.read_text()) for p in sorted(d.glob("run-*.json"))]


def e2_profile(runs: list, c5: float, per_day: float) -> dict:
    turns = [t for r in runs for t in r["turns"]]
    reqs = [q for r in runs for q in r["requests"]]
    cold = [t for t in turns if t["cold"]]
    warm = [t for t in turns if not t["cold"]]
    checks = [t["check"] for t in turns if t["check"] is not None]
    bg = sum(b["usd"] for r in runs for b in r["background"])
    cold_usd, warm_usd = mean([t["usd_5m"] for t in cold]), mean([t["usd_5m"] for t in warm])
    bg_per_turn = bg / len(turns) if turns else 0
    blended = c5 * cold_usd + (1 - c5) * warm_usd if cold and warm else None
    first_warm = [t["first_cached_share"] for t in warm]
    unc = []
    for r in runs:
        for t in r["turns"]:
            mine = [q for q in r["requests"] if q["turn"] == t["n"]]
            unc.append(sum(uncached(q["usage"], q["cold"]) for q in mine))
    prompt = [sum(q["usage"]["prompt"] for q in r["requests"] if q["turn"] == t["n"]) for r in runs for t in r["turns"]]
    out = {
        "runs": len(runs),
        "turns": len(turns),
        "cold_turns": len(cold),
        "requests": len(reqs),
        "requests_per_turn": round(len(reqs) / len(turns), 2) if turns else None,
        "round_guard_trips": sum(1 for t in turns if t.get("round_guard")),
        "checks_passed": {"k": sum(1 for c in checks if c), "n": len(checks)},
        "prompt_tokens_per_turn_mean": round(mean(prompt)) if prompt else None,
        "uncached_tokens_per_turn_mean": round(mean(unc)) if unc else None,
        "body_bytes_max": max((q["body_bytes"] for q in reqs), default=None),
        "output_tokens_per_turn_mean": round(mean([sum(q["usage"]["output"] for q in r["requests"] if q["turn"] == t["n"]) for r in runs for t in r["turns"]])),
        "thinking_tokens_per_turn_mean": round(mean([sum(q["usage"]["thinking"] for q in r["requests"] if q["turn"] == t["n"]) for r in runs for t in r["turns"]])),
        "cold_turn_usd": cold_usd,
        "warm_turn_usd": warm_usd,
        "blended_turn_usd_at_measured_cold_share": round(blended, 6) if blended else None,
        "background_usd_per_turn": round(bg_per_turn, 6),
        "billed_usd": round(sum(q["usd_billed"] for q in reqs) + bg, 4),
        "virtual_usd_5m": round(sum(t["usd_5m"] for t in turns), 4),
        "virtual_usd_1h": round(sum(t["usd_1h"] for t in turns), 4),
        "warm_first_request_cached_share_mean": round(mean(first_warm), 4) if first_warm else None,
        "compactions": sum(len(r["compactions"]) for r in runs),
        "change_points": {k: sum(r.get("changes", {}).get(k, 0) for r in runs) for k in ("pointer", "photo_left", "compaction")},
        "seconds": {
            cls: {
                "n": len(v),
                "p50": pct(v, 0.5),
                "p90": pct(v, 0.9),
            }
            for cls in ("chat", "tools", "photo")
            for v in [[t["seconds"] for t in turns if turn_class(t) == cls]]
        },
        "request_seconds": {
            "cold_first": {"n": len(v1), "p50": pct(v1, 0.5)} if (v1 := [q["s"] for q in reqs if q["cold"]]) else None,
            "warm": {"n": len(v2), "p50": pct(v2, 0.5)} if (v2 := [q["s"] for q in reqs if not q["cold"]]) else None,
        },
    }
    if blended:
        for n in sorted({12, per_day}):
            out[f"month_usd_at_{n}_turns_a_day"] = round(30 * n * (blended + bg_per_turn) + OTHER_SERVICES_USD, 2)
    return out


def e1_model(rec: dict, c5: float, per_day: float) -> dict:
    steps = [s for s in rec["steps"] if s.get("kind")]
    ok = [s for s in steps if not s.get("error") and not s.get("timed_out")]
    cold_usd = mean([s["usd_cold"] for s in ok])
    warm_usd = mean([s["usd_billed"] for s in ok])
    blended = c5 * cold_usd + (1 - c5) * warm_usd if ok else None
    tok = [s["tokens"] for s in ok]
    out = {
        "turns": len(steps),
        "failed_or_timed_out": len(steps) - len(ok),
        "held": rec.get("held"),
        "error": rec.get("error"),
        "head_requests_per_turn": round(mean([s["head_requests"] for s in ok]), 2) if ok else None,
        "other_requests_per_turn": round(mean([s["other_requests"] for s in ok]), 2) if ok else None,
        "prompt_tokens_per_turn_mean": round(mean([t["input"] for t in tok])) if tok else None,
        "cached_share": round(sum(t["cached"] for t in tok) / max(sum(t["input"] for t in tok), 1), 4),
        "cold_turn_usd": cold_usd,
        "warm_turn_usd": warm_usd,
        "shape_gap_usd_per_turn": mean([s["usd_shape"] for s in ok]),
        "blended_turn_usd_at_measured_cold_share": round(blended, 6) if blended else None,
        "latency_s": {"p50": pct([s["latency_s"] for s in ok], 0.5), "p90": pct([s["latency_s"] for s in ok], 0.9)},
        "billed_usd": round(sum(s["usd_billed"] for s in steps), 4),
    }
    if blended:
        for n in sorted({12, per_day}):
            out[f"month_usd_at_{n}_turns_a_day"] = round(30 * n * blended, 2)
    return out


def thinking_arm(d: Path) -> dict:
    out = {}
    for replay in (False, True):
        runs = [r for r in e2_runs(d) if r["replay_thinking"] == replay]
        warm = [t for r in runs for t in r["turns"][1:]]
        first = [q for r in runs for q in r["requests"] if q["round"] == 1 and q["turn"] > 1]
        out["replayed" if replay else "dropped (engine 2)"] = {
            "runs": len(runs),
            "warm_follow_ups": len(warm),
            "first_request_cached_share_mean": round(mean([t["first_cached_share"] for t in warm]), 4) if warm else None,
            "first_request_cached_share_min": min((t["first_cached_share"] for t in warm), default=None),
            "first_request_prompt_tokens_mean": round(mean([q["usage"]["prompt"] for q in first])) if first else None,
            "thinking_blocks_dropped_by_api": sum(q["dropped_thinking"] for r in runs for q in r["requests"]),
            "usd_per_turn": round(mean([t["usd_billed"] for t in warm]), 6) if warm else None,
        }
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--e2", type=Path, required=True)
    ap.add_argument("--e1", type=Path, required=True)
    ap.add_argument("--thinking", type=Path, required=True)
    ap.add_argument("--gaps", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    gaps = json.loads(a.gaps.read_text())
    c5 = gaps["cold"]["cold_5m"]["share"]
    per_day = round(gaps["turns_per_day"]["mean"], 1)
    runs = e2_runs(a.e2)
    e2 = {p: e2_profile([r for r in runs if r["profile"] == p and not r.get("warm")], c5, per_day) for p in PROFILES}
    by_shape = {
        f"{r['shape']} {r['profile']}": {
            "turns": len(r["turns"]),
            "checks_passed": f"{sum(1 for t in r['turns'] if t['check'])}/{sum(1 for t in r['turns'] if t['check'] is not None)}",
            "virtual_usd_5m": round(sum(t["usd_5m"] for t in r["turns"]), 4),
            "compactions": len(r["compactions"]),
            "max_resident": max(t["resident"] for t in r["turns"]),
        }
        for r in runs
        if not r.get("warm")
    }
    e1 = {}
    for p in sorted(a.e1.glob("*/*/e1.json")):
        rec = json.loads(p.read_text())
        e1[rec["model"]] = e1_model(rec, c5, per_day)
    holds = gaps["holds"]
    quiet = [q["last_after_first_ms"] / 1000 for q in holds["quiet_joins"] if q["last_after_first_ms"] is not None]
    out = {
        "lane": "RB-M0b",
        "generated": "2026-11-18",
        "method": "tests/evals/replay/README.md",
        "cold_share": gaps["cold"],
        "turns_per_day": gaps["turns_per_day"],
        "arrivals": {k: v for k, v in gaps["arrivals"].items()},
        "hold_payoffs": {
            "dispatches": holds["dispatches"],
            "joined": holds["joined"],
            "joined_by_running_turn": holds["joined_old"],
            "joined_by_hold": holds["joined_quiet"],
            "hold_joins_last_after_first_s": quiet,
            **{f"hold_joins_within_{s}s": {"k": sum(1 for x in quiet if x <= s), "n": len(quiet)} for s in (2, 3, 6)},
        },
        "device_tokens_v5": gaps["device_tokens"],
        "engine2_sonnet": e2,
        "engine2_runs": by_shape,
        "engine1_main": e1,
        "thinking_warm_follow_up": thinking_arm(a.thinking),
        "prd_assumptions": PRD,
    }
    a.out.write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
