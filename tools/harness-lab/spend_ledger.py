#!/usr/bin/env python3
"""Scratch spend ledger for live test legs, priced from provider-reported usage.

`nimbus-lab --record=<dir>` appends every provider usage object to
<dir>/usage.jsonl. This tool prices those rows at the live list rates (where a
rate is uncertain it uses the higher figure: money estimates fail closed), keeps
a JSONL ledger with per-provider caps, and answers "which provider may this leg
use?" with the overflow rule: a provider that is over its cap, or that refused
for credit or billing, hands the leg to Anthropic.

  spend_ledger.py price <usage.jsonl>...                  price rows, print totals
  spend_ledger.py add --ledger L --lane N --purpose P <usage.jsonl>...
  spend_ledger.py status --ledger L [--caps anthropic=0.5,openai=0.05,...]
  spend_ledger.py route <provider> --ledger L [--cost 0.02] [--caps ...]
  spend_ledger.py refuse <provider> --ledger L --why "credit balance too low"

Exit codes: 0 ok, 2 bad input, 3 a cap is exceeded (add/status) or no provider
can take the leg (route).
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

# The program caps (owner, 18 Oct 2026). A lane passes its own with --caps.
PROGRAM_CAPS = {"anthropic": 80.0, "openai": 5.0, "mistral": 10.0, "zai": 10.0}
OVERFLOW = "anthropic"

HOST_PROVIDER = {
    "api.anthropic.com": "anthropic",
    "api.openai.com": "openai",
    "api.mistral.ai": "mistral",
    "api.z.ai": "zai",
    "open.bigmodel.cn": "zai",
}

# USD per million tokens: input, output, cache read, cache write. Matched by the
# longest prefix of the served model id. Sources: provider price pages (Oct 2026);
# where a figure is uncertain the device's own fail-closed estimate is used.
PRICES = {
    "anthropic": [
        ("claude-haiku-5-5", 0.10, 0.50, 0.01, 0.125),
        ("claude-sonnet-5-5", 2.0, 10.0, 0.20, 2.50),
        ("claude-opus-5-5", 4.0, 20.0, 0.20, 5.00),
        ("claude-opus-5", 5.0, 25.0, 0.50, 6.25),
        ("claude-sonnet-5", 2.0, 10.0, 0.20, 2.50),
        ("claude-fable-5", 10.0, 50.0, 1.00, 12.50),
        ("claude-haiku-4-5", 1.0, 5.0, 0.10, 1.25),
        ("claude-sonnet-4", 3.0, 15.0, 0.30, 3.75),
        ("claude-opus-4", 5.0, 25.0, 0.50, 6.25),
    ],
    "openai": [
        ("gpt-5.6-luna", 0.20, 1.20, 0.02, 0.25),
        ("gpt-5.6-terra", 2.0, 12.0, 0.20, 2.50),
        ("gpt-5.6", 5.0, 30.0, 0.50, 6.25),  # gpt-5.6 is served as gpt-5.6-sol
        ("gpt-6-astra", 10.0, 50.0, 1.00, 12.50),
        ("text-embedding-3-small", 0.02, 0.0, 0.0, 0.0),
    ],
    "mistral": [
        # Chat-completions providers (Mistral, Z.ai) report cached reads but no
        # cache writes, so their write rate is 0 (nothing is ever billed there).
        ("mistral-small", 0.20, 0.60, 0.02, 0.0),
        ("mistral-medium", 2.0, 6.0, 0.20, 0.0),
        ("mistral-large", 2.0, 6.0, 0.20, 0.0),
        ("mistral-moderation", 0.10, 0.0, 0.0, 0.0),
        ("mistral-embed", 0.10, 0.0, 0.0, 0.0),
    ],
    "zai": [
        ("glm-5.3-flash", 0.15, 0.50, 0.03, 0.0),
        ("glm-5.3", 1.40, 4.40, 0.28, 0.0),
        ("glm-5.2", 1.40, 4.40, 0.28, 0.0),
    ],
}
# An unknown model is priced at the provider's highest rate (fail closed).
FALLBACK = {
    "anthropic": (10.0, 50.0, 1.00, 12.50),
    "openai": (10.0, 50.0, 1.00, 12.50),
    "mistral": (2.0, 6.0, 0.20, 0.0),
    "zai": (1.40, 4.40, 0.28, 0.0),
}
# claude-haiku-5-5 doubles as the long-context tier above 100K prompt tokens.
HAIKU55_LONG = (0.50, 2.50, 0.05, 0.625)
HAIKU55_LONG_AT = 100_000


def rates(provider: str, model: str) -> tuple[float, float, float, float]:
    best = None
    for row in PRICES.get(provider, []):
        if model.startswith(row[0]) and (best is None or len(row[0]) > len(best[0])):
            best = row
    return best[1:] if best else FALLBACK.get(provider, (10.0, 50.0, 1.0, 12.5))


def _anthropic_tokens(u: dict) -> dict:
    return {
        "in": int(u.get("input_tokens") or 0),
        "out": int(u.get("output_tokens") or 0),
        "cache_read": int(u.get("cache_read_input_tokens") or 0),
        "cache_write": int(u.get("cache_creation_input_tokens") or 0),
    }


def _responses_tokens(u: dict) -> dict:
    """OpenAI Responses: input_tokens includes both the cached and the written part."""
    det = u.get("input_tokens_details") or {}
    cached = int(det.get("cached_tokens") or 0)
    write = int(det.get("cache_write_tokens") or 0)
    total = int(u.get("input_tokens") or 0)
    return {
        "in": max(total - cached - write, 0),
        "out": int(u.get("output_tokens") or 0),
        "cache_read": cached,
        "cache_write": write,
    }


def _chat_tokens(u: dict) -> dict:
    """Chat completions (Mistral, Z.ai): prompt_tokens includes the cached part."""
    det = u.get("prompt_tokens_details") or {}
    cached = int(det.get("cached_tokens") or 0)
    prompt = int(u.get("prompt_tokens") or 0)
    return {
        "in": max(prompt - cached, 0),
        "out": int(u.get("completion_tokens") or 0),
        "cache_read": cached,
        "cache_write": 0,
    }


def tokens(provider: str, usage: dict) -> dict:
    """Normalize a raw usage object to fresh input / output / cache read / cache write."""
    if provider == "anthropic":
        return _anthropic_tokens(usage)
    if "input_tokens" in usage:
        return _responses_tokens(usage)
    return _chat_tokens(usage)


def cost(provider: str, model: str, usage: dict) -> tuple[float, dict]:
    t = tokens(provider, usage)
    r_in, r_out, r_read, r_write = rates(provider, model)
    if provider == "anthropic" and model.startswith("claude-haiku-5-5"):
        if t["in"] + t["cache_read"] + t["cache_write"] > HAIKU55_LONG_AT:
            r_in, r_out, r_read, r_write = HAIKU55_LONG
    usd = (t["in"] * r_in + t["out"] * r_out + t["cache_read"] * r_read + t["cache_write"] * r_write) / 1e6
    return usd, t


def price_file(path: Path) -> list[dict]:
    rows = []
    for n, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip():
            continue
        rec = json.loads(line)
        provider = HOST_PROVIDER.get(rec.get("host", ""))
        if not provider or not isinstance(rec.get("usage"), dict):
            continue
        model = rec.get("model") or rec.get("requested") or ""
        usd, t = cost(provider, model, rec["usage"])
        rows.append(
            {
                "src": f"{path.name}#{n}:{rec.get('ts', '')}",
                "provider": provider,
                "model": model,
                "cost_usd": round(usd, 8),
                **t,
            }
        )
    return rows


def parse_caps(s: str | None) -> dict:
    caps = dict(PROGRAM_CAPS)
    if not s:
        return caps
    for item in s.split(","):
        k, _, v = item.partition("=")
        if k.strip() not in caps:
            raise ValueError(f"unknown provider in --caps: {k}")
        caps[k.strip()] = float(v)
    return caps


def read_ledger(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return [json.loads(x) for x in path.read_text().splitlines() if x.strip()]


def totals(ledger: list[dict]) -> tuple[dict, set]:
    spent = {p: 0.0 for p in PROGRAM_CAPS}
    refused = set()
    for r in ledger:
        if r.get("kind") == "refusal":
            refused.add(r["provider"])
        elif r.get("provider") in spent:
            spent[r["provider"]] += float(r.get("cost_usd", 0))
    return spent, refused


def route(provider: str, ledger: list[dict], caps: dict, est: float = 0.0) -> str | None:
    """The provider a leg may use: its own, else the overflow provider, else None."""
    spent, refused = totals(ledger)
    if provider not in refused and spent.get(provider, 0.0) + est <= caps.get(provider, 0.0):
        return provider
    if OVERFLOW not in refused and spent[OVERFLOW] + est <= caps[OVERFLOW]:
        return OVERFLOW
    return None


def print_status(ledger: list[dict], caps: dict) -> bool:
    spent, refused = totals(ledger)
    over = False
    print(f"{'provider':10s} {'spent $':>10s} {'cap $':>8s}  route")
    for p in PROGRAM_CAPS:
        r = route(p, ledger, caps)
        flag = " REFUSED" if p in refused else (" OVER CAP" if spent[p] > caps[p] else "")
        over = over or spent[p] > caps[p]
        print(f"{p:10s} {spent[p]:10.4f} {caps[p]:8.2f}  {r or 'none'}{flag}")
    return not over


def cmd_price(args) -> int:
    grand = {}
    for f in args.files:
        for r in price_file(Path(f)):
            grand[r["provider"]] = grand.get(r["provider"], 0.0) + r["cost_usd"]
            print(
                f"{r['provider']:9s} {r['model']:28s} in={r['in']:>7d} out={r['out']:>6d} "
                f"cr={r['cache_read']:>7d} cw={r['cache_write']:>7d}  ${r['cost_usd']:.6f}"
            )
    for p, v in sorted(grand.items()):
        print(f"TOTAL {p}: ${v:.6f}")
    return 0


def cmd_add(args) -> int:
    ledger_path = Path(args.ledger)
    ledger = read_ledger(ledger_path)
    seen = {r.get("src") for r in ledger}
    now = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    added = 0
    with ledger_path.open("a") as out:
        for f in args.files:
            for r in price_file(Path(f)):
                if r["src"] in seen:
                    continue
                row = {"ts": now, "lane": args.lane, "purpose": args.purpose, "kind": "usage", **r}
                out.write(json.dumps(row) + "\n")
                ledger.append(row)
                added += 1
    print(f"added {added} priced row(s) to {ledger_path}")
    return 0 if print_status(ledger, parse_caps(args.caps)) else 3


def cmd_status(args) -> int:
    return 0 if print_status(read_ledger(Path(args.ledger)), parse_caps(args.caps)) else 3


def cmd_route(args) -> int:
    r = route(args.provider, read_ledger(Path(args.ledger)), parse_caps(args.caps), args.cost)
    print(r or "none")
    return 0 if r else 3


def cmd_refuse(args) -> int:
    now = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    with Path(args.ledger).open("a") as out:
        out.write(json.dumps({"ts": now, "kind": "refusal", "provider": args.provider, "why": args.why}) + "\n")
    print(
        f"{args.provider} marked refused; "
        + (f"its legs route to {OVERFLOW}" if args.provider != OVERFLOW else "no provider takes its legs")
    )
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("price")
    p.add_argument("files", nargs="+")
    p.set_defaults(fn=cmd_price)
    for name, fn in (("add", cmd_add), ("status", cmd_status), ("route", cmd_route), ("refuse", cmd_refuse)):
        q = sub.add_parser(name)
        q.add_argument("--ledger", required=True)
        if name in ("add", "status", "route"):
            q.add_argument("--caps")
        if name == "add":
            q.add_argument("--lane", required=True)
            q.add_argument("--purpose", default="")
            q.add_argument("files", nargs="+")
        if name in ("route", "refuse"):
            q.add_argument("provider", choices=sorted(PROGRAM_CAPS))
        if name == "route":
            q.add_argument("--cost", type=float, default=0.0)
        if name == "refuse":
            q.add_argument("--why", required=True)
        q.set_defaults(fn=fn)
    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except (ValueError, OSError, json.JSONDecodeError) as e:
        print(f"spend_ledger: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
