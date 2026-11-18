"""The scripted provider's Python twin, for the reference loop (tools/refloop/refloop.py).

It reads the same JSON script as the C++ ScriptedProvider (script.h) and answers the
reference loop's provider interface (user, step, results, is_owner, without_thinking) in
the Anthropic message shapes, so one script drives both arms of a parity run offline:
arm A (nimbusd or a host test, through ScriptedProvider at the transport seam) and arm B
(refloop.run_turn with this provider).

    model = ScriptedModel.from_file("example_script.json")
    reply = refloop.run_turn(conv, model, tools, ["where is my boat?"])

Running out of script raises, as the C++ provider fails the request: a loop that asks
more often than the script says is wrong.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools" / "refloop"))

import refloop  # noqa: E402


class ScriptExhausted(RuntimeError):
    pass


def call_id(step: int, i: int) -> str:
    """The Anthropic call id the C++ provider gives call i of request `step` (wire_answers.h)."""
    return f"toolu_s{step % 1000:03d}c{i % 100:02d}"


class ScriptedModel:
    """Answers each step() with the next scripted step. `seen` records each request's messages."""

    is_owner = staticmethod(refloop.Anthropic.is_owner)
    without_thinking = staticmethod(refloop.Anthropic.without_thinking)
    user = refloop.Anthropic.user
    results = refloop.Anthropic.results

    def __init__(self, steps: list):
        self.steps, self.seen = list(steps), []

    @classmethod
    def from_script(cls, script: dict) -> ScriptedModel:
        if script.get("dialect", "anthropic") != "anthropic":
            raise ValueError("the reference loop twin speaks the anthropic shapes only")
        return cls(script["steps"])

    @classmethod
    def from_file(cls, path) -> ScriptedModel:
        return cls.from_script(json.loads(Path(path).read_text()))

    def step(self, conv, messages=None) -> refloop.Step:
        n = len(self.seen)
        self.seen.append(json.loads(json.dumps(conv.messages if messages is None else messages)))
        if not self.steps:
            raise ScriptExhausted(f"script exhausted at request {n + 1}")
        s = self.steps.pop(0)
        content = []
        if s.get("thinking"):
            content.append({"type": "thinking", "thinking": s["thinking"], "signature": f"sig_scripted_{n}"})
        if s.get("text"):
            content.append({"type": "text", "text": s["text"]})
        calls = []
        for i, c in enumerate(s.get("calls", [])):
            cid = c.get("id") or call_id(n, i)
            args = c.get("args", {})
            content.append({"type": "tool_use", "id": cid, "name": c["name"], "input": args})
            calls.append((cid, c["name"], args))
        u = s.get("usage", {})
        prompt = u.get("input", 100) + u.get("cache_read", 0) + u.get("cache_write", 0)
        text = s.get("text", "")
        if s.get("finish") in ("cut", "refusal"):
            text += f"\n(stopped early: {'max_tokens' if s['finish'] == 'cut' else 'refusal'})"
        return refloop.Step([{"role": "assistant", "content": content}], calls, text, prompt)
