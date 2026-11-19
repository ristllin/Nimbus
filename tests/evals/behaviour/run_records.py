#!/usr/bin/env python3
"""run_records - the general-records arms (lane RB-REC6): drive episodes through a head and store one
record per episode, with the device's own file before and after.

  run_records.py --head claude-haiku-5-5 --arm S --evidence <dir> --env-file <dotenv> --aidserve <bin>
                 --work <dir> --max-usd 1.0 [--totals N] [--writes N] [--faults N] [--skills N] [--parallel 6]

PAID unless --scripted. Every file tool, calc and `more` answer, the system text and the tools array
come from the real engine-2 code through aidserve (records.py names the arms). A record is written to
<dir>/<head>/<arm>.jsonl as soon as its episode ends; a stored episode is skipped, so a rerun continues.
New episodes stop starting once this head's spend in <dir>/spend.jsonl reaches --max-usd.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import threading
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import manifest  # noqa: E402
import providers  # noqa: E402
import records  # noqa: E402
from run import Ledger  # noqa: E402

MAX_ROUNDS = 16
NO_MEMORY = "not enough memory for this call; nothing changed"  # aids::kAidNoMemory
WRITES = {"append_file", "write_file", "edit_file"}
AID_TOOLS = {
    "read_file",
    "write_file",
    "append_file",
    "edit_file",
    "list_files",
    "calc",
    "more",
}
F_ARGS = {
    "read_file": ["path", "offset", "limit"],
    "edit_file": ["path", "old", "new", "all"],
}
_lock = threading.Lock()


class Aid:
    """One aidserve process over one episode's directory."""

    def __init__(self, binary: str, root: Path):
        self.root = root
        if root.exists():
            shutil.rmtree(root)
        root.mkdir(parents=True)
        self.p = subprocess.Popen(
            [binary, str(root)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )

    def q(self, obj: dict) -> dict:
        self.p.stdin.write(json.dumps(obj) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("aidserve stopped")
        return json.loads(line)

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()
        self.p.stdout.close()
        shutil.rmtree(self.root, ignore_errors=True)


tool_name = records.tool_name


class World:
    """The device for one episode: the real file tools, faults, and small stand-ins for the rest."""

    def __init__(self, aid: Aid, arm: str, fault: str):
        self.aid, self.arm, self.fault = aid, arm, fault
        self.fault_hits = 0
        self.notes: dict = {}
        self.reminders: list = []

    def call(self, name: str, args) -> tuple:
        if not isinstance(args, dict):
            return f"{name}: arguments must be an object", True
        target = tool_name(name, args)
        targs = args.get("args") if name == "more" and isinstance(args.get("args"), dict) else args
        refused = self._arm_f_refusal(target, targs)
        if refused:
            return refused, True
        if self._faulted(target, targs):
            return self._refuse(name, target, args)
        if name in AID_TOOLS:
            return self._aid(name, target, args)
        if name == "note":
            return self._note(args)
        if name == "search":
            return "0 results", False
        if name == "schedule":
            return self._schedule(args)
        return f"{name}: done", False

    def _arm_f_refusal(self, target: str, targs: dict) -> str:
        """Arm F declares no column/from/to/sum and no line: the registry's refusal, naming F's arguments."""
        if self.arm != "F" or target not in F_ARGS:
            return ""
        extra = sorted(k for k in targs if k not in F_ARGS[target])
        if not extra:
            return ""
        acc, bad = ", ".join(F_ARGS[target]), ", ".join(extra)
        return f"{target} takes {acc}; not read: {bad}\n(not read: {bad}: this tool has no such argument)"

    def _faulted(self, target: str, targs: dict) -> bool:
        hit = self.fault and (target in WRITES or (target == "read_file" and targs.get("sum")))
        return bool(hit and (self.fault == "always" or self.fault_hits == 0))

    def _refuse(self, name: str, target: str, args: dict) -> tuple:
        self.fault_hits += 1
        if target in (
            "edit_file",
            "read_file",
        ):  # the real refusal: the call under a refusing allocator
            r = self.aid.q({"op": "call", "tool": name, "args": args, "refuse": True})
            return r["text"], not r["ok"]
        return (
            NO_MEMORY,
            True,
        )  # append and write take no working block: the core's stated refusal

    def _aid(self, name: str, target: str, args: dict) -> tuple:
        r = self.aid.q({"op": "call", "tool": name, "args": args})
        text = r["text"]
        if self.arm == "F" and target == "edit_file" and not r["ok"]:
            text = text.replace("; line picks one, all=true replaces all", "; all=true replaces all")
        return text, not r["ok"]

    def _note(self, args: dict) -> tuple:
        key, val = str(args.get("key", "")), str(args.get("value", ""))
        if args.get("op") == "remove":
            self.notes.pop(key, None)
            return f"removed note {key}", False
        self.notes[key] = val
        return f"stored note {key}: {val[:300]} (pinned {bool(args.get('pin'))})", False

    def _schedule(self, args: dict) -> tuple:
        """This device has no reminders or routines until one is created."""
        op = args.get("op")
        if op == "create":
            self.reminders.append(dict(args))
            return (
                f"stored {args.get('kind', 'reminder')} s{len(self.reminders)} at {args.get('when', '')}",
                False,
            )
        if op == "list" and not self.reminders:
            return "no reminders or routines", False
        if op == "list":
            rows = (f"s{k} {r.get('when', '')} {r.get('text', '')}" for k, r in enumerate(self.reminders, 1))
            return "\n".join(rows), False
        return f"no reminder or routine with id {args.get('id', '')}", True


# ------------------------------------------------------------------ requests
def compose(aid: Aid, ep: dict, arm: str, head: str) -> tuple:
    dom = ep["dom"]
    notes = records.note_lines(dom) if arm in ("F", "S") else records.note_lines(dom)[:2]
    skills = ""
    sid = records.DOMAINS[dom]["skill_id"]
    if arm == "K1":
        skills = f"skills/{sid}.md: {records.skill_body(dom).split('desc: ')[1].split(chr(10))[0]}\n"
    c = aid.q(
        {
            "op": "compose",
            "notes": "\n".join(notes),
            "skills": skills,
            "tz": "Europe/London",
            "model": head,
        }
    )
    system = c["core"] + ("\n\n" + c["notes"] if c["notes"] else "")
    tools = []
    for t in c["tools"]:
        if arm == "F" and t["name"] in F_ARGS:
            tools.append(manifest.CORE[t["name"]])
        else:
            tools.append(
                {
                    "name": t["name"],
                    "description": t["description"],
                    "schema": json.loads(t["schema"]),
                }
            )
    return system, tools


def attach_block(aid: Aid, ep: dict, arm: str) -> str:
    if arm not in ("K2", "K3"):
        return ""
    dom = ep["dom"]
    skills = [
        {
            "id": records.DOMAINS[dom]["skill_id"],
            "rev": 3,
            "body": records.skill_body(dom),
        }
    ]
    if arm == "K3":
        other, body = records.other_skill_body(dom)
        skills.append({"id": records.DOMAINS[other]["skill_id"], "rev": 3, "body": body})
    return aid.q({"op": "attach", "skills": skills})["text"]


def seed(aid: Aid, ep: dict, arm: str):
    dom = ep["dom"]
    aid.q(
        {
            "op": "seed",
            "path": records.DOMAINS[dom]["path"],
            "bytes": ep["table"].text(),
        }
    )
    if arm in ("K1", "K2", "K3"):
        aid.q(
            {
                "op": "skill",
                "id": records.DOMAINS[dom]["skill_id"],
                "body": records.skill_body(dom),
            }
        )
    if arm == "K3":
        other, body = records.other_skill_body(dom)
        aid.q({"op": "skill", "id": records.DOMAINS[other]["skill_id"], "body": body})


def run_turn(prov, system, tools, messages, w: World, cache_key: str) -> dict:
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
                    "result": out[:2500],
                    "err": err,
                    "step": steps,
                }
            )
        messages += prov.results(res)
    else:
        stop = "round_guard"
    return {"reply": text, "calls": calls, "steps": steps, "usage": usage, "stop": stop}


def first_write(calls: list) -> dict | None:
    for c in calls:
        if tool_name(c["name"], c["args"]) in WRITES:
            return c
    return None


def prior_items(aid: Aid, ep: dict) -> list:
    """The prior exchange as neutral items; its append runs on the device for a real receipt."""
    neutral = []
    path = records.DOMAINS[ep["dom"]]["path"]
    for n, pr in enumerate(ep.get("prior", [])):
        args = {"path": path, "text": pr["append"]}
        r = aid.q({"op": "call", "tool": "append_file", "args": args})
        cid = f"toolu_seed{n:02d}"
        neutral += [
            {"t": "user", "text": records.stamp(pr["at"]) + pr["text"]},
            {"t": "call", "id": cid, "name": "append_file", "args": args},
            {"t": "result", "id": cid, "text": r["text"], "err": not r["ok"]},
            {"t": "atext", "text": "Logged."},
        ]
    return neutral


def used_sum(calls: list) -> bool:
    for c in calls:
        a = c["args"] if isinstance(c["args"], dict) else {}
        if tool_name(c["name"], a) == "read_file" and (a.get("sum") or (a.get("args") or {}).get("sum")):
            return True
    return False


def finish(rec: dict, ep: dict, before: str, after: str):
    """The facts a report needs beside the raw turns (the report applies the checks again)."""
    last = rec["turns"][-1]
    if ep["kind"] == "total":
        rec["truth"] = ep["truth"]
        rec["total"] = records.judge_total(last["reply"], ep["truth"])
        rec["used_sum"] = used_sum(last["calls"])
        rec["used_calc"] = any(tool_name(c["name"], c["args"]) == "calc" for c in last["calls"])
    else:
        rec["expect"] = ep["expect"]
        rec["file"] = records.judge_write(ep, before, after)
        fw = first_write(last["calls"])
        rec["first_write"] = None if fw is None else {"name": tool_name(fw["name"], fw["args"]), "ok": not fw["err"]}
        rec["before"], rec["after"] = before, after
    if rec["arm"].startswith("K"):
        rec["pickup"] = records.pickup(rec["turns"][0]["calls"], ep["dom"])


def run_episode(head: str, arm: str, ep: dict, aid_bin: str, work: Path, prov=None) -> dict:
    prov = prov or providers.HEADS[head]()
    aid = Aid(aid_bin, work / f"{head}-{arm}-{ep['id']}")
    try:
        system, tools = compose(aid, ep, arm, head)
        seed(aid, ep, arm)
        w = World(aid, arm, ep.get("fault", ""))
        messages = prov.render(prior_items(aid, ep))
        path = records.DOMAINS[ep["dom"]]["path"]
        before = aid.q({"op": "cat", "path": path})["bytes"]
        block = attach_block(aid, ep, arm)
        rec = {
            "head": head,
            "arm": arm,
            "episode": ep["id"],
            "kind": ep["kind"],
            "sub": ep["sub"],
            "dom": ep["dom"],
            "fault": ep.get("fault", ""),
            "turns": [],
            "attach_bytes": len(block),
        }
        for turn in ep["turns"]:
            text = (block + "\n" if block else "") + records.stamp(turn["at"]) + turn["text"]
            messages += prov.user(text)
            t0 = time.time()
            out = run_turn(prov, system, tools, messages, w, f"rbrec6-{arm}-{ep['dom']}")
            out["seconds"] = round(time.time() - t0, 2)
            out["owner"] = turn["text"]
            out["usd"] = round(sum(providers.price(head, u) for u in out["usage"]), 6)
            rec["turns"].append(out)
        rec["usd"] = round(sum(t["usd"] for t in rec["turns"]), 6)
        rec["system_bytes"] = len(system.encode())
        finish(rec, ep, before, aid.q({"op": "cat", "path": path})["bytes"])
        return rec
    finally:
        aid.close()


# ------------------------------------------------------------------ plan and storage
def cell_plan(arm: str, n_total: int, n_write: int, n_fault: int, n_skill: int) -> list:
    if arm in ("F", "S"):
        return records.plan(n_total, n_write, n_fault if arm == "S" else 0)
    return records.plan_skills(arm, n_skill)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--head", required=True)
    ap.add_argument("--arm", required=True, choices=["F", "S", "K0", "K1", "K2", "K3"])
    ap.add_argument("--evidence", required=True)
    ap.add_argument("--env-file", default="")
    ap.add_argument("--aidserve", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument(
        "--max-usd",
        type=float,
        required=True,
        help="this head's cap in the spend ledger",
    )
    ap.add_argument("--totals", type=int, default=0)
    ap.add_argument("--writes", type=int, default=0)
    ap.add_argument("--faults", type=int, default=0)
    ap.add_argument("--skills", type=int, default=0)
    ap.add_argument("--parallel", type=int, default=6)
    ap.add_argument("--scripted", default="", help="a scripted head (offline): good | sloppy")
    args = ap.parse_args()
    if args.scripted:
        import scripted_heads

        make = lambda: scripted_heads.HEADS[args.scripted]()  # noqa: E731
        head = f"scripted-{args.scripted}"
    else:
        providers.load_env(args.env_file)
        make = providers.HEADS[args.head]
        head = args.head
    ev = Path(args.evidence)
    out_dir = ev / head
    out_dir.mkdir(parents=True, exist_ok=True)
    rec_path = out_dir / f"{args.arm}.jsonl"
    done = set()
    if rec_path.exists():
        done = {json.loads(x)["episode"] for x in rec_path.read_text().splitlines() if x.strip()}
    ledger = Ledger(ev / "spend.jsonl")
    todo = [e for e in cell_plan(args.arm, args.totals, args.writes, args.faults, args.skills) if e["id"] not in done]
    stats = {"ran": 0, "errors": 0, "stopped": False}

    def work(ep):
        if not args.scripted and ledger.total(f"head:{head}") >= args.max_usd:
            stats["stopped"] = True
            return
        try:
            rec = run_episode(head, args.arm, ep, args.aidserve, Path(args.work), prov=make())
        except Exception as e:
            with _lock:
                stats["errors"] += 1
                with open(out_dir / "errors.jsonl", "a") as f:
                    f.write(
                        json.dumps(
                            {
                                "arm": args.arm,
                                "episode": ep["id"],
                                "error": providers.redact(f"{type(e).__name__}: {str(e)[:600]}"),
                                "trace": providers.redact(traceback.format_exc()[-1500:]),
                            }
                        )
                        + "\n"
                    )
            return
        if not args.scripted:
            ledger.add(f"head:{head}", rec["usd"])
        with _lock:
            with open(rec_path, "a") as f:
                f.write(json.dumps(rec, ensure_ascii=False, default=str) + "\n")
            stats["ran"] += 1

    from concurrent.futures import ThreadPoolExecutor

    with ThreadPoolExecutor(args.parallel) as pool:
        list(pool.map(work, todo))
    print(
        json.dumps(
            {
                "head": head,
                "arm": args.arm,
                **stats,
                "spend_head": round(ledger.total(f"head:{head}"), 4),
                "spend_all": round(ledger.total(), 4),
                "records": str(rec_path),
            }
        )
    )


if __name__ == "__main__":
    main()
