#!/usr/bin/env python3
"""run - drive the episodes through a head under tool choice auto and store one record per episode.

  run.py --head claude-haiku-5-5 --evidence <dir> --env-file <dotenv> [--max-usd 1.0] [--limit N]
         [--only kinds] [--parallel 6]

PAID: every episode is real provider turns. A record is written to <dir>/<head>/records.jsonl as soon
as its episode ends; an episode already stored is skipped, so a rerun continues. New episodes stop
starting once the head's spend in <dir>/spend.jsonl reaches --max-usd.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
import traceback
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import episodes  # noqa: E402
import manifest  # noqa: E402
import providers  # noqa: E402
from world import World  # noqa: E402

MAX_ROUNDS = 16  # PRD section 1: the engine's round guard
_lock = threading.Lock()


def stamp(t, variant: str = "") -> str:
    """The send time on every owner message; arms L1y and L1uy add the year."""
    return t.strftime("[%a %d %b %Y %H:%M] " if variant in ("L1y", "L1uy") else "[%a %d %b %H:%M] ")


# ------------------------------------------------------------------ seeding
def seed_world(ep: dict, variant: str) -> World:
    w = World(now=ep["now"], fail={}, variant=variant)
    w.macro_warn = {True: "force", False: "off"}.get(ep.get("warn_label"), "prd")
    s = ep.get("setup", {})
    for row in s.get("rows", []):
        w.rows.append(w._add(row))
    w.unpinned.update(s.get("unpinned", {}))
    w.files.update(s.get("files", {}))
    w.pages.update(s.get("pages", {}))
    w.history.extend(s.get("history", []))
    w.memories.extend(s.get("memories", []))
    return w


def ledger_call(variant: str, rows: list) -> tuple:
    if variant == "L3":
        return "ledger_add", {"rows": rows}
    return "ledger_write", {"ops": [{"op": "add", "row": r} for r in rows]}


def render_prior(ep: dict, variant: str, w: World) -> list:
    """The prior exchanges as neutral transcript items; prior ledger calls run on the world for real receipts."""
    items, n = [], 0
    for it in ep.get("prior", []):
        if it["t"] == "user":
            w.now = it["at"]
            w.history.append((it["at"], "owner", it["text"]))
            items.append({"t": "user", "text": stamp(it["at"], variant) + it["text"]})
        elif it["t"] == "ledger_add":
            name, args = ledger_call(variant, it["rows"])
            n += 1
            cid = f"toolu_seed{n:02d}"
            text, err = w.call(name, args)
            items += [
                {"t": "call", "id": cid, "name": name, "args": args},
                {"t": "result", "id": cid, "text": text, "err": err},
            ]
        else:
            items.append(dict(it))
    w.seed_ids = [r.id for r in w.rows]
    w.log.clear()
    return items


# ------------------------------------------------------------------ checks
def _rows(w, a):
    out = []
    for r in w.rows:
        if "item" in a and a["item"].lower() not in r.item.lower() and r.item.lower() not in a["item"].lower():
            continue
        if "item_re" in a and not re.search(a["item_re"], r.item, re.I):
            continue
        out.append(r)
    return out


def _rid(w, v):
    return w.seed_ids[int(v[1:])] if isinstance(v, str) and v.startswith("$") else v


def _ledger_has(w, a, reply):
    def fits(r):
        kcal_ok = "kcal" not in a or a["kcal"][0] - 0.5 <= r.kcal <= a["kcal"][1] + 0.5
        qty_ok = "qty" not in a or (r.qty is not None and abs(r.qty - a["qty"]) < 1e-6)
        return kcal_ok and qty_ok and r.day == a.get("day", w.today)

    rows = _rows(w, a)
    return any(fits(r) for r in rows) and ("count" not in a or len(rows) == a["count"])


def _row_field(w, a, reply):
    r = w.row(_rid(w, a["id"]))
    if not r:
        return False
    v = getattr(r, a["field"])
    return abs(v - a["value"]) < 1e-6 if isinstance(a["value"], (int, float)) else v == a["value"]


CHECKS = {
    "ledger_has": _ledger_has,
    "ledger_count": lambda w, a, reply: len(_rows(w, a)) == a["count"],
    "row_field": _row_field,
    "row_absent": lambda w, a, reply: w.row(_rid(w, a["id"])) is None,
    "note_has": lambda w, a, reply: any(
        re.search(a["value_re"], f"{k} {v}", re.I) for k, v in {**w.pinned, **w.unpinned}.items()
    ),
    "file_has": lambda w, a, reply: any(
        re.search(a["path_re"], p) and re.search(a["content_re"], c, re.I) for p, c in w.files.items()
    ),
    "file_absent": lambda w, a, reply: a["path"] not in w.files,
    "file_present": lambda w, a, reply: a["path"] in w.files,
    "reminder": lambda w, a, reply: any(
        r["when"].startswith(a["when"]) and re.search(a["text_re"], r["text"], re.I) for r in w.reminders
    ),
    "job": lambda w, a, reply: any(
        re.search(a["task_re"], j.get("task", "") + " " + j.get("name", ""), re.I) for j in w.jobs
    ),
    "fetched": lambda w, a, reply: a["url"] in w.fetched,
    "reply": lambda w, a, reply: bool(re.search(a["re"], reply, re.I)),
}


def check(w: World, kind: str, a: dict, reply: str) -> bool:
    """One expectation on the world after a turn (an unknown kind is a bug in the episode)."""
    return bool(CHECKS[kind](w, a, reply))


# ------------------------------------------------------------------ the loop
def run_turn(prov, system, tools, messages, w: World, cache_key: str) -> dict:
    """One owner turn: call the model until it answers without a tool call (or the round guard trips)."""
    calls, usage, steps, text, stop = [], [], 0, "", ""
    for _ in range(MAX_ROUNDS):
        st = prov.step(system, tools, messages, cache_key)
        steps += 1
        messages += st.items
        usage.append(st.usage)
        text, stop = st.text, st.stop
        if not st.calls:
            break
        res = []
        for cid, name, args in st.calls:
            out, err = w.call(name, args)
            res.append((cid, out, err))
            calls.append(
                {
                    "name": name,
                    "args": args,
                    "result": out[:1500],
                    "err": err,
                    "step": steps,
                    "text_before": st.text[:300],
                }
            )
        messages += prov.results(res)
    else:
        stop = "round_guard"
    return {"reply": text, "calls": calls, "steps": steps, "usage": usage, "stop": stop}


def run_episode(head: str, variant: str, ep: dict, recall_line: bool, prov=None) -> dict:
    prov = prov or providers.HEADS[head]()
    system = manifest.system_text(variant, head, recall=recall_line)
    tools = manifest.tools_for(variant)
    w = seed_world(ep, variant)
    neutral = render_prior(ep, variant, w)
    w.fail = {k: tuple(v) for k, v in ep.get("fail", {}).items()}
    messages = prov.render(neutral)
    rec = {
        "head": head,
        "variant": variant,
        "episode": ep["id"],
        "kind": ep["kind"],
        "sub": ep.get("sub"),
        "recall_line": recall_line,
        "fail": ep.get("fail", {}),
        "target": ep.get("target"),
        "warn_label": ep.get("warn_label"),
        "label_kcal": ep.get("label_kcal"),
        "turns": [],
        "seed_ids": w.seed_ids,
    }
    for turn in ep["turns"]:
        w.now = turn["at"]
        w.history.append((turn["at"], "owner", turn["text"]))
        messages += prov.user(stamp(turn["at"], variant) + turn["text"])
        t0 = time.time()
        out = run_turn(prov, system, tools, messages, w, f"rbm0a-{variant}-{int(recall_line)}")
        out["seconds"] = round(time.time() - t0, 2)
        out["owner"] = turn["text"]
        out["expect"] = [{"check": k, "args": a, "ok": check(w, k, a, out["reply"])} for k, a in turn["expect"]]
        out["usd"] = round(sum(providers.price(head, u) for u in out["usage"]), 6)
        out["rows_after"] = [f"{r.day} {r.line()}" for r in w.rows] if ep["kind"] in ("ledger", "warning") else []
        rec["turns"].append(out)
    rec["usd"] = round(sum(t["usd"] for t in rec["turns"]), 6)
    return rec


# ------------------------------------------------------------------ spend and storage
class Ledger:
    def __init__(self, path: Path):
        self.path = path

    def total(self, label_prefix="") -> float:
        if not self.path.exists():
            return 0.0
        with _lock:
            rows = [json.loads(x) for x in self.path.read_text().splitlines() if x.strip()]
        return sum(r["usd"] for r in rows if r["label"].startswith(label_prefix))

    def add(self, label: str, usd: float):
        with _lock, open(self.path, "a") as f:
            f.write(json.dumps({"t": round(time.time(), 1), "label": label, "usd": round(usd, 6)}) + "\n")


def key(variant, ep, recall_line):
    return f"{variant}|{ep['id']}|{int(recall_line)}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--head", required=True, choices=sorted(providers.HEADS))
    ap.add_argument("--evidence", required=True)
    ap.add_argument("--env-file", required=True)
    ap.add_argument("--max-usd", type=float, required=True, help="this head's cap in the spend ledger")
    ap.add_argument("--limit", type=int, default=0, help="at most N new episodes")
    ap.add_argument("--only", default="", help="comma list of kinds")
    ap.add_argument("--parallel", type=int, default=6)
    ap.add_argument("--scale", default="", help="n_ledger,n_dispatch,n_other,n_warn,n_recall")
    ap.add_argument(
        "--follow-up", default="", help="comma list of follow-up arms (L1u, L1y, L1uy): ledger episodes only"
    )
    args = ap.parse_args()
    providers.load_env(args.env_file)
    ev = Path(args.evidence)
    out_dir = ev / args.head
    out_dir.mkdir(parents=True, exist_ok=True)
    rec_path = out_dir / "records.jsonl"
    done = set()
    if rec_path.exists():
        for line in rec_path.read_text().splitlines():
            r = json.loads(line)
            done.add(f"{r['variant']}|{r['episode']}|{int(r['recall_line'])}")
    ledger = Ledger(ev / "spend.jsonl")
    scale = [int(x) for x in args.scale.split(",")] if args.scale else []
    if args.follow_up:
        blocks = episodes.plan_follow_up(arms=tuple(args.follow_up.split(",")))
    else:
        blocks = episodes.plan(*scale)
    todo = [b for b in blocks if key(*b) not in done]
    if args.only:
        kinds = set(args.only.split(","))
        todo = [b for b in todo if b[1]["kind"] in kinds]
    if args.limit:
        todo = todo[: args.limit]
    stats = {"ran": 0, "errors": 0, "stopped": False}

    def work(b):
        variant, ep, rl = b
        if ledger.total(f"head:{args.head}") >= args.max_usd:
            stats["stopped"] = True
            return
        try:
            rec = run_episode(args.head, variant, ep, rl)
        except Exception as e:  # a provider refusal or error is recorded, never retried silently
            with _lock:
                stats["errors"] += 1
                with open(out_dir / "errors.jsonl", "a") as f:
                    f.write(
                        json.dumps(
                            {
                                "key": key(*b),
                                "error": f"{type(e).__name__}: {str(e)[:600]}",
                                "trace": traceback.format_exc()[-1500:],
                            }
                        )
                        + "\n"
                    )
            return
        ledger.add(f"head:{args.head}", rec["usd"])
        with _lock:
            with open(rec_path, "a") as f:
                f.write(json.dumps(rec, ensure_ascii=False, default=str) + "\n")
            stats["ran"] += 1

    with ThreadPoolExecutor(args.parallel) as pool:
        list(pool.map(work, todo))
    print(
        json.dumps(
            {
                "head": args.head,
                **stats,
                "spend_head": round(ledger.total(f"head:{args.head}"), 4),
                "spend_all": round(ledger.total(), 4),
                "records": str(rec_path),
            }
        )
    )


if __name__ == "__main__":
    main()
