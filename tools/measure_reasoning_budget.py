#!/usr/bin/env python3
"""Reasoning-versus-visible-output probe (lane POL1, ruling 3 Nov 09:15Z).

A one-line food turn cut mistral-large-4-0 at 4,125 output tokens, then again at
the 5,500 retry, for a 135-byte reply: the output budget went to reasoning even
at effort "low" (evidence/lumi-v490-watch/errlog-cut-20261103T091232Z.txt lines
216-232). This probe measures, per Mistral and GLM model on real turn shapes,
how much of the completion is reasoning and how much is the visible answer, with
NO output cap, so the numbers say what cap would have fit.

Turn shapes:
  food-tools  the golden owner-turn system prompt (the exact device bytes,
              test/golden/orch_prompt_default.txt) + a small tool array in the
              orch shape + the exact one-liner from the evidence.
  plain-qa    a plain question, no tools.

Usage (from the repo root):
  python3 tools/measure_reasoning_budget.py --out evidence.json [--reps 1]

Keys are read by name from the env file and never printed. Raw replies are kept
only as lengths and the first 120 characters (the probe runs on a Mac, not the
device, so nothing here is a device claim).
"""

import argparse
import json
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROMPT = (HERE.parent / "test/golden/orch_prompt_default.txt").read_text()
SCHEMA_NOTE = (
    "\n\n[OUTPUT FORMAT] Answer with ONE JSON object for your orch_turn and nothing "
    "outside it: {\"reply\": string, \"memory\": string, \"ask\": string, "
    "\"device\": [{\"type\": string, \"value\": string}]}."
)
FINAL_NOTE = (
    "\n\nThe tool result is in the transcript. Answer now with your orch_turn object. Do not call any more tools."
)
FOOD_TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "files_edit",
            "description": "Append or rewrite rows of a keyed CSV file the device keeps.",
            "parameters": {
                "type": "object",
                "properties": {
                    "project": {"type": "string"},
                    "name": {"type": "string"},
                    "op": {"type": "string", "enum": ["append", "rewrite"]},
                    "rows": {"type": "array", "items": {"type": "string"}},
                },
                "required": ["project", "name", "op", "rows"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "mem_write",
            "description": "Store a fact in long-term memory.",
            "parameters": {
                "type": "object",
                "properties": {"content": {"type": "string"}},
                "required": ["content"],
            },
        },
    },
]
SHAPES = {
    "food-tools": {
        "messages": [
            {"role": "system", "content": PROMPT + SCHEMA_NOTE},
            {"role": "user", "content": "1 protein bar like i had a few days ago"},
        ],
        "tools": FOOD_TOOLS,
    },
    # The round that burned the budget on Lumi (errlog lines 222-223): the tool
    # result is in the transcript and the head must now produce the final
    # orch_turn object. No output cap is sent, so the completion count says what
    # the model really wanted.
    "food-final": {
        "messages": [
            {"role": "system", "content": PROMPT + SCHEMA_NOTE + FINAL_NOTE},
            {"role": "user", "content": "1 protein bar like i had a few days ago"},
            {
                "role": "assistant",
                "content": "",
                "tool_calls": [
                    {
                        "id": "c1",
                        "type": "function",
                        "function": {
                            "name": "files_edit",
                            "arguments": json.dumps(
                                {
                                    "project": "food",
                                    "name": "food-log.csv",
                                    "op": "append",
                                    "rows": ["2026-11-03,snack,Quest Bar (1 bar),190,21"],
                                }
                            ),
                        },
                    }
                ],
            },
            {
                "role": "tool",
                "tool_call_id": "c1",
                "content": "ok: appended 1 row(s) to food/food-log.csv (keyed, 3054 B)",
            },
        ],
        "tools": FOOD_TOOLS,
    },
    "plain-qa": {
        "messages": [{"role": "user", "content": "What is 17 times 3? Answer with just the number."}],
        "tools": None,
    },
}


def big_history():
    """Pairs of plausible chat history, ~1.5 KB each, to size the prompt like the
    real turn (in=59282 tokens over 3 calls, so about 20k per call)."""
    out = []
    for i in range(30):
        out.append(
            {
                "role": "user",
                "content": "Note %d: breakfast was oats with berries and "
                "a spoon of honey, coffee black, then a walk to the station %s" % (i, "and the day went on. " * 30),
            }
        )
        out.append(
            {
                "role": "assistant",
                "content": "Logged: oats with berries and honey, coffee "
                "black. The walk counts too. %s" % ("Anything else you ate, tell me. " * 20),
            }
        )
    return out


SHAPES["food-big"] = {
    "messages": SHAPES["food-final"]["messages"][:-2] + big_history() + SHAPES["food-final"]["messages"][-2:],
    "tools": FOOD_TOOLS,
}
MODELS = [
    # (provider, model, reasoning_effort field value; "" = send no field)
    ("mistral", "mistral-large-4-0", "low"),
    ("mistral", "mistral-medium-latest", "none"),
    ("mistral", "mistral-small-latest", "none"),
    ("mistral", "mistral-large-latest", ""),
    ("zai", "glm-5.3", "low"),
    ("zai", "glm-5.3-flash", "low"),
]
ENDPOINTS = {
    "mistral": ("https://api.mistral.ai/v1/chat/completions", "MISTRAL_API_KEY"),
    "zai": ("https://api.z.ai/api/paas/v4/chat/completions", "Z_AI_TOKEN"),
}


def read_env_file(path):
    env = {}
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip("'\"")
    return env


def strip_descriptions(node):
    """The device sends the orch schema with descriptions stripped (mistralStripDescriptions)."""
    if isinstance(node, dict):
        return {k: strip_descriptions(v) for k, v in node.items() if k != "description"}
    if isinstance(node, list):
        return [strip_descriptions(v) for v in node]
    return node


ORCH_SCHEMA = strip_descriptions(json.loads((HERE.parent / "test/golden/orch_schema.json").read_text()))


def call(provider, model, effort, shape, key, schema=False):
    url, _ = ENDPOINTS[provider]
    body = {"model": model, "messages": SHAPES[shape]["messages"]}
    if effort:
        body["reasoning_effort"] = effort
    if SHAPES[shape]["tools"]:
        body["tools"] = SHAPES[shape]["tools"]
        body["tool_choice"] = "auto"
    if schema:
        body["response_format"] = {
            "type": "json_schema",
            "json_schema": {"name": "orch_turn", "strict": True, "schema": ORCH_SCHEMA},
        }
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode(),
        headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"},
    )
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            return json.loads(r.read()), (time.time() - t0) * 1000, None
    except urllib.error.HTTPError as e:
        return json.loads(e.read() or b"{}"), (time.time() - t0) * 1000, "HTTP %d" % e.code


def run_cell(spec, shape, schema, key):
    """One request; returns the measurement row (rep stamped by the caller)."""
    provider, model, effort = spec
    resp, ms, err = call(provider, model, effort, shape, key, schema)
    usage = resp.get("usage", {})
    msg = (resp.get("choices") or [{}])[0].get("message", {})
    text = msg.get("content") or ""
    reasoning = msg.get("reasoning_content") or ""
    return {
        "provider": provider,
        "model": model,
        "effort": effort or None,
        "shape": shape + ("-schema" if schema else ""),
        "schema": schema,
        "rep": 0,
        "error": err,
        "latency_ms": round(ms),
        "finish_reason": (resp.get("choices") or [{}])[0].get("finish_reason"),
        "prompt_tokens": usage.get("prompt_tokens"),
        "completion_tokens": usage.get("completion_tokens"),
        "visible_chars": len(text),
        "reasoning_chars": len(reasoning),
        "reply_head": text[:120],
    }


def probe_model(provider, model, effort, key, reps, rows):
    """All shapes and schema variants for one model."""
    for shape in SHAPES:
        if shape == "food-big" and model != "mistral-large-4-0":
            continue  # the big shape is the one observed failing: large 4 only
        schemas = [False, True] if shape in ("food-final", "food-big") else [False]
        for rep in range(reps):
            for schema in schemas:
                row = run_cell((provider, model, effort), shape, schema, key)
                row["rep"] = rep
                rows.append(row)
                print(".", end="", flush=True)


def report(rows):
    for r in rows:
        print(
            "%-22s %-10s effort=%-4s %-10s %5d ms  completion=%5s visible=%5d ch  reasoning=%6d ch  %s"
            % (
                r["model"],
                r["provider"],
                r["effort"] or "-",
                r["shape"],
                r["latency_ms"],
                r["completion_tokens"],
                r["visible_chars"],
                r["reasoning_chars"],
                r["error"] or r["finish_reason"] or "",
            )
        )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--env-file", default="nimbus/.env")
    ap.add_argument("--out", required=True)
    ap.add_argument("--reps", type=int, default=1)
    args = ap.parse_args()
    env = read_env_file(args.env_file)
    rows = []
    for provider, model, effort in MODELS:
        _, keyname = ENDPOINTS[provider]
        key = env.get(keyname, "")
        if not key:
            print("no key for", provider, "- skipped")
            continue
        reps = args.reps if model == "mistral-large-4-0" else 1
        probe_model(provider, model, effort, key, reps, rows)
    Path(args.out).write_text(json.dumps(rows, indent=1) + "\n")
    print("\nwrote", args.out, len(rows), "rows")
    report(rows)


if __name__ == "__main__":
    main()
