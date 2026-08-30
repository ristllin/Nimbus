"""Provider clients for the tool-count benchmark.

The benchmark talks to any OpenAI-compatible chat-completions endpoint, so a
single code path drives OpenAI and z.ai (and, for offline validation, a mock
provider that needs no key and no network).

Keys are read from the environment (or a repo .env file), never the command
line. Nothing here prints a secret.
"""

from __future__ import annotations

import json
import os
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Any


# Per-1K-token USD estimates, used only for a rough spend figure in the corpus.
# Keep these conservative and clearly labelled as estimates; they are not billing.
_PRICE_PER_1K = {
    # OpenAI
    "gpt-4o-mini": (0.00015, 0.00060),
    "gpt-4.1-mini": (0.00040, 0.00160),
    "gpt-4.1": (0.00200, 0.00800),
    "gpt-5-mini": (0.00025, 0.00200),
    "gpt-5": (0.00125, 0.01000),
    # z.ai (GLM). Rough public list-price estimates.
    "glm-4.6": (0.00060, 0.00220),
    "glm-4.5": (0.00060, 0.00220),
    "glm-4.5-air": (0.00020, 0.00110),
    "glm-4-flash": (0.00000, 0.00000),
}


def usd_estimate(model: str, prompt_tokens: int, completion_tokens: int) -> float:
    base = model.split("/", 1)[-1]
    pin, pout = _PRICE_PER_1K.get(base, (0.0, 0.0))
    return round(pin * prompt_tokens / 1000 + pout * completion_tokens / 1000, 6)


def load_dotenv(path: str | os.PathLike[str]) -> None:
    """Minimal .env loader. Only sets keys not already in the environment."""
    p = Path(path)
    if not p.exists():
        return
    for line in p.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        val = val.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = val


@dataclass
class ChatResult:
    """Normalized single-turn result from a provider."""

    tool_calls: list[dict[str, Any]]  # [{"name": str, "arguments": dict}]
    content: str
    prompt_tokens: int
    completion_tokens: int
    finish_reason: str
    raw: Any = None


class Provider:
    """An OpenAI-compatible chat-completions provider."""

    def __init__(self, name: str, model: str, base_url: str, api_key: str, temperature: float = 0.0):
        from openai import OpenAI  # local import so the mock path needs no dep

        self.name = name
        self.model = model
        self.temperature = temperature
        self._client = OpenAI(base_url=base_url, api_key=api_key)

    @property
    def label(self) -> str:
        return f"{self.name}/{self.model}"

    @staticmethod
    def _safe_name(name: str) -> str:
        # The OpenAI function-calling API requires ^[a-zA-Z0-9_-]+$, so the
        # device's dotted names (memory.write) must be sent as memory__write and
        # mapped back on the way out. z.ai/Anthropic accept dots, but sanitizing
        # unconditionally is harmless and keeps one code path.
        return name.replace(".", "__")

    def chat(self, messages: list[dict[str, Any]], tools: list[dict[str, Any]], seed: int | None = None) -> ChatResult:
        rev: dict[str, str] = {}
        safe_tools: list[dict[str, Any]] = []
        for t in tools:
            fn = t["function"]
            safe = self._safe_name(fn["name"])
            rev[safe] = fn["name"]
            safe_tools.append(
                {
                    "type": "function",
                    "function": {**fn, "name": safe},
                }
            )
        kwargs: dict[str, Any] = dict(
            model=self.model,
            messages=messages,
            temperature=self.temperature,
        )
        if safe_tools:
            kwargs["tools"] = safe_tools
            kwargs["tool_choice"] = "auto"
        if seed is not None:
            kwargs["seed"] = seed
        resp = self._client.chat.completions.create(**kwargs)
        choice = resp.choices[0]
        msg = choice.message
        tcs: list[dict[str, Any]] = []
        for tc in msg.tool_calls or []:
            try:
                args = json.loads(tc.function.arguments or "{}")
            except (json.JSONDecodeError, TypeError):
                args = {"__raw__": tc.function.arguments}
            name = rev.get(tc.function.name, tc.function.name.replace("__", "."))
            tcs.append({"id": tc.id, "name": name, "arguments": args})
        usage = resp.usage
        return ChatResult(
            tool_calls=tcs,
            content=msg.content or "",
            prompt_tokens=getattr(usage, "prompt_tokens", 0) or 0,
            completion_tokens=getattr(usage, "completion_tokens", 0) or 0,
            finish_reason=choice.finish_reason or "",
            raw=resp,
        )


class MockProvider:
    """Deterministic offline provider for validating the harness plumbing.

    It does not measure anything real: it follows the task's expected tool, which
    the runner embeds in the system message as `__expected__`, so the runner can
    produce a well-formed corpus with no network and no key. Used by tests and by
    `run_toolcount.py --mock`.
    """

    def __init__(self, name: str = "mock", model: str = "mock", error_rate: float = 0.0, seed: int = 0):
        self.name = name
        self.model = model
        self._rng = random.Random(seed)
        self._error_rate = error_rate

    @property
    def label(self) -> str:
        return f"{self.name}/{self.model}"

    def chat(self, messages: list[dict[str, Any]], tools: list[dict[str, Any]], seed: int | None = None) -> ChatResult:
        # Pick the tool named by the `__expected__` hint, injecting occasional
        # wrong picks per error_rate.
        expected = None
        for m in messages:
            if isinstance(m.get("content"), str) and "__expected__:" in m["content"]:
                expected = m["content"].split("__expected__:", 1)[1].strip() or None
        names = [t["function"]["name"] for t in tools]
        if self._rng.random() < self._error_rate and len(names) > 1:
            expected = self._rng.choice(names)
        if expected and expected in names:
            return ChatResult([{"id": "m0", "name": expected, "arguments": {}}], "", 100, 10, "tool_calls")
        if expected is None:
            return ChatResult([], "Answered directly.", 80, 12, "stop")
        # expected tool not visible (lazy pre-search): call search_tools if present
        if "search_tools" in names:
            return ChatResult(
                [{"id": "m0", "name": "search_tools", "arguments": {"query": expected}}], "", 90, 8, "tool_calls"
            )
        return ChatResult([], "No suitable tool.", 80, 10, "stop")


def build_provider(spec: str, temperature: float = 0.0) -> Provider | MockProvider:
    """Build a provider from a `provider:model` spec.

    Supported providers: `openai`, `zai`, `mock`. Model defaults are sensible
    cheap tiers. Keys come from OPENAI_API_KEY / Z_AI_TOKEN in the environment.
    """
    provider, _, model = spec.partition(":")
    provider = provider.lower()
    if provider == "mock":
        return MockProvider(model=model or "mock", error_rate=float(os.environ.get("TC_MOCK_ERR", "0")))
    if provider == "openai":
        key = os.environ.get("OPENAI_API_KEY", "")
        if not key:
            raise RuntimeError("OPENAI_API_KEY not set")
        return Provider("openai", model or "gpt-4o-mini", "https://api.openai.com/v1", key, temperature)
    if provider in ("zai", "z_ai", "z-ai", "glm"):
        key = os.environ.get("Z_AI_TOKEN", "")
        if not key:
            raise RuntimeError("Z_AI_TOKEN not set")
        return Provider("zai", model or "glm-4.6", "https://api.z.ai/api/paas/v4", key, temperature)
    raise ValueError(f"unknown provider in spec: {spec!r}")
