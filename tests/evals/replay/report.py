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
CHANGE_POINTS = ("pointer", "outside", "superseded", "photo_left", "thinking_dropped", "compaction")


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


def turn_prices(r: dict, t: dict) -> dict:
    """One turn's requests priced four ways: opening warm or cold (its first request cold), under the
    5-minute or the 1-hour write. Pure arithmetic over the reported usage."""
    from run_replay import price

    mine = [q for q in r["requests"] if q["turn"] == t["n"]]
    out = {}
    for ttl in ("5m", "1h"):
        for cold in (False, True):
            out[f"{ttl}_{'cold' if cold else 'warm'}"] = sum(
                price(r["model"], q["usage"], cold and k == 0, ttl) for k, q in enumerate(mine)
            )
    return out


def opened_cold(r: dict, t: dict) -> bool:
    """A turn opened cold when its first request was priced cold (over 300 s, or the run's first)."""
    return next(q["cold"] for q in r["requests"] if q["turn"] == t["n"])


def ttl_decision(runs: list, c5: float, c60: float) -> dict:
    """USD per turn under each write at the device's measured cold shares, from the replays' own token
    profiles. A turn after a gap over 60 minutes is cold under both writes; one after 5 to 60 minutes
    is cold under the 5-minute write and cache-warm under the 1-hour write (its transcript still opens
    cold: the stub pointer moves at 270 s whatever the cache); the rest are warm under both."""
    cold = [turn_prices(r, t) for r in runs for t in r["turns"] if opened_cold(r, t)]
    warm = [turn_prices(r, t) for r in runs for t in r["turns"] if not opened_cold(r, t)]
    if not cold or not warm:
        return {}

    def m(group, k):
        return statistics.mean(p[k] for p in group)

    five = c5 * m(cold, "5m_cold") + (1 - c5) * m(warm, "5m_warm")
    hour = c60 * m(cold, "1h_cold") + (c5 - c60) * m(cold, "1h_warm") + (1 - c5) * m(warm, "1h_warm")
    return {
        "turns": len(cold) + len(warm),
        "usd_per_turn_5m": round(five, 6),
        "usd_per_turn_1h": round(hour, 6),
        "one_hour_saves": round(1 - hour / five, 4),
        "decision": "1-hour write" if hour < five else "5-minute write",
    }


def turn_class(t: dict) -> str:
    if t["photos"]:
        return "photo"
    return "chat" if not t["tools"] else "tools"


def e2_runs(d: Path) -> list:
    return [json.loads(p.read_text()) for p in sorted(d.glob("run-*.json"))]


def turn_sum(r: dict, t: dict, key: str) -> int:
    return sum(q["usage"][key] for q in r["requests"] if q["turn"] == t["n"])


def turn_uncached(r: dict, t: dict) -> int:
    return sum(uncached(q["usage"], q["cold"]) for q in r["requests"] if q["turn"] == t["n"])


def per_turn_mean(runs: list, fn) -> int | None:
    vals = [fn(r, t) for r in runs for t in r["turns"]]
    return round(mean(vals)) if vals else None


def seconds_by_class(turns: list) -> dict:
    out = {}
    for cls in ("chat", "tools", "photo"):
        v = [t["seconds"] for t in turns if turn_class(t) == cls]
        out[cls] = {"n": len(v), "p50": pct(v, 0.5), "p90": pct(v, 0.9)}
    return out


def request_seconds(reqs: list) -> dict:
    out = {}
    for name, cold in (("cold_first", True), ("warm", False)):
        v = [q["s"] for q in reqs if q["cold"] == cold]
        out[name] = {"n": len(v), "p50": pct(v, 0.5)}
    return out


def months(out: dict, per_turn: float | None, per_day: float):
    """month_usd_at_<n>_turns_a_day for 12 and the measured rate, other services included."""
    if per_turn is None:
        return
    for n in sorted({12, per_day}):
        out[f"month_usd_at_{n}_turns_a_day"] = round(30 * n * per_turn + OTHER_SERVICES_USD, 2)


def e2_tokens(runs: list) -> dict:
    return {
        "prompt_tokens_per_turn_mean": per_turn_mean(runs, lambda r, t: turn_sum(r, t, "prompt")),
        "uncached_tokens_per_turn_mean": per_turn_mean(runs, turn_uncached),
        "output_tokens_per_turn_mean": per_turn_mean(runs, lambda r, t: turn_sum(r, t, "output")),
        "thinking_tokens_per_turn_mean": per_turn_mean(runs, lambda r, t: turn_sum(r, t, "thinking")),
    }


def split_cold(runs: list) -> tuple:
    cold = [t for r in runs for t in r["turns"] if opened_cold(r, t)]
    warm = [t for r in runs for t in r["turns"] if not opened_cold(r, t)]
    return cold, warm


def e2_costs(runs: list, c5: float) -> dict:
    """Cold and warm USD per turn on the virtual clock, blended at the device's cold share, and the
    background (digest) USD per turn."""
    turns = [t for r in runs for t in r["turns"]]
    cold, warm = split_cold(runs)
    bg = sum(b["usd"] for r in runs for b in r["background"])
    cold_usd, warm_usd = mean([t["usd_5m"] for t in cold]), mean([t["usd_5m"] for t in warm])
    blended = c5 * cold_usd + (1 - c5) * warm_usd if cold and warm else None
    return {
        "cold_turns": len(cold),
        "cold_turn_usd": cold_usd,
        "warm_turn_usd": warm_usd,
        "blended_turn_usd_at_measured_cold_share": round(blended, 6) if blended else None,
        "background_usd_per_turn": round(bg / len(turns), 6),
        "billed_usd": round(sum(q["usd_billed"] for r in runs for q in r["requests"]) + bg, 4),
    }


def e2_counts(runs: list) -> dict:
    turns = [t for r in runs for t in r["turns"]]
    reqs = [q for r in runs for q in r["requests"]]
    checks = [t["check"] for t in turns if t["check"] is not None]
    return {
        "runs": len(runs),
        "turns": len(turns),
        "requests": len(reqs),
        "requests_per_turn": round(len(reqs) / len(turns), 2),
        "round_guard_trips": sum(1 for t in turns if t.get("round_guard")),
        "checks_passed": {"k": sum(1 for c in checks if c), "n": len(checks)},
        "body_bytes_max": max(q["body_bytes"] for q in reqs),
        "seconds": seconds_by_class(turns),
        "request_seconds": request_seconds(reqs),
    }


def e2_cache(runs: list) -> dict:
    _, warm = split_cold(runs)
    first_warm = [t["first_cached_share"] for t in warm if t["n"] > 1]
    return {
        "warm_first_request_cached_share_mean": round(mean(first_warm), 4) if first_warm else None,
        "compactions": sum(len(r["compactions"]) for r in runs),
        "change_points": {k: sum(r.get("changes", {}).get(k, 0) for r in runs) for k in CHANGE_POINTS},
    }


def e2_profile(runs: list, c5: float, per_day: float, c60: float = 0.0) -> dict:
    if not any(r["turns"] for r in runs):
        return {"runs": 0}
    out = {**e2_counts(runs), **e2_tokens(runs), **e2_cache(runs), **e2_costs(runs, c5)}
    blended = out["blended_turn_usd_at_measured_cold_share"]
    months(out, blended + out["background_usd_per_turn"] if blended else None, per_day)
    out["cache_write_ttl"] = ttl_decision(runs, c5, c60)
    return out


def e1_tokens(ok: list) -> dict:
    tok = [s["tokens"] for s in ok]
    lat = [s["latency_s"] for s in ok]
    return {
        "head_requests_per_turn": round(mean([s["head_requests"] for s in ok]), 2),
        "other_requests_per_turn": round(mean([s["other_requests"] for s in ok]), 2),
        "prompt_tokens_per_turn_mean": round(mean([t["input"] for t in tok])),
        "cached_share": round(sum(t["cached"] for t in tok) / max(sum(t["input"] for t in tok), 1), 4),
        "latency_s": {"p50": pct(lat, 0.5), "p90": pct(lat, 0.9)},
    }


def e1_model(rec: dict, c5: float, per_day: float) -> dict:
    steps = [s for s in rec["steps"] if s.get("kind")]
    ok = [s for s in steps if not s.get("error") and not s.get("timed_out")]
    out = {
        "turns": len(steps),
        "failed_or_timed_out": len(steps) - len(ok),
        "held": rec.get("held"),
        "error": rec.get("error"),
    }
    if not ok:
        return out
    cold_usd, warm_usd = mean([s["usd_cold"] for s in ok]), mean([s["usd_billed"] for s in ok])
    blended = c5 * cold_usd + (1 - c5) * warm_usd
    out.update(e1_tokens(ok))
    out.update(
        {
            "cold_turn_usd": cold_usd,
            "warm_turn_usd": warm_usd,
            "shape_gap_usd_per_turn": mean([s["usd_shape"] for s in ok]),
            "blended_turn_usd_at_measured_cold_share": round(blended, 6),
            "billed_usd": round(sum(s["usd_billed"] for s in steps), 4),
        }
    )
    months(out, blended, per_day)
    return out


def thinking_side(runs: list) -> dict:
    warm = [t for r in runs for t in r["turns"][1:]]
    first = [q["usage"]["prompt"] for r in runs for q in r["requests"] if q["round"] == 1 and q["turn"] > 1]
    shares = [t["first_cached_share"] for t in warm]
    return {
        "runs": len(runs),
        "warm_follow_ups": len(warm),
        "first_request_cached_share_mean": round(mean(shares), 4) if shares else None,
        "first_request_cached_share_min": min(shares, default=None),
        "first_request_prompt_tokens_mean": round(mean(first)) if first else None,
        "thinking_blocks_dropped_by_api": sum(q["dropped_thinking"] for r in runs for q in r["requests"]),
        "usd_per_turn": mean([t["usd_billed"] for t in warm]),
    }


def thinking_arm(d: Path) -> dict:
    runs = e2_runs(d)
    return {
        "dropped (engine 2)": thinking_side([r for r in runs if not r["replay_thinking"]]),
        "replayed": thinking_side([r for r in runs if r["replay_thinking"]]),
    }


def rule_fix_ab(d: Path) -> dict:
    """Prompt tokens and virtual USD per turn of the same replay under the rules as first run (old) and
    after the review fixes (new), on Haiku: the size of the correction to the Sonnet figures."""
    out = {}
    for side in ("old", "new"):
        for r in e2_runs(d / side):
            out.setdefault(r["profile"], {})[side] = {
                "turns": len(r["turns"]),
                "prompt_tokens_per_turn": per_turn_mean([r], lambda r_, t: turn_sum(r_, t, "prompt")),
                "virtual_usd_5m_per_turn": mean([t["usd_5m"] for t in r["turns"]]),
            }
    for v in out.values():
        if "old" in v and "new" in v:
            v["tokens_ratio_new_over_old"] = round(
                v["new"]["prompt_tokens_per_turn"] / v["old"]["prompt_tokens_per_turn"], 4
            )
            v["usd_ratio_new_over_old"] = round(
                v["new"]["virtual_usd_5m_per_turn"] / v["old"]["virtual_usd_5m_per_turn"], 4
            )
    return out


def run_line(r: dict) -> dict:
    judged = [t["check"] for t in r["turns"] if t["check"] is not None]
    return {
        "turns": len(r["turns"]),
        "checks_passed": f"{sum(1 for c in judged if c)}/{len(judged)}",
        "virtual_usd_5m": round(sum(t["usd_5m"] for t in r["turns"]), 4),
        "compactions": len(r["compactions"]),
        "max_resident": max(t["resident"] for t in r["turns"]),
    }


def e1_section(d: Path, c5: float, per_day: float) -> dict:
    out = {}
    for p in sorted(d.glob("*/*/e1.json")):
        rec = json.loads(p.read_text())
        if "ended" not in rec:
            out[rec["model"]] = {"unfinished": f"{len(rec['steps'])} turns stored, run not ended"}
        else:
            out[rec["model"]] = e1_model(rec, c5, per_day)
    return out


def hold_section(holds: dict) -> dict:
    quiet = [q["last_after_first_ms"] / 1000 for q in holds["quiet_joins"] if q["last_after_first_ms"] is not None]
    return {
        "dispatches": holds["dispatches"],
        "joined": holds["joined"],
        "joined_by_running_turn": holds["joined_old"],
        "joined_by_hold": holds["joined_quiet"],
        "hold_joins_last_after_first_s": quiet,
        **{f"hold_joins_within_{s}s": {"k": sum(1 for x in quiet if x <= s), "n": len(quiet)} for s in (2, 3, 6)},
    }


def profiles(runs: list, c5: float, per_day: float, c60: float) -> dict:
    return {p: e2_profile([r for r in runs if r["profile"] == p], c5, per_day, c60) for p in PROFILES}


def corrected(e2: dict, ab: dict | None) -> dict | None:
    """The Sonnet months scaled by the Haiku A/B ratio of the rule fixes (USD per turn, new over old):
    standard and tight measured, lean and wide given the range of the two."""
    if not ab or not all("usd_ratio_new_over_old" in ab.get(p, {}) for p in ("standard", "tight")):
        return None
    lo, hi = sorted((ab["tight"]["usd_ratio_new_over_old"], ab["standard"]["usd_ratio_new_over_old"]))
    out = {}
    for prof, v in e2.items():
        per_turn = v["blended_turn_usd_at_measured_cold_share"] + v["background_usd_per_turn"]
        ratios = (
            [ab[prof]["usd_ratio_new_over_old"]] if prof in ab and "usd_ratio_new_over_old" in ab[prof] else [lo, hi]
        )
        out[prof] = {
            "usd_ratio_applied": ratios,
            "month_usd_at_12_turns_a_day": [round(360 * per_turn * r + OTHER_SERVICES_USD, 2) for r in ratios],
        }
    return out


def build(a) -> dict:
    gaps = json.loads(a.gaps.read_text())
    c5, c60 = gaps["cold"]["cold_5m"]["share"], gaps["cold"]["cold_60m"]["share"]
    per_day = round(gaps["turns_per_day"]["mean"], 1)
    runs = [r for r in e2_runs(a.e2) if r.get("aged") and not r.get("warm")]
    fresh = [r for r in e2_runs(a.e2_fresh) if not r.get("warm")] if a.e2_fresh else []
    return {
        "lane": "RB-M0b",
        "generated": "2026-11-18",
        "method": "tests/evals/replay/README.md",
        "cold_share": gaps["cold"],
        "turns_per_day": gaps["turns_per_day"],
        "arrivals": gaps["arrivals"],
        "hold_payoffs": hold_section(gaps["holds"]),
        "device_tokens_v5": gaps["device_tokens"],
        "engine2_sonnet": profiles(runs, c5, per_day, c60),
        "engine2_runs": {f"{r['shape']} {r['profile']}": run_line(r) for r in runs},
        "engine2_sonnet_fresh_state": profiles(fresh, c5, per_day, c60) if fresh else None,
        "rule_fix_ab_haiku": rule_fix_ab(a.ab) if a.ab else None,
        "engine2_sonnet_corrected_for_rule_fixes": None,
        "engine1_main": e1_section(a.e1, c5, per_day),
        "thinking_warm_follow_up": thinking_arm(a.thinking),
        "prd_assumptions": PRD,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--e2", type=Path, required=True, help="aged-state replays")
    ap.add_argument("--e1", type=Path, required=True)
    ap.add_argument("--thinking", type=Path, required=True)
    ap.add_argument("--e2-fresh", type=Path, help="replays from an empty transcript, reported apart")
    ap.add_argument("--ab", type=Path, help="Haiku A/B of the rule fixes: <dir>/old and <dir>/new")
    ap.add_argument("--gaps", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    out = build(a)
    out["engine2_sonnet_corrected_for_rule_fixes"] = corrected(out["engine2_sonnet"], out["rule_fix_ab_haiku"])
    a.out.write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
