"""Tool-exposure conditions for the tool-count benchmark.

A `catalog.json` enumerates the device tool surface (name, description, params,
category, and whether the tool is in the curated core). Three conditions turn
that catalog into what the model actually sees on a turn:

- `full`   - every catalog tool exposed at once (the current device reality).
- `curated`- only the tools flagged `core: true` (the ~15-20 target set).
- `lazy`   - a small always-on base plus a single `search_tools` meta-tool; the
             real tools are revealed only after the model searches for them,
             modelling a lazy-loaded / tool-search surface.

The retrieval behind `search_tools` is deliberately simple and deterministic
(lexical token overlap over name + description + keywords), so a run is
reproducible and costs nothing extra.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any


_WORD = re.compile(r"[a-z0-9]+")

# Common words carry no retrieval signal and, worse, pollute the name-substring
# boost (e.g. "i" and "list" matching half the catalog). Drop them.
_STOP = {
    "the",
    "a",
    "an",
    "to",
    "of",
    "on",
    "in",
    "at",
    "by",
    "or",
    "and",
    "is",
    "are",
    "be",
    "am",
    "it",
    "its",
    "this",
    "that",
    "these",
    "those",
    "for",
    "you",
    "your",
    "me",
    "my",
    "mine",
    "i",
    "we",
    "us",
    "our",
    "do",
    "does",
    "did",
    "how",
    "what",
    "which",
    "when",
    "where",
    "who",
    "can",
    "could",
    "please",
    "there",
    "here",
    "some",
    "any",
    "with",
    "up",
    "down",
    "out",
    "so",
    "if",
    "as",
    "was",
    "were",
    "will",
    "would",
    "should",
    "now",
    "later",
    "right",
    "get",
    "got",
    "let",
    "them",
    "they",
    "he",
    "she",
    "his",
    "her",
}


def _tokens(text: str) -> set[str]:
    return {w for w in _WORD.findall(text.lower()) if len(w) >= 3 and w not in _STOP}


def load_catalog(path: str | Path) -> list[dict[str, Any]]:
    data = json.loads(Path(path).read_text())
    tools = data["tools"] if isinstance(data, dict) else data
    for t in tools:
        t.setdefault("keywords", [])
        t.setdefault("params", {})
        t.setdefault("core", False)
        t.setdefault("category", "misc")
    return tools


def to_schema(tool: dict[str, Any]) -> dict[str, Any]:
    """Render one catalog entry as an OpenAI function-tool schema."""
    props: dict[str, Any] = {}
    required: list[str] = []
    for pname, pspec in tool.get("params", {}).items():
        if isinstance(pspec, str):
            pspec = {"type": pspec}
        props[pname] = {"type": pspec.get("type", "string")}
        if pspec.get("desc"):
            props[pname]["description"] = pspec["desc"]
        if pspec.get("enum"):
            props[pname]["enum"] = pspec["enum"]
        if pspec.get("required", True):
            required.append(pname)
    return {
        "type": "function",
        "function": {
            "name": tool["name"],
            "description": tool["description"],
            "parameters": {
                "type": "object",
                "properties": props,
                "required": required,
            },
        },
    }


SEARCH_TOOLS_SCHEMA = {
    "type": "function",
    "function": {
        "name": "search_tools",
        "description": (
            "Find device tools that can help with the task. Call this first when "
            "you are unsure which tool to use. Returns a short list of candidate "
            "tools (name + what each does); after this call those tools become "
            "available for you to call directly."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "query": {
                    "type": "string",
                    "description": "What you are trying to do, in a few words.",
                },
            },
            "required": ["query"],
        },
    },
}


def base_schemas(catalog: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """The always-on tools in the lazy condition: those flagged `always: true`."""
    return [to_schema(t) for t in catalog if t.get("always")]


def exposed_tools(condition: str, catalog: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """The initial tool schema list the model sees for a condition."""
    if condition == "full":
        return [to_schema(t) for t in catalog]
    if condition == "curated":
        return [to_schema(t) for t in catalog if t.get("core")]
    if condition == "lazy":
        return base_schemas(catalog) + [SEARCH_TOOLS_SCHEMA]
    raise ValueError(f"unknown condition: {condition!r}")


def search_catalog(query: str, catalog: list[dict[str, Any]], k: int = 5) -> list[dict[str, Any]]:
    """Rank catalog tools against a query by lexical overlap; return the top k."""
    q = _tokens(query)
    scored: list[tuple[float, dict[str, Any]]] = []
    for t in catalog:
        hay = _tokens(
            " ".join(
                [
                    t["name"],
                    t["description"],
                    " ".join(t.get("keywords", [])),
                    t.get("category", ""),
                ]
            )
        )
        overlap = len(q & hay)
        # small boost when a query token is a substring of the tool name
        namehit = sum(1 for tok in q if tok in t["name"].lower())
        score = overlap + 0.5 * namehit
        if score > 0:
            scored.append((score, t))
    scored.sort(key=lambda s: s[0], reverse=True)
    return [t for _, t in scored[:k]]
