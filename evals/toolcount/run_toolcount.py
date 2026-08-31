#!/usr/bin/env python3
"""Runner for the tool-count benchmark.

For every (model x condition x task x repetition) it runs one turn against a
provider, measures which tool the model selected, and appends a JSONL corpus row
(schema in `README.md`). The turn loop honors the lazy condition: a
`search_tools` call is answered with matching candidate tools, which are then
added to the exposed set so the model can call the real tool.

The deterministic tool-selection check is the hard signal (an "oracle" in the
spirit of the device benchmark). An optional cross-provider judge scores answer
quality as a soft signal and never gates anything.

Runs are paid and live unless `--mock` is given. Keys come from the environment
or the repo `.env`; nothing here echoes a secret.

Usage:
    python3 evals/toolcount/run_toolcount.py --mock --reps 2
    python3 evals/toolcount/run_toolcount.py \
        --models openai:gpt-4o-mini,zai:glm-4.6 \
        --conditions full,curated,lazy --reps 5 --out evals/toolcount/runs/x.jsonl
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import conditions as C  # noqa: E402
import providers as P  # noqa: E402

MAX_HOPS = 4
SYSTEM = (
    "You are the on-device assistant for a small hardware device. When a task "
    "needs an action, call exactly one appropriate tool with correct arguments. "
    "If no tool fits, answer in one short sentence and call no tool. Prefer the "
    "most specific tool for the request."
)


def _handle_search_hop(
    tc: dict[str, Any],
    catalog: list[dict[str, Any]],
    exposed: list[dict[str, Any]],
    exposed_names: set[str],
    messages: list[dict[str, Any]],
) -> None:
    # search_tools meta-hop (lazy condition): reveal the matched tools for
    # subsequent turns and feed the listing back as the tool result.
    query = str(tc["arguments"].get("query", ""))
    hits = C.search_catalog(query, catalog, k=5)
    listing = "\n".join(f"- {h['name']}: {h['description']}" for h in hits)
    for h in hits:
        if h["name"] not in exposed_names:
            exposed.append(C.to_schema(h))
            exposed_names.add(h["name"])
    messages.append(
        {
            "role": "assistant",
            "content": None,
            "tool_calls": [
                {
                    "id": tc["id"],
                    "type": "function",
                    "function": {"name": "search_tools", "arguments": json.dumps(tc["arguments"])},
                }
            ],
        }
    )
    messages.append(
        {
            "role": "tool",
            "tool_call_id": tc["id"],
            "content": f"Candidate tools:\n{listing}\nThese are now available to call.",
        }
    )


def _run_one_turn(
    provider, task: dict[str, Any], condition: str, catalog: list[dict[str, Any]], mock: bool
) -> dict[str, Any]:
    exposed = C.exposed_tools(condition, catalog)
    n_exposed = len(exposed)
    sys_msg = SYSTEM
    if mock:
        sys_msg += f"\n__expected__:{task.get('expected_tool') or ''}"
    messages: list[dict[str, Any]] = [
        {"role": "system", "content": sys_msg},
        {"role": "user", "content": task["prompt"]},
    ]
    exposed_names = {t["function"]["name"] for t in exposed}
    called: list[str] = []
    search_hops = 0
    selected: str | None = None
    final_content = ""
    ptok = ctok = 0
    err: str | None = None
    t0 = time.time()

    for _ in range(MAX_HOPS):
        try:
            res = provider.chat(messages, exposed)
        except Exception as exc:  # provider/network error
            err = f"{type(exc).__name__}: {exc}"[:200]
            break
        ptok += res.prompt_tokens
        ctok += res.completion_tokens
        if not res.tool_calls:
            final_content = res.content
            break
        tc = res.tool_calls[0]
        name = tc["name"]
        called.append(name)
        if name == "search_tools" and condition == "lazy":
            search_hops += 1
            _handle_search_hop(tc, catalog, exposed, exposed_names, messages)
            continue
        # a real (non-meta) tool call: this is the selection we measure
        selected = name
        break

    latency_ms = int((time.time() - t0) * 1000)
    expected = task.get("expected_tool")
    selected_correct = selected == expected
    wrong_call = selected is not None and expected is not None and selected != expected
    if expected is None:
        # a "no tool needed" task: correct iff the model called no real tool
        wrong_call = selected is not None
        selected_correct = selected is None
    return {
        "task": task["id"],
        "cat": task.get("cat", "misc"),
        "subset": task.get("subset", "core"),
        "condition": condition,
        "model": provider.label,
        "expected_tool": expected,
        "selected_tool": selected,
        "selected_correct": bool(selected_correct),
        "wrong_call": bool(wrong_call),
        "finished_no_tool": selected is None and err is None,
        "called_tools": called,
        "search_hops": search_hops,
        "n_tools_exposed": n_exposed,
        "prompt_tokens": ptok,
        "completion_tokens": ctok,
        "total_tokens": ptok + ctok,
        "latency_ms": latency_ms,
        "usd_est": P.usd_estimate(provider.model, ptok, ctok),
        "final_content": final_content[:400],
        "error": err,
    }


def _judge(judge_provider, task: dict[str, Any], row: dict[str, Any]) -> dict[str, Any]:
    """Soft cross-provider quality score for the model's action on the task."""
    action = (
        f"called tool `{row['selected_tool']}`"
        if row["selected_tool"]
        else f"gave a direct answer: {row['final_content']!r}"
    )
    prompt = (
        "Score how well an assistant handled a device task. Reply as compact "
        "JSON only: {\"score\": 0..1, \"appropriate\": true/false, "
        "\"rationale\": \"<=140 chars\"}.\n\n"
        f"Task: {task['prompt']}\n"
        f"Ideal tool for this task: {task.get('expected_tool') or 'none (answer directly)'}\n"
        f"Assistant {action}.\n"
        f"Search hops before acting: {row['search_hops']}.\n"
    )
    try:
        res = judge_provider.chat([{"role": "user", "content": prompt}], tools=[])
        text = res.content.strip()
        start, end = text.find("{"), text.rfind("}")
        obj = json.loads(text[start : end + 1])
        return {
            "score": float(obj.get("score", 0.0)),
            "appropriate": bool(obj.get("appropriate", False)),
            "rationale": str(obj.get("rationale", ""))[:140],
            "by": judge_provider.label,
        }
    except Exception as exc:
        return {"error": f"{type(exc).__name__}: {exc}"[:160]}


def _make_judge_picker(args):
    """Return a fn mapping a model label to its judge provider (or None).

    "cross" judges each row with a provider from a DIFFERENT family than the one
    under test (a model is never its own judge, mirroring the device benchmark),
    on a fast, cheap tier so the soft signal never bottlenecks the run. Any other
    value is a single fixed judge provider.
    """
    if args.mock or not args.judge:
        return lambda _label: None
    if args.judge != "cross":
        fixed = P.build_provider(args.judge, temperature=0.0)
        return lambda _label: fixed
    judge_openai = P.build_provider("openai:gpt-4o-mini", temperature=0.0)
    judge_zai = P.build_provider("zai:glm-4.5-air", temperature=0.0)
    return lambda label: judge_zai if label.startswith("openai/") else judge_openai


def _log_row(provider_label: str, cond: str, task: dict, row: dict, rep: int) -> None:
    mark = "OK" if row["selected_correct"] else "XX"
    print(
        f"[{mark}] {provider_label} {cond} {task['id']} rep{rep} -> "
        f"{row['selected_tool']} (exp {row['expected_tool']}, "
        f"hops {row['search_hops']})",
        file=sys.stderr,
    )


def _run_matrix(fh, plan: dict, args, pick_judge) -> int:
    specs, conds, tasks, catalog = plan["specs"], plan["conds"], plan["tasks"], plan["catalog"]
    n = 0
    for spec in specs:
        provider = P.build_provider("mock:mock") if args.mock else P.build_provider(spec, temperature=args.temp)
        for cond in conds:
            for task in tasks:
                for rep in range(args.reps):
                    row = _run_one_turn(provider, task, cond, catalog, args.mock)
                    row["rep"] = rep
                    row["temp"] = getattr(provider, "temperature", 0.0)
                    row["ts"] = time.time()
                    jp = pick_judge(provider.label)
                    row["judge"] = _judge(jp, task, row) if jp else None
                    fh.write(json.dumps(row) + "\n")
                    fh.flush()
                    n += 1
                    _log_row(provider.label, cond, task, row, rep)
    return n


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Tool-count benchmark runner")
    ap.add_argument("--models", default="mock:mock", help="comma list of provider:model specs")
    ap.add_argument("--conditions", default="full,curated,lazy")
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--catalog", default=str(HERE / "catalog.json"))
    ap.add_argument("--tasks", default=str(HERE / "tasks.json"))
    ap.add_argument("--out", default="")
    ap.add_argument("--mock", action="store_true", help="use the offline mock provider regardless of --models")
    ap.add_argument("--judge", default="", help="provider:model for the soft quality judge (optional)")
    ap.add_argument("--temp", type=float, default=0.7, help="sampling temperature for models under test")
    ap.add_argument("--env", default=str(HERE.parents[1] / ".env"), help="path to a .env for keys")
    ap.add_argument("--limit-tasks", type=int, default=0, help="cap number of tasks (smoke runs)")
    args = ap.parse_args(argv)

    P.load_dotenv(args.env)
    catalog = C.load_catalog(args.catalog)
    tasks = json.loads(Path(args.tasks).read_text())["tasks"]
    if args.limit_tasks:
        tasks = tasks[: args.limit_tasks]
    conds = [c.strip() for c in args.conditions.split(",") if c.strip()]
    specs = ["mock:mock"] if args.mock else [m.strip() for m in args.models.split(",") if m.strip()]

    out_path = Path(args.out) if args.out else (HERE / "runs" / f"toolcount_{int(time.time())}.jsonl")
    out_path.parent.mkdir(parents=True, exist_ok=True)

    pick_judge = _make_judge_picker(args)
    plan = {"specs": specs, "conds": conds, "tasks": tasks, "catalog": catalog}
    with out_path.open("w") as fh:
        n = _run_matrix(fh, plan, args, pick_judge)
    print(f"wrote {n} rows -> {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
