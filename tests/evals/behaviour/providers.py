#!/usr/bin/env python3
"""providers - the three head wires of engine 2 (Anthropic Messages, OpenAI Responses, chat completions),
tool choice auto, each priced per exchange from the usage the provider reported.

A provider turns the neutral transcript (user / atext / call / result items) into its own messages,
and returns each step's native items so thinking and reasoning are replayed inside the turn exactly
as served. Keys come from the process environment only and are never printed.
"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass

# model -> (input, output, cache read, cache write) USD per million tokens
# (lib/core/include/nimbus/orch/model_price_defaults.h and PRD "Checked against the tree").
PRICES = {
    "claude-sonnet-5-5": (2.00, 10.00, 0.10, 2.50),
    "claude-haiku-5-5": (0.10, 0.50, 0.01, 0.125),
    "gpt-5.6-luna": (0.20, 1.20, 0.02, 0.20),
    "gpt-5.6": (5.00, 30.00, 0.50, 6.25),
    "glm-5.3": (1.40, 4.40, 0.26, 1.40),
    # Lane RB-R1 (ruling 102): the firmware's own row for mistral-medium (model_price_defaults.h, 1.5 / 7.5 USD per
    # 1M; cache read 0.1x and write 1.25x, that table's default share), not a provider quote.
    "mistral-medium-latest": (1.50, 7.50, 0.15, 1.875),
}
HAIKU_LONG = (0.50, 2.50, 0.05, 0.625)
MAX_TOKENS = 16000


def price(model: str, usage: dict) -> float:
    p_in, p_out, p_read, p_write = PRICES[model]
    if model == "claude-haiku-5-5" and usage.get("prompt", 0) > 100_000:
        p_in, p_out, p_read, p_write = HAIKU_LONG
    fresh = max(usage.get("prompt", 0) - usage.get("cached", 0) - usage.get("written", 0), 0)
    return (
        fresh * p_in
        + usage.get("cached", 0) * p_read
        + usage.get("written", 0) * p_write
        + usage.get("output", 0) * p_out
    ) / 1e6


@dataclass
class Step:
    items: list  # native items to append
    calls: list  # (call id, name, args)
    text: str
    usage: dict  # prompt (whole prompt), cached, written, output
    stop: str


def _args(raw):
    try:
        v = json.loads(raw or "{}")
        return v if isinstance(v, dict) else {"_raw": raw}
    except ValueError:
        return {"_raw": raw}


class Anthropic:
    wire = "anthropic"

    def __init__(self, model: str, effort: str = "medium"):
        import anthropic

        self.client = anthropic.Anthropic(api_key=os.environ["ANTHROPIC_API_KEY"], max_retries=5, timeout=180)
        self.model, self.effort = model, effort

    def render(self, items: list) -> list:
        out = []

        def push(role, block):
            if (
                out
                and out[-1]["role"] == role
                and not (
                    role == "user"
                    and block["type"] == "text"
                    and any(b["type"] == "tool_result" for b in out[-1]["content"])
                )
            ):
                out[-1]["content"].append(block)
            else:
                out.append({"role": role, "content": [block]})

        for it in items:
            if it["t"] == "user":
                push("user", {"type": "text", "text": it["text"]})
            elif it["t"] == "atext":
                push("assistant", {"type": "text", "text": it["text"]})
            elif it["t"] == "call":
                push("assistant", {"type": "tool_use", "id": it["id"], "name": it["name"], "input": it["args"]})
            elif it["t"] == "result":
                push(
                    "user",
                    {"type": "tool_result", "tool_use_id": it["id"], "content": it["text"], "is_error": it["err"]},
                )
        return out

    def user(self, text):
        return [{"role": "user", "content": [{"type": "text", "text": text}]}]

    def results(self, res):
        return [
            {
                "role": "user",
                "content": [{"type": "tool_result", "tool_use_id": i, "content": t, "is_error": e} for i, t, e in res],
            }
        ]

    def step(self, system, tools, messages, cache_key=None) -> Step:
        r = self.client.messages.create(
            model=self.model,
            max_tokens=MAX_TOKENS,
            output_config={"effort": self.effort},
            system=[{"type": "text", "text": system, "cache_control": {"type": "ephemeral"}}],
            cache_control={"type": "ephemeral"},
            tools=[{"name": t["name"], "description": t["description"], "input_schema": t["schema"]} for t in tools],
            tool_choice={"type": "auto"},
            messages=messages,
        )
        content = r.to_dict()["content"]
        calls = [(b["id"], b["name"], b["input"]) for b in content if b["type"] == "tool_use"]
        text = "".join(b["text"] for b in content if b["type"] == "text")
        u = r.usage
        cached, written = u.cache_read_input_tokens or 0, u.cache_creation_input_tokens or 0
        usage = {
            "prompt": u.input_tokens + cached + written,
            "cached": cached,
            "written": written,
            "output": u.output_tokens,
        }
        return Step([{"role": "assistant", "content": content}], calls, text, usage, r.stop_reason or "")


class OpenAIResponses:
    wire = "openai"

    def __init__(self, model: str, effort: str | None = None):
        import openai

        self.client = openai.OpenAI(api_key=os.environ["OPENAI_API_KEY"], max_retries=5, timeout=240)
        self.model, self.effort = model, effort

    def render(self, items):
        out = []
        for it in items:
            if it["t"] == "user":
                out.append({"role": "user", "content": it["text"]})
            elif it["t"] == "atext":
                out.append({"role": "assistant", "content": it["text"]})
            elif it["t"] == "call":
                out.append(
                    {
                        "type": "function_call",
                        "call_id": it["id"],
                        "name": it["name"],
                        "arguments": json.dumps(it["args"]),
                    }
                )
            elif it["t"] == "result":
                out.append({"type": "function_call_output", "call_id": it["id"], "output": it["text"]})
        return out

    def user(self, text):
        return [{"role": "user", "content": text}]

    def results(self, res):
        return [{"type": "function_call_output", "call_id": i, "output": t} for i, t, _ in res]

    def step(self, system, tools, messages, cache_key=None) -> Step:
        r = self.client.responses.create(
            model=self.model,
            instructions=system,
            store=False,
            input=messages,
            reasoning={"effort": self.effort} if self.effort else {},
            include=["reasoning.encrypted_content"],
            prompt_cache_key=cache_key,
            tool_choice="auto",
            tools=[
                {
                    "type": "function",
                    "name": t["name"],
                    "description": t["description"],
                    "parameters": t["schema"],
                    "strict": False,
                }
                for t in tools
            ],
        )
        items = [item.to_dict() for item in r.output]
        calls = [
            (it["call_id"], it["name"], _args(it.get("arguments"))) for it in items if it["type"] == "function_call"
        ]
        u = r.usage
        cached = getattr(getattr(u, "input_tokens_details", None), "cached_tokens", 0) or 0
        usage = {"prompt": u.input_tokens, "cached": cached, "written": 0, "output": u.output_tokens}
        stop = getattr(r, "status", "") or ""
        if getattr(r, "incomplete_details", None):
            stop = f"incomplete:{r.incomplete_details.reason}"
        return Step(items, calls, r.output_text or "", usage, stop)


class ChatCompletions:
    """Z.ai glm-5.3 over its OpenAI-compatible chat completions path (/api/paas/v4); Mistral over its own
    /v1/chat/completions (lane RB-R1, ruling 102)."""

    wire = "chat"

    def __init__(self, model: str, base_url="https://api.z.ai/api/paas/v4", key_env="Z_AI_TOKEN"):
        import openai

        self.client = openai.OpenAI(api_key=os.environ[key_env], base_url=base_url, max_retries=3, timeout=120)
        self.model = model

    def render(self, items):
        out = []
        for it in items:
            if it["t"] == "user":
                out.append({"role": "user", "content": it["text"]})
            elif it["t"] == "atext":
                out.append({"role": "assistant", "content": it["text"]})
            elif it["t"] == "call":
                call = {
                    "id": it["id"],
                    "type": "function",
                    "function": {"name": it["name"], "arguments": json.dumps(it["args"])},
                }
                if out and out[-1]["role"] == "assistant":
                    out[-1].setdefault("tool_calls", []).append(call)
                else:
                    out.append({"role": "assistant", "content": "", "tool_calls": [call]})
            elif it["t"] == "result":
                out.append({"role": "tool", "tool_call_id": it["id"], "content": it["text"]})
        return out

    def user(self, text):
        return [{"role": "user", "content": text}]

    def results(self, res):
        return [{"role": "tool", "tool_call_id": i, "content": t} for i, t, _ in res]

    def step(self, system, tools, messages, cache_key=None) -> Step:
        r = self.client.chat.completions.create(
            model=self.model,
            messages=[{"role": "system", "content": system}, *messages],
            tools=[
                {
                    "type": "function",
                    "function": {"name": t["name"], "description": t["description"], "parameters": t["schema"]},
                }
                for t in tools
            ],
            tool_choice="auto",
            max_tokens=8000,
        )
        m = r.choices[0].message
        msg = {"role": "assistant", "content": m.content or ""}
        calls = []
        if m.tool_calls:
            msg["tool_calls"] = [
                {
                    "id": c.id,
                    "type": "function",
                    "function": {"name": c.function.name, "arguments": c.function.arguments},
                }
                for c in m.tool_calls
            ]
            calls = [(c.id, c.function.name, _args(c.function.arguments)) for c in m.tool_calls]
        u = r.usage
        cached = getattr(getattr(u, "prompt_tokens_details", None), "cached_tokens", 0) or 0
        usage = {"prompt": u.prompt_tokens, "cached": cached, "written": 0, "output": u.completion_tokens}
        return Step([msg], calls, m.content or "", usage, r.choices[0].finish_reason or "")


HEADS = {
    "claude-sonnet-5-5": lambda: Anthropic("claude-sonnet-5-5"),
    "claude-haiku-5-5": lambda: Anthropic("claude-haiku-5-5"),
    "gpt-5.6-luna": lambda: OpenAIResponses("gpt-5.6-luna"),
    "gpt-5.6": lambda: OpenAIResponses("gpt-5.6"),
    "glm-5.3": lambda: ChatCompletions("glm-5.3"),
    "mistral-medium-latest": lambda: ChatCompletions(
        "mistral-medium-latest", base_url="https://api.mistral.ai/v1", key_env="MISTRAL_API_KEY"
    ),
}


KEY_ENVS = ("ANTHROPIC_API_KEY", "OPENAI_API_KEY", "Z_AI_TOKEN", "MISTRAL_API_KEY")


def redact(text: str) -> str:
    """Error text with every loaded key value and anything key-shaped masked, before it is written anywhere."""
    for k in KEY_ENVS:
        v = os.environ.get(k)
        if v and len(v) >= 8:
            text = text.replace(v, "[key]")
    return re.sub(r"(sk-[A-Za-z0-9_\-]{2})[A-Za-z0-9_\-*.]+", r"\1[masked]", text)


def load_env(path: str):
    """Put the provider keys this eval uses into os.environ; values never leave the process. Several files may be
    joined by commas: the FIRST file that names a key wins it (as an exported variable would)."""
    wanted = set(KEY_ENVS)
    for one in [p for p in path.split(",") if p]:
        _load_one(one, wanted)


def _load_one(path: str, wanted: set):
    with open(path) as f:
        for line in f:
            s = line.strip()
            if s.startswith("#") or "=" not in s:
                continue
            k, v = s.removeprefix("export ").split("=", 1)
            k = k.strip()
            if k in wanted:
                os.environ.setdefault(k, v.strip().strip("'\""))
