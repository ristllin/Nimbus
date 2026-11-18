#!/usr/bin/env python3
"""Timing figures from a saved device corpus: cold share, arrival gaps of follow-on messages,
the hold's pay-offs, and the numeric shape of a day (read-only, numbers only).

Input is a folder holding a device's day-stream export (episodic.jsonl) and its rotated logs
(nimbus.log.N, nimbus.log). Nothing an owner wrote ever leaves this tool: every output is a
count, a size in bytes, a time or a gap, a tool name or a model name.

  gaps     owner turns with exact times (the Telegram send time on the turn's first message,
           the 'at' time on its reply): the gap from the previous owner turn's reply to this
           turn's first message; the cold share at 270 s, 5 min and 60 min; turns a day; the
           1-hour cache decision (PRD section 2: the 1-hour write only if c60 < 0.625 x c5)
  arrivals follow-on messages: an owner message sent before the device replied to the one
           before it. Gaps by send time, the share within 2, 3 and 6 s, album members apart
  holds    the log's 'settle dispatch n=N after M ms (quiet|old)' lines: how many dispatches
           joined more than one message, and whether the hold (quiet) or a running turn (old)
           joined them; each quiet join matched to send times by wall clock
  shape    one or more local days as numbers: per owner turn its start (seconds after the
           first turn), exact or hour-estimated, messages, photos, text bytes, tool calls
           with result bytes, reply bytes, device tokens

    python3 tools/context_gaps.py --dir <corpus> gaps [--since 2026-11-03]
    python3 tools/context_gaps.py --dir <corpus> shape --days 2026-11-15,2026-11-16,2026-11-17 --out x.json

Time: the day stream's 'ts' is whole hours since the epoch; 'at' (replies, texts) and 'sent'
(photos) carry seconds. Rows before v4.6.0 have hours only; those turns are placed inside their
hour in order and marked exact=false, and the cold-share and gap figures use exact rows only.
Local time is Europe/London.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
from datetime import datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

TZ = ZoneInfo("Europe/London")
COLD_OPEN_S = 270  # PRD section 2: a turn opening more than this after the last request is cold
CACHE_5M_S = 300
CACHE_60M_S = 3600
FOLLOW_ON_MAX_S = 300  # a follow-on is sent before the reply and within this of the arrival before it
ALBUM_S = 1  # photos sent within this many seconds of each other are taken as one album
HOLD_SHARES = (2, 3, 6)
ONE_HOUR_RATIO = 0.625  # 2.50 / 4.00: the 5-minute write price over the 1-hour write price (Sonnet)
DISPATCH_RE = re.compile(r"settle dispatch n=(\d+) after (\d+) ms \((\w+)\)")
BOOT_RE = re.compile(r"\bboot boot: .*?\bt=(\d+)")


# ------------------------------------------------------------------ loading
def tags_of(row: dict) -> dict:
    out = {}
    for t in (row.get("tags") or "").split(","):
        if ":" in t:
            k, v = t.split(":", 1)
            out.setdefault(k, v)
    return out


def load_rows(corpus: Path) -> list:
    rows = []
    with open(corpus / "episodic.jsonl", encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                rows.append(json.loads(line))
            except ValueError:
                continue
    return sorted(rows, key=lambda r: r["id"])  # ids are a hex sequence: the store's own order


def exact_time(row: dict):
    tg = tags_of(row)
    v = tg.get("at") or tg.get("sent")
    return int(v) if v and v.isdigit() else None


NAME_RE = re.compile(r"^[A-Za-z0-9_.-]{1,40}$")


def safe_name(v) -> str:
    """A tool or model name as written by the device, or '?' when it is anything else."""
    return v if isinstance(v, str) and NAME_RE.fullmatch(v) else "?"


def safe_int(v) -> int:
    try:
        return int(v)
    except (TypeError, ValueError):
        return 0


def _turn_end(r: dict):
    """The device's turn-end metrics row (a JSON object), or None."""
    if r["role"] != "system" or r["kind"] != "log" or not r["text"].startswith("{"):
        return None
    try:
        return json.loads(r["text"])
    except ValueError:
        return None


def index_turns(rows: list) -> tuple:
    """({turn: turn-end metrics}, {turn: [(tool, result bytes)]}, {turn: [reply rows]})."""
    ends, tools, replies = {}, {}, {}
    for r in rows:
        turn = tags_of(r).get("turn")
        if not turn:
            continue
        end = _turn_end(r)
        if end is not None:
            ends[turn] = end
        elif r["role"] == "tool" and r["kind"] == "tool_output":
            tools.setdefault(turn, []).append((safe_name(tags_of(r).get("tool")), len(r["text"].encode())))
        elif r["role"] == "assistant" and r["kind"] == "message":
            replies.setdefault(turn, []).append(r)
    return ends, tools, replies


def _text_bytes(rows: list) -> int:
    return sum(len(r["text"].encode()) for r in rows)


def _turn_record(first: dict, mine: list, end: dict, tools: list, reps: list) -> dict:
    t0 = exact_time(first)
    rep_at = [exact_time(r) for r in reps if exact_time(r)]
    texts = [r for r in mine if r["kind"] in ("message", "audio")]
    return {
        "turn": first["id"],
        "hour": first["ts"],
        "t": t0,
        "exact": t0 is not None,
        "end": max(rep_at) if rep_at else None,
        "msgs": len(texts),
        "photos": sum(1 for r in mine if r["kind"] == "image"),
        "text_bytes": _text_bytes(texts),
        "tools": tools,
        "reply_bytes": _text_bytes(reps),
        "model": safe_name(end.get("model")),
        "ok": bool(end.get("ok")),
        "tokens": {k: safe_int(end.get(k, 0)) for k in ("in", "out", "cr", "cw")},
    }


def owner_turns(rows: list) -> list:
    """Owner turns in order, as numbers only.

    A turn is named by its first owner row (the 'turn:' tag on its replies, tool rows and turn-end
    row). Owner rows after it and before the next turn's first row ride that turn. src=owner on the
    turn-end row marks an owner turn; a turn-end row without src (before v4.6) counts as owner when
    its turn row is an owner message."""
    by_id = {r["id"]: r for r in rows}
    ends, tools, replies = index_turns(rows)
    starts = sorted(
        t for t, e in ends.items() if t in by_id and by_id[t]["role"] == "user" and e.get("src", "owner") == "owner"
    )
    user_rows = [r for r in rows if r["role"] == "user"]
    out = []
    for n, tid in enumerate(starts):
        nxt = starts[n + 1] if n + 1 < len(starts) else "m~"
        mine = [r for r in user_rows if tid <= r["id"] < nxt]
        out.append(_turn_record(by_id[tid], mine, ends[tid], tools.get(tid, []), replies.get(tid, [])))
    return out


# ------------------------------------------------------------------ cold share
def cold_share(turns: list, since: datetime | None = None) -> dict:
    """The gap from the previous owner turn's reply to this turn's first message, exact rows only."""
    gaps = []
    prev_end = None
    for t in turns:
        if since and t["t"] is not None and t["t"] < since.timestamp():
            prev_end = t["end"]
            continue
        if t["exact"] and prev_end is not None:
            gaps.append(max(0, t["t"] - prev_end))
        prev_end = t["end"]
    n = len(gaps)

    def share(limit):
        k = sum(1 for g in gaps if g > limit)
        return {"k": k, "n": n, "share": round(k / n, 4) if n else None}

    c5, c60 = share(CACHE_5M_S), share(CACHE_60M_S)
    return {
        "turns_with_gap": n,
        "gap_s_median": statistics.median(gaps) if gaps else None,
        "cold_open_270s": share(COLD_OPEN_S),
        "cold_5m": c5,
        "cold_60m": c60,
        "between_5m_and_60m": {"k": c5["k"] - c60["k"], "n": n},
        "one_hour_write": one_hour_decision(c5["share"], c60["share"]),
    }


def one_hour_decision(c5, c60) -> dict:
    """PRD section 2: write the 1-hour cache only if cold share at 60 min < 0.625 x cold share at 5 min."""
    if not c5:
        return {"rule": "c60 < 0.625 x c5", "decision": "unmeasured"}
    ratio = round(c60 / c5, 4)
    return {
        "rule": "c60 < 0.625 x c5 (4.00 x c60 < 2.50 x c5 on Sonnet)",
        "c60_over_c5": ratio,
        "decision": "1-hour write" if ratio < ONE_HOUR_RATIO else "5-minute write",
    }


def turns_per_day(turns: list, since: datetime | None = None) -> dict:
    days = {}
    for t in turns:
        ts = t["t"] if t["t"] is not None else t["hour"] * 3600
        if since and ts < since.timestamp():
            continue
        d = datetime.fromtimestamp(ts, TZ).date().isoformat()
        days[d] = days.get(d, 0) + 1
    if not days:
        return {"days": 0, "turns": 0, "mean": None, "median": None}
    first, last = min(days), max(days)
    span = (datetime.fromisoformat(last) - datetime.fromisoformat(first)).days + 1
    vals = [days.get((datetime.fromisoformat(first) + timedelta(days=k)).date().isoformat(), 0) for k in range(span)]
    return {
        "days": span,
        "turns": sum(vals),
        "mean": round(sum(vals) / span, 2),
        "median": statistics.median(vals),
        "note": "every calendar day from the first to the last, quiet days as 0; the last day may be partial",
    }


def device_tokens(turns: list, since: datetime | None = None) -> dict:
    """What the device itself reported per owner turn (turn-end rows), by model."""
    out = {}
    for t in turns:
        if since and (t["t"] or t["hour"] * 3600) < since.timestamp():
            continue
        m = out.setdefault(t["model"] or "?", {"turns": 0, "in": 0, "out": 0, "cr": 0, "cw": 0})
        m["turns"] += 1
        for k in ("in", "out", "cr", "cw"):
            m[k] += t["tokens"].get(k, 0)
    for m in out.values():
        m["in_per_turn"] = round(m["in"] / m["turns"])
        m["cached_share"] = round(m["cr"] / m["in"], 4) if m["in"] else None
    return out


# ------------------------------------------------------------------ arrivals
def owner_arrivals(rows: list) -> list:
    """(send time, kind) of every owner arrival with an exact time, in send order.

    A photo or voice row carries Telegram's send time ('sent'); a text row its own ('at'). A text
    row tagged 'att' is the text of a media turn, stored after the media was fetched: its time is
    the store time, not a send time, so it is not an arrival."""
    out = []
    for r in rows:
        if r["role"] != "user" or "att" in tags_of(r):
            continue
        t = exact_time(r)
        if t is not None:
            out.append((t, r["kind"]))
    return sorted(out, key=lambda a: a[0])


def follow_ons(rows: list) -> dict:
    """Owner arrivals sent after an earlier one and before the device's reply to it.

    Photos sent in the same second are one album (Telegram joins them by group id whatever the
    hold) and are counted apart."""
    replies = sorted(exact_time(r) for r in rows if r["role"] == "assistant" and exact_time(r))
    arr = owner_arrivals(rows)
    gaps, album = [], 0
    for (t0, k0), (t1, k1) in zip(arr, arr[1:]):
        if t1 - t0 > FOLLOW_ON_MAX_S:
            continue
        if any(t0 <= rt <= t1 for rt in replies):
            continue  # the device answered in between: a new turn, not a follow-on
        if k0 == k1 == "image" and t1 - t0 <= ALBUM_S:
            album += 1
            continue
        gaps.append(t1 - t0)
    n = len(gaps)
    shares = {f"within_{s}s": {"k": sum(1 for g in gaps if g <= s), "n": n} for s in HOLD_SHARES}
    for v in shares.values():
        v["share"] = round(v["k"] / n, 4) if n else None
    return {
        "follow_ons": n,
        "album_members": album,
        "gaps_s": sorted(gaps),
        **shares,
        "note": "send times have 1 s resolution",
    }


def log_lines(corpus: Path) -> list:
    """(wall time or None, millis, text) for every line of the rotated logs, oldest file first."""
    files = sorted(corpus.glob("nimbus.log.*"), key=lambda p: -int(p.suffix[1:])) + [corpus / "nimbus.log"]
    out, boot_t, boot_ms = [], None, 0
    for p in files:
        if not p.exists():
            continue
        for line in p.read_text(errors="replace").splitlines():
            parts = line.split(" ", 2)
            if len(parts) < 3 or not parts[1].isdigit():
                continue
            ms = int(parts[1])
            m = BOOT_RE.search(line)
            if m:
                boot_t, boot_ms = int(m.group(1)), ms
            wall = boot_t + (ms - boot_ms) / 1000 if boot_t else None
            out.append((wall, ms, parts[2]))
    return out


def hold_payoffs(corpus: Path) -> dict:
    """Dispatches that joined more than one message: by the hold (quiet) or by a busy turn (old).

    A quiet dispatch waits until no message has arrived for the hold window, so for a quiet join
    of n messages the last one arrived (after_ms - window) after the first. The window is read
    from the single-message quiet dispatches of the same boot (their after_ms is the window)."""
    counts = {"dispatches": 0, "joined": 0, "joined_quiet": 0, "joined_old": 0}
    holds, joins, boot = [], [], 0
    windows = {}
    for _, _, text in log_lines(corpus):
        if BOOT_RE.search(text):
            boot += 1
            continue
        m = DISPATCH_RE.search(text)
        if not m:
            continue
        n, after_ms, why = int(m.group(1)), int(m.group(2)), m.group(3)
        counts["dispatches"] += 1
        if why == "quiet" and n == 1:
            holds.append(after_ms)
            windows.setdefault(boot, []).append(after_ms)
        if n >= 2:
            counts["joined"] += 1
            key = f"joined_{why}" if why in ("quiet", "old") else "joined_other"
            counts[key] = counts.get(key, 0) + 1
            if why == "quiet":
                joins.append((boot, n, after_ms))
    quiet = []
    for b, n, after_ms in joins:
        w = statistics.median(windows[b]) if b in windows else None
        quiet.append({"n": n, "after_ms": after_ms, "window_ms": w, "last_after_first_ms": after_ms - w if w else None})
    counts["single_quiet_hold_ms_median"] = statistics.median(holds) if holds else None
    counts["quiet_joins"] = quiet
    return counts


# ------------------------------------------------------------------ shape
def day_shape(turns: list, days: list) -> dict:
    """The numeric shape of the given local days: no text, only sizes, counts and gaps."""
    sel = []
    for t in turns:
        ts = t["t"] if t["t"] is not None else t["hour"] * 3600
        if datetime.fromtimestamp(ts, TZ).date().isoformat() in days:
            sel.append(dict(t, _ts=ts))
    # Hour-only rows: spread in order inside their hour.
    by_hour = {}
    for t in sel:
        if not t["exact"]:
            by_hour.setdefault(t["hour"], []).append(t)
    for hour, group in by_hour.items():
        for k, t in enumerate(group):
            t["_ts"] = hour * 3600 + int(3600 * (k + 0.5) / len(group))
    sel.sort(key=lambda t: t["_ts"])
    if not sel:
        return {"days": days, "turns": []}
    t0 = sel[0]["_ts"]
    first_local = datetime.fromtimestamp(t0, TZ)
    return {
        "days": days,
        "first_local": first_local.strftime("%Y-%m-%d %H:%M:%S"),
        "turns": [
            {
                "at_s": t["_ts"] - t0,
                "exact": t["exact"],
                "msgs": t["msgs"],
                "photos": t["photos"],
                "text_bytes": t["text_bytes"],
                "tools": [[name, size] for name, size in t["tools"]],
                "reply_bytes": t["reply_bytes"],
                "device_model": t["model"],
                "device_tokens": t["tokens"],
            }
            for t in sel
        ],
    }


# ------------------------------------------------------------------ cli
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dir", required=True, type=Path, help="folder with episodic.jsonl and nimbus.log*")
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gaps", help="cold share, arrivals, hold pay-offs, turns a day")
    g.add_argument("--since", default="2026-11-03", help="local date: exact send times exist from v4.6")
    g.add_argument("--json", type=Path)
    s = sub.add_parser("shape", help="the numeric shape of local days")
    s.add_argument("--days", required=True, help="comma-separated YYYY-MM-DD")
    s.add_argument("--out", type=Path)
    a = ap.parse_args(argv)
    rows = load_rows(a.dir)
    turns = owner_turns(rows)
    if a.cmd == "gaps":
        since = datetime.fromisoformat(a.since).replace(tzinfo=TZ)
        out = {
            "since": a.since,
            "owner_turns": len([t for t in turns if (t["t"] or t["hour"] * 3600) >= since.timestamp()]),
            "turns_per_day": turns_per_day(turns, since),
            "cold": cold_share(turns, since),
            "arrivals": follow_ons(rows),
            "holds": hold_payoffs(a.dir),
            "device_tokens": device_tokens(turns, since),
        }
        text = json.dumps(out, indent=1)
        if a.json:
            a.json.write_text(text + "\n")
        print(text)
    else:
        out = day_shape(turns, a.days.split(","))
        text = json.dumps(out, indent=1)
        if a.out:
            a.out.write_text(text + "\n")
        else:
            print(text)


if __name__ == "__main__":
    main()
