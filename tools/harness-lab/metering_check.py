#!/usr/bin/env python3
"""Compare what the turn engine metered with what the providers reported.

Two inputs per run:
  <dir>/usage.jsonl   every provider usage object, as the provider sent it
                      (`nimbus-lab --record=<dir>`)
  the run's output    one "lab-ledger: {...}" line per row the engine filed
                      (BudgetConfig::recordTokens; the metering scenarios print them)

The engine converts a provider's counts into ledger tokens in one place
(nimbus::orch::ledgerDelta); this tool applies the same rule to the recorded
usage and checks, per host, that the ledger holds exactly what was billed:

  anthropic  in = input_tokens, cr = cache_read_input_tokens,
             cw = cache_creation_input_tokens (both outside input_tokens)
  openai     input_tokens includes cached and written tokens:
             in = input - cached - written, cr = cached, cw = written
  others     in = prompt tokens, cache not split (cr = cw = 0)

Only chat requests count (embeddings and moderation are metered elsewhere).

  metering_check.py <usage.jsonl> <run.log>   exit 0 when every host matches, 1 otherwise
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

HOST_PROVIDER = {
    "api.anthropic.com": "anthropic",
    "api.openai.com": "openai",
    "api.mistral.ai": "mistral",
    "api.z.ai": "zai",
    "open.bigmodel.cn": "zai",
}
CHAT_PATHS = ("/v1/messages", "/v1/chat/completions", "/v1/responses", "/api/paas/v4/chat/completions")


def _n(v) -> int:
    return int(v) if isinstance(v, (int, float)) and v > 0 else 0


def ledger_delta(provider: str, usage: dict) -> dict:
    """One usage object in ledger units (mirrors ledgerDelta and tokenUsageFromJson)."""
    inp = _n(usage.get("input_tokens")) or _n(usage.get("prompt_tokens"))
    out = _n(usage.get("output_tokens")) or _n(usage.get("completion_tokens"))
    det_in = usage.get("input_tokens_details") or {}
    det_prompt = usage.get("prompt_tokens_details") or {}
    cr = (
        _n(usage.get("cache_read_input_tokens"))
        or _n(det_in.get("cached_tokens"))
        or _n(det_prompt.get("cached_tokens"))
    )
    cw = _n(usage.get("cache_creation_input_tokens")) or _n(det_in.get("cache_write_tokens"))
    if provider == "anthropic":
        return {"in": inp, "out": out, "cr": cr, "cw": cw}
    if provider == "openai":
        cr = min(cr, inp)
        cw = min(cw, inp - cr)
        return {"in": inp - cr - cw, "out": out, "cr": cr, "cw": cw}
    return {"in": inp, "out": out, "cr": 0, "cw": 0}


LEDGER_PREFIX = "lab-ledger: "


def _rows(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def ledger_rows_from_log(text: str) -> list[dict]:
    return [json.loads(line[len(LEDGER_PREFIX) :]) for line in text.splitlines() if line.startswith(LEDGER_PREFIX)]


def billed_by_host(usage_rows: list[dict]) -> dict:
    tot: dict[str, dict] = {}
    for r in usage_rows:
        prov = HOST_PROVIDER.get(r.get("host", ""))
        if not prov or not str(r.get("path", "")).startswith(CHAT_PATHS):
            continue
        d = ledger_delta(prov, r.get("usage") or {})
        t = tot.setdefault(prov, {"in": 0, "out": 0, "cr": 0, "cw": 0, "responses": 0})
        for k in ("in", "out", "cr", "cw"):
            t[k] += d[k]
        t["responses"] += 1
    return tot


def metered_by_host(ledger_rows: list[dict]) -> dict:
    tot: dict[str, dict] = {}
    for r in ledger_rows:
        t = tot.setdefault(r["host"], {"in": 0, "out": 0, "cr": 0, "cw": 0, "rows": 0})
        for k in ("in", "out", "cr", "cw"):
            t[k] += _n(r.get(k))
        t["rows"] += 1
    return tot


def compare(usage_rows: list[dict], ledger_rows: list[dict]) -> tuple[bool, list[str]]:
    billed = billed_by_host(usage_rows)
    metered = metered_by_host(ledger_rows)
    ok = True
    lines = []
    for host in sorted(set(billed) | set(metered)):
        b = billed.get(host, {"in": 0, "out": 0, "cr": 0, "cw": 0, "responses": 0})
        m = metered.get(host, {"in": 0, "out": 0, "cr": 0, "cw": 0, "rows": 0})
        same = all(b[k] == m[k] for k in ("in", "out", "cr", "cw"))
        ok = ok and same
        lines.append(
            f"{'MATCH' if same else 'DIFF '} {host}: provider in={b['in']} out={b['out']} "
            f"cr={b['cr']} cw={b['cw']} ({b['responses']} responses) | ledger in={m['in']} "
            f"out={m['out']} cr={m['cr']} cw={m['cw']} ({m['rows']} rows)"
        )
    if not lines:
        ok = False
        lines.append("DIFF no billed usage and no ledger rows recorded")
    return ok, lines


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    log = Path(argv[2]).read_text(errors="replace")
    ok, lines = compare(_rows(Path(argv[1])), ledger_rows_from_log(log))
    print("\n".join(lines))
    print("METERING " + ("MATCH" if ok else "DIFF"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
