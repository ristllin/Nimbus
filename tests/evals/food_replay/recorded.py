#!/usr/bin/env python3
"""Read the replies a food coach eval already recorded through a tree's checks, with no model calls.

Every run under an eval label holds what the model wrote and what the device added. This strips the device's
lines, rebuilds what the turn knew (the ledger as the run left it, the owner's words, the stored rules and
memory, the tool results) and asks the tree what it would add now. So a change to the checks is read against
every real reply of every model before anything is spent on new runs.

  recorded.py --after <tree> --label <dir with scenario/repN.json> [--label ...] [--out table.md]

Exit 1 when a run the labels hold would still carry a device line. Each one is listed with its reply, to be read:
a device line on a reply that was wrong is the check doing its job.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import replay

DEVICE_LINE = re.compile(r"\s*\((?:Correction|Note):[^)]*\)")
LEDGER = "food/food-log.csv"
# The lines these checks add. The driver holds no record of what a run's tools did, so with tool calls in the
# turn it cannot judge the others (a save claim, a lookup).
FOOD_LINES = (
    "no source for a daily target",
    "the food log has",
    "a row of today's food log",
    "has no protein figure",
    "cites a date",
)


def strip_device_lines(text: str) -> str:
    return DEVICE_LINE.sub("", text).strip()


def device_lines(text: str) -> list[str]:
    return [m.group(0).strip() for m in DEVICE_LINE.finditer(text)]


def sources_of(run: dict, turn: dict) -> str:
    """What a target may come from: the seeded rules, memory and files, and what the turn read. Not the ledger."""
    seed = run.get("seed", {})
    parts = [json.dumps(v) for k, v in seed.items() if k not in ("files", "skills")]
    parts += [f.get("text", "") for f in seed.get("files", []) if f.get("path") != LEDGER]
    parts += list(run.get("profile", {}).values()) if isinstance(run.get("profile"), dict) else []
    parts += [str(x) for x in turn.get("tool_results", [])]
    return " ".join(str(p) for p in parts)


RECEIPT_KCAL = re.compile(r"today's kcal \([^)]*\): ([\d.]+) from (\d+) rows?")
RECEIPT_PROTEIN = re.compile(r"today's protein_g: ([\d.]+) from \d+ rows?(?:, and (\d+) rows? ha)?")


def facts_after(facts: dict | None, turn: dict) -> tuple[dict | None, bool]:
    """The ledger as this turn left it, from the keyed write's own receipt. True when the turn changed the
    ledger and left no whole receipt (older runs carry no protein line, an edit carries none at all)."""
    stale = False
    wrote_ledger = any("food-log" in str(c) for c in turn.get("tool_calls", []))
    seen = False
    for out in turn.get("tool_results", []):
        kcal = RECEIPT_KCAL.search(str(out))
        if not kcal:
            continue
        seen = True
        facts = dict(facts or {}, kcal=float(kcal.group(1)), rows=int(kcal.group(2)), kcal_known=True)
        protein = RECEIPT_PROTEIN.search(str(out))
        if protein:
            facts.update(protein=float(protein.group(1)), protein_unknown=int(protein.group(2) or 0))
        else:
            stale = True
    return facts, stale or (wrote_ledger and not seen)


def seed_ledger(run: dict) -> str:
    for f in run.get("seed", {}).get("files", []):
        if f.get("path") == LEDGER:
            return f.get("text", "")
    return ""


def facts_from(res: dict) -> dict | None:
    if "kcal" not in res:
        return None
    return {
        "kcal": res["kcal"],
        "protein": res.get("protein", 0),
        "rows": res.get("rows", 0),
        "protein_unknown": res.get("protein_unknown", 0),
        "kcal_known": not res.get("unread"),
    }


def run_cases(binary: Path, label: Path) -> list[dict]:
    """One reply case per turn of every run under a label, with what the turn knew."""
    out = []
    for path in sorted(label.glob("*/rep*.json")):
        if not re.fullmatch(r"rep\d+\.json", path.name):
            continue
        rec = json.loads(path.read_text(encoding="utf-8"))
        if not rec.get("turns"):
            continue
        run = json.loads(path.with_name(path.stem + ".run.json").read_text(encoding="utf-8"))
        ledger = seed_ledger(run)
        rows = re.findall(r"^(\d{4}-\d{2}-\d{2}),", ledger, re.M)
        day = rows[-1] if rows else ""
        seeded = replay.run(binary, [{"id": "seed", "kind": "sum", "file": ledger, "day": day}])["seed"]
        facts = facts_from(seeded)
        owners = ""
        stale = False
        for n, turn in enumerate(rec["turns"]):
            owners += turn.get("owner", "") + "\n"
            facts, stale_now = facts_after(facts, turn)
            stale = stale or stale_now
            said = "\n".join(turn.get("deliveries") or [turn.get("reply", "")])
            case = {
                "id": f"{label.name}/{path.parent.name}/{path.stem}/{n}",
                "kind": "reply",
                "owner": owners,
                "reply": strip_device_lines(said),
                "sources": sources_of(run, turn),
                "was": device_lines(said),
                "wrote": bool(turn.get("tool_calls")),
                "stale_total": stale,
            }
            if facts:
                case["facts"] = facts
            out.append(case)
    return out


def run_label(binary: Path, label: Path) -> list[dict]:
    replies = run_cases(binary, label)
    res = replay.run(binary, replies)
    out = []
    for c in replies:
        now = device_lines(res[c["id"]].get("tail", ""))
        if c["wrote"]:
            now = [x for x in now if any(f in x for f in FOOD_LINES)]
        if c["stale_total"]:  # the ledger changed in a way this record holds no receipt for
            now = [x for x in now if "the food log has" not in x]
        out.append({"id": c["id"], "was": c["was"], "now": now, "reply": c["reply"]})
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--after", required=True)
    ap.add_argument("--label", action="append", required=True)
    ap.add_argument("--work", default="/tmp")
    ap.add_argument("--out")
    args = ap.parse_args()
    binary = replay.build(Path(args.after).resolve(), Path(args.work) / "replay_after")
    lines = ["| label | turns | runs with a device line, as delivered | with the tree's checks |", "|---|---|---|---|"]
    detail = []
    still = 0
    for name in args.label:
        rows = run_label(binary, Path(name))
        was = sum(1 for r in rows if r["was"])
        now = sum(1 for r in rows if r["now"])
        still += now
        lines.append(f"| {Path(name).name} | {len(rows)} | {was} | {now} |")
        detail += [f"- {r['id']}: {' '.join(r['now'])}\n  reply: {r['reply'][:600]!r}" for r in rows if r["now"]]
    text = "\n".join(lines + ["", "Lines the tree would still add:", ""] + detail) + "\n"
    print(text)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    return 1 if still else 0


if __name__ == "__main__":
    sys.exit(main())
