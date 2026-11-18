#!/usr/bin/env python3
"""Context, cache, fold and cost figures from saved device logs (read-only).

It never contacts a device: save the logs first (the rotated SD copies nimbus.log.N,
or /api/log text), then point this at them. It turns them into before-and-after
figures per provider and per turn, for the history and summary fix of v5.1.2
(program 2026-10-18, lumi-history-2026-11-15):

  history  owner turns whose history was cut ('history: N message(s) left out'),
           messages left out, and from v5.1.2 the context line (rows sent k/n, today
           t/T, cut bytes, summary age, round-0 body against the budget, history read
           ms, card reads, lock ms)
  cache    per host/model from 'turn spend': turns, requests, input, cached reads and
           writes, the cached share, the in-turn ceiling sum(in x (n-1)/n) / sum(in),
           achieved/ceiling, uncached tokens per turn, and cost three ways: the
           device ledger, the repo price row with cache rates, and published prices
  refits   'headloop: round N body B over the X B budget' (round 0 and later), the
           turns with a refit at round 1 or later and their cache shortfall
  turn fit 'turn fit: left out' lines and what each dropped
  capped   'fabric-loop: ... cap=<reason>' final rounds by reason
  x-turn   round-0 cross-turn cache hits on Mistral (cR - in x (n-1)/n > 5,000 tokens)
           by idle time since the previous request; per host, the round-0 read share of
           a turn that follows another inside 300 s (a share over 100% of the in-turn
           ceiling in the cache table means the same thing: round 0 read a warm cache)
  fold     successful folds and the last one, old and new failure lines (rung i/n,
           cycle failed), paid fold requests per day, the blob's size and save time
  upload   transport heaptrace bodies (largest, over 85,000 B, af and fail counts),
           and from v5.1.2 wms/pms per KB and the 'transport: up' lines

Log line shape: '<seq> <millis> <kind> <text>', kind '-' or 'boot'. Wall time is the
boot line's t= plus (millis - boot millis) / 1000; lines before the first boot line have
none. A serial console capture ('<UTC stamp> [agent] <text>', the bench drivers'
console.log) is read too, on the host's stamps. A turn is every event since the previous turn end (or boot) up to its
'turn end: <status> host=... src=<source>' line: the budget and history lines come
before 'orchestrator: turn host=', so the turn owns them. A boot closes a turn that
never ended (counted as unfinished, never as an owner turn).

    python3 tools/context_report.py nimbus.log.3 nimbus.log.2 nimbus.log.1 nimbus.log
    python3 tools/context_report.py --dir <folder with nimbus.log*> [--json out.json]
    python3 tools/context_report.py --dir <folder> --check-baseline   # exit 1 on a miss

--check-baseline compares the figures with the 15 Nov 2026 baseline the plan quotes
(BASELINE below) and prints each one with match or MISS. The output carries numbers
and timestamps only: no message text from the log is ever printed.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import statistics
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PRICE_HEADER = REPO / "lib" / "core" / "include" / "nimbus" / "orch" / "model_price_defaults.h"

# The 15 Nov 2026 baseline (plan v3 measurement_plan, investigation-result.json): the
# figures this tool must reproduce from the four saved Lumi logs before its other
# numbers are trusted.
BASELINE = {
    "owner_turns_cut": 121,
    "owner_turns": 123,
    "large_turns": 173,
    "large_requests": 602,
    "large_cached_share_pct": 59.4,
    "large_ceiling_pct": 71.6,
    "large_of_ceiling_pct": 82.9,
    "refusals": 67,
    "refits_after_round0": 49,
    "refit_turns": 26,
    "turn_fits": 30,
    "capped_finals": 28,
    "xturn_hits": 18,
    "xturn_turns": 201,
    "recall_fits_freeing_nothing": 4,
    "recall_fits_measurable": 4,
}
# The release that added the wire guard ('history: N message(s) left out'): the
# baseline counts owner turns from its first boot on.
DEFAULT_CUT_SINCE = "v4.9.0"
XTURN_HIT_TOKENS = 5000  # a round-0 read above this many tokens is a cross-turn hit
XTURN_BUCKETS = [(0, 60), (60, 120), (120, 300), (300, 600), (600, 3600), (3600, None)]

# Published list prices, USD per 1M tokens: input, output, cache read, cache write.
# Matched by the longest model prefix; a model with no row here is priced at its repo
# row with the default cache shares (reads 0.1x, writes 1.25x).
PUBLISHED = {
    # docs.mistral.ai/models/mistral-large-4 and /inference/pricing, fetched 15 Nov 2026
    # (investigation-result.json, cache area); cached reads at 10% of input.
    ("mistral", "mistral-large"): (0.70, 2.10, 0.07, 0.0),
    ("mistral", "mistral-small"): (0.20, 0.60, 0.02, 0.0),
}

LINE = re.compile(r"^(\d+) (\d+) (\S+) (.*)$")
# A serial console capture (the bench drivers' console.log): '<UTC stamp> [agent] <text>'.
CONSOLE_LINE = re.compile(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ) \[agent\] (.*)$")
BOOT = re.compile(r"boot: n=(\S+) fw=(\S+)")
BOOT_T = re.compile(r"\bt=(\d+)")
SLOT = re.compile(r"([\w.-]+)/(\S+?) in=(\d+) out=(\d+) cR=(\d+) cW=(\d+) n=(\d+)(?: id=(\S+))?")
TURN_SPEND = re.compile(r"^turn spend: (\S+) (\S+) \|")
TURN_END = re.compile(r"^turn end: (\S+) host=\S* .*\bsrc=(\S+)")
TURN_START = re.compile(r"^orchestrator: turn host=")
HIST_CUT = re.compile(r"^history: (\d+) message\(s\) left out to keep the request under (\d+) B")
HIST_SHIFT = re.compile(r"^history\(.*overflow shift")
TURN_FIT = re.compile(r"^turn fit: left out (.*)")
REFUSAL = re.compile(r"^headloop: round (\d+) body (\d+) B over the (\d+) B budget")
FABRIC_LOOP = re.compile(r"^fabric-loop: rounds=(\d+) cap=(\S*)")
FABRIC_SPEND = re.compile(r"^fabric-spend: ")
CORRECTION = re.compile(r"final answer sent back for one correction")
PHOTO = re.compile(r"^vision: .*photo\(s\)")
HEAPTRACE = re.compile(r"^transport: heaptrace (\S+) body=(\d+) .*?\bfail=(\S+) af=(\d+)")
HT_MS = re.compile(r"\bms=(\d+)")
HT_WMS = re.compile(r"\bwms=(\d+)")
HT_PMS = re.compile(r"\bpms=(\d+)")
UPLOAD = re.compile(r"^transport: up (\d+) B in (\d+) ms, paced (\d+)")
FOLD_OK = re.compile(r"^fold\(([^)]*)\): ok on (\S+)")
FOLD_RUNG_FAIL = re.compile(r"^fold\(([^)]*)\): rung (\d+)/(\d+) (\S+) failed -> (\S+)")
FOLD_OLD_FAIL = re.compile(r"^fold\(([^)]*)\): (\S+) failed")
FOLD_NO_SUMMARY = re.compile(r"^fold\(([^)]*)\): no summary in reply")
FOLD_CYCLE_FAIL = re.compile(r"^fold\(([^)]*)\): cycle failed")
FOLD_ANY = re.compile(r"^fold\(")
# The fold blob save time (lane F, orch_persist.cpp; bench row B05 times it):
# 'fold: chatsum.txt <B> B saved in <ms> ms' ('NOT saved' when the write failed).
FOLD_BLOB = re.compile(r"^fold: chatsum\.txt (\d+) B (saved|NOT saved) in (\d+) ms")
FOLD_RUNG_SKIP = re.compile(r"^fold\(([^)]*)\): rung (\d+)/(\d+) (\S+) passed over")
# v5.1.2 lines (lane H): the per-turn context line and the budget line.
CONTEXT = re.compile(
    r"^context: sent (\d+)/(\d+) live rows \(today (\d+)/(\d+)\), cut (\d+) \((\d+) (?:B|bytes)\), "
    r"summary age (\S+), round0 (\d+) B / budget (\d+) B, hist (\d+) ms (\d+) card reads lock (\d+)"
)
BUDGET_NEW = re.compile(
    r"^budget: ([\w.-]+)/(\S+) window (\d+)K target (\d+)K tok \((\d+) KB\) hist (\d+)/(\d+)/(\d+) KB \((\w+)\)"
)


# ---- parsing --------------------------------------------------------------------------------


def fw_key(fw: str | None) -> tuple:
    """'v4.9.0' or '5.1.1-rc' -> (4, 9, 0) / (5, 1, 1); unknown sorts first."""
    m = re.search(r"(\d+)\.(\d+)\.(\d+)", fw or "")
    return tuple(int(x) for x in m.groups()) if m else (-1,)


def rotation_files(folder: str) -> list[str]:
    """The nimbus.log rotation set in a folder, oldest first (nimbus.log.N ... nimbus.log)."""
    names = [n for n in os.listdir(folder) if re.fullmatch(r"nimbus\.log(\.\d+)?", n)]

    def age(n: str) -> int:
        return int(n.rsplit(".", 1)[1]) if n.count(".") == 2 else 0

    return [os.path.join(folder, n) for n in sorted(names, key=age, reverse=True)]


def console_line(line: str):
    """(millis, kind, text, wall) of a console capture line, or None."""
    c = CONSOLE_LINE.match(line)
    if not c:
        return None
    at = dt.datetime.strptime(c.group(1), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc)
    return 0, "boot" if c.group(2).startswith("boot: n=") else "-", c.group(2), at.timestamp()


def read_lines(paths: list[str]):
    """Yield (millis, kind, text, wall) for every well-formed log line, file by file.

    wall is None for a device log line (its time comes from the boot line) and the host's
    stamp for a console capture line, which has no millis."""
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = raw.rstrip("\n")
                m = LINE.match(line)
                rec = (int(m.group(2)), m.group(3), m.group(4), None) if m else console_line(line)
                if rec:
                    yield rec


def slots_of(text: str) -> list[dict]:
    out = []
    for host, model, i, o, cr, cw, n, rid in SLOT.findall(text):
        out.append(dict(host=host, model=model, id=rid or "", inp=int(i), out=int(o), cr=int(cr), cw=int(cw), n=int(n)))
    return out


def new_turn() -> dict:
    return dict(
        order=[],
        hist_cut=[],
        shifts=0,
        fits=[],
        refusals=[],
        cap=None,
        rounds=None,
        corrections=0,
        photos=0,
        heaptraces=[],
        context=None,
        budget=None,
        started=False,
        start_wall=None,
        slots=[],
        spend_status=None,
    )


class Log:
    """One pass over the lines: turns, stream events and per-line tallies."""

    def __init__(self) -> None:
        self.turns: list[dict] = []
        self.unfinished: list[dict] = []
        self.boots: list[dict] = []
        self.fw_first: dict = {}
        self.lines = 0
        self.first_wall = None
        self.last_wall = None
        self.days: set = set()
        self.refusals: list[dict] = []
        self.turn_fits: list[dict] = []
        self.caps: list[str] = []
        self.heaptraces: list[dict] = []
        self.uploads: list[dict] = []
        self.folds: list[dict] = []
        self.fold_blob: list[dict] = []
        self.xturn: list[dict] = []
        self.last_fold_line = None
        self._boot = None
        self._cur = new_turn()
        self._xstart = None
        self._xlast = None

    # -- clock --
    def wall(self, ms: int) -> float | None:
        b = self._boot
        if not b or b["t"] is None:
            return None
        return b["t"] + (ms - b["ms"]) / 1000.0

    def fw(self) -> str | None:
        return self._boot["fw"] if self._boot else None

    # -- events --
    def feed(self, ms: int, kind: str, text: str, wall: float | None = None) -> None:
        self.lines += 1
        if kind == "boot" and text.startswith("boot: n="):
            self.on_boot(ms, text)
            return
        w = wall if wall is not None else self.wall(ms)
        if w is not None:
            self.first_wall = w if self.first_wall is None else self.first_wall
            self.last_wall = w
            self.days.add(day_of(w))
        if FOLD_BLOB.match(text):  # rewritten every turn: not a fold, so not the "last fold line"
            self.on_fold(text, w)
        elif FOLD_ANY.match(text):
            self.on_fold(text, w)
            self.last_fold_line = self.lines
        self.on_turn_line(text, w)
        self.on_stream_line(text, w)

    def on_boot(self, ms: int, text: str) -> None:
        m = BOOT.search(text)
        mt = BOOT_T.search(text)
        t = int(mt.group(1)) if mt and int(mt.group(1)) > 1_600_000_000 else None
        self._boot = dict(n=m.group(1), fw=m.group(2), t=t, ms=ms)
        self.boots.append(dict(self._boot))
        if m.group(2) not in self.fw_first:
            self.fw_first[m.group(2)] = t
        if self._cur["started"] or self._cur["slots"]:
            self.unfinished.append(self._cur)
        self._cur = new_turn()
        self._xstart = None
        self._xlast = None

    def on_turn_line(self, text: str, w: float | None) -> None:
        cur = self._cur
        m = HIST_CUT.match(text)
        if m:
            cur["hist_cut"].append(int(m.group(1)))
            return
        if HIST_SHIFT.match(text):
            cur["shifts"] += 1
            return
        m = TURN_FIT.match(text)
        if m:
            cur["fits"].append(m.group(1))
            cur["order"].append(("fit", fit_kinds(m.group(1))))
            self.turn_fits.append(dict(wall=w, what=fit_kinds(m.group(1))))
            return
        m = REFUSAL.match(text)
        if m:
            r = dict(
                round=int(m.group(1)),
                body=int(m.group(2)),
                budget=int(m.group(3)),
                wall=w,
                tools="replayed tool rounds" in text,
            )
            cur["refusals"].append(r)
            cur["order"].append(("refusal", r))
            self.refusals.append(r)
            return
        m = FABRIC_LOOP.match(text)
        if m:
            cur["rounds"], cur["cap"] = int(m.group(1)), m.group(2)
            if m.group(2):
                self.caps.append(m.group(2))
            return
        if CORRECTION.search(text):
            cur["corrections"] += 1
            return
        if PHOTO.match(text):
            cur["photos"] += 1
            return
        m = CONTEXT.match(text)
        if m:
            cur["context"] = context_fields(m)
            return
        m = BUDGET_NEW.match(text)
        if m:
            cur["budget"] = dict(
                host=m.group(1),
                model=m.group(2),
                window_k=int(m.group(3)),
                target_k=int(m.group(4)),
                kb=int(m.group(5)),
                hist_kb=[int(m.group(i)) for i in (6, 7, 8)],
                mode=m.group(9),
            )
            return
        if TURN_START.match(text):
            cur["started"], cur["start_wall"] = True, w
            return
        m = TURN_SPEND.match(text)
        if m:
            cur["spend_status"], cur["spend_src"] = m.group(1), m.group(2)
            cur["slots"] = slots_of(text)
            return
        m = TURN_END.match(text)
        if m:
            cur.update(status=m.group(1), src=m.group(2), end_wall=w, fw=self.fw())
            self.turns.append(cur)
            self._cur = new_turn()

    def on_stream_line(self, text: str, w: float | None) -> None:
        m = HEAPTRACE.match(text)
        if m:
            h = dict(host=m.group(1), body=int(m.group(2)), fail=m.group(3), af=int(m.group(4)), wall=w)
            for key, rx in (("ms", HT_MS), ("wms", HT_WMS), ("pms", HT_PMS)):
                mm = rx.search(text)
                h[key] = int(mm.group(1)) if mm else None
            self.heaptraces.append(h)
            self._cur["heaptraces"].append(h)
            return
        m = UPLOAD.match(text)
        if m:
            self.uploads.append(dict(body=int(m.group(1)), ms=int(m.group(2)), paced=int(m.group(3)), wall=w))
            return
        if TURN_START.match(text):
            self._xstart = w
            return
        if FABRIC_SPEND.match(text):
            self.on_fabric_spend(text, w)

    def on_fabric_spend(self, text: str, w: float | None) -> None:
        """A cross-turn round-0 sample: the turn's largest Mistral slot (the 15 Nov baseline's
        rule), else its largest slot on any host; idle since the last request of any model."""
        slots = slots_of(text)
        mistral = [s for s in slots if s["host"] == "mistral"]
        pool = mistral or slots
        if pool and self._xstart is not None:
            s = max(pool, key=lambda s: s["inp"])
            prompt = prompt_tokens(s["host"], s["model"], s["inp"], s["cr"], s["cw"])
            h0 = s["cr"] - ideal_read(s)
            idle = (self._xstart - self._xlast) if self._xlast is not None else None
            self.xturn.append(
                dict(wall=self._xstart, host=s["host"], model=s["model"], n=s["n"], h0=h0, idle=idle, prompt=prompt)
            )
        if w is not None:
            self._xlast = w
        self._xstart = None

    def on_fold(self, text: str, w: float | None) -> None:
        m = FOLD_BLOB.match(text)
        if m:
            self.fold_blob.append(dict(bytes=int(m.group(1)), ok=m.group(2) == "saved", ms=int(m.group(3)), wall=w))
            return
        ev = None
        if FOLD_OK.match(text):
            ev = dict(kind="ok", host=FOLD_OK.match(text).group(2))
        elif FOLD_RUNG_FAIL.match(text):
            m = FOLD_RUNG_FAIL.match(text)
            ev = dict(kind="rung_failed", rung=int(m.group(2)), of=int(m.group(3)), host=m.group(4), why=m.group(5))
        elif FOLD_RUNG_SKIP.match(text):
            ev = dict(kind="rung_passed_over")
        elif FOLD_CYCLE_FAIL.match(text):
            ev = dict(kind="cycle_failed")
        elif FOLD_NO_SUMMARY.match(text):
            ev = dict(kind="no_summary")
        elif FOLD_OLD_FAIL.match(text):
            ev = dict(kind="failed", host=FOLD_OLD_FAIL.match(text).group(2))
        if ev:
            ev["wall"] = w
            self.folds.append(ev)

    def finish(self) -> None:
        if self._cur["started"] or self._cur["slots"]:
            self.unfinished.append(self._cur)
        self._cur = new_turn()


def day_of(w: float) -> str:
    return dt.datetime.fromtimestamp(w, dt.timezone.utc).strftime("%Y-%m-%d")


def stamp(w: float | None) -> str | None:
    return dt.datetime.fromtimestamp(w, dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC") if w else None


def fit_kinds(text: str) -> list[str]:
    """What one 'turn fit: left out ...' line dropped, as short kinds."""
    kinds = []
    for kind, rx in (
        ("recalled", r"recalled memor"),
        ("playbook", r"playbook"),
        ("older_history", r"older conversation"),
        ("skills", r"skill"),
        ("results", r"result"),
    ):
        if re.search(rx, text):
            kinds.append(kind)
    return kinds or ["other"]


def context_fields(m: re.Match) -> dict:
    age = m.group(7)
    try:
        age_h = float(age.rstrip("h"))
    except ValueError:
        age_h = None
    keys = ("sent", "live", "today_sent", "today_total", "cut_rows", "cut_bytes")
    out = {k: int(m.group(i + 1)) for i, k in enumerate(keys)}
    out.update(
        summary_age_h=age_h,
        round0=int(m.group(8)),
        budget=int(m.group(9)),
        hist_ms=int(m.group(10)),
        card_reads=int(m.group(11)),
        lock_ms=int(m.group(12)),
    )
    return out


def parse(paths: list[str]) -> Log:
    log = Log()
    for ms, kind, text, wall in read_lines(paths):
        log.feed(ms, kind, text, wall)
    log.finish()
    return log


# ---- prices ---------------------------------------------------------------------------------


def repo_price_rows(header: Path = PRICE_HEADER) -> dict:
    """{(slot, family): (in, out, cacheRead, cacheWrite)} USD per 1M from model_price_defaults.h."""
    rows = {}
    if not header.exists():
        return rows
    rx = re.compile(r'\{"(\w+)", "([^"]+)", (\d+), (\d+), (\d+)(?:, (\d+), (\d+))?')
    for slot, fam, mi, mo, _calls, cr, cw in rx.findall(header.read_text()):
        pin, pout = int(mi) / 1e6, int(mo) / 1e6
        crate = int(cr) / 1e6 if cr and int(cr) else pin * 0.1
        cwrate = int(cw) / 1e6 if cw and int(cw) else pin * 1.25
        rows[(slot, fam)] = (pin, pout, crate, cwrate)
    return rows


def match_row(rows: dict, host: str, model: str):
    best = None
    for (slot, fam), row in rows.items():
        if slot == host and fam != "*" and model.startswith(fam) and (best is None or len(fam) > len(best[0])):
            best = (fam, row)
    if best:
        return best[1]
    return rows.get((host, "*"))


def upstream(host: str, model: str) -> tuple[str, str]:
    """The provider whose usage shape and price apply: a routed slot (cumulo/openai/gpt-5.6) is its upstream's."""
    if host == "cumulo" and "/" in model:
        up, m = model.split("/", 1)
        return up, m
    return host, model


def prompt_tokens(host: str, model: str, inp: int, cr: int, cw: int) -> int:
    """A request's whole prompt: Anthropic reports cache reads and writes beside input_tokens, outside it
    (token_usage_json.h); OpenAI and the chat-completions providers count them inside it."""
    return inp + cr + cw if upstream(host, model)[0] == "anthropic" else inp


def costs(host: str, model: str, agg: dict, rows: dict) -> dict:
    """USD three ways: the v5.1.1 device ledger, the repo row with cache rates, published prices."""
    prov, m = upstream(host, model)
    repo = match_row(rows, prov, m)
    if not repo:
        return {}
    pub = match_row(PUBLISHED, prov, m) or repo
    inp, cr, cw, out = agg["inp"], agg["cr"], agg["cw"], agg["out"]
    if prov == "anthropic":  # input excludes both cache counts; the ledger prices each
        plain = inp
    elif prov == "openai":  # both cache counts are part of input; the ledger splits them out
        plain = inp - cr - cw
    else:  # orch_usage_ledger.cpp: cache traffic not split, the whole input at the input rate
        plain = inp - cr
        cw = 0

    def priced(row: tuple, split_reads: bool) -> float:
        pin, pout, pcr, pcw = row
        reads = cr * pcr if split_reads else cr * pin
        return (plain * pin + reads + cw * pcw + out * pout) / 1e6

    ledger_splits = prov in ("anthropic", "openai")
    return {
        "ledger": round(priced(repo, ledger_splits), 2),
        "repo_cache": round(priced(repo, True), 2),
        "published": round(priced(pub, True), 2),
    }


# ---- figures --------------------------------------------------------------------------------


def pct(a: float, b: float) -> float | None:
    return round(100.0 * a / b, 1) if b else None


def spread(xs: list) -> dict | None:
    xs = [x for x in xs if x is not None]
    if not xs:
        return None
    s = sorted(xs)
    p95 = s[min(len(s) - 1, int(round(0.95 * (len(s) - 1))))]
    return {
        "n": len(s),
        "min": s[0],
        "median": statistics.median(s),
        "mean": round(statistics.mean(s), 1),
        "p95": p95,
        "max": s[-1],
    }


def count(xs, pred) -> int:
    return sum(1 for x in xs if pred(x))


def context_line_figures(ctx: list[dict]) -> dict:
    """The v5.1.2 context lines of owner turns (lane H)."""
    out = {
        "turns": len(ctx),
        "today_complete_pct": pct(count(ctx, lambda c: c["today_sent"] == c["today_total"]), len(ctx)),
        "turns_with_cut": count(ctx, lambda c: c["cut_rows"] > 0),
        "round0_of_budget_pct": spread([pct(c["round0"], c["budget"]) for c in ctx]),
    }
    for key, field in (
        ("rows_sent", "sent"),
        ("live_rows", "live"),
        ("cut_bytes", "cut_bytes"),
        ("summary_age_h", "summary_age_h"),
        ("hist_ms", "hist_ms"),
        ("card_reads", "card_reads"),
        ("lock_ms", "lock_ms"),
    ):
        out[key] = spread([c[field] for c in ctx])
    return out


def history_figures(log: Log, since: str) -> dict:
    owner = [t for t in log.turns if t.get("src") == "owner"]
    after = [t for t in owner if fw_key(t.get("fw")) >= fw_key(since)]
    before = [t for t in owner if fw_key(t.get("fw")) < fw_key(since)]
    cut_after = [t for t in after if t["hist_cut"]]
    out = {
        "since_fw": since,
        "owner_turns": len(after),
        "owner_turns_cut": len(cut_after),
        "owner_turns_before": len(before),
        "owner_turns_cut_before": count(before, lambda t: t["hist_cut"]),
        "left_out_per_cut_turn": spread([max(t["hist_cut"]) for t in cut_after]),
        "overflow_shifts": sum(t["shifts"] for t in log.turns),
        "unfinished_turns": len(log.unfinished),
    }
    ctx = [t["context"] for t in owner if t["context"]]
    if ctx:
        out["context_lines"] = context_line_figures(ctx)
    return out


def cache_figures(log: Log, rows: dict) -> dict:
    agg: dict = {}
    for t in log.turns:
        for s in t["slots"]:
            key = f"{s['host']}/{s['model']}"
            a = agg.setdefault(
                key,
                dict(
                    host=s["host"],
                    model=s["model"],
                    turns=0,
                    requests=0,
                    inp=0,
                    cr=0,
                    cw=0,
                    out=0,
                    ceiling=0.0,
                    owner_turns=0,
                    prompt=0,
                ),
            )
            prompt = prompt_tokens(s["host"], s["model"], s["inp"], s["cr"], s["cw"])
            a["turns"] += 1
            a["owner_turns"] += 1 if t.get("src") == "owner" else 0
            a["requests"] += s["n"]
            a["prompt"] += prompt
            for k in ("inp", "cr", "cw", "out"):
                a[k] += s[k]
            a["ceiling"] += ideal_read(s)
    out = {}
    for key, a in sorted(agg.items()):
        out[key] = {
            "turns": a["turns"],
            "owner_turns": a["owner_turns"],
            "requests": a["requests"],
            "input": a["inp"],
            "prompt": a["prompt"],
            "cached_read": a["cr"],
            "cached_write": a["cw"],
            "output": a["out"],
            "cached_share_pct": pct(a["cr"], a["prompt"]),
            "in_turn_ceiling_pct": pct(a["ceiling"], a["prompt"]),
            "of_ceiling_pct": pct(a["cr"], a["ceiling"]),
            "ceiling_tokens": round(a["ceiling"]),
            "uncached": a["prompt"] - a["cr"],
            "uncached_per_turn": round((a["prompt"] - a["cr"]) / a["turns"]) if a["turns"] else None,
            "usd": costs(a["host"], a["model"], a, rows),
        }
    return out


def family_rollup(cache: dict, prefix: str) -> dict:
    """Sum the per-model rows whose model starts with prefix (e.g. mistral/mistral-large)."""
    keys = [k for k in cache if k.startswith(prefix)]
    if not keys:
        return {}
    tot = {k: sum(cache[x][k] for x in keys) for k in ("turns", "requests", "input", "prompt", "cached_read", "output")}
    ceiling = sum(cache[x]["ceiling_tokens"] for x in keys)
    tot["cached_share_pct"] = pct(tot["cached_read"], tot["prompt"])
    tot["in_turn_ceiling_pct"] = pct(ceiling, tot["prompt"])
    tot["of_ceiling_pct"] = pct(tot["cached_read"], ceiling)
    tot["usd"] = {
        k: round(sum(cache[x]["usd"].get(k, 0.0) for x in keys), 2) for k in ("ledger", "repo_cache", "published")
    }
    tot["models"] = keys
    return tot


def ideal_read(s: dict) -> float:
    """The in-turn cache ceiling of one slot: every request after the first re-reads its prefix."""
    prompt = prompt_tokens(s["host"], s["model"], s["inp"], s["cr"], s["cw"])
    return prompt * (s["n"] - 1) / s["n"] if s["n"] else 0.0


def large_refit_turn(t: dict) -> dict | None:
    """A Mistral Large turn with a refit at round 1 or later: its refits and cache shortfall."""
    late = [r for r in t["refusals"] if r["round"] >= 1]
    large = [s for s in t["slots"] if s["model"].startswith("mistral-large")]
    if not (late and large):
        return None
    return dict(refits=len(late), shortfall=max(0.0, ideal_read(large[0]) - large[0]["cr"]))


def refit_figures(log: Log) -> dict:
    turns = [x for x in (large_refit_turn(t) for t in log.turns) if x]
    return {
        "refusals": len(log.refusals),
        "at_round0": count(log.refusals, lambda r: r["round"] == 0),
        "after_round0": count(log.refusals, lambda r: r["round"] >= 1),
        "large_refit_turns": len(turns),
        "large_refits_after_round0": sum(t["refits"] for t in turns),
        "large_refit_shortfall_tokens": round(sum(t["shortfall"] for t in turns)),
        "budgets": sorted({r["budget"] for r in log.refusals}),
        "largest_refused_body": max((r["body"] for r in log.refusals), default=None),
    }


def recall_only_deltas(t: dict) -> list:
    """The body change each recall-only turn fit made, where the log shows it (bytes).

    The refit line that follows a turn fit names the body measured before that fit ('round N
    body B B over ... folded <what the fit gave>'), so a recall-only fit's effect is the next
    same-round refit's body minus its own, read only where the very next event is the next fit
    and its refit (nothing else moved the body). A rung that frees nothing but adds its
    '[LEFT OUT ...]' note grows the body: the phantom recall note (recall was not in it)."""
    out, ev = [], t["order"]
    for i, (kind, what) in enumerate(ev):
        if kind != "fit" or what != ["recalled"]:
            continue
        # fit A, its refit, the next fit B, B's refit: nothing else moved the body in between
        nxt = [e[0] for e in ev[i + 1 : i + 4]]
        clean = nxt == ["refusal", "fit", "refusal"] and not ev[i + 1][1]["tools"]  # A's refit folded only A
        if clean and ev[i + 1][1]["round"] == ev[i + 3][1]["round"]:
            out.append(ev[i + 3][1]["body"] - ev[i + 1][1]["body"])
    return out


def fit_figures(log: Log) -> dict:
    kinds = tally(k for f in log.turn_fits for k in f["what"])
    walls = [f["wall"] for f in log.turn_fits if f["wall"]]
    deltas = [d for t in log.turns + log.unfinished for d in recall_only_deltas(t)]
    return {
        "turn_fits": len(log.turn_fits),
        "dropped": kinds,
        "recall_only_measurable": len(deltas),
        "recall_only_freed_nothing": count(deltas, lambda d: d >= 0),
        "recall_only_body_change": sorted(set(deltas)),
        "first": stamp(min(walls)) if walls else None,
        "last": stamp(max(walls)) if walls else None,
    }


def cap_figures(log: Log) -> dict:
    return {"capped_finals": len(log.caps), "by_reason": tally(log.caps)}


NEXT_TURN_TTL_S = 300  # the shortest provider cache lifetime (Anthropic's 5 minute write)


def round0_share(x: dict) -> float:
    """The share of a turn's first request read from the cache left by the turn before:
    the read beyond the in-turn ideal, over one request's prompt (the turn's prompt / n)."""
    one = x["prompt"] / x["n"] if x["n"] else 0
    return round(100.0 * max(0.0, x["h0"]) / one, 1) if one else 0.0


def next_turn_by_host(samples: list) -> dict:
    """Per host: turns that follow another request, those inside the cache lifetime, and the
    round-0 read share of those (the plan's 'next turn inside the TTL reads at least 90%')."""
    out = {}
    for host in sorted({x["host"] for x in samples}):
        mine = [x for x in samples if x["host"] == host]
        warm = [round0_share(x) for x in mine if x["idle"] < NEXT_TURN_TTL_S]
        out[host] = {
            "turns": len(mine),
            "inside_ttl": len(warm),
            "round0_read_pct": spread(warm),
            "at_least_90_pct": count(warm, lambda v: v >= 90.0),
        }
    return out


def xturn_figures(log: Log) -> dict:
    after_a_request = [x for x in log.xturn if x["idle"] is not None]
    samples = [x for x in after_a_request if x["host"] == "mistral"]
    hits = [x for x in samples if x["h0"] > XTURN_HIT_TOKENS]
    buckets = []
    for lo, hi in XTURN_BUCKETS:
        inb = [x for x in samples if x["idle"] >= lo and (hi is None or x["idle"] < hi)]
        buckets.append(
            {
                "idle_s": f"{lo}-{hi if hi else 'inf'}",
                "hits": sum(1 for x in inb if x["h0"] > XTURN_HIT_TOKENS),
                "turns": len(inb),
            }
        )
    return {
        "hits": len(hits),
        "turns": len(samples),
        "mistral_turns": count(log.xturn, lambda x: x["host"] == "mistral"),
        "hit_tokens": spread([round(x["h0"]) for x in hits]),
        "by_idle": buckets,
        "next_turn_by_host": next_turn_by_host(after_a_request),
    }


# A fold request the daily cap counts (fold_ladder.h paidRequests): a 2xx answer (a summary, an
# empty or thin one, a cut one) and a timeout after the upload; a refusal or a transport
# failure is not paid. The v5.1.1 lines carry no kind: every attempt is counted.
PAID_RUNG_KINDS = ("empty", "thin", "cut", "timeout")


def fold_paid(f: dict) -> bool:
    if f["kind"] == "rung_failed":
        return f["why"] in PAID_RUNG_KINDS
    return f["kind"] in ("ok", "failed", "no_summary")


def tally(keys) -> dict:
    out: dict = {}
    for k in keys:
        out[k] = out.get(k, 0) + 1
    return out


def fold_figures(log: Log) -> dict:
    oks = [f["wall"] for f in log.folds if f["kind"] == "ok"]
    paid = tally(day_of(f["wall"]) for f in log.folds if f["wall"] and fold_paid(f))
    return {
        "ok": len(oks),
        "last_ok": stamp(max((w for w in oks if w), default=None)),
        "failures": dict(sorted(tally(f["kind"] for f in log.folds if f["kind"] != "ok").items())),
        "lines_after_last_fold_line": (log.lines - log.last_fold_line) if log.last_fold_line else None,
        "attempts_per_day_max": max(paid.values(), default=0),
        "blob_save_ms": spread([b["ms"] for b in log.fold_blob]),
        "blob_bytes_max": max((b["bytes"] for b in log.fold_blob), default=None),
        "blob_not_saved": count(log.fold_blob, lambda b: not b["ok"]),
    }


def ms_per_kb(ms: int, body: int) -> float:
    return round(ms / (body / 1024.0), 1)


def upload_figures(log: Log) -> dict:
    ht = log.heaptraces
    timed = [h for h in ht if h["wms"] is not None and h["body"]]
    return {
        "requests": len(ht),
        "largest_body": max((h["body"] for h in ht), default=None),
        "largest_body_ok": max((h["body"] for h in ht if h["fail"] == "none"), default=None),
        "over_85000": count(ht, lambda h: h["body"] > 85000),
        "af_nonzero": count(ht, lambda h: h["af"] > 0),
        "failed": count(ht, lambda h: h["fail"] != "none"),
        "wms_per_kb": spread([ms_per_kb(h["wms"], h["body"]) for h in timed]),
        "pms": spread([h["pms"] for h in timed]),
        "up_lines": len(log.uploads),
        "up_ms_per_kb": spread([ms_per_kb(u["ms"], u["body"]) for u in log.uploads if u["body"]]),
        "up_over_6s": count(log.uploads, lambda u: u["ms"] > 6000),
    }


def report(log: Log, since: str = DEFAULT_CUT_SINCE) -> dict:
    rows = repo_price_rows()
    cache = cache_figures(log, rows)
    return {
        "window": {
            "first": stamp(log.first_wall),
            "last": stamp(log.last_wall),
            "days_with_logs": len(log.days),
            "lines": log.lines,
            "boots": len(log.boots),
            "firmware": sorted(log.fw_first, key=fw_key),
            "turns": len(log.turns),
            "turns_by_source": {
                s: sum(1 for t in log.turns if t.get("src") == s) for s in sorted({t.get("src") for t in log.turns})
            },
        },
        "history": history_figures(log, since),
        "cache": cache,
        "mistral_large": family_rollup(cache, "mistral/mistral-large"),
        "refits": refit_figures(log),
        "turn_fit": fit_figures(log),
        "capped": cap_figures(log),
        "xturn": xturn_figures(log),
        "fold": fold_figures(log),
        "upload": upload_figures(log),
    }


def baseline_view(r: dict) -> dict:
    ml = r["mistral_large"] or {}
    return {
        "owner_turns_cut": r["history"]["owner_turns_cut"],
        "owner_turns": r["history"]["owner_turns"],
        "large_turns": ml.get("turns"),
        "large_requests": ml.get("requests"),
        "large_cached_share_pct": ml.get("cached_share_pct"),
        "large_ceiling_pct": ml.get("in_turn_ceiling_pct"),
        "large_of_ceiling_pct": ml.get("of_ceiling_pct"),
        "refusals": r["refits"]["refusals"],
        "refits_after_round0": r["refits"]["large_refits_after_round0"],
        "refit_turns": r["refits"]["large_refit_turns"],
        "turn_fits": r["turn_fit"]["turn_fits"],
        "capped_finals": r["capped"]["capped_finals"],
        "xturn_hits": r["xturn"]["hits"],
        "xturn_turns": r["xturn"]["turns"],
        "recall_fits_freeing_nothing": r["turn_fit"]["recall_only_freed_nothing"],
        "recall_fits_measurable": r["turn_fit"]["recall_only_measurable"],
    }


def check_baseline(r: dict) -> list[str]:
    got = baseline_view(r)
    misses = []
    for k, want in BASELINE.items():
        if got.get(k) != want:
            misses.append(f"{k}: got {got.get(k)}, baseline {want}")
    return misses


# ---- text output ----------------------------------------------------------------------------


def text_report(r: dict) -> str:
    w, h, ml = r["window"], r["history"], r["mistral_large"]
    lines = [
        f"window {w['first']} .. {w['last']} ({w['days_with_logs']} days with logs, {w['lines']} lines, "
        f"{w['boots']} boots, firmware {', '.join(w['firmware'])})",
        f"turns {w['turns']} by source {w['turns_by_source']}",
        "",
        f"history: owner turns cut {h['owner_turns_cut']}/{h['owner_turns']} on {h['since_fw']} and later "
        f"({h['owner_turns_cut_before']}/{h['owner_turns_before']} before); left out per cut turn "
        f"{h['left_out_per_cut_turn']}; overflow shifts {h['overflow_shifts']}; unfinished turns {h['unfinished_turns']}",
    ]
    if "context_lines" in h:
        c = h["context_lines"]
        lines.append(
            f"context lines: {c['turns']} turns, today complete {c['today_complete_pct']}%, cut on "
            f"{c['turns_with_cut']}; rows sent {c['rows_sent']}; hist ms {c['hist_ms']}; card reads "
            f"{c['card_reads']}; lock ms {c['lock_ms']}; summary age h {c['summary_age_h']}"
        )
    lines += ["", "cache (turn spend slots):"]
    for key, c in r["cache"].items():
        lines.append(
            f"  {key}: turns {c['turns']} (owner {c['owner_turns']}), requests {c['requests']}, prompt {c['prompt']}, "
            f"cR {c['cached_read']} ({c['cached_share_pct']}%), ceiling {c['in_turn_ceiling_pct']}%, "
            f"of ceiling {c['of_ceiling_pct']}%, uncached/turn {c['uncached_per_turn']}, out {c['output']}, "
            f"USD {c['usd']}"
        )
    if ml:
        lines.append(
            f"  mistral-large (all ids): turns {ml['turns']}, requests {ml['requests']}, cached share "
            f"{ml['cached_share_pct']}%, in-turn ceiling {ml['in_turn_ceiling_pct']}%, of ceiling "
            f"{ml['of_ceiling_pct']}%, USD {ml['usd']}"
        )
    rf, tf, cp, x, f, u = r["refits"], r["turn_fit"], r["capped"], r["xturn"], r["fold"], r["upload"]
    lines += [
        "",
        f"refits: {rf['refusals']} over-budget refusals ({rf['at_round0']} at round 0, {rf['after_round0']} later; "
        f"budgets {rf['budgets']}); Large turns with a later refit {rf['large_refit_turns']} holding "
        f"{rf['large_refits_after_round0']} refits, cache shortfall {rf['large_refit_shortfall_tokens']} tokens",
        f"turn fit: {tf['turn_fits']} ({tf['first']} .. {tf['last']}), dropped {tf['dropped']}; recall-only fits "
        f"freeing nothing {tf['recall_only_freed_nothing']} of {tf['recall_only_measurable']} measurable "
        f"(body change {tf['recall_only_body_change']} B)",
        f"capped final rounds: {cp['capped_finals']} {cp['by_reason']}",
        f"cross-turn round-0 hits (Mistral): {x['hits']} of {x['turns']}; by idle {x['by_idle']}",
        f"next turn inside {NEXT_TURN_TTL_S} s, round-0 read share by host: {x['next_turn_by_host']}",
        f"fold: ok {f['ok']}, last ok {f['last_ok']}, failures {f['failures']}, lines after the last fold line "
        f"{f['lines_after_last_fold_line']}, most paid requests in a day {f['attempts_per_day_max']}, blob {f['blob_bytes_max']} B max, save ms "
        f"{f['blob_save_ms']}",
        f"upload: {u['requests']} requests, largest body {u['largest_body']} (ok {u['largest_body_ok']}), over 85,000 B "
        f"{u['over_85000']}, af>0 {u['af_nonzero']}, failed {u['failed']}, wms/KB {u['wms_per_kb']}, pms {u['pms']}, "
        f"'transport: up' {u['up_lines']} (over 6 s {u['up_over_6s']})",
    ]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*", help="saved log files, oldest first")
    ap.add_argument("--dir", help="a folder holding the nimbus.log rotation set (read oldest first)")
    ap.add_argument("--since", default=DEFAULT_CUT_SINCE, help="firmware from which owner turns are counted")
    ap.add_argument("--json", help="also write the full report as JSON to this path")
    ap.add_argument("--check-baseline", action="store_true", help="compare with the 15 Nov 2026 baseline")
    args = ap.parse_args(argv)
    paths = list(args.logs)
    if args.dir:
        paths = rotation_files(args.dir) + paths
    if not paths:
        ap.error("no log files (pass files or --dir)")
    r = report(parse(paths), args.since)
    print(text_report(r))
    if args.json:
        Path(args.json).write_text(json.dumps(r, indent=1))
    if args.check_baseline:
        got = baseline_view(r)
        misses = check_baseline(r)
        print("\nbaseline (15 Nov 2026):")
        for k, want in BASELINE.items():
            print(f"  {k}: {got.get(k)} (baseline {want}) {'match' if got.get(k) == want else 'MISS'}")
        print(f"BASELINE {'REPRODUCED' if not misses else 'MISSED'} ({len(BASELINE) - len(misses)}/{len(BASELINE)})")
        return 1 if misses else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
