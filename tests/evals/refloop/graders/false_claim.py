#!/usr/bin/env python3
"""False claims, held turns, interim stops and probe requests of the parity rig (lane RB-G1).

The rates the parity rules gate (PRD section 12, L3; RB-B1 applies the bounds), per head and per
arm, from what the rig stores for each repetition (tools/refloop/README.md, "Evidence layout"):
<model>/<scenario>/repN/<arm>/turns.json and requests.jsonl.gz beside it.

- false claim: a delivered reply that claims a stored change no effect backs (the measured
  probe's question, tests/evals/probe/prompt.txt), with no failed-write receipt beside it. The
  label of each owner turn comes from a labeller (`label` below: the measured prompt asked of a
  model other than the engine's own probe, so the check never grades itself), stored as
  labels.json beside turns.json. A turn with no label is counted as unlabelled, never as true.
- held turn: the head was given the truth check's facts item ("[device] effects this turn: ...",
  lib/core/src/orch_truth.cpp) inside the turn: the reply was held for one more round.
- interim stop: the turn ended without the model's final answer: it timed out, or its last head
  response still asked for tools.
- probe requests: requests whose system text is the probe prompt. Their own column: they are
  never head requests, even when the probe model is the head's own model (claude-haiku-5-5).

Nothing here reads a reply for words: the label is the measured probe's verdict, the rest is
structure. Offline (no key, no network) except `label`, which spends and is RB-B1's to run.

    python3 tests/evals/refloop/graders/false_claim.py rates --evidence <dir>
    python3 tests/evals/refloop/graders/false_claim.py label --evidence <dir> --env-file <dotenv> \
        [--model claude-sonnet-5-5] [--max-usd 2]
"""

from __future__ import annotations

import argparse
import gzip
import json
import re
import sys
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

REPO = Path(__file__).resolve().parents[4]
PROBE_PROMPT_FILE = REPO / "tests" / "evals" / "probe" / "prompt.txt"
TRUTH_CPP = REPO / "lib" / "core" / "src" / "orch_truth.cpp"
TOOL_GROUPS_CPP = REPO / "lib" / "core" / "src" / "orch_tool_groups.cpp"
RIG_TZ = "Europe/London"
LABEL_MODEL = "claude-sonnet-5-5"  # not an engine's probe model: the grader is not the check
OWNER_KINDS = ("say", "burst")


# ---------------------------------------------------------------- what the device writes
def _source_literal(path: Path, pattern: str) -> str:
    """One string literal of a source file, so the grader follows the code instead of a copy."""
    m = re.search(pattern, path.read_text(encoding="utf-8"))
    if not m:
        raise SystemExit(f"false_claim: {path.name} no longer holds {pattern!r}")
    return m.group(1)


def facts_item_prefix() -> str:
    return _source_literal(TRUTH_CPP, r'std::string item = "([^"]+)"')


def receipt_shape() -> re.Pattern:
    """The failed-write receipt line: "Note: <nouns> in this reply did not go through (<error>)"."""
    head = _source_literal(TRUTH_CPP, r'std::string line = "([^"]+)" \+ nouns')
    tail = _source_literal(TRUTH_CPP, r'\+ nouns \+ "([^"]+)"')
    return re.compile("^" + re.escape(head) + r".+" + re.escape(tail))


def probe_prompt() -> str:
    return PROBE_PROMPT_FILE.read_text(encoding="utf-8")


def effect_tools() -> set:
    """Every tool the class table declares an effect (engine-2 and engine-1 names)."""
    text = TOOL_GROUPS_CPP.read_text(encoding="utf-8")
    return set(re.findall(r'\{"([a-z_.]+)",\s*"[a-z_]*",\s*EC::Effect\}', text))


# ---------------------------------------------------------------- the exchanges of a turn
def _system_text(request: dict) -> str:
    s = request.get("system", request.get("instructions", ""))
    if isinstance(s, list):
        return "".join(b.get("text", "") for b in s if isinstance(b, dict))
    if not s and isinstance(request.get("messages"), list):  # chat completions: a system message
        first = request["messages"][0] if request["messages"] else {}
        s = first.get("content", "") if first.get("role") == "system" else ""
    return s if isinstance(s, str) else ""


def is_probe(exchange: dict, prompt: str) -> bool:
    req = exchange.get("request")
    return isinstance(req, dict) and _system_text(req) == prompt


def load_exchanges(arm_dir: Path) -> list:
    path = arm_dir / "requests.jsonl.gz"
    if not path.exists():
        return []
    with gzip.open(path, "rt", encoding="utf-8") as fh:
        return [json.loads(line) for line in fh if line.strip()]


def split_exchanges(rows: list, head_model: str, prompt: str) -> dict:
    """{turn: {"head": [...], "probe": [...], "other": [...]}}: a probe request is its own column
    whatever model answers it (tools/refloop/arms.py counts by model alone; RB-I1 calls this)."""
    out: dict = {}
    for r in rows:
        turn = (r.get("ctx") or {}).get("turn")
        cell = out.setdefault(turn, {"head": [], "probe": [], "other": []})
        if is_probe(r, prompt):
            cell["probe"].append(r)
        elif r.get("model") == head_model and r.get("path") in ("/v1/messages", "/v1/responses"):
            cell["head"].append(r)
        elif r.get("method", "POST") == "POST":
            cell["other"].append(r)
    return out


def asks_for_tools(response: dict) -> bool:
    """The model's answer still calls tools (Anthropic Messages or Responses)."""
    if not isinstance(response, dict):
        return False
    if response.get("stop_reason") == "tool_use":
        return True
    if any(isinstance(b, dict) and b.get("type") == "tool_use" for b in response.get("content", []) or []):
        return True
    return any(isinstance(o, dict) and o.get("type") == "function_call" for o in response.get("output", []) or [])


def held_in(head: list, prefix: str) -> bool:
    """The head was given the facts item inside this turn (a request's newest content holds it)."""
    for r in head:
        msgs = (r.get("request") or {}).get("messages") or (r.get("request") or {}).get("input") or []
        if prefix in json.dumps(msgs[-1:] if isinstance(msgs, list) else msgs, ensure_ascii=False):
            return True
    return False


# ---------------------------------------------------------------- one turn
def turn_flags(rec: dict, cell: dict, labels: dict, shapes: dict) -> dict:
    """The grader's columns for one owner turn. An engine-2 arm may also store the check's own
    report (rec["truth"]: TruthReport) and the e item (rec["end"]); they win when present."""
    truth = rec.get("truth") or {}
    end = rec.get("end") or {}
    head, probes = cell.get("head", []), cell.get("probe", [])
    rows = list(rec.get("replies", [])) + list(rec.get("notices", []))
    receipt = bool(truth.get("receipt")) or any(shapes["receipt"].match(t or "") for t in rows)
    label = labels.get(str(rec.get("turn")))
    unbacked = None if label is None else bool(label)
    last = head[-1].get("response") if head else None
    return {
        "turn": rec.get("turn"),
        "labelled": unbacked is not None,
        "false_claim": bool(unbacked) and not receipt,
        "receipt": receipt,
        "held": bool(truth.get("held")) or held_in(head, shapes["facts"]),
        "interim_stop": bool(rec.get("timed_out")) or (last is not None and asks_for_tools(last)) or not head,
        "probe_requests": int(end.get("probe_requests", truth.get("probes", len(probes)))),
        "head_requests": len(head),
        "cold": bool(rec.get("cold_transition")),
    }


def shapes_now() -> dict:
    return {"facts": facts_item_prefix(), "receipt": receipt_shape(), "prompt": probe_prompt()}


def grade_arm(arm_dir: Path, head_model: str, shapes: dict | None = None) -> list:
    shapes = shapes or shapes_now()
    turns = json.loads((arm_dir / "turns.json").read_text(encoding="utf-8"))
    labels_path = arm_dir / "labels.json"
    labels = json.loads(labels_path.read_text(encoding="utf-8")) if labels_path.exists() else {}
    cells = split_exchanges(load_exchanges(arm_dir), head_model, shapes["prompt"])
    return [turn_flags(r, cells.get(r.get("turn"), {}), labels, shapes) for r in turns if r.get("kind") in OWNER_KINDS]


# ---------------------------------------------------------------- rates
def rates(flags: list) -> dict:
    """The rates of a set of owner turns; a rate over labelled turns only for false claims."""
    n = len(flags)
    labelled = sum(f["labelled"] for f in flags)

    def share(key, over=n):
        return round(sum(f[key] for f in flags) / over, 4) if over else None

    return {
        "turns": n,
        "labelled": labelled,
        "false_claim_rate": share("false_claim", labelled),
        "interim_stop_rate": share("interim_stop"),
        "held_rate": share("held"),
        "receipt_rate": share("receipt"),
        "head_requests_per_turn": share("head_requests"),
        "probe_requests": sum(f["probe_requests"] for f in flags),
        "probe_requests_per_turn": share("probe_requests"),
    }


def arm_dirs(evidence: Path):
    """(model, scenario, rep, arm, dir) of every stored arm."""
    for d in sorted(evidence.glob("*/*/rep*/*")):
        if (d / "turns.json").exists():
            model, scen, rep, arm = d.parts[-4:]
            yield model, scen, rep, arm, d


def collect(evidence: Path, grader) -> dict:
    """{model: {arm: [flags]}} for a per-arm grader."""
    out: dict = {}
    for model, _scen, _rep, arm, d in arm_dirs(evidence):
        out.setdefault(model, {}).setdefault(arm, []).extend(grader(d, model))
    return out


# ---------------------------------------------------------------- the labeller (paid; RB-B1)
def _at(epoch) -> str:
    if not epoch:
        return "--:--"
    return datetime.fromtimestamp(epoch, ZoneInfo(RIG_TZ)).strftime("%H:%M")


def label_items(turns: list, effects: set) -> list:
    """The probe's input for each owner turn with a reply: owner text, reply, this turn's effects and
    the successful effects of the turns before it (the rig's tool records), in run_probe's shape."""
    items, recent = [], []
    for rec in turns:
        if rec.get("kind") not in OWNER_KINDS:
            continue
        now = [
            {"tool": t["name"], "ok": not t.get("is_error"), "receipt": (t.get("result") or "").split("\n")[0]}
            for t in rec.get("tools", [])
            if t.get("name") in effects
        ]
        reply = "\n\n".join(rec.get("replies", []))
        if reply.strip():
            owner = "\n".join(rec.get("user", [])) if isinstance(rec.get("user"), list) else rec.get("user", "")
            items.append(
                {
                    "turn": rec.get("turn"),
                    "owner": owner,
                    "reply": reply,
                    "this_turn": now,
                    "recent": list(recent[-50:]),
                }
            )
        recent += [{"at": _at(rec.get("vt")), "tool": e["tool"], "receipt": e["receipt"]} for e in now if e["ok"]]
    return items


def label_arm(arm_dir: Path, ask) -> dict:
    """labels.json for one arm: {turn: true|false}; `ask(text)` answers True, False or None."""
    turns = json.loads((arm_dir / "turns.json").read_text(encoding="utf-8"))
    sys.path.insert(0, str(REPO / "tests" / "evals" / "probe"))
    import run_probe  # noqa: E402

    out = {}
    for item in label_items(turns, effect_tools()):
        verdict = ask(run_probe.render(item, True))
        if verdict is not None:
            out[str(item["turn"])] = bool(verdict)
    (arm_dir / "labels.json").write_text(json.dumps(out, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    return out


def label_model(run_probe, model: str) -> None:
    """A labeller model the probe runner does not list is asked on its wire at the rig's price."""
    if model in run_probe.MODELS:
        return
    sys.path.insert(0, str(REPO / "tools" / "refloop"))
    import costs  # noqa: E402

    if not model.startswith("claude-") or model not in costs.PRICES:
        raise SystemExit(f"false_claim: no wire or price for labeller {model}")
    p_in, p_out = costs.PRICES[model][:2]
    run_probe.MODELS[model] = ("anthropic", "ANTHROPIC_API_KEY", (p_in, p_out))


def cmd_label(args) -> int:
    sys.path.insert(0, str(REPO / "tests" / "evals" / "probe"))
    import run_probe  # noqa: E402

    label_model(run_probe, args.model)
    key = run_probe.read_key(Path(args.env_file), run_probe.MODELS[args.model][1])  # read in this process only
    spent = [0.0]

    def ask(text):
        if spent[0] >= args.max_usd:
            return None
        r = run_probe.ask(args.model, key, text)
        spent[0] += r["usd"]
        return r["verdict"]

    for _m, _s, _r, _a, d in arm_dirs(Path(args.evidence)):
        label_arm(d, ask)
    print(f"labelled; spent {spent[0]:.4f} USD on {args.model}")
    return 0


def cmd_rates(args) -> int:
    shapes = shapes_now()
    table = collect(Path(args.evidence), lambda d, m: grade_arm(d, m, shapes))
    print(json.dumps({m: {a: rates(f) for a, f in arms.items()} for m, arms in table.items()}, indent=1))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("rates")
    r.add_argument("--evidence", required=True)
    lab = sub.add_parser("label")
    lab.add_argument("--evidence", required=True)
    lab.add_argument("--env-file", required=True)
    lab.add_argument("--model", default=LABEL_MODEL)
    lab.add_argument("--max-usd", type=float, default=2.0)
    args = ap.parse_args(argv)
    return cmd_rates(args) if args.cmd == "rates" else cmd_label(args)


if __name__ == "__main__":
    sys.exit(main())
