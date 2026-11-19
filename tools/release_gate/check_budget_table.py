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

  6. the fold trigger: never under 24 KB (RULINGS-v3 item 3); the soft mark on a head with
     room, v5.1.1's 48 KB on a head whose bound is the floor;
  7. the output caps of a final round (first / retry, healthy and fragmented heap): a first
     cap under minFinal on a head above the floor, a retry that adds under 400 tokens, a
     fragmented cap over the healthy one, or a first cap whose round does not fit the read
     window (at the floor a cap the wire raised to minFinal is v5.1.1's and exempt; the
     cap columns are the wire's own fittedCap, lane CTXR2);
  8. the release shape: while kRoutedBudgetLift is false in request_budget.h (v5.1.2 ships
     that way, RULINGS-v3), every routed head sits on the 85,000 B floor.

  9. lane RB-BG: the history profiles (full, the default, marked as such and the head's own marks;
     under an owner's smaller profile a direct head's fold is due at no more than its H, never
     under 24 KB, as its trigger; a larger profile never gives a smaller mark) and the
     floor heads (a measured floor head's window is the body less the measured overhead of
     its wire, the summary, the margin and one tool round, in window bytes, with the shift at
     4/5 and the keep at 3/4, and
     larger than v5.1.1's; a head left on v5.1.1's window had no more room than that).

Every table of the file with a model column is read, by its section heading (TABLES):
every recorded model (all of the above, and coverage), the routed heads and the custom
endpoint (all of the above; a routed head is compared with its upstream's v5.1.1 row), the
routed heads once the router meters cached tokens (a preview with few columns: the clamp and
the floor only), Virtual Nimbus (a server: no upload and no pacing in a round's time), and the
heads with the tool loop off (lane CTXL: a Mistral, Z.ai, custom or routed Mistral/Z.ai head
sends one prompt string, so it is sized from kFlatPromptBytes and keeps v5.1.1's history
window, fold slice, rows and 48 KB trigger exactly, with no field below v5.1.1 and the same
Bytes as its row in the tables above; an Anthropic or OpenAI row there fails, since a split
prompt does not change with the loop off). A
table under a heading the gate does not know fails, so a new table cannot dodge it. Columns
are found by name (see COLUMNS), so a renamed column fails loudly instead of being skipped:
every check names the columns it needs.

Usage:
    python3 tools/release_gate/check_budget_table.py [--table PATH] [--v511 PATH] [--budget-header PATH]
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
BUDGET_HEADER = os.path.join(REPO, "lib", "core", "include", "nimbus", "orch", "request_budget.h")
CONTEXT_HEADER = os.path.join(REPO, "lib", "core", "include", "nimbus", "orch", "budget.h")
WIRE_LIMITS = os.path.join(REPO, "lib", "core", "include", "nimbus", "orch", "wire_limits.h")
V511 = os.path.join(REPO, "test", "support", "fixtures", "budgets_v5_1_1.json")

TARGET_CLAMP = 100_000
FLOOR_BYTES = 85_000
CONNECT_S = 1.5
UPLOAD_BPS = 70_000
PACE_BASE_S, PACE_SLICE_S, SLICE_B = 2.0, 0.1, 4096
TRIGGER_MIN = 24 * 1024  # RULINGS-v3 item 3: the fold trigger is never below 24 KB
TRIGGER_V511 = 48 * 1024  # the stored compactAtKB default, a floor head's trigger
RETRY_GAIN = 400  # a retry is sent only when it gives at least this many more tokens

# Each table of request-budgets.md by its section heading: coverage (every v5.1.1 model has
# a row), server (a Virtual Nimbus: no upload rate and no heap pacing), routed (the floor
# while the routed lift is off), preview (few columns: the clamp and the floor only).
TABLES = {
    "every recorded model": {"coverage": True},
    "routed heads and the custom endpoint": {"routed": True},
    "routed heads once the router meters cached tokens": {"preview": True},
    "virtual nimbus": {"server": True},
    "with the tool loop off": {"loop_off": True},
    "history profiles": {"profiles": True},
    "floor heads": {"floor_heads": True},
}
# Wires whose loop-off turn is one prompt string (custom.cpp, mistral.cpp, zai): a routed head
# goes out on its upstream's wire (wire_of).
FLAT_WIRES = ("mistral", "zai", "custom")
# Loop-off rows keep v5.1.1's window exactly (RULINGS-v3: no head below its v5.1.1 behavior).
LOOP_OFF_V511 = ("historyMaxBytes", "historySoftBytes", "historyKeepBytes", "historyRows", "foldSliceBytes")

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
    "bound": ("bound",),
    "sized_from": ("sized from",),
    "trigger": ("trigger", "fold trigger"),
    "profile": ("profile",),
    "marks": ("h l", "h / l"),
    "left": ("left after the overhead",),
    "branch": ("branch",),
    "caps_healthy": ("caps healthy",),
    "caps_fragmented": ("caps fragmented",),
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


def parse_tables(text: str) -> "list[tuple[str, list[dict]]]":
    """Every Markdown table with a model column: (its section heading, its rows keyed by
    canonical column key), in file order."""
    lines, heading, out = text.splitlines(), "", []
    for i in range(len(lines) - 1):
        if lines[i].startswith("#"):
            heading = lines[i].lstrip("#").strip()
        if not lines[i].strip().startswith("|") or not SEPARATOR.match(lines[i + 1]):
            continue
        keys = column_keys(lines[i])
        if "model" in keys:
            out.append((heading, table_rows(lines[i + 2 :], keys)))
    return out


def parse_table(text: str) -> "list[dict]":
    """The first table with a model column."""
    tables = parse_tables(text)
    return tables[0][1] if tables else []


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


def round_s(row: dict, out_tokens: float, server: bool, paced: bool = True) -> "float | None":
    """A round's time with out_tokens of output: connect, the upload and the fragmented-heap
    pace (neither on a server), the prefill of the target and the decode of the output."""
    need = [val(row, k) for k in ("bytes", "target", "prefill", "decode")]
    if None in need or not need[2] or not need[3]:
        return None
    nbytes, target, prefill, decode = need
    pace = PACE_BASE_S + PACE_SLICE_S * math.ceil(nbytes / SLICE_B) if paced else 0.0
    send = 0.0 if server else nbytes / UPLOAD_BPS + pace
    return CONNECT_S + send + target / prefill + out_tokens / decode


def worst_round_s(row: dict, server: bool = False) -> "float | None":
    min_final = val(row, "min_final")
    return None if min_final is None else round_s(row, min_final, server)


def round_problems(row: dict, server: bool = False) -> "list[str]":
    if at_floor(row):
        return []  # v5.1.1's budget: its rounds fit through the load-aware caps (item 3 above)
    t, window = worst_round_s(row, server), seconds(row)
    if t is None or window is None:
        return [f"{row_name(row)}: needs bytes, target, prefill, decode, minfinal and read window columns"]
    if t > window:
        return [f"{row_name(row)}: worst round {t:.1f} s over the {window:.0f} s read window"]
    return []


# What the fold store keeps of a summary (compact.h kChatSummaryMax): no head's summary field
# may exceed it, and v5.1.1's figure counts only up to it (lane CTXR1: a gpt-6 row was told
# 8,192 B and stored at 6,144 B).
CHAT_SUMMARY_MAX = 6_144


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
        if field == "chatSummaryBytes":
            want = min(want, CHAT_SUMMARY_MAX)
        if abs(got - want) > max(1000, 0.01 * want):
            errs.append(f"{name}: {field} {got:.0f}, the protocol gives {want:.0f}")
    return errs


def shrink_problems(row: dict, old: dict) -> "list[str]":
    errs = []
    for field in NO_SHRINK:
        got = val(row, field)
        floor = old.get(field)
        if field == "chatSummaryBytes" and floor is not None:
            floor = min(floor, CHAT_SUMMARY_MAX)
        if got is not None and floor is not None and got < floor:
            errs.append(f"{row_name(row)}: {field} {got:.0f} under its v5.1.1 value {old[field]}")
    got = val(row, "chatSummaryBytes")
    if got is not None and got > CHAT_SUMMARY_MAX:
        errs.append(f"{row_name(row)}: chatSummaryBytes {got:.0f} over the {CHAT_SUMMARY_MAX} B the fold store keeps")
    return errs


def trigger_problems(row: dict) -> "list[str]":
    trigger, soft = val(row, "trigger"), val(row, "historySoftBytes")
    if trigger is None or soft is None or "bound" not in row:
        return [f"{row_name(row)}: needs trigger, hist soft and bound columns"]
    if trigger < TRIGGER_MIN:
        return [f"{row_name(row)}: fold trigger {trigger:.0f} B under {TRIGGER_MIN} B"]
    want = TRIGGER_V511 if row["bound"].lower() == "floor" else soft
    if abs(trigger - want) > 1:
        return [f"{row_name(row)}: fold trigger {trigger:.0f} B, expected {want:.0f} B ({row['bound']} bound)"]
    return []


def caps_of(cell: str) -> "tuple[float, float | None] | None":
    """'6370 / 8494' -> (6370, 8494); '1600 / -' -> (1600, None); 'none' -> None (no cap is sent)."""
    first, _, retry = (cell or "").partition("/")
    return None if number(first) is None else (number(first), number(retry))


def one_cap_problems(row: dict, label: str, caps: tuple, server: bool) -> "list[str]":
    """One heap state's first / retry caps: enough for minFinal above the floor, a retry worth
    sending, and a round at the first cap inside the read window (paced on a fragmented heap)."""
    errs, name = [], row_name(row)
    first, retry = caps
    min_final, window = val(row, "min_final"), seconds(row)
    if min_final is not None and first < min_final and not at_floor(row):
        errs.append(f"{name}: {label} cap {first:.0f} under minFinal {min_final:.0f} above the floor")
    if retry is not None and retry < first + RETRY_GAIN:
        errs.append(f"{name}: {label} retry {retry:.0f} adds under {RETRY_GAIN} tokens to {first:.0f}")
    # At the floor the wire's fittedCap raises a cap the round cannot fit to the host's minimum
    # final, as v5.1.1 sent it (output_caps.cpp kFirstFloor, 3000): that round is v5.1.1's and
    # is exempt like the floor's worst round. Any other cap is checked: fittedCap leaves a cap
    # under the minimum only when the round fits it, and one above it is this release's choice.
    if at_floor(row) and min_final is not None and first == min_final:
        return errs
    t = round_s(row, first, server, paced=label == "fragmented")
    if t is not None and window is not None and t > window + 0.5:
        errs.append(f"{name}: a {label} round at its {first:.0f} cap takes {t:.1f} s of {window:.0f} s")
    return errs


def caps_problems(row: dict, server: bool) -> "list[str]":
    if "caps_healthy" not in row or "caps_fragmented" not in row:
        return [f"{row_name(row)}: needs caps healthy and caps fragmented columns"]
    errs = []
    healthy, fragmented = caps_of(row["caps_healthy"]), caps_of(row["caps_fragmented"])
    if healthy and fragmented and fragmented[0] > healthy[0]:
        errs.append(f"{row_name(row)}: fragmented cap {fragmented[0]:.0f} over the healthy cap {healthy[0]:.0f}")
    for label, caps in (("healthy", healthy), ("fragmented", fragmented)):
        if caps:
            errs += one_cap_problems(row, label, caps, server)
    return errs


def routed_lift(header_path: str) -> "bool | None":
    """kRoutedBudgetLift as request_budget.h has it (None: not found)."""
    if not os.path.exists(header_path):
        return None
    with open(header_path, encoding="utf-8") as f:
        m = re.search(r"\bkRoutedBudgetLift\s*=\s*(true|false)\b", f.read())
    return None if not m else m.group(1) == "true"


def routed_problems(row: dict, lift: "bool | None") -> "list[str]":
    if row.get("provider", "").lower() != "cumulo":
        return []
    if lift is None:
        return [f"{row_name(row)}: kRoutedBudgetLift not found in request_budget.h (the release shape is unknown)"]
    nbytes = val(row, "bytes")
    if not lift and nbytes is not None and abs(nbytes - FLOOR_BYTES) > 1000:
        return [
            f"{row_name(row)}: a routed head at {nbytes:.0f} B while kRoutedBudgetLift is false (floor {FLOOR_BYTES})"
        ]
    return []


def flat_prompt_bytes(header_path: str, limits_path: str) -> "int | None":
    """kFlatPromptBytes as the code defines it (kJsonStrGuard less a margin); None if not found."""
    try:
        with open(header_path, encoding="utf-8") as f:
            m = re.search(r"\bkFlatPromptBytes\s*=\s*\(uint32_t\)\s*kJsonStrGuard\s*-\s*(\d+)", f.read())
        with open(limits_path, encoding="utf-8") as f:
            g = re.search(r"\bkJsonStrGuard\s*=\s*(\d+)", f.read())
    except OSError:
        return None
    return int(g.group(1)) - int(m.group(1)) if m and g else None


def loop_off_window_problems(row: dict, base: "dict | None") -> "list[str]":
    """v5.1.1's fold trigger and window, field for field (none of them below v5.1.1 either)."""
    name, errs = row_name(row), []
    trigger = val(row, "trigger")
    if trigger is None or abs(trigger - TRIGGER_V511) > 1:
        errs.append(f"{name}: fold trigger {trigger}, v5.1.1's is {TRIGGER_V511} with the loop off")
    for field in LOOP_OFF_V511 if base else ():
        got = val(row, field)
        if got is not None and field in base and abs(got - base[field]) > 1:
            errs.append(f"{name}: {field} {got:.0f}, v5.1.1 has {base[field]} (the loop-off window is v5.1.1's)")
    return errs + shrink_problems(row, base or {})


def loop_off_problems(row: dict, base: "dict | None", flat: "int | None", main_bytes: "float | None") -> "list[str]":
    """A head with the tool loop off: one prompt string sized from the flat budget, v5.1.1's window."""
    name, errs = row_name(row), []
    if wire_of(row.get("provider", "").lower(), row.get("model", "")) not in FLAT_WIRES:
        return [f"{name}: a split prompt wire in the loop-off table (its rows do not change with the loop off)"]
    if flat is None:
        return [f"{name}: kFlatPromptBytes not found in request_budget.h / wire_limits.h"]
    sized, nbytes = val(row, "sized_from"), val(row, "bytes")
    if sized is None or abs(sized - flat) > 1:
        errs.append(f"{name}: sized from {sized if sized is None else round(sized)}, not kFlatPromptBytes {flat}")
    if main_bytes is not None and nbytes is not None and abs(nbytes - main_bytes) > 1:
        errs.append(f"{name}: {nbytes:.0f} B here, {main_bytes:.0f} B in its row above")
    return errs + loop_off_window_problems(row, base)


def preview_problems(row: dict) -> "list[str]":
    """The lifted routed heads to come: few columns, so the clamp and the floor only."""
    target, nbytes = val(row, "target"), val(row, "bytes")
    if None in (target, nbytes):
        return [f"{row_name(row)}: needs target and bytes columns"]
    errs = []
    if target > TARGET_CLAMP:
        errs.append(f"{row_name(row)}: target {target:.0f} tokens over the {TARGET_CLAMP} clamp")
    if nbytes < FLOOR_BYTES:
        errs.append(f"{row_name(row)}: {nbytes:.0f} B under the {FLOOR_BYTES} B floor")
    return errs


def baseline_of(row: dict, old: dict) -> "dict | None":
    """The row's v5.1.1 budget (budgets_v5_1_1.json records the listed models of the four
    direct providers): its own row; for a routed head its upstream's (cumulo
    anthropic/claude-sonnet-5-5); for a custom endpoint the same model id on any provider;
    for an alias the listed id it leads (gpt-5.6 -> gpt-5.6-sol, the same window family).
    None when v5.1.1 recorded nothing like it (a custom llama)."""
    provider, model = row.get("provider", ""), row.get("model", "")
    wire, bare = wire_of(provider, model), model.rpartition("/")[2]
    for key in ((provider, model), (wire, bare)):
        if key in old:
            return old[key]
    same_id = sorted(k for k in old if k[1] == bare)
    led = sorted((k for k in old if k[0] == wire and k[1].startswith(bare + "-")), key=lambda k: (len(k[1]), k[1]))
    pick = (same_id or led or [None])[0]
    return old[pick] if pick else None


# Lane RB-BG: the profile order and the measured floor's constants (budget.h).
PROFILE_ORDER = ("tiny", "small", "medium", "large", "full")


def floor_constants(header_path: str) -> "dict | None":
    """kFloorFixed*Bytes, kFloorMarginBytes and kFloorWirePerWindow*Pct from budget.h."""
    try:
        with open(header_path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None
    out = {}
    for name in ("kFloorFixedAnthropicBytes", "kFloorFixedOpenAIBytes", "kFloorMarginBytes",
                 "kFloorRoundReserveBytes", "kFloorWirePerWindowAnthropicPct",
                 "kFloorWirePerWindowOpenAIPct"):  # fmt: skip
        m = re.search(r"\b" + name + r"\s*=\s*(\d+)", text)
        if not m:
            return None
        out[name] = int(m.group(1))
    return out


def full_row_problems(row: dict, name: str, marks: str) -> "list[str]":
    """Full, the default: marked as such, and its trigger is the head's own soft mark."""
    soft, trig = val(row, "historySoftBytes"), val(row, "trigger")
    if soft is None or trig is None or "(default)" not in marks:
        return [f"{name} (full): needs hist soft and trigger columns, and full marked as the default"]
    return [] if abs(trig - soft) <= 1 else [f"{name} (full): trigger {trig:.0f} is not the soft mark {soft:.0f}"]


def profile_row_problems(row: dict) -> "list[str]":
    """A direct head under a profile: the due mark is the trigger; under an owner's smaller
    profile it is at most H (never under 24 KB), keep at most the due mark and the high water
    at most 5/4 of it. Full, the default, is the head's own marks (owner ruling 81)."""
    name, marks = row_name(row), row.get("marks", "")
    m = re.match(r"\s*(\d+)\s*/\s*(\d+)", marks)
    if row.get("profile", "").lower() == "full":
        return full_row_problems(row, name, marks)
    soft, keep, hmax, trig = (
        val(row, k) for k in ("historySoftBytes", "historyKeepBytes", "historyMaxBytes", "trigger")
    )
    if not m or None in (soft, keep, hmax, trig) or row.get("profile", "").lower() not in PROFILE_ORDER:
        return [f"{name}: needs profile, H / L, hist max, soft, keep and trigger columns"]
    due = max(int(m.group(1)), TRIGGER_MIN)
    errs = []
    if soft > due or soft < TRIGGER_MIN:
        errs.append(f"{name} ({row['profile']}): hist soft {soft:.0f} outside [{TRIGGER_MIN}, {due}]")
    if abs(trig - soft) > 1:
        errs.append(f"{name} ({row['profile']}): trigger {trig:.0f} is not the soft mark {soft:.0f}")
    if keep > soft or hmax > soft * 5 / 4 + 1 and soft == due:
        errs.append(f"{name} ({row['profile']}): keep {keep:.0f} or max {hmax:.0f} out of the profile's shape")
    return errs


def profile_order_problems(rows: "list[dict]") -> "list[str]":
    """Per model, a larger profile never gives a smaller mark."""
    errs, by = [], {}
    for r in rows:
        by.setdefault((r.get("provider", ""), r.get("model", "")), []).append(r)
    for key, rs in by.items():
        rs = sorted(rs, key=lambda r: PROFILE_ORDER.index(r.get("profile", "").lower())
                    if r.get("profile", "").lower() in PROFILE_ORDER else 0)  # fmt: skip
        for a, b in zip(rs, rs[1:]):
            for f in ("historyMaxBytes", "historySoftBytes", "historyKeepBytes"):
                if (val(b, f) or 0) < (val(a, f) or 0):
                    errs.append(f"{key[0]}/{key[1]}: {f} falls from {a['profile']} to {b['profile']}")
    return errs


def floor_room(row: dict, consts: dict, nbytes: float, summ: float) -> int:
    """The window a measured wire leaves after its overhead, in window bytes (0 elsewhere)."""
    wire = wire_of(row.get("provider", ""), row.get("model", ""))
    if wire not in ("anthropic", "openai"):  # only the two wires the overhead was measured on
        return 0
    anthropic = wire == "anthropic"
    fixed = consts["kFloorFixedAnthropicBytes" if anthropic else "kFloorFixedOpenAIBytes"]
    pct = consts["kFloorWirePerWindowAnthropicPct" if anthropic else "kFloorWirePerWindowOpenAIPct"]
    left = nbytes - fixed - summ - consts["kFloorMarginBytes"] - consts["kFloorRoundReserveBytes"]
    return max(0, int(left * 100 // pct))


def measured_problems(name: str, room: int, hmax: float, soft: float, keep: float, v511: int) -> "list[str]":
    errs = []
    if abs(hmax - room) > 1:
        errs.append(f"{name}: hist max {hmax:.0f}, the measured room gives {room}")
    if abs(soft - room * 4 // 5) > 1 or abs(keep - room * 3 // 4) > 1:
        errs.append(f"{name}: soft {soft:.0f} and keep {keep:.0f} are not 4/5 and 3/4 of {room}")
    if v511 and hmax <= v511:
        errs.append(f"{name}: a measured window {hmax:.0f} no larger than v5.1.1's {v511}")
    return errs


def floor_head_problems(row: dict, consts: "dict | None", old: dict) -> "list[str]":
    """A measured floor head: the window is (bytes - the wire's measured overhead - the summary
    - the margin - one tool round) in window bytes (Anthropic and OpenAI wires only), the
    shift at 4/5 of it and the fold's keep at 3/4, and it is more than v5.1.1's window; a head
    left on v5.1.1's window would have had no more than that; with the routed lift on, a
    routed head has room of its own."""
    name = row_name(row)
    if consts is None:
        return [f"{name}: the measured floor constants are not readable in budget.h"]
    keys = ("bytes", "chatSummaryBytes", "historyMaxBytes", "historySoftBytes", "historyKeepBytes")
    nbytes, summ, hmax, soft, keep = (val(row, k) for k in keys)
    if None in (nbytes, summ, hmax, soft, keep) or "branch" not in row:
        return [f"{name}: needs bytes, summary, hist max, soft, keep and branch columns"]
    room = floor_room(row, consts, nbytes, summ)
    v511 = (baseline_of(row, old) or {}).get("historyMaxBytes", 0)
    branch = row["branch"].lower()
    if branch == "measured":
        return measured_problems(name, room, hmax, soft, keep, v511)
    if branch == "room":  # the routed lift is on: the head has its model's budget, not the floor
        return [] if nbytes > FLOOR_BYTES else [f"{name}: 'room' at {nbytes:.0f} B, on the floor"]
    if branch.startswith("v5.1.1"):
        if v511 and room > v511:
            return [f"{name}: left on v5.1.1's window {v511} although the measured room gives {room}"]
        return []
    return [f"{name}: branch '{row['branch']}' is neither measured nor v5.1.1's window"]


def row_problems(row: dict, profile: dict, old: dict, lift: "bool | None", ctx: "dict | None" = None) -> "list[str]":
    if profile.get("preview"):
        return preview_problems(row)
    if profile.get("profiles"):
        return profile_row_problems(row)
    if profile.get("floor_heads"):
        return floor_head_problems(row, (ctx or {}).get("floor"), old)
    if profile.get("loop_off"):
        ctx = ctx or {}
        main = ctx.get("bytes", {}).get((row.get("provider", ""), row.get("model", "")))
        return loop_off_problems(row, baseline_of(row, old), ctx.get("flat"), main)
    server, base = bool(profile.get("server")), baseline_of(row, old)
    errs = bound_problems(row) + round_problems(row, server)
    errs += shrink_problems(row, base or {})
    # A floor head keeps v5.1.1's figures: with no v5.1.1 row to read them from there is
    # nothing to compare (its bounds, trigger and caps are still checked).
    if base or not at_floor(row):
        errs += derivation_problems(row, base)
    errs += trigger_problems(row) + caps_problems(row, server)
    if profile.get("routed"):
        errs += routed_problems(row, lift)
    return errs


def table_problems(
    heading: str, rows: "list[dict]", old: dict, lift: "bool | None", ctx: "dict | None" = None
) -> "list[str]":
    profile = TABLES.get(norm(heading))
    if profile is None:
        return [f"a table under '{heading}' is not one the gate knows (TABLES): {len(rows)} rows unchecked"]
    errs = []
    if profile.get("coverage"):
        have = {(r.get("provider", ""), r.get("model", "")) for r in rows}
        errs += [f"{p}/{m}: in budgets_v5_1_1.json, no row in the table" for (p, m) in old if (p, m) not in have]
    for row in rows:
        errs += [f"[{heading}] {e}" for e in row_problems(row, profile, old, lift, ctx)]
    if profile.get("profiles"):
        errs += [f"[{heading}] {e}" for e in profile_order_problems(rows)]
    return errs


def problems(
    table_path: str,
    v511_path: str,
    header_path: str = BUDGET_HEADER,
    limits_path: str = WIRE_LIMITS,
    context_path: str = CONTEXT_HEADER,
) -> "list[str]":
    for p, what in ((table_path, "the budget table"), (v511_path, "the v5.1.1 budgets")):
        if not os.path.exists(p):
            return [f"{what} {os.path.relpath(p, REPO)} does not exist"]
    with open(table_path, encoding="utf-8") as f:
        tables = parse_tables(f.read())
    with open(v511_path, encoding="utf-8") as f:
        old = {(m["provider"], m["id"]): m for m in json.load(f)["models"]}
    if not tables:
        return [f"{os.path.relpath(table_path, REPO)} has no table with a model column"]
    errs = []
    if not any(TABLES.get(norm(h), {}).get("coverage") for h, _ in tables):
        errs.append("no 'Every recorded model' table: coverage of budgets_v5_1_1.json is unchecked")
    lift = routed_lift(header_path)
    # The loop-off table names device heads of the tables above: their Bytes must agree (a
    # Virtual Nimbus row is a server's budget, not the device's).
    sized = [
        r
        for h, rs in tables
        if not ({"preview", "loop_off", "server", "profiles", "floor_heads"} & set(TABLES.get(norm(h), {})))
        for r in rs
    ]
    ctx = {"flat": flat_prompt_bytes(header_path, limits_path),
           "bytes": {(r.get("provider", ""), r.get("model", "")): val(r, "bytes") for r in sized},
           "floor": floor_constants(context_path)}  # fmt: skip
    for heading, rows in tables:
        errs += table_problems(heading, rows, old, lift, ctx)
    return errs


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Request budget table release gate (v5.1.2)")
    ap.add_argument("--table", default=TABLE)
    ap.add_argument("--v511", default=V511)
    ap.add_argument("--budget-header", default=BUDGET_HEADER, help="request_budget.h (kRoutedBudgetLift)")
    args = ap.parse_args(argv)
    errs = problems(args.table, args.v511, args.budget_header)
    for e in errs:
        print(f"FAIL budget table: {e}")
    if errs:
        print(f"FAIL budget table: {len(errs)} problem(s), release blocked")
        return 1
    print(
        "PASS budget table: every head of every table inside its bounds, its rounds and caps inside its window, "
        "no field below v5.1.1, routed heads on the floor, loop-off heads at v5.1.1's window"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
