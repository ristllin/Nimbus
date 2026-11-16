#!/usr/bin/env python3
"""Turn a bench `TOOLS?` capture into the host suites' tool-manifest fixture.

The test console's `TOOLS?` prints the tool list an owner turn's model gets, under the
names it sees them by, one JSON object per line between `TOOLS BEGIN n=<count>
bytes=<size>` and `TOOLS END` (src/test_console.cpp). The host suites size their wire
bodies against that real manifest instead of a synthetic one
(test/test_prompt_layout), so the fixture is regenerated from a capture whenever the
device's tool list changes on purpose.

Usage:
    python3 tools/gen_tool_manifest_fixture.py <capture.log> [--out <fixture.json>] [--source <text>]

The capture may hold anything around the block (boot lines, other replies); the last
complete block is used. Exit 1 when no complete block is found, or when the count or
the byte total the device announced does not match what the block holds.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_OUT = REPO / "test/support/fixtures/tool_manifest_v5_1_1.json"
BEGIN = re.compile(r"TOOLS BEGIN n=(\d+) bytes=(\d+)")


def last_block(text: str) -> tuple[int, int, list[str]] | None:
    """The announced count, byte total and lines of the last complete block."""
    lines = text.splitlines()
    found = None
    i = 0
    while i < len(lines):
        m = BEGIN.search(lines[i])
        if not m:
            i += 1
            continue
        body = []
        j = i + 1
        while j < len(lines) and lines[j].strip() != "TOOLS END":
            if lines[j].strip():
                body.append(lines[j].strip())
            j += 1
        if j < len(lines):
            found = (int(m.group(1)), int(m.group(2)), body)
        i = j + 1
    return found


def parse(block: tuple[int, int, list[str]]) -> list[dict]:
    count, total, body = block
    tools = []
    for line in body:
        t = json.loads(line)
        tools.append({"name": t["name"], "description": t["description"], "schema": t["schema"]})
    if len(tools) != count:
        raise ValueError(f"the device announced {count} tools, the block holds {len(tools)}")
    held = sum(len(t["name"].encode()) + len(t["description"].encode()) + len(t["schema"].encode()) for t in tools)
    if held != total:
        raise ValueError(f"the device announced {total} bytes, the block holds {held}")
    for t in tools:
        json.loads(t["schema"])  # every schema is JSON
    return tools


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("capture")
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--source", default="bench TOOLS? capture")
    a = ap.parse_args()
    block = last_block(Path(a.capture).read_text(encoding="utf-8", errors="replace"))
    if not block:
        print("no complete TOOLS BEGIN ... TOOLS END block in the capture", file=sys.stderr)
        return 1
    try:
        tools = parse(block)
    except (ValueError, KeyError, json.JSONDecodeError) as e:
        print(f"capture rejected: {e}", file=sys.stderr)
        return 1
    out = {
        "source": a.source,
        "count": len(tools),
        "bytes": block[1],
        "tools": tools,
    }
    Path(a.out).write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {a.out}: {len(tools)} tools, {block[1]} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
