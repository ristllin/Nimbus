#!/usr/bin/env python3
"""grade - deterministic checks per scenario, plus an LLM judge that is blind to the arm.

Checks: every `expect` item of a turn and every `final` item of a scenario is one named
check (CHECKS below), run the same way on both arms' records. Prefer a check on the
instance's state (a file, a routine, a memory) over the wording of a reply.

Judge: claude-sonnet-5-5 reads both arms' transcripts of one repetition as "X" and "Y", with
the scenario's ground truth, and scores each on five criteria (1 to 5): correct, context,
tools, honest, concise. It does so twice, the second time with X and Y swapped, and a
repetition is scored on the mean of the two passes.

Outcome of one repetition, from the harness's side (arm A):
  composite = 0.5 * share of checks passed + 0.5 * (judge mean - 1) / 4      (0 to 1)
  win when A's composite is more than 0.05 above B's, loss when more than 0.05 below, else tie.
"""

from __future__ import annotations

import hashlib
import json
import random
import re
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import fixtures
from costs import add_spend, cost_usd
from nimbusd_ctl import PROVIDER_ENV
from scenario import user_turns

JUDGE_MODEL = "claude-sonnet-5-5"
JUDGE_PASSES = 2  # the second pass shows the two transcripts in the opposite order
RUBRIC = ("correct", "context", "tools", "honest", "concise")
TIE_BAND = 0.05
FLAGS = re.I | re.S


# ------------------------------------------------------------------- checks
WORDS = "zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen".split()
WORDS += "seventeen eighteen nineteen twenty".split()


def _ints(text: str) -> set:
    """Whole numbers in a text: digits with thousands separators removed ("1,260" is 1260), and "nine" as 9."""
    text = text or ""
    found = {int(m.replace(",", "")) for m in re.findall(r"(?<![\d.])\d{1,3}(?:,\d{3})+|\d+", text)}
    return found | {n for n, word in enumerate(WORDS) if re.search(rf"\b{word}\b", text, re.I)}


def _memory_text(state: dict) -> str:
    return "\n".join([*state.get("memories", []), state.get("rules", ""), state.get("scratchpad", "")])


def _difference_from_turn(arg: dict, step: dict, turns: list) -> tuple:
    """The reply states how far the assistant's own number in an earlier turn was from the truth."""
    source = turns[arg["turn"] - 1].get("reply", "") if arg["turn"] <= len(turns) else ""
    low, high = arg.get("range", (arg["truth"] // 4, arg["truth"] * 4))
    said = sorted(n for n in _ints(source) if low <= n <= high)
    said += [(a + b) // 2 for a, b in zip(said, said[1:])]  # "500 to 550" may be used as 525
    seen = _ints(step.get("reply", ""))
    for estimate in said:
        gap = abs(estimate - arg["truth"])
        if gap in seen or (gap == 0 and re.search(r"spot on|exact|no difference|same", step.get("reply", ""), re.I)):
            return True, f"estimate {estimate}, difference {gap}"
    return False, f"earlier numbers {said[:6]}, numbers in this reply {sorted(seen)[:12]}"


def _file_matching(arg: dict, state: dict) -> tuple:
    for path, body in state.get("files", {}).items():
        if all(re.search(p, body, FLAGS) for p in arg.get("all", [])) and not any(
            re.search(p, body, FLAGS) for p in arg.get("none", [])
        ):
            return True, path
    return False, f"no file fits; files: {sorted(state.get('files', {}))}"


def _tool_names(step: dict) -> list:
    return [t["name"].replace(".", "_") for t in step.get("tools") or []]


def _one_file(arg: dict, state: dict) -> str | None:
    return state.get("files", {}).get(f"{arg['project']}/{arg['name']}")


# name -> fn(arg, step, state, turns) -> (passed, what was seen). `state` is the instance's state
# after the turn (or at the end, for a final check); `turns` is every owner-turn record so far.
CHECKS = {
    "reply_regex": lambda a, s, st, t: (bool(re.search(a, s.get("reply", ""), FLAGS)), s.get("reply", "")[:200]),
    "reply_not_regex": lambda a, s, st, t: (not re.search(a, s.get("reply", ""), FLAGS), s.get("reply", "")[:200]),
    "last_reply_regex": lambda a, s, st, t: (
        bool(s.get("replies")) and bool(re.search(a, s["replies"][-1], FLAGS)),
        (s.get("replies") or [""])[-1][:200],
    ),
    "max_replies": lambda a, s, st, t: (0 < len(s.get("replies") or []) <= a, f"{len(s.get('replies') or [])} replies"),
    "difference_from_turn": lambda a, s, st, t: _difference_from_turn(a, s, t),
    "tool_called": lambda a, s, st, t: (any(n in _tool_names(s) for n in a), str(_tool_names(s))),
    "tool_not_called": lambda a, s, st, t: (not any(n in _tool_names(s) for n in a), str(_tool_names(s))),
    "min_tool_calls": lambda a, s, st, t: (len(_tool_names(s)) >= a, f"{len(_tool_names(s))} calls"),
    "file_matching": lambda a, s, st, t: _file_matching(a, st),
    "file_contains": lambda a, s, st, t: (
        bool(re.search(a["regex"], _one_file(a, st) or "", FLAGS)),
        (_one_file(a, st) or "(no such file)")[:200],
    ),
    "file_absent": lambda a, s, st, t: (_one_file(a, st) is None, str(sorted(st.get("files", {})))),
    "files_at_most": lambda a, s, st, t: (len(st.get("files", {})) <= a, str(sorted(st.get("files", {})))),
    "loops_count": lambda a, s, st, t: (len(st.get("loops") or []) == a, json.dumps(st.get("loops"))[:300]),
    "loops_regex": lambda a, s, st, t: (
        bool(re.search(a, json.dumps(st.get("loops")), FLAGS)),
        json.dumps(st.get("loops"))[:300],
    ),
    "loops_not_regex": lambda a, s, st, t: (
        not re.search(a, json.dumps(st.get("loops")), FLAGS),
        json.dumps(st.get("loops"))[:300],
    ),
    "memory_regex": lambda a, s, st, t: (bool(re.search(a, _memory_text(st), FLAGS)), _memory_text(st)[:300]),
    "memory_not_regex": lambda a, s, st, t: (not re.search(a, _memory_text(st), FLAGS), _memory_text(st)[:300]),
}


def run_checks(sc: dict, record: dict) -> list:
    """Every check of the scenario against one arm's record. A turn that never ran fails its checks."""
    turns = [s for s in record.get("steps", []) if s.get("kind") in ("say", "burst")]
    out = []
    for n, step in enumerate(user_turns(sc)):
        got = turns[n] if n < len(turns) else {}
        for check in step.get("expect") or []:
            ((name, arg),) = check.items()
            passed, seen = (
                CHECKS[name](arg, got, got.get("state") or {}, turns[: n + 1]) if got else (False, "turn not run")
            )
            out.append({"turn": n + 1, "check": name, "arg": arg, "pass": bool(passed), "seen": str(seen)[:300]})
    for check in sc.get("final") or []:
        ((name, arg),) = check.items()
        state = record.get("final_state")
        passed, seen = CHECKS[name](arg, {}, state, turns) if state else (False, "no final state")
        out.append({"turn": "final", "check": name, "arg": arg, "pass": bool(passed), "seen": str(seen)[:300]})
    return out


# -------------------------------------------------------------------- judge
JUDGE_SYSTEM = (
    "You grade AI personal assistants. You are given one scripted conversation between an owner and an "
    "assistant, run twice with two different assistants, X and Y, on the same model and the same tools. "
    "Grade each from what its transcript shows, against the ground truth. Be strict and specific: a wrong "
    "number, a forgotten fact, an action claimed with no tool call behind it, or a needless tool call each "
    "cost points. Do not reward length. Do not guess which system produced which transcript.\n"
    "Score each assistant from 1 (bad) to 5 (excellent) on:\n"
    "- correct: answers and resulting state are right, per the ground truth\n"
    "- context: uses what was said, shown or done earlier (and long-term memory) wherever a turn needs it\n"
    "- tools: calls the right tools when needed and none when not needed; nothing done twice or left half done\n"
    "- honest: what it says it did matches the tool calls shown; nothing invented; limits stated plainly\n"
    "- concise: as short as the owner's request allows; no filler, no internal ids or plumbing\n"
    "Then say which assistant served the owner better overall: X, Y or tie."
)
_SCORES = {
    "type": "object",
    "additionalProperties": False,
    "required": [*RUBRIC, "notes"],
    "properties": {**{k: {"type": "integer"} for k in RUBRIC}, "notes": {"type": "string"}},
}
JUDGE_SCHEMA = {
    "type": "object",
    "additionalProperties": False,
    "required": ["X", "Y", "better", "why"],
    "properties": {
        "X": _SCORES,
        "Y": _SCORES,
        "better": {"type": "string", "enum": ["X", "Y", "tie"]},
        "why": {"type": "string"},
    },
}


def _cut(text, n: int) -> str:
    text = text if isinstance(text, str) else json.dumps(text, ensure_ascii=False)
    return text if len(text) <= n else text[:n] + f" ...[{len(text) - n} more chars]"


def _render_context(step: dict) -> list:
    """What the assistant was handed for the turn: the photos, and any memory the system recalled for it."""
    lines = [f"  [photo attached: {fixtures.photo_truth(name)}]" for name in step.get("photos", [])]
    if step.get("recalled"):
        shown = "; ".join(_cut(m, 200) for m in step["recalled"][:8])
        lines.append(f"  [long-term memories the system itself put in front of the assistant for this turn: {shown}]")
    return lines


def _render_outcome(step: dict) -> list:
    """What came back: a failure, the system's own notices, the assistant's replies."""
    lines = []
    if step.get("error") or step.get("timed_out"):
        lines.append(f"  [the turn failed: {step.get('error') or 'no reply in time'}]")
    for notice in step.get("notices") or []:
        lines.append(f"  [the system itself also showed the owner this notice: {_cut(notice, 300)}]")
    replies = step.get("replies") or []
    for n, reply in enumerate(replies):
        lines.append(f"  Assistant{f' (reply {n + 1})' if len(replies) > 1 else ''}: {_cut(reply, 3000)}")
    return lines if replies else [*lines, "  Assistant: (no reply)"]


def _render_turn(step: dict) -> list:
    said = step["user"]
    several = f" ({len(said)} quick messages)" if len(said) > 1 else ""
    lines = [f"Turn {step['turn']} - Owner{several}: " + " / ".join(said), *_render_context(step)]
    for t in step.get("tools") or []:
        name = t["name"].replace(".", "_")
        lines.append(f"  tool {name}({_cut(t.get('args'), 300)}) -> {_cut(t.get('result', ''), 300)}")
    return lines + _render_outcome(step)


MARKERS = {
    "restart": "[the assistant's process was restarted here]",
    "new_conversation": "[a NEW conversation starts here: the earlier chat is no longer in view; memory and files remain]",
}


def render_transcript(record: dict) -> str:
    """One arm's run as the judge sees it: owner messages, tool calls with results, replies. No arm names."""
    lines = []
    for step in record.get("steps", []):
        kind = step.get("kind")
        lines += [MARKERS[kind]] if kind in MARKERS else _render_turn(step) if kind in ("say", "burst") else []
    if record.get("error"):
        lines.append(f"[the run stopped early: {record['error']}]")
    return "\n".join(lines)


def judge_pair(client, sc: dict, rec_a: dict, rec_b: dict, a_is_x: bool) -> dict:
    """One blind judgment of both arms. Returns per-arm scores, the verdict and the order used."""
    first, second = (rec_a, rec_b) if a_is_x else (rec_b, rec_a)
    prompt = (
        f"SCENARIO: {sc['title']}\nWHAT IT TESTS: {sc.get('pain', '')}\nGROUND TRUTH: {sc['judge_notes']}\n\n"
        f"===== ASSISTANT X =====\n{render_transcript(first)}\n\n"
        f"===== ASSISTANT Y =====\n{render_transcript(second)}\n\nGrade X and Y."
    )
    resp = client.messages.create(
        model=JUDGE_MODEL,
        max_tokens=4000,
        system=JUDGE_SYSTEM,
        output_config={"effort": "medium", "format": {"type": "json_schema", "schema": JUDGE_SCHEMA}},
        messages=[{"role": "user", "content": prompt}],
    )
    verdict = json.loads(next(b.text for b in resp.content if b.type == "text"))
    usage = {"input": resp.usage.input_tokens, "cached": 0, "cache_write": 0, "output": resp.usage.output_tokens}
    arm_of = {"X": "A" if a_is_x else "B", "Y": "B" if a_is_x else "A", "tie": "tie"}
    return {
        "A": verdict["X" if a_is_x else "Y"],
        "B": verdict["Y" if a_is_x else "X"],
        "better": arm_of[verdict["better"]],
        "why": verdict["why"],
        "order": "A=X" if a_is_x else "A=Y",
        "judge_model": JUDGE_MODEL,
        "cost_usd": round(cost_usd(JUDGE_MODEL, usage), 6),
    }


def merge_passes(passes: list) -> dict | None:
    """The judgment a repetition is scored on: the mean of its passes, per arm and criterion.

    Two passes see the two transcripts in opposite order, so a preference for X or Y as such
    cancels out. `better` is the arm both passes preferred, else "split".
    """
    if not passes:
        return None
    out = {
        "passes": len(passes),
        "order": "+".join(p["order"] for p in passes),
        "judge_model": passes[0]["judge_model"],
    }
    for arm in ("A", "B"):
        out[arm] = {k: sum(p[arm][k] for p in passes) / len(passes) for k in RUBRIC}
        out[arm]["notes"] = " / ".join(p[arm]["notes"] for p in passes)
    prefers = {p["better"] for p in passes}
    out["better"] = prefers.pop() if len(prefers) == 1 else "split"
    out["why"] = " / ".join(p["why"] for p in passes)
    out["cost_usd"] = round(sum(p["cost_usd"] for p in passes), 6)
    return out


# ------------------------------------------------------------------- scoring
def judge_mean(scores: dict | None) -> float | None:
    return sum(scores[k] for k in RUBRIC) / len(RUBRIC) if scores else None


def composite(check_rate: float, scores: dict | None) -> float:
    """0 to 1. With no judge verdict the checks stand alone."""
    mean = judge_mean(scores)
    return check_rate if mean is None else 0.5 * check_rate + 0.5 * (mean - 1) / 4


def outcome(a: float, b: float) -> str:
    return "win" if a - b > TIE_BAND else "loss" if b - a > TIE_BAND else "tie"


def _checked(sc: dict, rec: dict) -> dict:
    checks = run_checks(sc, rec)
    passed = sum(c["pass"] for c in checks)
    return {
        "checks": checks,
        "passed": passed,
        "total": len(checks),
        "check_rate": round(passed / max(len(checks), 1), 4),
        "crashed": "error" in rec,
    }


def _stored_passes(old: dict) -> list:
    """The judge passes a stored grade holds (a grade from before two-pass judging holds one)."""
    if old.get("judge_passes"):
        return old["judge_passes"]
    return [old["judge"]] if old.get("judge") and "passes" not in old["judge"] else []


def _judge(client, sc: dict, recs: dict, g: dict, ledger: Path):
    """Fill the passes g is missing. The first order is seeded; the second is its opposite."""
    tag = f"{g['model']} {sc['id']} rep{g['rep']}"
    seed = hashlib.sha256(f"{g['model']}/{sc['id']}/{g['rep']}".encode()).hexdigest()
    while len(g["judge_passes"]) < JUDGE_PASSES:
        done = g["judge_passes"]
        a_is_x = done[-1]["order"] != "A=X" if done else random.Random(seed).random() < 0.5
        try:
            done.append(judge_pair(client, sc, recs["A"], recs["B"], a_is_x))
            add_spend(ledger, f"judge {tag} pass {len(done)}", done[-1]["cost_usd"])
        except Exception as e:  # a judge failure leaves what is there standing and is retried on the next grade
            g["judge_error"] = f"{type(e).__name__}: {e}"[:400]
            return


def grade_job(job_dir: Path, sc: dict, client, ledger: Path, force: bool = False) -> dict | None:
    """Grade one repetition (both arms must be stored). Writes grade.json and returns it."""
    paths = {arm: job_dir / arm / "turns.json" for arm in ("A", "B")}
    if not all(p.exists() for p in paths.values()):
        return None
    recs = {arm: json.loads(p.read_text()) for arm, p in paths.items()}
    out_path = job_dir / "grade.json"
    old = json.loads(out_path.read_text()) if out_path.exists() else {}
    g = {"scenario": sc["id"], "model": recs["A"]["model"], "rep": recs["A"]["rep"]}
    g["arms"] = {arm: _checked(sc, rec) for arm, rec in recs.items()}
    g["judge_passes"] = [] if force else _stored_passes(old)
    if client:
        _judge(client, sc, recs, g, ledger)
    g["judge"] = merge_passes(g["judge_passes"])
    for arm in ("A", "B"):
        scores = (g["judge"] or {}).get(arm)
        g["arms"][arm]["judge_mean"] = judge_mean(scores)
        g["arms"][arm]["composite"] = round(composite(g["arms"][arm]["check_rate"], scores), 4)
    g["outcome_for_A"] = outcome(g["arms"]["A"]["composite"], g["arms"]["B"]["composite"])
    out_path.write_text(json.dumps(g, indent=1, ensure_ascii=False))
    return g


def grade_all(evidence: Path, scenarios: list, env: dict, judge: bool = True, force: bool = False) -> list:
    client = None
    if judge:
        import anthropic

        client = anthropic.Anthropic(api_key=env.get(PROVIDER_ENV["anthropic"]))
    by_id = {s["id"]: s for s in scenarios}
    dirs = [d for d in sorted(evidence.glob("*/*/rep*")) if d.parent.name in by_id]
    with ThreadPoolExecutor(max_workers=4) as pool:
        grades = [
            g
            for g in pool.map(
                lambda d: grade_job(d, by_id[d.parent.name], client, evidence / "spend.jsonl", force), dirs
            )
            if g
        ]
    print(f"graded {len(grades)} repetitions", flush=True)
    return grades
