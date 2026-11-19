#!/usr/bin/env python3
"""run_replay - replay a day's shape through the emulated engine-2 transcript on a live head.

  run_replay.py --shape day-2026-11-04 --profile standard --evidence <dir> --env-file <dotenv>
                [--model claude-sonnet-5-5] [--replay-thinking] [--limit N] [--max-usd 25]

PAID. One run is one shape at one profile: the shape's owner turns (synth.py) at their real
gaps on a virtual clock, through tools/refloop/refloop.py E2Transcript (stub pointer, photo
retention, closed-turn thinking, resident bound with Haiku digests at idle), over the engine-2
tool world of tests/evals/behaviour (L1uy: the shapes ruling 11 binds). Every head request is
recorded with its usage, body bytes, real seconds and its virtual gap.

Pricing on the virtual clock (the provider's cache lives in real time; the replay does not wait):
a request is cold when it is a run's first or comes more than 300 s after the previous head
request ended; a cold request pays every cached token at the cache-write price. The same tokens
are also priced under the 1-hour write (writes at 2 x input, cold only after 3,600 s).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tests" / "evals" / "behaviour"))
sys.path.insert(0, str(REPO / "tools" / "refloop"))

import manifest  # noqa: E402
import providers  # noqa: E402
import synth  # noqa: E402
from refloop import PROFILES, E2Transcript  # noqa: E402
from world import World  # noqa: E402

TZ = ZoneInfo("Europe/London")
MAX_ROUNDS = 16
MAX_TOKENS = 16000
CACHE_5M_S, CACHE_1H_S = 300, 3600
WARM_GAP_S = 45  # the warm follow-up arm: owner turns this far apart (under the 270 s cold open)
IDLE_COMPACT_S = 120  # compaction runs only after this much quiet
SLICE_MAX = 24_000
SUMMARY_REQUESTS_PER_IDLE = 2  # plain_compact: kCompactRequestsPerWindow
COMPACT_WINDOW_S = 3600  # kCompactWindowS: a new idle window an hour
COMPACT_PAID_PER_DAY = 6  # kCompactPaidPerDay
SUMMARIZER = "claude-haiku-5-5"
VARIANT = "L1uy"
# model -> (input, output, cache read, 5-minute write, 1-hour write), USD per million tokens.
PRICES = {
    "claude-sonnet-5-5": (2.00, 10.00, 0.10, 2.50, 4.00),
    "claude-haiku-5-5": (0.10, 0.50, 0.01, 0.125, 0.20),
}
BETAS = ["thinking-binding-controls-2026-08-01"]
SUMMARY_ASK = (
    "Write a digest of the conversation below for the assistant's own later use: facts the owner "
    "stated with his exact numbers, decisions, what was asked and answered, and anything still open. "
    "Plain sentences, at most 700 characters. Do not call tools."
)
# Lumi's pinned notes run to about 4 KB; manifest's six lines are a few hundred bytes. Made-up
# lines bring the system text to the PRD's typical fixed overhead (system plus tools near 14,000 B).
EXTRA_NOTES = [
    f"note-{i:02d}: " + s
    for i, s in enumerate(
        [
            "prefers metric units and 24-hour times",
            "gym Monday, Wednesday, Friday mornings; easy run Saturday",
            "coffee: one cappuccino a day, oat milk at home, full milk out",
            "partner Alex is vegetarian; shared dinners are meat-free",
            "weekday lunches at the office canteen on Tue and Thu",
            "allergic to nothing; dislikes olives",
            "weight checked Sunday mornings only",
            "wants the day total after every logged meal",
            "bedtime target 23:00, wake 06:45",
            "half-marathon in March; long run builds 1 km a week",
            "beer at the weekend counts toward the day, 500 ml pint",
            "label figures beat estimates; say which one a row used",
        ]
        * 6
    )
]
RECENT_FILES = (
    "Recent files: food/ledger-2026-11.md (ledger), notes/training.md, notes/shopping.md, "
    "projects/kitchen/plan.md, inbox/receipts.md"
)

_lock = threading.Lock()


# ------------------------------------------------------------------ pricing
def price(model: str, u: dict, cold: bool, ttl: str = "5m") -> float:
    p_in, p_out, p_read, p_w5, p_w1 = PRICES[model]
    cached, written = u.get("cached", 0), u.get("written", 0)
    if cold:
        cached, written = 0, written + cached
    fresh = max(u.get("prompt", 0) - u.get("cached", 0) - u.get("written", 0), 0)
    p_write = p_w1 if ttl == "1h" else p_w5
    return (fresh * p_in + cached * p_read + written * p_write + u.get("output", 0) * p_out) / 1e6


def system_blocks(model: str, tx: E2Transcript) -> list:
    core = manifest.system_text(VARIANT, model) + "\n" + "\n".join(EXTRA_NOTES)
    return [
        {"type": "text", "text": core, "cache_control": {"type": "ephemeral"}},
        {
            "type": "text",
            "text": (tx.digest_block() + "\n\n" + RECENT_FILES).strip(),
            "cache_control": {"type": "ephemeral"},
        },
    ]


# ------------------------------------------------------------------ head
class Head:
    def __init__(self, model: str):
        import anthropic

        self.client = anthropic.Anthropic(api_key=os.environ["ANTHROPIC_API_KEY"], max_retries=6, timeout=300)
        self.model = model

    def step(self, system: list, tools: list, messages: list) -> dict:
        t0 = time.time()
        r = self.client.beta.messages.create(
            model=self.model,
            max_tokens=MAX_TOKENS,
            betas=BETAS,
            thinking={"type": "adaptive", "block_binding": {"prefix_mismatch_behavior": "drop_block"}},
            output_config={"effort": "medium"},
            cache_control={"type": "ephemeral"},
            system=system,
            tools=[{"name": t["name"], "description": t["description"], "input_schema": t["schema"]} for t in tools],
            tool_choice={"type": "auto"},
            messages=messages,
        )
        d = r.to_dict()
        u = d["usage"]
        cached, written = u.get("cache_read_input_tokens") or 0, u.get("cache_creation_input_tokens") or 0
        return {
            "content": d["content"],
            "calls": [(b["id"], b["name"], b["input"]) for b in d["content"] if b["type"] == "tool_use"],
            "text": "".join(b.get("text", "") for b in d["content"] if b["type"] == "text"),
            "stop": d.get("stop_reason"),
            "usage": {
                "prompt": u["input_tokens"] + cached + written,
                "cached": cached,
                "written": written,
                "output": u["output_tokens"],
                "thinking": (u.get("output_tokens_details") or {}).get("thinking_tokens", 0),
            },
            "dropped_thinking": sum(
                1 for x in d.get("input_transformations") or [] if x.get("type") == "thinking_dropped"
            ),
            "s": round(time.time() - t0, 2),
        }

    def summarize(self, text: str) -> tuple:
        r = self.client.messages.create(
            model=SUMMARIZER,
            max_tokens=400,
            thinking={"type": "disabled"},
            output_config={"effort": "medium"},
            messages=[{"role": "user", "content": SUMMARY_ASK + "\n\n" + text[:SLICE_MAX]}],
        )
        u = r.usage
        usage = {"prompt": u.input_tokens, "cached": 0, "written": 0, "output": u.output_tokens}
        return "".join(b.text for b in r.content if b.type == "text"), usage, r.stop_reason


# ------------------------------------------------------------------ world
def page_text(query: str) -> str:
    """A made-up search result page of a few KB: outside content is bulk in engine 2."""
    para = (
        f"Results for {query}. Sources disagree in places; figures vary by preparation and brand. "
        "Typical values are listed with ranges, and a note on how they were measured follows each one. "
    )
    return "[outside content] " + " ".join(f"{n}. {para}" for n in range(1, 30))


def call_tool(w: World, tx: E2Transcript, name: str, args) -> tuple:
    if name == "view":
        return tx.view((args or {}).get("id", ""))
    out, err = w.call(name, args)
    if name == "web_search" and not err:
        out = out + "\n" + page_text(str((args or {}).get("query", "")))
    return out, err


def check(w: World, chk, reply: str, rows_before: int) -> bool | None:
    if not chk:
        return None
    kind, arg = chk
    if kind == "rows_added":
        return len(w.rows) - rows_before >= arg
    if kind == "reply":
        return bool(re.search(arg, reply, re.I | re.S))
    if kind == "total":
        kcal = sum(r.kcal for r in w.rows if r.day == w.today)
        nums = [float(x.replace(",", "")) for x in re.findall(r"\d[\d,]*(?:\.\d+)?", reply)]
        return any(abs(n - kcal) <= max(15, 0.03 * kcal) for n in nums)
    raise ValueError(kind)


# ------------------------------------------------------------------ run
def epoch_of(local: str) -> float:
    return datetime.strptime(local, "%Y-%m-%d %H:%M:%S").replace(tzinfo=TZ).timestamp()


def idle_requests(gap_s: float, paid_today: int) -> int:
    """Summarizer requests an idle gap allows, as the engine's driver paces them: one window per hour
    of quiet after IDLE_COMPACT_S, SUMMARY_REQUESTS_PER_IDLE each, at most COMPACT_PAID_PER_DAY a day."""
    if gap_s < IDLE_COMPACT_S:
        return 0
    windows = 1 + int((gap_s - IDLE_COMPACT_S) // COMPACT_WINDOW_S)
    return max(0, min(windows * SUMMARY_REQUESTS_PER_IDLE, COMPACT_PAID_PER_DAY - paid_today))


def compact(head: Head, tx: E2Transcript, now: float, rec: dict, requests: int = SUMMARY_REQUESTS_PER_IDLE):
    """Idle compaction: at most `requests` summarizer requests, one per local day of the slice."""
    turns = tx.compaction_slice(now)
    if not turns or requests <= 0:
        return
    by_day = tx.slice_text(turns)
    days = list(by_day)[:requests]
    digests = []
    for day in days:  # oldest first; each day is stored on its own, as the engine stores each slice
        text, usage, stop = head.summarize(by_day[day])
        rec["background"].append({"day": day, "usage": usage, "stop": stop, "usd": price(SUMMARIZER, usage, False)})
        if stop == "max_tokens" or len(text.encode()) < 120:
            break  # not accepted: this day and the ones after it stay
        digests.append((day, text))
    if not digests:
        return
    kept = [d for d, _ in digests]
    done_turns = [t for t in turns if any(d in tx.slice_text([t]) for d in kept)]
    tx.apply_digests(done_turns, digests)
    rec["compactions"].append(
        {
            "at": now,
            "turns": len(done_turns),
            "digests": len(digests),
            "resident": tx.resident(),
            "before_turn": len(rec["turns"]) + 1,
        }
    )


def warm_shape(shape: dict, n: int, gap_s: int = WARM_GAP_S) -> dict:
    """The first n turns of a shape back to back: every follow-up opens warm."""
    turns = [dict(t, at_s=i * gap_s) for i, t in enumerate(shape["turns"][:n])]
    return dict(shape, turns=turns)


class Replay:
    """One shape at one profile through the emulated engine-2 transcript. cfg: the parsed arguments
    (shape, profile, model, replay_thinking, limit, warm, aged)."""

    def __init__(self, cfg, out: Path):
        shape = synth.load_shape(cfg.shape)
        if cfg.warm:
            shape = warm_shape(shape, cfg.warm)
        self.cfg, self.out, self.model = cfg, out, cfg.model
        self.turns = synth.turns_for(shape, seed=cfg.shape)[: cfg.limit or None]
        self.start = epoch_of(shape["first_local"])
        self.head = Head(cfg.model)
        self.tx = E2Transcript(cfg.profile, replay_thinking=cfg.replay_thinking)
        if cfg.aged:
            # Steady state: resident halfway between L and H, the digest block full.
            h, low, _ = PROFILES[cfg.profile]
            history = synth.prior_history(self.start, (h + low) // 2, TZ, day_turns=getattr(cfg, "aged_day_turns", 10))
            self.tx.seed(history, synth.prior_digests(self.start, 30, TZ))
        self.w = World(now=datetime.fromtimestamp(self.start, TZ), fail={}, variant=VARIANT)
        self.tools = manifest.tools_for(VARIANT)
        self.vnow = self.start
        self.paid = {}  # summarizer requests paid per local day (the engine's daily cap)
        self.rec = {
            "shape": cfg.shape,
            "profile": cfg.profile,
            "model": cfg.model,
            "replay_thinking": cfg.replay_thinking,
            "warm": cfg.warm,
            "aged": cfg.aged,
            "turns": [],
            "requests": [],
            "background": [],
            "compactions": [],
        }

    def run(self) -> dict:
        for n, t in enumerate(self.turns):
            at = self.start + t["at_s"]
            if n and at - self.vnow >= IDLE_COMPACT_S:
                day = datetime.fromtimestamp(self.vnow, TZ).strftime("%Y-%m-%d")
                asked = len(self.rec["background"])
                compact(
                    self.head,
                    self.tx,
                    self.vnow + IDLE_COMPACT_S,
                    self.rec,
                    idle_requests(at - self.vnow, self.paid.get(day, 0)),
                )
                self.paid[day] = self.paid.get(day, 0) + len(self.rec["background"]) - asked
            self.vnow = max(self.vnow, at)
            self.rec["turns"].append(self.turn(n + 1, t))
            self.save()
        self.rec["changes"] = self.tx.changes
        self.rec["final_rows"] = len(self.w.rows)
        self.save()
        return self.rec

    def save(self):
        self.out.write_text(json.dumps(self.rec, indent=1))

    def turn(self, n: int, t: dict) -> dict:
        w, tx = self.w, self.tx
        w.now = datetime.fromtimestamp(self.vnow, TZ)
        w.history.append((w.now, "owner", t["text"]))
        photos = [synth.photo(k, i) for k, i in t["photos"]]
        opened = tx.open_turn(self.vnow, w.now.strftime("[%a %d %b %Y %H:%M] ") + t["text"], photos)
        rows_before = len(w.rows)
        rec = {"n": n, "kind": t["kind"], "photos": len(photos), "device_tools": t["device_tools"], **opened}
        calls, reply, real0 = [], "", time.time()
        for rnd in range(1, MAX_ROUNDS + 1):
            st = self.request(n, rnd)
            reply = st["text"]
            if not st["calls"]:
                break
            calls += self.dispatch(st["calls"])
        else:
            rec["round_guard"] = True
        w.history.append((datetime.fromtimestamp(self.vnow, TZ), "assistant", reply))
        reqs = [r for r in self.rec["requests"] if r["turn"] == n]
        rec.update(
            {
                "rounds": len(reqs),
                "tools": calls,
                "seconds": round(time.time() - real0, 2),
                "check": check(w, t["check"], reply, rows_before),
                "reply": reply[:400],
                **{k: round(sum(r[k] for r in reqs), 6) for k in ("usd_5m", "usd_1h", "usd_billed")},
                "first_cached_share": round(reqs[0]["usage"]["cached"] / max(reqs[0]["usage"]["prompt"], 1), 4),
                "resident": tx.resident(),
                "digests": len(tx.digests),
            }
        )
        return rec

    def request(self, n: int, rnd: int) -> dict:
        """One head request: rendered, sent, appended, recorded with its price on the virtual clock."""
        tx = self.tx
        system, messages = system_blocks(self.model, tx), tx.render_anthropic()
        body = len(json.dumps({"system": system, "tools": self.tools, "messages": messages}).encode())
        gap = None if tx.last_req_end is None else self.vnow - tx.last_req_end
        cold, cold_1h = gap is None or gap > CACHE_5M_S, gap is None or gap > CACHE_1H_S
        st = self.head.step(system, self.tools, messages)
        self.vnow += st["s"]
        tx.mark_request(self.vnow)
        tx.add_assistant(st["content"])
        u = st["usage"]
        tx.note_prompt(u["prompt"])
        self.rec["requests"].append(
            {
                "turn": n,
                "round": rnd,
                "usage": u,
                "body_bytes": body,
                "s": st["s"],
                "gap_s": None if gap is None else round(gap, 1),
                "cold": cold,
                "usd_5m": price(self.model, u, cold),
                "usd_1h": price(self.model, u, cold_1h, "1h"),
                "usd_billed": price(self.model, u, False),
                "dropped_thinking": st["dropped_thinking"],
                "stop": st["stop"],
            }
        )
        return st

    def dispatch(self, calls: list) -> list:
        res = [(cid, name, args, *call_tool(self.w, self.tx, name, args)) for cid, name, args in calls]
        self.tx.add_results(res, self.vnow)
        return [name for _, name, _ in calls]


def spent(evidence: Path) -> float:
    total = 0.0
    for p in evidence.glob("**/run-*.json"):
        r = json.loads(p.read_text())
        total += sum(q["usd_billed"] for q in r["requests"]) + sum(b["usd"] for b in r["background"])
    return total


def tag_of(a) -> str:
    tag = f"{a.shape}-{a.profile}"
    if a.replay_thinking:
        tag += "-thinking"
    if a.warm:
        tag += f"-warm{a.warm}-rep{a.rep}"
    return tag + ("-aged" if a.aged else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--shape", required=True)
    ap.add_argument("--profile", default="standard", choices=["tight", "lean", "standard", "wide", "full"])
    ap.add_argument("--model", default="claude-sonnet-5-5")
    ap.add_argument("--replay-thinking", action="store_true")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--warm", type=int, default=0, help="the first N turns back to back (the thinking arm)")
    ap.add_argument("--rep", type=int, default=1)
    ap.add_argument("--aged", action="store_true", help="start from steady state: (H + L) / 2 resident, digests full")
    ap.add_argument("--aged-day-turns", type=int, default=10, help="turns a seeded earlier day carries (its size)")
    ap.add_argument("--evidence", required=True, type=Path)
    ap.add_argument("--env-file", required=True)
    ap.add_argument("--max-usd", type=float, default=12.0, help="no new run once the evidence dir has billed this")
    a = ap.parse_args()
    providers.load_env(a.env_file)
    a.evidence.mkdir(parents=True, exist_ok=True)
    if spent(a.evidence) >= a.max_usd:
        raise SystemExit(f"HELD: the evidence dir's spend reached {a.max_usd} USD")
    tag = tag_of(a)
    try:
        rec = Replay(a, a.evidence / f"run-{tag}.json").run()
    except Exception as e:  # report and keep what was stored
        print(f"FAIL {tag}: {providers.redact(f'{type(e).__name__}: {e}')[:400]}")
        raise SystemExit(1)
    usd = sum(q["usd_billed"] for q in rec["requests"])
    print(f"done {tag}: {len(rec['turns'])} turns, {len(rec['requests'])} requests, billed {usd:.3f} USD")


if __name__ == "__main__":
    main()
