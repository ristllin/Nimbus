#!/usr/bin/env python3
"""Release gate: the request budget table agrees with the protocol and never shrinks a head.

Program 2026-10-18, history and summary fix (v5.1.2, lane E). Lane B derives every head's
request budget from one formula and writes docs/reference/request-budgets.md from it (the
golden test_request_budget re-blesses). Lane P0 recorded every model's v5.1.1 budget in
test/support/fixtures/budgets_v5_1_1.json. This gate reads both and fails on:

  1. coverage: a model in budgets_v5_1_1.json with no row in the table;
  2. bounds (plan v3 protocols, RULINGS-v3): a target over 100,000 tokens; a target over
     0.75 x window x rSafe / rNom (a head at the floor excepted); bytes under the floor
     min(85,000, 0.85 x window x rSafe); a custom endpoint off the 85,000 B floor;
  3. the worst round: connect 1.5 s + bytes at 70 KB/s + the fragmented-heap pace (2 s plus
     0.1 s per 4 KB slice, no deadline/10 cap, RULINGS item 1) + target / prefill floor +
     minFinal / decode floor must fit the read window. A head at the floor is exempt: the
     floor is v5.1.1's budget, which no head goes under (RULINGS-v3), and its rounds fit
     through the load-aware caps (lane B's test_output_caps), not through minFinal;
  4. no field below v5.1.1: every budget field the table shows is at least that model's
     budgets_v5_1_1.json value (RULINGS: no field shrinks);
  5. the derived fields agree with the protocol: system cap max(32,768, 12%) at most 56,000;
     tool result max(8,192, 3%) at most 16,384; recall 3% in 4,096-12,288; replay 15% in
     16,384-65,536 and never under the sending wire's v5.1.1 cap (16,384 on the Anthropic
     wire, 24,576 on every other); summary 6,144 at 60k tokens and above, else 4,096; message cap 12,288 at
     60k and above, else 6,144; soft 0.8 x max; rows max(64, max/192). Each is compared after
     the no-shrink max with the v5.1.1 value, within 1% or 1,000 B for rounding.

The table is the first Markdown table in the file with a model column; columns are found by
name (see COLUMNS), so a renamed column fails loudly instead of being skipped: every check
names the columns it needs.

Usage:
    python3 tools/release_gate/check_budget_table.py [--table PATH] [--v511 PATH]
Exit 0 = pass; exit 1 = a violation, a missing input or a missing column.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TABLE = os.path.join(REPO, "docs", "reference", "request-budgets.md")
V511 = os.path.join(REPO, "test", "support", "fixtures", "budgets_v5_1_1.json")

TARGET_CLAMP = 100_000
FLOOR_BYTES = 85_000
CONNECT_S = 1.5
UPLOAD_BPS = 70_000
PACE_BASE_S, PACE_SLICE_S, SLICE_B = 2.0, 0.1, 4096

# canonical key -> accepted header names (lowercase, spaces and punctuation squeezed)
COLUMNS = {
    "provider": ("provider", "host"),
    "model": ("model", "id", "model id"),
    "window": ("window", "window tokens", "listed window", "windowtokens"),
    "target": ("target", "target tokens", "targettokens"),
    "bytes": ("bytes", "budget bytes", "wire bytes", "wirebodybytes", "budget"),
    "rnom": ("rnom", "nominal ratio", "rnomx100"),
    "rsafe": ("rsafe", "dense ratio", "rsafex100"),
    "read_window_s": ("read window", "read window s", "roundms", "round ms", "read window ms"),
    "prefill": ("prefill", "prefill floor", "prefilltokpers"),
    "decode": ("decode", "decode floor", "decodetokpers"),
    "min_final": ("minfinal", "min final"),
    "systemPromptBytes": ("system cap", "systempromptbytes"),
    "toolResultBytes": ("tool result", "toolresultbytes"),
    "recallBytes": ("recall", "recallbytes"),
    "replayBytes": ("replay", "replaybytes"),
    "chatSummaryBytes": ("summary", "chatsummarybytes"),
    "historyMessageCap": ("message cap", "historymessagecap"),
    "historyMaxBytes": ("hist max", "history max", "historymaxbytes"),
    "historySoftBytes": ("hist soft", "history soft", "historysoftbytes"),
    "historyKeepBytes": ("hist keep", "history keep", "historykeepbytes"),
    "historyRows": ("rows", "history rows", "historyrows"),
    "foldStepBytes": ("fold step", "foldstepbytes"),
    "foldSliceBytes": ("fold slice", "foldslicebytes"),
    "perTurnFloorBytes": ("per turn floor", "perturnfloorbytes"),
}
# The replay floor is the sending wire's v5.1.1 cap (RULINGS-v3 item 8: no field shrinks):
# kReplayToolBytes is 16,384 in anthropic.cpp and 24,576 in mistral.cpp, openai.cpp and
# custom.cpp, while budgets_v5_1_1.json records the smallest, 16,384, for every model.
REPLAY_V511 = {"anthropic": 16_384}
REPLAY_V511_OTHER = 24_576


def wire_of(provider: str, model: str) -> str:
    """The adapter a head's requests go out on: a router selector's upstream ("openai" when bare)."""
    if provider != "cumulo":
        return provider
    head, sep, _ = model.partition("/")
    return head if sep and head else "openai"


NO_SHRINK = (
    "systemPromptBytes", "toolResultBytes", "recallBytes", "replayBytes", "chatSummaryBytes", "historyMessageCap",
    "historyMaxBytes", "historyRows", "foldSliceBytes", "perTurnFloorBytes",
)  # fmt: skip


def norm(h: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", h.lower()).strip()


def number(cell: str) -> "float | None":
    """'360 KB' -> 360000, '100k' -> 100000, '1,048,576' -> 1048576, '3.6' -> 3.6; '-' -> None."""
    m = re.match(r"\s*([0-9][0-9,]*(?:\.[0-9]+)?)\s*([kKmM]?)", cell or "")
    if not m:
        return None
    v = float(m.group(1).replace(",", ""))
    return v * {"": 1, "k": 1e3, "m": 1e6}[m.group(2).lower()]


SEPARATOR = re.compile(r"^\s*\|[\s:|-]+\|\s*$")


def cells_of(line: str) -> "list[str]":
    return [c.strip() for c in line.strip().strip("|").split("|")]


def column_keys(header: str) -> "list[str | None]":
    """The canonical key of each header cell (None for a column no check reads)."""
    aliases = {k: {norm(a) for a in al} for k, al in COLUMNS.items()}
    return [next((k for k, al in aliases.items() if norm(h) in al), None) for h in cells_of(header)]


def table_rows(lines: "list[str]", keys: "list[str | None]") -> "list[dict]":
    rows = []
    for body in lines:
        if not body.strip().startswith("|"):
            break
        rows.append({k: c for k, c in zip(keys, cells_of(body)) if k})
    return rows


def parse_table(text: str) -> "list[dict]":
    """The first Markdown table with a model column, as rows keyed by canonical column key."""
    lines = text.splitlines()
    for i in range(len(lines) - 1):
        if not lines[i].strip().startswith("|") or not SEPARATOR.match(lines[i + 1]):
            continue
        keys = column_keys(lines[i])
        if "model" in keys:
            return table_rows(lines[i + 2 :], keys)
    return []


def val(row: dict, key: str) -> "float | None":
    return number(row[key]) if key in row else None


def ratio(row: dict, key: str) -> "float | None":
    v = val(row, key)
    return v / 100.0 if v is not None and v > 20 else v  # rNomX100 = 360 -> 3.6


def seconds(row: dict) -> "float | None":
    v = val(row, "read_window_s")
    return v / 1000.0 if v is not None and v > 1000 else v  # ms -> s


def row_name(row: dict) -> str:
    return f"{row.get('provider', '?')}/{row.get('model', '?')}"


def bound_problems(row: dict) -> "list[str]":
    errs, name = [], row_name(row)
    target, nbytes, window = val(row, "target"), val(row, "bytes"), val(row, "window")
    rnom, rsafe = ratio(row, "rnom"), ratio(row, "rsafe")
    if None in (target, nbytes):
        return [f"{name}: needs target and bytes columns"]
    if target > TARGET_CLAMP:
        errs.append(f"{name}: target {target:.0f} tokens over the {TARGET_CLAMP} clamp")
    if row.get("provider", "").lower() == "custom" and abs(nbytes - FLOOR_BYTES) > 1000:
        errs.append(f"{name}: a custom endpoint at {nbytes:.0f} B, not the {FLOOR_BYTES} B floor")
    if None in (window, rnom, rsafe):
        return errs + [f"{name}: needs window, rnom and rsafe columns for the window bounds"]
    floor = min(FLOOR_BYTES, 0.85 * window * rsafe)
    if nbytes + 1000 < floor:
        errs.append(f"{name}: {nbytes:.0f} B under the floor {floor:.0f} B")
    at_floor = abs(nbytes - floor) <= 1000
    if not at_floor and target > 0.75 * window * rsafe / rnom + 1000:
        errs.append(
            f"{name}: target {target:.0f} over 0.75 x window x rSafe / rNom = {0.75 * window * rsafe / rnom:.0f}"
        )
    return errs


def at_floor(row: dict) -> bool:
    nbytes, window, rsafe = val(row, "bytes"), val(row, "window"), ratio(row, "rsafe")
    if None in (nbytes, window, rsafe):
        return False
    return abs(nbytes - min(FLOOR_BYTES, 0.85 * window * rsafe)) <= 1000


def worst_round_s(row: dict) -> "float | None":
    need = [val(row, k) for k in ("bytes", "target", "prefill", "decode", "min_final")]
    if None in need or not need[2] or not need[3]:
        return None
    nbytes, target, prefill, decode, min_final = need
    pace = PACE_BASE_S + PACE_SLICE_S * math.ceil(nbytes / SLICE_B)
    return CONNECT_S + nbytes / UPLOAD_BPS + pace + target / prefill + min_final / decode


def round_problems(row: dict) -> "list[str]":
    if at_floor(row):
        return []  # v5.1.1's budget: its rounds fit through the load-aware caps (item 3 above)
    t, window = worst_round_s(row), seconds(row)
    if t is None or window is None:
        return [f"{row_name(row)}: needs bytes, target, prefill, decode, minfinal and read window columns"]
    if t > window:
        return [f"{row_name(row)}: worst round {t:.1f} s over the {window:.0f} s read window"]
    return []


def derived(nbytes: float, target: float, hist_max: "float | None", wire: str = "") -> dict:
    clamp = lambda v, lo, hi: max(lo, min(hi, v))  # noqa: E731
    replay_floor = REPLAY_V511.get(wire, REPLAY_V511_OTHER) if wire else 16_384
    out = {
        "systemPromptBytes": min(56_000, max(32_768, 0.12 * nbytes)),
        "toolResultBytes": min(16_384, max(8_192, 0.03 * nbytes)),
        "recallBytes": clamp(0.03 * nbytes, 4_096, 12_288),
        "replayBytes": max(replay_floor, clamp(0.15 * nbytes, 16_384, 65_536)),
        "chatSummaryBytes": 6_144 if target >= 60_000 else 4_096,
        "historyMessageCap": 12_288 if target >= 60_000 else 6_144,
    }
    if hist_max is not None and hist_max > 0:
        out["historySoftBytes"] = 0.8 * hist_max
        out["historyRows"] = max(64, hist_max / 192)
    return out


def derivation_problems(row: dict, old: "dict | None") -> "list[str]":
    errs, name = [], row_name(row)
    nbytes, target = val(row, "bytes"), val(row, "target")
    if None in (nbytes, target):
        return []
    want_all = derived(
        nbytes, target, val(row, "historyMaxBytes"), wire_of(row.get("provider", ""), row.get("model", ""))
    )
    hist_max = val(row, "historyMaxBytes")
    if old and hist_max is not None and hist_max <= old.get("historyMaxBytes", 0):
        # The floor branch (RULINGS item 3): at or below v5.1.1's history the head keeps
        # v5.1.1's window exactly, so its marks are v5.1.1's, not 0.8 x max.
        want_all["historySoftBytes"] = old.get("historySoftBytes", want_all.get("historySoftBytes"))
        want_all["historyRows"] = old.get("historyRows", want_all.get("historyRows"))
    for field, want in want_all.items():
        if want is None:
            continue
        got = val(row, field)
        if got is None:
            continue
        if old and field in old and field != "historySoftBytes":
            want = max(want, old[field])
        if abs(got - want) > max(1000, 0.01 * want):
            errs.append(f"{name}: {field} {got:.0f}, the protocol gives {want:.0f}")
    return errs


def shrink_problems(row: dict, old: dict) -> "list[str]":
    errs = []
    for field in NO_SHRINK:
        got = val(row, field)
        if got is not None and field in old and got < old[field]:
            errs.append(f"{row_name(row)}: {field} {got:.0f} under its v5.1.1 value {old[field]}")
    return errs


def problems(table_path: str, v511_path: str) -> "list[str]":
    for p, what in ((table_path, "the budget table"), (v511_path, "the v5.1.1 budgets")):
        if not os.path.exists(p):
            return [f"{what} {os.path.relpath(p, REPO)} does not exist"]
    with open(table_path, encoding="utf-8") as f:
        rows = parse_table(f.read())
    with open(v511_path, encoding="utf-8") as f:
        old = {(m["provider"], m["id"]): m for m in json.load(f)["models"]}
    if not rows:
        return [f"{os.path.relpath(table_path, REPO)} has no table with a model column"]
    by_key = {(r.get("provider", ""), r.get("model", "")): r for r in rows}
    errs = [f"{p}/{m}: in budgets_v5_1_1.json, no row in the table" for (p, m) in old if (p, m) not in by_key]
    for key, row in by_key.items():
        errs += bound_problems(row) + round_problems(row)
        errs += shrink_problems(row, old.get(key, {})) + derivation_problems(row, old.get(key))
    return errs


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Request budget table release gate (v5.1.2)")
    ap.add_argument("--table", default=TABLE)
    ap.add_argument("--v511", default=V511)
    args = ap.parse_args(argv)
    errs = problems(args.table, args.v511)
    for e in errs:
        print(f"FAIL budget table: {e}")
    if errs:
        print(f"FAIL budget table: {len(errs)} problem(s), release blocked")
        return 1
    print("PASS budget table: every head inside its bounds, its worst round inside its window, no field below v5.1.1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
