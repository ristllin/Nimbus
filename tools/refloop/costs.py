#!/usr/bin/env python3
"""costs - price provider-reported usage, and keep the rig's spend ledger.

Prices are USD per million tokens, list prices as recorded in
lib/core/include/nimbus/orch/model_price_defaults.h (as of 2026-10). Both arms are
priced by the same function from the usage each provider reported on the wire.

Virtual time: a scenario's advance step moves the daemons' clocks, but a provider's
prompt cache lives in real time and is still warm after an advance. So a request whose
gap on the virtual clock since the previous request on the same model is over
COLD_GAP_S is priced cold from its token counts (cold_priced_usd): what it would have
cost had the gap been real. What was billed stays the spend ledger's figure.
"""

from __future__ import annotations

import json
import threading
import time
from pathlib import Path

# model prefix -> (input, output, cache read, cache write). Longest prefix wins.
PRICES = {
    "claude-sonnet-5-5": (2.00, 10.00, 0.10, 2.50),
    "claude-haiku-5-5": (0.10, 0.50, 0.01, 0.125),
    "gpt-5.6-luna": (0.20, 1.20, 0.02, 0.20),
    "gpt-6-luna": (0.10, 0.50, 0.01, 0.10),
    "text-embedding-3-small": (0.02, 0.0, 0.02, 0.02),
    # Mistral (lane RB-R1, ruling 102): the FIRMWARE's own price table, not a provider quote
    # (lib/core/include/nimbus/orch/model_price_defaults.h, kBuiltinPrices, as of 2026-10: mistral-medium
    # 1.5 / 7.5, mistral-small 0.2 / 0.6, mistral-large 2 / 6 USD per 1M; cache read 0.1x and write 1.25x of the
    # input rate, that table's default share).
    "mistral-medium": (1.50, 7.50, 0.15, 1.875),
    "mistral-small": (0.20, 0.60, 0.02, 0.25),
    "mistral-large": (2.00, 6.00, 0.20, 2.50),
}
# claude-haiku-5-5 bills a request whose prompt is over 100K tokens at its own rates.
HAIKU_LONG_OVER = 100_000
HAIKU_LONG = (0.50, 2.50, 0.05, 0.625)
UNKNOWN = (3.00, 15.00, 0.30, 3.75)  # an unlisted model prices high, never free

_lock = threading.Lock()
# A provider prompt cache lives 5 minutes from its last use (the default write). A virtual gap
# longer than this means the request would have found it expired.
COLD_GAP_S = 300


def price_row(model: str | None, prompt_tokens: int = 0) -> tuple:
    model = model or ""
    if model.startswith("claude-haiku-5-5") and prompt_tokens > HAIKU_LONG_OVER:
        return HAIKU_LONG
    best = max((p for p in PRICES if model.startswith(p)), key=len, default=None)
    return PRICES[best] if best else UNKNOWN


def cost_usd(model: str | None, usage: dict | None) -> float:
    """USD for one exchange. usage is capture_proxy.normalize_usage's shape."""
    if not usage:
        return 0.0
    p_in, p_out, p_read, p_write = price_row(model, usage.get("input", 0))
    cached, written = usage.get("cached", 0), usage.get("cache_write", 0)
    fresh = max(usage.get("input", 0) - cached - written, 0)
    return (fresh * p_in + cached * p_read + written * p_write + usage.get("output", 0) * p_out) / 1e6


def cold_cost_usd(model: str | None, usage: dict | None) -> float:
    """USD for the same exchange with no prompt cache at all: every prompt token at the input price.

    Runs made close together share the provider's cache of the fixed prefix (tools and system
    prompt), so the measured cost is a warm figure. This is the other bound: a conversation
    whose cache has expired between turns pays close to this.
    """
    if not usage:
        return 0.0
    p_in, p_out, _, _ = price_row(model, usage.get("input", 0))
    return (usage.get("input", 0) * p_in + usage.get("output", 0) * p_out) / 1e6


def cold_priced_usd(model: str | None, usage: dict | None) -> float:
    """USD for the same exchange had its cache expired: every token it read from the cache is
    written again at the cache-write price (on a model with no write premium that is the input
    price); fresh, written and output tokens price as reported."""
    if not usage:
        return 0.0
    cached = usage.get("cached", 0)
    return cost_usd(model, {**usage, "cached": 0, "cache_write": usage.get("cache_write", 0) + cached})


def offset_at(clock_marks: list, t: float) -> int:
    """The clock offset in force at real time t: the last (real time, offset) mark at or before t."""
    offset = 0
    for at, off in clock_marks:
        if at <= t:
            offset = off
    return offset


def virtual_costs(rows: list, clock_marks: list) -> dict:
    """{seq: (usd, cold)} for every row, in the order the requests were made.

    A row is cold when the virtual gap from the end of the previous request on the same model
    (for a model's first request: from the start of the run's first request) to its start is
    over COLD_GAP_S; its cost is then cold_priced_usd, else what was billed.
    """
    last_end, out, first = {}, {}, None
    for r in sorted(rows, key=lambda r: r["seq"]):
        offset = offset_at(clock_marks, r["t_start"])
        start, model = r["t_start"] + offset, r.get("model")
        first = start if first is None else first
        cold = start - last_end.get(model, first) > COLD_GAP_S
        usd = (cold_priced_usd if cold else cost_usd)(model, r.get("usage"))
        out[r["seq"]] = (usd, cold)
        last_end[model] = start + r.get("ms", 0) / 1000
    return out


def rows_cost(rows: list) -> float:
    return sum(cost_usd(r.get("model"), r.get("usage")) for r in rows)


def add_spend(ledger: Path, label: str, usd: float, **extra):
    """Append one line to the spend ledger (thread-safe within a process)."""
    line = json.dumps({"t": round(time.time(), 1), "label": label, "usd": round(usd, 6), **extra})
    with _lock:
        ledger.parent.mkdir(parents=True, exist_ok=True)
        with open(ledger, "a") as f:
            f.write(line + "\n")


def total_spend(ledger: Path) -> float:
    if not ledger.exists():
        return 0.0
    with _lock:
        return sum(json.loads(line)["usd"] for line in ledger.read_text().splitlines() if line.strip())
