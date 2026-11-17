#!/usr/bin/env python3
"""costs - price provider-reported usage, and keep the rig's spend ledger.

Prices are USD per million tokens, list prices as recorded in
lib/core/include/nimbus/orch/model_price_defaults.h (as of 2026-10). Both arms are
priced by the same function from the usage each provider reported on the wire.
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
}
# claude-haiku-5-5 bills a request whose prompt is over 100K tokens at its own rates.
HAIKU_LONG_OVER = 100_000
HAIKU_LONG = (0.50, 2.50, 0.05, 0.625)
UNKNOWN = (3.00, 15.00, 0.30, 3.75)   # an unlisted model prices high, never free

_lock = threading.Lock()


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
