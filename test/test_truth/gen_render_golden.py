#!/usr/bin/env python3
"""The probe user message of every measured row, as RB-M0c rendered it (run_probe.render).

test_truth renders the same rows with truth::renderUser and compares length and crc32 with
this golden, so the device sends the bytes that were measured. One line per row and
condition: set, id, condition, byte length, crc32 (8 lower-case hex).

  python3 test/test_truth/gen_render_golden.py           # check the golden is fresh
  python3 test/test_truth/gen_render_golden.py --write   # regenerate it
"""

import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "tests" / "evals" / "probe"))
import run_probe  # noqa: E402

GOLDEN = HERE / "render_golden.txt"


def lines() -> str:
    out = []
    for item in run_probe.load_items():
        for cond, with_recent in (("with", True), ("without", False)):
            b = run_probe.render(item, with_recent).encode("utf-8")
            out.append(f"{item['set']}\t{item['id']}\t{cond}\t{len(b)}\t{zlib.crc32(b):08x}")
    return "\n".join(out) + "\n"


def main() -> int:
    text = lines()
    if "--write" in sys.argv:
        GOLDEN.write_text(text)
        print(f"wrote {GOLDEN} ({text.count(chr(10))} rows)")
        return 0
    if not GOLDEN.exists() or GOLDEN.read_text() != text:
        print("render golden is stale: run with --write")
        return 1
    print("RENDER GOLDEN FRESH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
