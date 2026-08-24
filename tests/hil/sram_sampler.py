#!/usr/bin/env python3
"""SRAM sampler for N7 (before/after + 24 h soak).

Read-only HIL helper (not a pytest test): it polls the device's ``/api/state``
JSON contract and records the internal-SRAM picture over time, for the CUM-24
before/after comparison and the CUM-36 24 h dual-board soak. It NEVER flashes,
provisions, or writes to the device.

Contract sampled (src/net/webui.cpp ``/api/state``, verified @ lane/N7 29fab1b):
  top-level: heap, heapTotal, heapMin, psramFree, psramTotal
  mem.{intFree, intLargest, dmaFree, spiFree, spiLargest, pollStackMin, asyncStackMin}

The scarce axis is ``mem.intLargest`` (largest contiguous internal block - what a
TLS handshake needs) and ``heapMin`` (lowest-ever free internal). A win shows up as
a higher intLargest floor / heapMin floor under the same load, with NO change to
the turn heap floors and NO regression in pollStackMin/asyncStackMin.

Usage:
  # single snapshot (before/after a change), appended to a JSONL with a label
  sram_sampler.py --ip 192.168.1.42 --token "$NIMBUS_ADMIN_TOKEN" \
      --label before --out n7_baseline.jsonl --once

  # 24 h soak sample every 60 s (run the load driver separately: turns+web+BLE)
  sram_sampler.py --ip 192.168.1.42 --token "$NIMBUS_ADMIN_TOKEN" \
      --hours 24 --interval 60 --out n7_soak.jsonl

  # summarize / diff two runs
  sram_sampler.py --summarize n7_soak.jsonl
  sram_sampler.py --diff n7_before.jsonl n7_after.jsonl

  # host self-test (no hardware): validates the summary math
  sram_sampler.py --selftest

Exit codes: 0 ok; 2 device unreachable (skip-clean for T5); 3 bad args.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import urllib.error
import urllib.request

# The internal-SRAM fields we track. (source, json-path, "lower is worse"?)
FIELDS = [
    ("heap", ["heap"]),
    ("heapMin", ["heapMin"]),
    ("intFree", ["mem", "intFree"]),
    ("intLargest", ["mem", "intLargest"]),
    ("dmaFree", ["mem", "dmaFree"]),
    ("spiFree", ["mem", "spiFree"]),
    ("spiLargest", ["mem", "spiLargest"]),
    ("pollStackMin", ["mem", "pollStackMin"]),
    ("asyncStackMin", ["mem", "asyncStackMin"]),
    ("psramFree", ["psramFree"]),
]
# The headroom metrics whose FLOOR (min over the run) is the acceptance number.
FLOOR_METRICS = ["intLargest", "heapMin", "intFree", "pollStackMin", "asyncStackMin"]


def _dig(obj, path):
    for k in path:
        if not isinstance(obj, dict) or k not in obj:
            return None
        obj = obj[k]
    return obj


def sample_once(ip, token, timeout=5.0):
    """Return a flat dict of the tracked fields from /api/state, or raise."""
    url = f"http://{ip}/api/state"
    req = urllib.request.Request(url)
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=timeout) as r:  # noqa: S310 (LAN only)
        doc = json.loads(r.read().decode("utf-8"))
    row = {"t": round(time.time(), 3)}
    for name, path in FIELDS:
        row[name] = _dig(doc, path)
    return row


def summarize(rows, label=None):
    """Min/median/max per field; FLOOR is the min (the acceptance number)."""
    numeric = [r for r in rows if isinstance(r.get("intFree"), (int, float))]
    out = {"label": label, "n": len(numeric), "fields": {}}
    if not numeric:
        return out
    for name, _ in FIELDS:
        vals = [r[name] for r in numeric if isinstance(r.get(name), (int, float))]
        if not vals:
            continue
        out["fields"][name] = {
            "min": min(vals),
            "median": round(statistics.median(vals)),
            "max": max(vals),
        }
    return out


def _fmt_kb(b):
    return "-" if b is None else f"{b / 1024:.1f}K"


def print_summary(summ):
    lbl = f" [{summ['label']}]" if summ.get("label") else ""
    print(f"=== SRAM summary{lbl}  (n={summ['n']} samples) ===")
    print(f"{'field':<14}{'min':>10}{'median':>10}{'max':>10}   note")
    for name, _ in FIELDS:
        s = summ["fields"].get(name)
        if not s:
            continue
        note = "<- FLOOR (acceptance)" if name in FLOOR_METRICS else ""
        print(f"{name:<14}{_fmt_kb(s['min']):>10}{_fmt_kb(s['median']):>10}{_fmt_kb(s['max']):>10}   {note}")


def print_diff(before, after):
    b, a = summarize(before, "before"), summarize(after, "after")
    print("=== before -> after (FLOOR = min over run; positive delta = more headroom) ===")
    print(f"{'field':<14}{'before.min':>12}{'after.min':>12}{'delta':>12}")
    for name in FLOOR_METRICS:
        bs, as_ = b["fields"].get(name), a["fields"].get(name)
        if not bs or not as_:
            continue
        d = as_["min"] - bs["min"]
        print(f"{name:<14}{_fmt_kb(bs['min']):>12}{_fmt_kb(as_['min']):>12}{('+' if d >= 0 else '') + _fmt_kb(d):>12}")


def load_jsonl(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def selftest():
    synth = [
        {
            "intFree": 40000,
            "intLargest": 20000,
            "heapMin": 14000,
            "heap": 42000,
            "pollStackMin": 9000,
            "asyncStackMin": 6000,
            "dmaFree": 30000,
            "spiFree": 8_000_000,
            "spiLargest": 7_900_000,
            "psramFree": 8_000_000,
        },
        {
            "intFree": 36000,
            "intLargest": 8000,
            "heapMin": 13056,
            "heap": 37000,
            "pollStackMin": 8408,
            "asyncStackMin": 5748,
            "dmaFree": 28000,
            "spiFree": 7_900_000,
            "spiLargest": 7_800_000,
            "psramFree": 7_900_000,
        },
    ]
    s = summarize(synth, "selftest")
    assert s["n"] == 2, s
    assert s["fields"]["intLargest"]["min"] == 8000
    assert s["fields"]["heapMin"]["min"] == 13056
    assert s["fields"]["intFree"]["max"] == 40000
    after = [dict(r) for r in synth]
    for r in after:
        r["intLargest"] += 16000  # a candidate-#1 style win: +16K contiguous
        r["heapMin"] += 4000
    print_summary(s)
    print()
    print_diff(synth, after)
    da = summarize(after)["fields"]["intLargest"]["min"] - s["fields"]["intLargest"]["min"]
    assert da == 16000, da
    print("\nselftest OK")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description="N7 internal-SRAM sampler (read-only)")
    p.add_argument("--ip", help="device LAN IP")
    p.add_argument("--token", default="", help="admin bearer token (LAN only)")
    p.add_argument("--out", help="JSONL output path (append)")
    p.add_argument("--label", help="tag written on each sampled row")
    p.add_argument("--once", action="store_true", help="single snapshot then exit")
    p.add_argument("--interval", type=float, default=60.0, help="seconds between samples")
    p.add_argument("--hours", type=float, default=0.0, help="soak duration in hours")
    p.add_argument("--minutes", type=float, default=0.0, help="soak duration in minutes")
    p.add_argument("--summarize", metavar="JSONL", help="summarize a run and exit")
    p.add_argument("--diff", nargs=2, metavar=("BEFORE", "AFTER"), help="diff two runs")
    p.add_argument("--selftest", action="store_true", help="host self-test, no hardware")
    args = p.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.summarize:
        print_summary(summarize(load_jsonl(args.summarize), args.summarize))
        return 0
    if args.diff:
        print_diff(load_jsonl(args.diff[0]), load_jsonl(args.diff[1]))
        return 0
    if not args.ip:
        p.error("--ip is required for live sampling")

    def _emit(row):
        if args.label:
            row["label"] = args.label
        line = json.dumps(row)
        if args.out:
            with open(args.out, "a", encoding="utf-8") as f:
                f.write(line + "\n")
        print(line)

    try:
        first = sample_once(args.ip, args.token)
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        print(f"device unreachable at {args.ip}: {e} (skip-clean)", file=sys.stderr)
        return 2

    _emit(first)
    if args.once:
        return 0

    total_s = args.hours * 3600 + args.minutes * 60
    if total_s <= 0:
        return 0
    deadline = time.time() + total_s
    collected = [first]
    while time.time() < deadline:
        time.sleep(max(1.0, args.interval))
        try:
            row = sample_once(args.ip, args.token)
            _emit(row)
            collected.append(row)
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            # A dropped sample is data too (device fell off the LAN under pressure).
            drop = {"t": round(time.time(), 3), "error": str(e)}
            _emit(drop)
    print_summary(summarize(collected, args.label or "soak"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
