#!/usr/bin/env python3
"""mechanisms - count known harness behaviors in arm A's captured requests.

Reads <evidence>/<model>/<scenario>/repN/A/{requests.jsonl.gz,turns.json} and counts, per
model and scenario, the things the harness does to a turn that the reference loop does not:

  finalize_nudge     a second request only to wrap a prose answer in orch_turn ("Now call orch_turn ...")
  discarded_answers  a prose answer the harness threw away: the round is asked again, not wrapped
  final_notice       a request whose per-turn block was rewritten to say "This is your FINAL turn ... your tools
                     are no longer available" (the tool-round budget ran out, or an answer is being wrapped)
  forced_final       a round sent with tool_choice forced to orch_turn (OpenAI wire: the round cap was reached)
  round caps told    the tool-round cap the turn's context states ("at most N"; 0 = no such line)
  truncated_results  tool results cut on the wire (the harness's "[truncated]" mark)
  bounced_final      an orch_turn the harness refused and sent back (reply check)
  correction_added   a "(Correction: ...)" the harness appended to the reply
  food_block         the food playbook attached to the turn's context
  earlier_tool_rows  tool calls or results of EARLIER turns present in a request (0 = none are replayed)
  per-turn block     size of the context block wrapped around the owner's message
  extra_lookups      (LOSSES.md) a turn where the harness made more read-only calls (files_read, files_list,
                     memory_search, memory_episodic, docs_search, image_look, ...) than the reference loop

  python3 tools/refloop/mechanisms.py --evidence <dir> [--json out.json]

It prints one table per model and writes <evidence>/MECHANISMS.md (the same tables) and
<evidence>/LOSSES.md: every repetition the harness lost, with the checks it failed, the judge's
reason, the mechanisms that fired (with turn numbers) and the evidence path.
"""

from __future__ import annotations

import argparse
import gzip
import json
import re
from collections import defaultdict
from pathlib import Path

HEAD_PATHS = ("/v1/messages", "/v1/responses", "/v1/chat/completions")
NUDGE = "Now call orch_turn with your final complete turn"
FINAL_NOTICE = "This is your FINAL turn for this request"
TRUNC = "[truncated]"
LOOKUPS = {
    "files_read",
    "files_list",
    "memory_search",
    "memory_episodic",
    "docs_search",
    "docs_read",
    "image_look",
    "skill_get",
    "skill_list",
    "loop_list",
    "device_status",
}
COUNTED = (
    "finalize_nudge",
    "discarded_answers",
    "final_notice",
    "forced_final",
    "truncated_results",
    "bounced_final",
    "correction_added",
    "food_block",
    "earlier_tool_rows",
)
SUMMED = ("dyn_chars", "requests", "tool_calls", *COUNTED)
# Scenarios where a food block belongs: everywhere else it is off topic.
FOOD_SCENARIOS = ("meal-log-day", "recent-compare-food", "burst-three")


# ------------------------------------------------------------- reading a request
def _block_texts(block: dict) -> list:
    out = [block["text"]] if isinstance(block.get("text"), str) else []
    if block.get("type") == "tool_result":
        inner = block.get("content")
        out.append(inner if isinstance(inner, str) else " ".join(b.get("text", "") for b in inner or []))
    return out


def _texts(item: dict) -> list:
    """Every text one request item carries (either wire)."""
    content = item.get("content")
    if isinstance(content, str):
        return [content]
    out = [t for block in content or [] if isinstance(block, dict) for t in _block_texts(block)]
    return [*out, item["output"]] if isinstance(item.get("output"), str) else out


def _items(request: dict) -> list:
    return request.get("messages") or (request.get("input") if isinstance(request.get("input"), list) else []) or []


def _tool_rows(item: dict) -> int:
    """Tool calls and tool results in one request item."""
    if item.get("type") in ("function_call", "function_call_output"):
        return 1
    content = item.get("content") if isinstance(item.get("content"), list) else []
    return sum(1 for b in content if isinstance(b, dict) and b.get("type") in ("tool_use", "tool_result"))


def _owner_block_index(items: list) -> int:
    """Index of the item holding this turn's owner message (the harness wraps it in a context block)."""
    for i in range(len(items) - 1, -1, -1):
        if items[i].get("role") == "user" and any("[USER]" in t or "Current time:" in t for t in _texts(items[i])):
            return i
    return -1


def _has_tool_call(response) -> bool:
    if not isinstance(response, dict):
        return False
    blocks = (response.get("content") or []) + (response.get("output") or [])
    return any(isinstance(b, dict) and b.get("type") in ("tool_use", "function_call") for b in blocks)


# ------------------------------------------------------------------ one turn
def _first_request_facts(items: list, at: int) -> dict:
    block = "\n".join(_texts(items[at]))
    cap = re.search(r"at most (\d+)\)", block)
    return {
        "dyn_chars": len(block),
        "round_cap": int(cap.group(1)) if cap else 0,
        "food_block": int("[FOOD PLAYBOOK" in block),
        "earlier_tool_rows": sum(_tool_rows(i) for i in items[:at]),
    }


def _request_flags(ex: dict, previous: dict | None) -> dict:
    """What one head request shows: a forced final, a nudge, the final notice, a discarded answer."""
    req = ex["request"] if isinstance(ex["request"], dict) else {}
    items = _items(req)
    choice = req.get("tool_choice")
    nudged = bool(items) and any(NUDGE in t for t in _texts(items[-1]))
    at = _owner_block_index(items)
    redo = previous is not None and not nudged and ex["status"] == 200 and previous["status"] == 200
    return {
        "forced_final": int(isinstance(choice, dict) and choice.get("name") == "orch_turn"),
        "finalize_nudge": int(nudged),
        "final_notice": int(at >= 0 and any(FINAL_NOTICE in t for t in _texts(items[at]))),
        # The answer before this request was prose, and this request does not keep it.
        "discarded_answers": int(redo and not _has_tool_call(previous["response"])),
    }


def _truncated(ex: dict) -> int:
    items = _items(ex["request"] if isinstance(ex["request"], dict) else {})
    return sum(1 for i in items if _tool_rows(i) for t in _texts(i) if t.rstrip().endswith(TRUNC))


def scan_turn(exchanges: list, step: dict) -> dict:
    """The counted behaviors of one harness turn, from its head exchanges in order."""
    out = defaultdict(int)
    for n, ex in enumerate(exchanges):
        for key, value in _request_flags(ex, exchanges[n - 1] if n else None).items():
            out[key] += value
    if exchanges:
        first = _items(exchanges[0]["request"] if isinstance(exchanges[0]["request"], dict) else {})
        at = _owner_block_index(first)
        out.update(_first_request_facts(first, at) if at >= 0 else {})
        out["truncated_results"] = _truncated(exchanges[-1])
    out["bounced_final"] = len((step.get("harness") or {}).get("bounced") or [])
    out["correction_added"] = len(re.findall(r"\(Correction:", step.get("reply", "")))
    out["requests"] = len(exchanges)
    out["tool_calls"] = len([t for t in step.get("tools") or [] if not t.get("via")])
    return dict(out)


def _owner_turns(arm_dir: Path) -> dict:
    rec = json.loads((arm_dir / "turns.json").read_text())
    return {s["turn"]: s for s in rec["steps"] if s.get("kind") in ("say", "burst")}


def scan_run(arm_dir: Path) -> list:
    by_turn = defaultdict(list)
    for line in gzip.open(arm_dir / "requests.jsonl.gz", "rt"):
        row = json.loads(line)
        if row["path"] in HEAD_PATHS and row["ctx"].get("turn"):
            by_turn[row["ctx"]["turn"]].append(row)
    return [{"turn": n, **scan_turn(by_turn.get(n, []), step)} for n, step in _owner_turns(arm_dir).items()]


# ---------------------------------------------------------------- the tables
def _empty_cell() -> dict:
    return {"turns": 0, "runs": 0, **{k: 0 for k in SUMMED}, "turns_with": {k: 0 for k in COUNTED}, "round_caps": {}}


def _add_turn(cell: dict, turn: dict):
    cell["turns"] += 1
    for k in SUMMED:
        cell[k] += turn.get(k, 0)
    for k in COUNTED:
        cell["turns_with"][k] += 1 if turn.get(k, 0) else 0
    cap = str(turn.get("round_cap", 0))
    cell["round_caps"][cap] = cell["round_caps"].get(cap, 0) + 1


def _merge(total: dict, cell: dict):
    for k, v in cell.items():
        if isinstance(v, dict):
            for kk, vv in v.items():
                total[k][kk] = total[k].get(kk, 0) + vv
        else:
            total[k] += v


def summarize(evidence: Path) -> dict:
    """{model: {scenario: counts, ..., "ALL": counts}} over every stored arm A run."""
    out = {}
    for arm_dir in sorted(evidence.glob("*/*/rep*/A")):
        if not (arm_dir / "turns.json").exists():
            continue
        cell = out.setdefault(arm_dir.parts[-4], {}).setdefault(arm_dir.parts[-3], _empty_cell())
        cell["runs"] += 1
        for turn in scan_run(arm_dir):
            _add_turn(cell, turn)
    for cells in out.values():
        total = _empty_cell()
        for cell in list(cells.values()):
            _merge(total, cell)
        cells["ALL"] = total
    return out


def tables_markdown(data: dict) -> str:
    out = [
        "# Harness mechanisms, counted from arm A's captured requests",
        "",
        "Each cell is `turns where it happened (times it happened)`. Definitions: tools/refloop/mechanisms.py.",
    ]
    head = ["scenario", "turns", "requests/turn", "per-turn block chars", *COUNTED, "round caps told (cap:turns)"]
    for model, cells in data.items():
        out += ["", f"## {model}", "", "| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
        for sid, c in cells.items():
            n = max(c["turns"], 1)
            caps = ", ".join(f"{k}:{v}" for k, v in sorted(c["round_caps"].items()))
            row = [sid, str(c["turns"]), f"{c['requests'] / n:.2f}", f"{c['dyn_chars'] // n:,}"]
            row += [f"{c['turns_with'][k]} ({c[k]})" for k in COUNTED]
            out.append("| " + " | ".join([*row, caps]) + " |")
    return "\n".join(out) + "\n"


# ------------------------------------------------------------ the loss ledger
def _lookups(step: dict) -> int:
    return sum(1 for x in step.get("tools") or [] if not x.get("via") and x["name"].replace(".", "_") in LOOKUPS)


def _turn_tags(scan: dict, a: dict, b: dict, food_expected: bool) -> list:
    """The tags that fired on one turn."""
    tags = [k for k in COUNTED[:7] if scan.get(k)]
    if scan.get("food_block") and not food_expected:
        tags.append("food_block_off_topic")
    if _lookups(a) > _lookups(b):
        tags.append("extra_lookups")
    if len(a.get("replies") or []) > 1 and a["kind"] == "say":
        tags.append("several_replies")
    if a.get("timed_out") or a.get("error") or not a.get("replies"):
        tags.append("failed_turn")
    return tags


def loss_tags(job_dir: Path) -> list:
    """The mechanisms that fired in one repetition, as short tags with the turns they fired on."""
    a_turns, b_turns = _owner_turns(job_dir / "A"), _owner_turns(job_dir / "B")
    fired = defaultdict(list)
    for scan in scan_run(job_dir / "A"):
        n = scan["turn"]
        for tag in _turn_tags(scan, a_turns[n], b_turns.get(n, {}), job_dir.parent.name in FOOD_SCENARIOS):
            fired[tag].append(n)
    return [f"{k} (turn {', '.join(map(str, v))})" for k, v in fired.items()]


def _loss_line(gpath: Path, g: dict, evidence: Path) -> str:
    failed = [f"turn {c['turn']} {c['check']}" for c in g["arms"]["A"]["checks"] if not c["pass"]]
    notes = ((g.get("judge") or {}).get("A") or {}).get("notes", "")
    return (
        f"- **{g['model']} / {g['scenario']} / rep{g['rep']}** - composite A {g['arms']['A']['composite']:.2f} "
        f"vs B {g['arms']['B']['composite']:.2f}. Failed checks: {', '.join(failed) or 'none'}. "
        f"Mechanisms: {'; '.join(loss_tags(gpath.parent)) or 'none of the counted ones'}. "
        f"Judge on A: {notes} Evidence: `{gpath.parent.relative_to(evidence)}/A/`"
    )


def losses_markdown(evidence: Path) -> str:
    out = [
        "# Every repetition the harness lost",
        "",
        "One line per repetition where arm A's composite was more than 0.05 below arm B's. `Mechanisms` are read",
        "off A's captured requests by tools/refloop/mechanisms.py (its header says what each tag means).",
        "",
    ]
    for gpath in sorted(evidence.glob("*/*/rep*/grade.json")):
        g = json.loads(gpath.read_text())
        if g.get("outcome_for_A") == "loss":
            out.append(_loss_line(gpath, g, evidence))
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--evidence", type=Path, required=True)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    data = summarize(args.evidence)
    if args.json:
        args.json.write_text(json.dumps(data, indent=1))
    tables = tables_markdown(data)
    (args.evidence / "MECHANISMS.md").write_text(tables)
    if list(args.evidence.glob("*/*/rep*/grade.json")):
        (args.evidence / "LOSSES.md").write_text(losses_markdown(args.evidence))
    print(tables)


if __name__ == "__main__":
    main()
