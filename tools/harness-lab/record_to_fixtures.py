#!/usr/bin/env python3
"""Turn `nimbus-lab --record=<dir>` output into scrubbed test fixtures.

  record_to_fixtures.py --out test/support/fixtures/usage_2026-10 <record-dir>...

Writes one file per (provider, served model) holding up to three distinct usage
objects (a cache write, a cache read, a plain call, whichever occurred), plus
errors.json with each distinct provider error body. Request ids and timestamps
are scrubbed; the usage objects are kept byte-for-byte otherwise, because the
field names are the contract the metering code is tested against.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

HOST_PROVIDER = {
    "api.anthropic.com": "anthropic",
    "api.openai.com": "openai",
    "api.mistral.ai": "mistral",
    "api.z.ai": "zai",
    "open.bigmodel.cn": "zai",
}
_REQ_ID = re.compile(r'"(request_id|id)"\s*:\s*"[^"]*"')


def scrub_text(s: str) -> str:
    return _REQ_ID.sub(lambda m: f'"{m.group(1)}":"scrubbed"', s)


def usage_shape(u: dict) -> tuple:
    """Which cache path a usage object shows (one example kept per shape)."""
    det = u.get("input_tokens_details") or u.get("prompt_tokens_details") or {}
    write = bool(u.get("cache_creation_input_tokens") or det.get("cache_write_tokens"))
    read = bool(u.get("cache_read_input_tokens") or det.get("cached_tokens"))
    return (write, read)


def collect(dirs: list[Path]) -> tuple[dict, list]:
    usage: dict[tuple, dict] = {}
    errors: dict[tuple, dict] = {}
    for d in dirs:
        for row in _rows(d / "usage.jsonl"):
            prov = HOST_PROVIDER.get(row.get("host", ""))
            if not prov or not isinstance(row.get("usage"), dict):
                continue
            key = (prov, row.get("model") or row.get("requested") or "unknown")
            slot = usage.setdefault(
                key,
                {
                    "provider": prov,
                    "endpoint": row.get("path", ""),
                    "requested": row.get("requested", ""),
                    "served": key[1],
                    "shapes": {},
                },
            )
            slot["shapes"].setdefault(usage_shape(row["usage"]), row["usage"])
        for row in _rows(d / "errors.jsonl"):
            prov = HOST_PROVIDER.get(row.get("host", ""))
            if not prov:
                continue
            body = scrub_text(row.get("body", ""))
            errors.setdefault(
                (prov, row.get("status"), body),
                {
                    "provider": prov,
                    "endpoint": row.get("path", ""),
                    "status": row.get("status"),
                    "requested": row.get("requested", ""),
                    "body": body,
                },
            )
    return usage, list(errors.values())


def _rows(path: Path):
    if not path.exists():
        return
    for line in path.read_text().splitlines():
        if line.strip():
            yield json.loads(line)


def write(out: Path, usage: dict, errors: list, recorded: str) -> list[Path]:
    out.mkdir(parents=True, exist_ok=True)
    written = []
    for (prov, model), slot in sorted(usage.items()):
        doc = {
            "provider": prov,
            "endpoint": slot["endpoint"],
            "requested": slot["requested"],
            "served": model,
            "recorded_utc": recorded,
            "source": "nimbus-lab --record, tools/harness-lab/record_to_fixtures.py",
            "usage": [slot["shapes"][k] for k in sorted(slot["shapes"])],
        }
        p = out / f"{prov}_{model}.json"
        p.write_text(json.dumps(doc, indent=1, sort_keys=False) + "\n")
        written.append(p)
    if errors:
        p = out / "errors.json"
        p.write_text(
            json.dumps(
                {"recorded_utc": recorded, "errors": sorted(errors, key=lambda e: (e["provider"], e["status"]))},
                indent=1,
            )
            + "\n"
        )
        written.append(p)
    return written


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--recorded", default="2026-10-18", help="recording date stamped into each file")
    ap.add_argument("dirs", nargs="+")
    args = ap.parse_args(argv)
    usage, errors = collect([Path(d) for d in args.dirs])
    if not usage and not errors:
        print("nothing recorded in the given directories", file=sys.stderr)
        return 2
    for p in write(Path(args.out), usage, errors, args.recorded):
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
