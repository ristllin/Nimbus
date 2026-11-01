#!/usr/bin/env python3
"""Self test of parity_table.py: free, no docker. A failing scenario must read FAIL."""

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import parity_table  # noqa: E402


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "parity-a").mkdir()
        (root / "parity-a" / "summary.json").write_text(
            json.dumps({"scenarios": [{"id": "chat", "pass": True, "judge": {"min_score": 0.9}}, {"id": "web", "pass": False}]})
        )
        (root / "parity-b").mkdir()
        (root / "parity-b" / "summary.json").write_text(json.dumps({"scenarios": [{"id": "chat", "pass": False}]}))
        table = parity_table.render(parity_table.load_summaries(root), "ev/")
    assert "[PASS 0.90](ev/parity-a/chat/)" in table, table
    assert "[FAIL](ev/parity-a/web/)" in table, table
    assert "| parity-b | [FAIL](ev/parity-b/chat/) | - |" in table, table
    print("parity_table self test OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
