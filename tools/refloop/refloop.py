#!/usr/bin/env python3
"""refloop - the reference loop: the simplest competent agent loop.

It is the measuring stick for the Nimbus harness: same model, same tools (nimbusd's own
registry over its MCP endpoint), and none of the harness's machinery. What it does:

  * a short fixed system prompt: who the assistant is, the date and time, the channel;
  * the whole conversation kept verbatim as the provider's own messages (owner text
    and images, assistant text, thinking and tool calls, tool results), append only;
  * native tool calling, tool choice auto, looping until the model answers without a call;
  * prompt caching over the growing history;
  * one summary of the oldest half, only when the prompt passes about 150k tokens;
  * the transcript saved after every step, so a restart reloads it.

Run it by hand against a running nimbusd:
  refloop.py --mcp http://127.0.0.1:8787 --token <web token> --model claude-sonnet-5-5
Keys come from the environment (ANTHROPIC_API_KEY, OPENAI_API_KEY) or --env-file.
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

SYSTEM = (
    "You are Nimbus, a personal AI assistant with long-term memory, files and routines, working for one "
    "person: your owner. This conversation started on {date} at {time} ({tz}) and reaches you over "
    "{channel}. Each owner message starts with the time it was sent.\n"
    "Use your tools when they help, and trust their results over your own recollection. Say plainly what "
    "you did; never claim an action that no tool call made. Lead with the answer and keep replies short. "
    "Light Markdown is fine."
)
MAX_ROUNDS = 40             # provider requests in one owner turn before the loop gives up
SUMMARIZE_AT = 150_000      # prompt tokens: past this, the oldest half becomes one summary
MAX_TOKENS = 16000
SUMMARY_ASK = (
    "Summarize everything above for your own later use: facts about the owner, decisions, numbers, names, "
    "files and routines touched, what was asked and what was answered, and anything still open. "
    "Do not call tools."
)
# Registry tools the loop does not offer: they hand an image back to the head, which an MCP
# text result cannot carry, and the loop keeps every photo in the conversation anyway.
SKIP_TOOLS = ("image.look", "image.note")


@dataclass
class Step:
    """One provider answer: the items to append verbatim, the tool calls in them, the text."""
    items: list
    calls: list          # (call id, tool name, arguments)
    text: str
    prompt_tokens: int   # the whole prompt of this request, cached part included


# ------------------------------------------------------------------------- tools
class McpTools:
    """nimbusd's tool registry over POST /mcp (tools/list, tools/call)."""

    def __init__(self, url: str, token: str):
        self.url, self.token = url.rstrip("/") + "/mcp", token
        self.dotted = {}

    def _rpc(self, method: str, params: dict) -> dict:
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
        for _ in range(240):     # 503 is the daemon's "engine busy": wait it out
            req = urllib.request.Request(self.url, data=body, method="POST")
            req.add_header("X-Nimbus-Token", self.token)
            req.add_header("Content-Type", "application/json")
            try:
                with urllib.request.urlopen(req, timeout=60) as r:
                    return json.loads(r.read())["result"]
            except urllib.error.HTTPError as e:
                if e.code != 503:
                    raise
                time.sleep(0.5)
        raise RuntimeError("nimbusd stayed busy")

    def specs(self) -> list:
        """Every registry tool as {name, description, schema}. Providers refuse a dot in a name."""
        out = []
        for t in self._rpc("tools/list", {})["tools"]:
            if t["name"] in SKIP_TOOLS:
                continue
            schema = t.get("inputSchema") or {}
            schema = schema if schema.get("type") == "object" else {"type": "object", "properties": {}}
            out.append({"name": t["name"].replace(".", "_"), "description": t["description"], "schema": schema})
            self.dotted[out[-1]["name"]] = t["name"]
        return out

    def call(self, name: str, args: dict) -> tuple:
        """(text, is_error), exactly as the registry answers."""
        if not self.dotted:
            self.specs()
        result = self._rpc("tools/call", {"name": self.dotted.get(name, name), "arguments": args})
        text = "\n".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")
        return text, bool(result.get("isError"))


# -------------------------------------------------------------------- providers
class Anthropic:
    """claude-sonnet-5-5 and claude-haiku-5-5 over the Messages API, with prompt caching."""

    def __init__(self, model: str, effort: str = "medium", api_key: str | None = None, base_url: str | None = None):
        import anthropic
        self.client = anthropic.Anthropic(api_key=api_key, base_url=base_url)
        self.model, self.effort = model, effort

    def user(self, text: str, images=()) -> list:
        blocks = [{"type": "image", "source": {"type": "base64", "media_type": mt, "data": b64}} for mt, b64 in images]
        return [{"role": "user", "content": [*blocks, {"type": "text", "text": text}]}]

    def step(self, conv, messages=None) -> Step:
        r = self.client.messages.create(
            model=self.model, max_tokens=MAX_TOKENS,
            # Thinking stays adaptive (the default); effort is the one knob, as in the harness.
            output_config={"effort": self.effort},
            # Two cache points: the fixed tools + system prefix, and the end of the history.
            system=[{"type": "text", "text": conv.system, "cache_control": {"type": "ephemeral"}}],
            cache_control={"type": "ephemeral"},
            tools=[{"name": s["name"], "description": s["description"], "input_schema": s["schema"]}
                   for s in conv.specs],
            messages=conv.messages if messages is None else messages,
        )
        content = r.to_dict()["content"]     # the blocks as served: thinking and signatures intact
        calls = [(b["id"], b["name"], b["input"]) for b in content if b["type"] == "tool_use"]
        text = "".join(b["text"] for b in content if b["type"] == "text")
        if r.stop_reason in ("max_tokens", "refusal"):
            text += f"\n(stopped early: {r.stop_reason})"
        u = r.usage
        prompt = u.input_tokens + (u.cache_read_input_tokens or 0) + (u.cache_creation_input_tokens or 0)
        return Step([{"role": "assistant", "content": content}], calls, text, prompt)

    def results(self, results: list) -> list:
        return [{"role": "user", "content": [
            {"type": "tool_result", "tool_use_id": i, "content": text, "is_error": err} for i, text, err in results]}]

    @staticmethod
    def is_owner(item: dict) -> bool:
        return item.get("role") == "user" and not any(b.get("type") == "tool_result" for b in item["content"])

    @staticmethod
    def without_thinking(items: list) -> list:
        """A thinking block is bound to the history before it: once that is summarized, it must go."""
        out = []
        for m in items:
            if m.get("role") == "assistant":
                kept = [b for b in m["content"] if b.get("type") not in ("thinking", "redacted_thinking")]
                m = {**m, "content": kept or [{"type": "text", "text": "(no text)"}]}
            out.append(m)
        return out


class OpenAI:
    """gpt-5.6-luna over the Responses API, stateless: the loop owns the whole history."""

    def __init__(self, model: str, effort: str = "low", api_key: str | None = None, base_url: str | None = None):
        import openai
        self.client = openai.OpenAI(api_key=api_key, base_url=base_url)
        self.model, self.effort = model, effort

    def user(self, text: str, images=()) -> list:
        parts = [{"type": "input_image", "image_url": f"data:{mt};base64,{b64}"} for mt, b64 in images]
        return [{"role": "user", "content": [*parts, {"type": "input_text", "text": text}]}]

    def step(self, conv, messages=None) -> Step:
        r = self.client.responses.create(
            model=self.model, instructions=conv.system, store=False,
            input=conv.messages if messages is None else messages,
            reasoning={"effort": self.effort}, include=["reasoning.encrypted_content"],
            prompt_cache_key=conv.cache_key,
            tools=[{"type": "function", "name": s["name"], "description": s["description"],
                    "parameters": s["schema"], "strict": False} for s in conv.specs],
        )
        items = [item.to_dict() for item in r.output]   # reasoning, function_call and message items
        calls = []
        for item in items:
            if item["type"] == "function_call":
                try:
                    calls.append((item["call_id"], item["name"], json.loads(item["arguments"] or "{}")))
                except ValueError:
                    calls.append((item["call_id"], item["name"], {}))
        return Step(items, calls, r.output_text, r.usage.input_tokens)

    def results(self, results: list) -> list:
        return [{"type": "function_call_output", "call_id": i, "output": text} for i, text, _ in results]

    @staticmethod
    def is_owner(item: dict) -> bool:
        return item.get("role") == "user"

    @staticmethod
    def without_thinking(items: list) -> list:
        return items     # an encrypted reasoning item stands on its own


def provider_for(model: str, **kw):
    return (Anthropic if model.startswith("claude") else OpenAI)(model, **kw)


# ----------------------------------------------------------------- conversation
class Conversation:
    """The state of one conversation: system prompt, tool list and messages, saved after every step."""

    def __init__(self, specs: list, path: Path | None = None, channel: str = "the web chat",
                 tz: str = "Europe/London"):
        now = datetime.now(ZoneInfo(tz))
        self.system = SYSTEM.format(date=now.strftime("%A %d %B %Y"), time=now.strftime("%H:%M"), tz=tz,
                                    channel=channel)
        self.specs, self.messages, self.tz = specs, [], tz
        self.path = Path(path) if path else None
        self.prompt_tokens = 0
        self.cache_key = f"refloop-{int(now.timestamp())}"

    def stamp(self, text: str) -> str:
        return datetime.now(ZoneInfo(self.tz)).strftime("[%a %d %b %Y %H:%M] ") + text

    def save(self):
        if self.path:
            tmp = self.path.with_suffix(".tmp")
            tmp.write_text(json.dumps(self.__dict__ | {"path": str(self.path)}, ensure_ascii=False))
            tmp.replace(self.path)

    @classmethod
    def load(cls, path: Path):
        conv = cls.__new__(cls)
        conv.__dict__.update(json.loads(Path(path).read_text()))
        conv.path = Path(path)
        return conv


# --------------------------------------------------------------------- the loop
def run_turn(conv: Conversation, provider, tools, texts: list, images=(), on_tool=None) -> str:
    """One owner turn: append what the owner sent, then call the model until it answers without a tool call."""
    for n, text in enumerate(texts):
        conv.messages += provider.user(conv.stamp(text), images if n == len(texts) - 1 else ())
    for _ in range(MAX_ROUNDS):
        step = provider.step(conv)
        conv.messages += step.items
        conv.prompt_tokens = step.prompt_tokens
        conv.save()
        if not step.calls:
            if conv.prompt_tokens > SUMMARIZE_AT:
                summarize_oldest_half(conv, provider)
            return step.text
        results = []
        for call_id, name, args in step.calls:
            text, is_error = tools.call(name, args)
            results.append((call_id, text, is_error))
            if on_tool:
                on_tool(name, args, text, is_error)
        conv.messages += provider.results(results)
        conv.save()
    return "(stopped: too many tool rounds in one turn)"


def summarize_oldest_half(conv: Conversation, provider):
    """Replace the oldest half of the history, cut at an owner message, with one summary message."""
    starts = [i for i, item in enumerate(conv.messages) if provider.is_owner(item)]
    if len(starts) < 2:
        return
    cut = starts[len(starts) // 2]
    summary = provider.step(conv, conv.messages[:cut] + provider.user(SUMMARY_ASK)).text
    head = provider.user("[Summary of the conversation before this point]\n" + summary)
    conv.messages = head + provider.without_thinking(conv.messages[cut:])
    conv.save()


def serve(conv: Conversation, provider, tools, inbox: queue.Queue, deliver, on_tool=None):
    """The channel side: one turn at a time; messages that arrive during a turn ride the next one together."""
    while True:
        batch = [inbox.get()]
        while batch[-1] is not None and not inbox.empty():
            batch.append(inbox.get())
        if batch[-1] is None:      # None closes the channel
            return
        deliver(run_turn(conv, provider, tools, batch, on_tool=on_tool))


# -------------------------------------------------------------------------- cli
def main():
    ap = argparse.ArgumentParser(description="Chat with the reference loop over a running nimbusd's tools.")
    ap.add_argument("--mcp", required=True, help="nimbusd control surface, for example http://127.0.0.1:8787")
    ap.add_argument("--token", default=os.environ.get("NIMBUSD_WEB_TOKEN", ""), help="nimbusd web token")
    ap.add_argument("--model", default="claude-sonnet-5-5")
    ap.add_argument("--effort", default=None, help="provider effort (default: medium on Claude, low on GPT)")
    ap.add_argument("--transcript", default="refloop-transcript.json", help="saved after every step, reloaded if present")
    ap.add_argument("--env-file", help="dotenv file holding the provider key")
    args = ap.parse_args()
    if args.env_file:
        for line in Path(args.env_file).read_text().splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.strip().removeprefix("export ").split("=", 1)
                os.environ.setdefault(k.strip(), v.strip().strip("'\""))
    provider = provider_for(args.model, **({"effort": args.effort} if args.effort else {}))
    tools = McpTools(args.mcp, args.token)
    path = Path(args.transcript)
    conv = Conversation.load(path) if path.exists() else Conversation(tools.specs(), path)
    print(f"refloop on {args.model}, {len(conv.specs)} tools, {len(conv.messages)} messages loaded. Ctrl-D ends.")
    while True:
        try:
            text = input("> ").strip()
        except EOFError:
            return
        if text:
            print(run_turn(conv, provider, tools, [text],
                           on_tool=lambda name, a, out, err: print(f"  [{name}] {json.dumps(a)[:100]}")))


if __name__ == "__main__":
    main()
