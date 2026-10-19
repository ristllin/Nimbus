#!/usr/bin/env python3
"""Per-turn input and cache use from a saved device log (read-only).

Reads one or more saved log files (the device /api/log or /api/errlog text, or an
SD log copy) and sums the engine's per-turn usage lines:

    orchestrator: turn -> ... [tokens in=N out=N cacheR=N cacheW=N]

and prints turns, mean input per turn, mean cache reads and writes per turn, and
the share of the prompt read from the provider cache, next to a baseline (by
default the 15 Oct 2026 production day before the cacheable prompt layout:
1,574,396 input tokens over 39 turns).

    python3 tools/prompt_cache_usage.py saved-log.txt [more.txt] [--baseline-in N --baseline-turns N]

It never contacts a device; save the log first.
"""

from __future__ import annotations

import argparse
import re
import sys

TURN = re.compile(r"orchestrator: turn -> .*\[tokens in=(\d+) out=(\d+) cacheR=(\d+) cacheW=(\d+)\]")


def turns_from(text: str) -> list[tuple[int, int, int, int]]:
    """Every per-turn usage tuple (in, out, cacheR, cacheW) in the text, in order."""
    return [tuple(int(x) for x in m.groups()) for m in TURN.finditer(text)]


def summarize(turns: list[tuple[int, int, int, int]]) -> dict:
    n = len(turns)
    if n == 0:
        return {"turns": 0}
    tin = sum(t[0] for t in turns)
    tout = sum(t[1] for t in turns)
    tr = sum(t[2] for t in turns)
    tw = sum(t[3] for t in turns)
    prompt = tin + tr + tw
    return {
        "turns": n,
        "in_per_turn": round(tin / n),
        "out_per_turn": round(tout / n),
        "cache_read_per_turn": round(tr / n),
        "cache_write_per_turn": round(tw / n),
        "cache_read_share": round(tr / prompt, 3) if prompt else 0.0,
    }


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--baseline-in", type=int, default=1574396)
    ap.add_argument("--baseline-turns", type=int, default=39)
    a = ap.parse_args(argv)
    turns: list[tuple[int, int, int, int]] = []
    for path in a.files:
        with open(path, encoding="utf-8", errors="replace") as f:
            turns += turns_from(f.read())
    s = summarize(turns)
    base = round(a.baseline_in / a.baseline_turns) if a.baseline_turns else 0
    print(f"turns: {s['turns']}")
    if s["turns"]:
        print(f"input per turn: {s['in_per_turn']} (baseline {base})")
        print(f"cache reads per turn: {s['cache_read_per_turn']}")
        print(f"cache writes per turn: {s['cache_write_per_turn']}")
        print(f"share of the prompt read from the cache: {s['cache_read_share']:.1%}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
