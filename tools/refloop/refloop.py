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
import uuid
from dataclasses import dataclass
from datetime import datetime, timedelta
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
MAX_ROUNDS = 40  # provider requests in one owner turn before the loop gives up
SUMMARIZE_AT = 150_000  # prompt tokens: past this, the oldest half becomes one summary
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
    calls: list  # (call id, tool name, arguments)
    text: str
    prompt_tokens: int  # the whole prompt of this request, cached part included


# ------------------------------------------------------------------------- tools
class McpTools:
    """nimbusd's tool registry over POST /mcp (tools/list, tools/call)."""

    def __init__(self, url: str, token: str):
        self.url, self.token = url.rstrip("/") + "/mcp", token
        self.dotted = {}

    def _rpc(self, method: str, params: dict) -> dict:
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
        for _ in range(240):  # 503 is the daemon's "engine busy": wait it out
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
            model=self.model,
            max_tokens=MAX_TOKENS,
            # Thinking stays adaptive (the default); effort is the one knob, as in the harness.
            output_config={"effort": self.effort},
            # Two cache points: the fixed tools + system prefix, and the end of the history.
            system=[{"type": "text", "text": conv.system, "cache_control": {"type": "ephemeral"}}],
            cache_control={"type": "ephemeral"},
            tools=[
                {"name": s["name"], "description": s["description"], "input_schema": s["schema"]} for s in conv.specs
            ],
            messages=conv.messages if messages is None else messages,
        )
        content = r.to_dict()["content"]  # the blocks as served: thinking and signatures intact
        calls = [(b["id"], b["name"], b["input"]) for b in content if b["type"] == "tool_use"]
        text = "".join(b["text"] for b in content if b["type"] == "text")
        if r.stop_reason in ("max_tokens", "refusal"):
            text += f"\n(stopped early: {r.stop_reason})"
        u = r.usage
        prompt = u.input_tokens + (u.cache_read_input_tokens or 0) + (u.cache_creation_input_tokens or 0)
        return Step([{"role": "assistant", "content": content}], calls, text, prompt)

    def results(self, results: list) -> list:
        return [
            {
                "role": "user",
                "content": [
                    {"type": "tool_result", "tool_use_id": i, "content": text, "is_error": err}
                    for i, text, err in results
                ],
            }
        ]

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

    def __init__(self, model: str, effort: str | None = None, api_key: str | None = None, base_url: str | None = None):
        import openai

        self.client = openai.OpenAI(api_key=api_key, base_url=base_url)
        self.model, self.effort = model, effort

    def user(self, text: str, images=()) -> list:
        parts = [{"type": "input_image", "image_url": f"data:{mt};base64,{b64}"} for mt, b64 in images]
        return [{"role": "user", "content": [*parts, {"type": "input_text", "text": text}]}]

    def step(self, conv, messages=None) -> Step:
        r = self.client.responses.create(
            model=self.model,
            instructions=conv.system,
            store=False,
            input=conv.messages if messages is None else messages,
            # No effort field unless asked for: the provider default, which is what the harness uses.
            reasoning={"effort": self.effort} if self.effort else {},
            include=["reasoning.encrypted_content"],
            prompt_cache_key=conv.cache_key,
            tools=[
                {
                    "type": "function",
                    "name": s["name"],
                    "description": s["description"],
                    "parameters": s["schema"],
                    "strict": False,
                }
                for s in conv.specs
            ],
        )
        items = [item.to_dict() for item in r.output]  # reasoning, function_call and message items
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
        return items  # an encrypted reasoning item stands on its own


def _json_args(raw) -> dict:
    try:
        return json.loads(raw or "{}") if isinstance(raw, str) else (raw or {})
    except ValueError:
        return {}


def _content_text(content) -> str:
    """A Chat Completions message's text: a string, or the text of its content parts."""
    if isinstance(content, list):
        return "".join(p.get("text", "") for p in content if isinstance(p, dict))
    return content or ""


class MistralChat:
    """A Mistral head over its Chat Completions API (lane RB-R1, ruling 102), stateless like the OpenAI arm: the loop
    owns the whole history. The wire the harness itself uses for Mistral (lib/harness/src/providers/mistral.cpp:
    non-streamed /v1/chat/completions with tools). The effort is the one the harness sends for the model
    (mistralHeadEffort: "low" for Large 4, none for Medium), as reasoning_effort, or no field at all."""

    def __init__(self, model: str, effort: str | None = None, api_key: str | None = None, base_url: str | None = None):
        import openai  # the Chat Completions client; Mistral's endpoint takes the same body

        self.client = openai.OpenAI(api_key=api_key, base_url=base_url or "https://api.mistral.ai/v1")
        self.model, self.effort = model, effort

    def user(self, text: str, images=()) -> list:
        parts = [{"type": "image_url", "image_url": f"data:{mt};base64,{b64}"} for mt, b64 in images]
        return [{"role": "user", "content": [*parts, {"type": "text", "text": text}] if parts else text}]

    def step(self, conv, messages=None) -> Step:
        r = self.client.chat.completions.create(
            model=self.model,
            max_tokens=MAX_TOKENS,
            messages=[{"role": "system", "content": conv.system}, *(conv.messages if messages is None else messages)],
            tools=[
                {
                    "type": "function",
                    "function": {"name": s["name"], "description": s["description"], "parameters": s["schema"]},
                }
                for s in conv.specs
            ],
            extra_body={"reasoning_effort": self.effort} if self.effort else None,
        )
        msg = r.choices[0].message.to_dict()
        msg = {k: v for k, v in msg.items() if k in ("role", "content", "tool_calls") and v is not None}
        calls = [
            (c["id"], c["function"]["name"], _json_args(c["function"]["arguments"]))
            for c in msg.get("tool_calls") or []
        ]
        text = _content_text(msg.get("content"))
        if r.choices[0].finish_reason == "length":
            text += "\n(stopped early: length)"
        return Step([msg], calls, text, r.usage.prompt_tokens if r.usage else 0)

    def results(self, results: list) -> list:
        return [{"role": "tool", "tool_call_id": i, "content": text} for i, text, _ in results]

    @staticmethod
    def is_owner(item: dict) -> bool:
        return item.get("role") == "user"

    @staticmethod
    def without_thinking(items: list) -> list:
        return items


def provider_for(model: str, **kw):
    if model.startswith(("mistral", "magistral")):
        return MistralChat(model, **kw)
    return (Anthropic if model.startswith("claude") else OpenAI)(model, **kw)


DAY_STARTS_MIN = 4 * 60  # the device's "day starts at" default (lib/core day_start.h)


def message_stamp(when: datetime, day_start_min: int = DAY_STARTS_MIN) -> str:
    """The device's message stamp (RB-REC5, ruling 64), so both arms see the same bytes.

    "[2026-11-19 Thu 08:10]" at or after the day start; "[2026-11-19 Thu 00:30 (day 2026-11-18
    until 04:00)]" before it, where the running day is still yesterday. Weekday names are the
    C locale's, as on the device.
    """
    days = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
    plain = f"{when:%Y-%m-%d} {days[when.weekday()]} {when:%H:%M}"
    minute = when.hour * 60 + when.minute
    if day_start_min and minute < day_start_min:
        running = (when - timedelta(days=1)).strftime("%Y-%m-%d")
        plain += f" (day {running} until {day_start_min // 60:02d}:{day_start_min % 60:02d})"
    return f"[{plain}]"


# ----------------------------------------------------------------- conversation
class Conversation:
    """The state of one conversation: system prompt, tool list and messages, saved after every step.

    Its clock is the host clock plus clock_offset_s, which only the rig moves (advance), so a
    scenario's virtual time reaches the stamps exactly as it reaches nimbusd. chat_key names the
    conversation; it is the provider's cache key, so two conversations never share one.
    """

    clock_offset_s = 0  # transcripts saved before the virtual clock existed load at offset 0
    CHANNEL = "the web chat"
    TZ = "Europe/London"

    def __init__(self, specs: list, path: Path | None = None, note: str = "", chat_key: str = "", clock_offset_s=0):
        self.tz, self.clock_offset_s = self.TZ, int(clock_offset_s)
        now = self.now()
        self.system = SYSTEM.format(
            date=now.strftime("%A %d %B %Y"), time=now.strftime("%H:%M"), tz=self.tz, channel=self.CHANNEL
        ) + (f"\n{note}" if note else "")  # note: for experiments
        self.specs, self.messages = specs, []
        self.path = Path(path) if path else None
        self.prompt_tokens = 0
        self.cache_key = f"refloop-{chat_key}" if chat_key else f"refloop-{uuid.uuid4().hex[:12]}"

    def now(self) -> datetime:
        return datetime.now(ZoneInfo(self.tz)) + timedelta(seconds=self.clock_offset_s)

    def advance(self, seconds: int):
        """Move this conversation's clock forward (the rig's advance step), and save it."""
        if seconds < 0:
            raise ValueError("the clock only moves forward")
        self.clock_offset_s += int(seconds)
        self.save()

    def stamp(self, text: str) -> str:
        return f"{message_stamp(self.now())} {text}"

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
        texts = [text for text in batch if text is not None]
        if texts:
            deliver(run_turn(conv, provider, tools, texts, on_tool=on_tool))
        if batch[-1] is None:  # None closes the channel, after what was queued before it is answered
            return


# ------------------------------------------------- engine-2 transcript (emulated)
# PRD section 2, as a renderer over a neutral item list, for measuring what the design costs
# before it exists in firmware (tests/evals/replay). The constants are the PRD's.
PROFILES = {  # name -> (H, L, digest block bytes)
    "tight": (24_000, 12_000, 2_048),
    "lean": (40_000, 20_000, 4_096),
    "standard": (60_000, 30_000, 6_144),
    "wide": (100_000, 50_000, 6_144),
    # Lane RB-T4 (owner ruling 82): full, for claude-sonnet-5-5. Due at 96,000 prompt tokens (the
    # 100,000-token aim less the next request's growth), keeping 70,000 (tx::budgetFor). In this
    # emulation's resident unit the marks come from RB-M0b's aged replays, where the first request of
    # a photo-free turn counted tokens = 6,288 + 0.538 x resident (124 turns): 96,000 and 70,000
    # tokens are about 166,000 and 118,000 B. The byte mark is the backstop; the token rule is first.
    "full": (166_000, 118_000, 6_144),
}
FULL_DUE_TOKENS = 96_000  # compaction is due once a request reached this many prompt tokens
COLD_OPEN_S = 270  # a turn opening this long after the last request moves the stub pointer
RECEIPT_MAX = 2_048  # internal results up to this stay whole until compacted (tier 1)
LEDGER_RECEIPT_MAX = 4_096
LEDGER_TOOLS = ("ledger_write", "ledger_day", "ledger_add", "ledger_update", "ledger_remove", "ledger_target")
OUTSIDE_TOOLS = ("web_search", "fetch_url")
WARM_BULK_MAX = 24_576  # whole internal bulk above the pointer that moves it on a warm open
FILE_WRITE_TOOLS = ("write_file", "append_file", "edit_file")
PHOTO_TURNS, PHOTO_MAX, PHOTO_IDLE_S = 2, 4, 3_600  # file-reference heads
DIGEST_MAX = 768
KEEP_NEWEST_TURNS = 6  # compaction never takes the newest owner turns
LEDGER_DAY_START_H = 4


def _one_line(text: str, n: int) -> str:
    return " ".join(text.split())[:n]


class E2Transcript:
    """One chat's engine-2 transcript: append-only items, rendered per request by pure rules.

    Items: u (owner message: stamped text, photos), a (assistant content as served), r (tool
    results). Rendering applies the stub pointer, the photo retention rule, closed-turn thinking
    (dropped unless replay_thinking) and the digests through thru. Every rule that changes the
    rendered prefix is counted in self.changes so a run can report its cache change points.
    """

    def __init__(
        self,
        profile: str = "standard",
        replay_thinking: bool = False,
        tz: str = "Europe/London",
        warm_bulk_max: int = WARM_BULK_MAX,
    ):
        self.H, self.L, self.digest_cap = PROFILES[profile]
        self.due_tokens = FULL_DUE_TOKENS if profile == "full" else None
        self.last_prompt = 0  # the newest request's prompt tokens (note_prompt)
        self.warm_bulk_max = warm_bulk_max  # 8,192 B on an 85,000 B head
        self.profile, self.replay_thinking, self.tz = profile, replay_thinking, ZoneInfo(tz)
        self.items, self.q, self.turn = [], 0, 0
        self.pointer = 0  # results at or below this q render as stubs (when bulk)
        self.thru = 0  # items at or below this q are covered by digests
        self.digests, self.archived = [], []  # [(day, text)]
        self.visible_photos = set()
        self.last_req_end = None  # virtual epoch seconds
        self.changes = {}

    # ------------------------------------------------------------ appending
    def _add(self, kind: str, **kw) -> dict:
        self.q += 1
        item = {"q": self.q, "turn": self.turn, "k": kind, **kw}
        self.items.append(item)
        return item

    def _change(self, what: str):
        self.changes[what] = self.changes.get(what, 0) + 1

    def open_turn(self, now: float, text: str, photos=()) -> dict:
        """A new owner turn at virtual time now. photos: [(media type, base64, ref)]."""
        gap = None if self.last_req_end is None else now - self.last_req_end
        cold = gap is None or gap > COLD_OPEN_S
        before = self._stubbed_ids()
        if cold or self._whole_bulk_above_pointer() > self.warm_bulk_max:
            self.pointer = max(self.pointer, self.q)
        if not self.replay_thinking and any(
            b.get("type") == "thinking"
            for it in self.items
            if it["k"] == "a" and it["turn"] == self.turn
            for b in it["content"]
        ):
            self._change("thinking_dropped")
        self.turn += 1
        causes = {self._stub_cause(r) for r in self.items if r["k"] == "r" and r["q"] in self._stubbed_ids() - before}
        for cause in sorted(causes):
            self._change(cause)
        self._add("u", text=text, photos=list(photos), t=now)
        self._retain_photos(gap)
        return {"cold": cold, "gap_s": gap, "pointer_moved": "pointer" in causes}

    def add_assistant(self, content: list):
        self._add("a", content=content)

    def add_results(self, results: list, now: float):
        """results: [(call id, tool name, args, text or content list, is_error)]."""
        for cid, name, args, out, err in results:
            text = out if isinstance(out, str) else json.dumps(out)
            if not err:
                self._supersede(name, args if isinstance(args, dict) else {}, now)
            self._add("r", id=cid, n=name, args=args, out=out, err=bool(err), t=now, size=len(text.encode()))

    def _day_of(self, day, t: float) -> str:
        if isinstance(day, str) and len(day) == 10 and day[4] == "-":
            return day
        back = 1 if day == "yesterday" else 0
        return self._ledger_day(t - back * 86400)

    def _supersede(self, name: str, args: dict, now: float):
        """A successful write marks every earlier whole read of its path (a ledger write: every earlier
        view of its day) superseded at the close of this turn."""
        if name in FILE_WRITE_TOOLS and args.get("path"):
            path = args["path"]

            def hit(r):
                return r["n"] == "read_file" and isinstance(r["args"], dict) and r["args"].get("path") == path

        elif name == "ledger_write":
            days = {self._day_of(op.get("day"), now) for op in args.get("ops", []) if isinstance(op, dict)}

            def hit(r):
                return r["n"] == "ledger_day" and self._day_of((r["args"] or {}).get("day"), r["t"]) in days

        else:
            return
        for r in self.items:
            if r["k"] == "r" and "sup" not in r and not r["err"] and hit(r):
                r["sup"] = self.turn

    def mark_request(self, end: float):
        self.last_req_end = end

    def seed(self, history: list, digests: list):
        """Aged state: closed text-only turns [(t, owner text, reply text)] and digests [(day, text)],
        as if earlier days had been compacted into the block and recent ones kept verbatim."""
        for t, owner, reply in history:
            self.turn += 1
            self._add("u", text=owner, photos=[], t=t)
            self._add("a", content=[{"type": "text", "text": reply}])
            self.last_req_end = t + 10
        self.pointer = self.q
        self.digests = list(digests)
        while self.digests and len(self.digest_block().encode()) > self.digest_cap:
            self.archived.append(self.digests.pop(0))

    # ------------------------------------------------------------ rules
    def _is_bulk(self, r: dict) -> bool:
        limit = LEDGER_RECEIPT_MAX if r["n"] in LEDGER_TOOLS else RECEIPT_MAX
        return r["n"] in OUTSIDE_TOOLS or r["size"] > limit or not isinstance(r["out"], str)

    def _stubbed(self, r: dict) -> bool:
        return self._stub_cause(r) is not None

    def _stub_cause(self, r: dict):
        if r["n"] in OUTSIDE_TOOLS and r["turn"] < self.turn:
            return "outside"  # outside results are stubs from the close of their own turn
        if r.get("sup") is not None and r["sup"] < self.turn:
            return "superseded"
        if self._is_bulk(r) and r["q"] <= self.pointer:
            return "pointer"
        return None

    def _stubbed_ids(self) -> set:
        return {r["q"] for r in self.items if r["k"] == "r" and self._stubbed(r)}

    def _whole_bulk_above_pointer(self) -> int:
        return sum(
            r["size"]
            for r in self.items
            if r["k"] == "r" and r["q"] > self.pointer and r["n"] not in OUTSIDE_TOOLS and self._is_bulk(r)
        )

    def stub_text(self, r: dict) -> str:
        hhmm = datetime.fromtimestamp(r["t"], self.tz).strftime("%H:%M")
        if r["n"] in OUTSIDE_TOOLS:
            args = _one_line(json.dumps(r["args"], ensure_ascii=False), 120)
            return f"[stub {r['n']}({args}) r{r['q']} {r['size']}B outside content, not shown: view id=r{r['q']}]"
        text = r["out"] if isinstance(r["out"], str) else "(image)"
        lines = text.count("\n") + 1
        head = _one_line(text, 200)
        return f"[stub {r['n']} r{r['q']} {r['size']}B {lines}L {hhmm}] {head} ... (full: view id=r{r['q']})"

    def _retain_photos(self, gap):
        """Photos of the last two photo-carrying owner turns, at most 4, for the next two owner
        turns; none after 60 minutes idle. Monotonic: a photo that left view never returns."""
        photo_turns = [u for u in self.items if u["k"] == "u" and u["photos"]][-PHOTO_TURNS:]
        keep = []
        for u in reversed(photo_turns):
            if self.turn - u["turn"] > PHOTO_TURNS:
                continue
            if u["turn"] != self.turn and gap is not None and gap > PHOTO_IDLE_S:
                continue
            keep += [ref for _, _, ref in reversed(u["photos"])]
        keep = set(keep[:PHOTO_MAX])
        current = {ref for _, _, ref in self.items[-1]["photos"]}
        new_visible = {ref for ref in keep if ref in current or ref in self.visible_photos}
        if self.visible_photos - new_visible:
            self._change("photo_left")
        self.visible_photos = new_visible

    # ------------------------------------------------------------ rendering (Anthropic Messages)
    def _photo_stub(self, u: dict, ref: str) -> str:
        when = datetime.fromtimestamp(u["t"], self.tz).strftime("%d %b %Y %H:%M")
        return f"[photo p{ref} {when} not shown: view id=p{ref}]"

    def _user_blocks(self, it: dict) -> list:
        blocks = [
            {"type": "image", "source": {"type": "base64", "media_type": mt, "data": b64}}
            if ref in self.visible_photos
            else {"type": "text", "text": self._photo_stub(it, ref)}
            for mt, b64, ref in it["photos"]
        ]
        return blocks + [{"type": "text", "text": it["text"]}]

    def _assistant_blocks(self, it: dict) -> list:
        content = it["content"]
        if it["turn"] < self.turn and not self.replay_thinking:
            content = [b for b in content if b.get("type") not in ("thinking", "redacted_thinking")]
        return content or [{"type": "text", "text": "(no text)"}]

    def _result_blocks(self, it: dict) -> list:
        body = self.stub_text(it) if self._stubbed(it) else it["out"]
        return [{"type": "tool_result", "tool_use_id": it["id"], "content": body, "is_error": it["err"]}]

    def render_anthropic(self) -> list:
        """The messages of the next request. An owner message always starts a message of its own."""
        msgs, last_prev = [], None  # last_prev: index of the last message of the previous turn
        for it in self.items:
            if it["q"] <= self.thru:
                continue
            role = "assistant" if it["k"] == "a" else "user"
            if it["k"] == "u":
                last_prev = len(msgs) - 1 if msgs else None
            blocks = {"u": self._user_blocks, "a": self._assistant_blocks, "r": self._result_blocks}[it["k"]](it)
            if msgs and msgs[-1]["role"] == role and it["k"] != "u":
                msgs[-1]["content"].extend(blocks)
            else:
                msgs.append({"role": role, "content": list(blocks)})
        if last_prev is not None and last_prev >= 0:  # cache point: the last block of the previous turn
            blk = dict(msgs[last_prev]["content"][-1])
            if blk.get("type") not in ("thinking", "redacted_thinking"):
                blk["cache_control"] = {"type": "ephemeral"}
                msgs[last_prev]["content"][-1] = blk
        return msgs

    def view(self, ident: str):
        """(content, is_error) for view id=r<q> (the stored result) or p<ref> (the photo)."""
        ident = str(ident).strip()
        for it in self.items:
            if it["k"] == "r" and ident == f"r{it['q']}":
                return it["out"], False
            if it["k"] == "u":
                for mt, b64, ref in it["photos"]:
                    if ident == f"p{ref}":
                        return [{"type": "image", "source": {"type": "base64", "media_type": mt, "data": b64}}], False
        return f"error: no stored result or photo {ident}", True

    # ------------------------------------------------------------ resident bound and digests
    def item_bytes(self, it: dict) -> int:
        if it["k"] == "u":
            return len(it["text"].encode()) + sum(len(self._photo_stub(it, r).encode()) for _, _, r in it["photos"])
        if it["k"] == "a":
            return sum(
                len((b.get("text") or json.dumps(b.get("input", ""))).encode())
                for b in it["content"]
                if b.get("type") in ("text", "tool_use")
            )
        return len(self.stub_text(it).encode()) if (self._stubbed(it) or self._is_bulk(it)) else it["size"]

    def resident(self) -> int:
        """Tier 1 plus stubs since thru (bulk counts as its stub: it leaves view at the next cold open)."""
        return sum(self.item_bytes(it) for it in self.items if it["q"] > self.thru)

    def digest_block(self) -> str:
        if not self.digests:
            return ""
        return "Earlier days (digests):\n" + "\n".join(f"{day}: {text}" for day, text in self.digests)

    def _ledger_day(self, t: float) -> str:
        return (datetime.fromtimestamp(t, self.tz) - timedelta(hours=LEDGER_DAY_START_H)).strftime("%Y-%m-%d")

    def note_prompt(self, tokens: int):
        """The newest request's prompt tokens, as the provider counted them (the hot trigger reads it)."""
        self.last_prompt = tokens

    def _due(self) -> bool:
        hot = self.due_tokens is not None and self.last_prompt >= self.due_tokens
        return hot or self.resident() > self.H

    def compaction_slice(self, now: float) -> list:
        """Whole turns from thru forward until the rest is at or under L, none of the newest six owner
        turns and none of the current ledger day's turns (unless that day alone is over H: then its
        oldest half). [] when nothing is due: resident at or under H and, at full, the last request
        under the due mark in tokens."""
        if not self._due():
            return []
        turns = sorted({it["turn"] for it in self.items if it["q"] > self.thru})
        protected = set(turns[-KEEP_NEWEST_TURNS:])
        today = self._ledger_day(now)
        day_turns = [t for t in turns if self._ledger_day(self._turn_time(t)) == today]
        if sum(self._turn_bytes(t) for t in day_turns) > self.H:
            day_turns = day_turns[len(day_turns) // 2 :]
        protected |= set(day_turns)
        rest, picked = self.resident(), []
        for t in turns:
            if rest <= self.L or t in protected:
                break
            picked.append(t)
            rest -= self._turn_bytes(t)
        return picked

    def _turn_time(self, turn: int) -> float:
        return next(it["t"] for it in self.items if it["turn"] == turn and it["k"] == "u")

    def _turn_bytes(self, turn: int) -> int:
        return sum(self.item_bytes(it) for it in self.items if it["turn"] == turn and it["q"] > self.thru)

    @staticmethod
    def _digest_line(it: dict) -> str:
        """One item as the summarizer sees it: words verbatim, every result as a one-line receipt."""
        if it["k"] == "u":
            return it["text"] + "".join(f" [photo p{r}]" for _, _, r in it["photos"])
        if it["k"] == "a":
            words = " ".join(b["text"] for b in it["content"] if b.get("type") == "text" and b.get("text"))
            return f"assistant: {words}" if words else ""
        if it["n"] in LEDGER_TOOLS:
            return "[ledger view]" if it["n"] == "ledger_day" else "[ledger change]"
        if it["n"] in OUTSIDE_TOOLS:
            return "[outside content read]"
        return "[file work]" if "file" in it["n"] else f"[{it['n']}]"

    def slice_text(self, turns: list) -> dict:
        """{local day: text} of the given turns for the summarizer: owner and assistant text verbatim,
        receipts as one line each; ledger numbers, file contents and outside text never reach it."""
        out = {}
        for it in self.items:
            if it["turn"] not in turns:
                continue
            line = self._digest_line(it)
            if line:
                day = datetime.fromtimestamp(self._turn_time(it["turn"]), self.tz).strftime("%a %d %b %Y")
                out.setdefault(day, []).append(line)
        return {d: "\n".join(lines) for d, lines in out.items()}

    def apply_digests(self, turns: list, digests: list):
        """Append one digest per day (cut at a sentence end under DIGEST_MAX), move thru past the
        turns, and keep the block under its profile cap (the oldest leave it, still searchable)."""
        for day, text in digests:
            text = " ".join(text.split())
            if len(text.encode()) > DIGEST_MAX:
                cut = text.encode()[:DIGEST_MAX].decode(errors="ignore")
                text = cut[: cut.rfind(". ") + 1] or cut
            self.digests.append((day, text))
        self.thru = max(it["q"] for it in self.items if it["turn"] in turns)
        while self.digests and len(self.digest_block().encode()) > self.digest_cap:
            self.archived.append(self.digests.pop(0))
        self._change("compaction")


# -------------------------------------------------------------------------- cli
def main():
    ap = argparse.ArgumentParser(description="Chat with the reference loop over a running nimbusd's tools.")
    ap.add_argument("--mcp", required=True, help="nimbusd control surface, for example http://127.0.0.1:8787")
    ap.add_argument("--token", default=os.environ.get("NIMBUSD_WEB_TOKEN", ""), help="nimbusd web token")
    ap.add_argument("--model", default="claude-sonnet-5-5")
    ap.add_argument("--effort", default=None, help="provider effort (default: medium on Claude, unset on GPT)")
    ap.add_argument(
        "--transcript", default="refloop-transcript.json", help="saved after every step, reloaded if present"
    )
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
            print(
                run_turn(
                    conv,
                    provider,
                    tools,
                    [text],
                    on_tool=lambda name, a, out, err: print(f"  [{name}] {json.dumps(a)[:100]}"),
                )
            )


if __name__ == "__main__":
    main()
