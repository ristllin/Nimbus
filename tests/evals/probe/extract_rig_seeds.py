#!/usr/bin/env python3
"""Extract the reference rig's false corrections as truth-probe seed rows.

The rig (tools/refloop) ran synthetic scenarios against the old harness. On 23 turns the
harness appended a "(Correction: ...)" line to a reply that was true; those replies are the
seeds of the true past-reference set. Each row keeps the owner's message, the reply as the
model wrote it (the correction line removed), the turn's tool calls and a recent-effects
ring rebuilt from the earlier turns' writes in the same run.

  python3 tests/evals/probe/extract_rig_seeds.py --evidence <RBREF evidence dir> [--out seeds_rig.json]

The scenarios are synthetic (tests/evals/refloop/scenarios), so no owner text is involved.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORRECTION = re.compile(r"\n*\(Correction:[^)]*\)\n*")
DASH = "—"  # the reply text's long dash becomes a hyphen (project style rule)
# Tools that only read. Everything else the rig's registry exposes changes state.
READS = {
    "device_status",
    "docs_search",
    "files_list",
    "files_read",
    "food_portions",
    "image_look",
    "loop_list",
    "memory_episodic",
    "memory_search",
    "session_spawn",
}
RECEIPT_MAX = 160  # bytes, the ring's receipt field (PRD section 8, item 8)


def receipt(text: str) -> str:
    line = " ".join((text or "").split())
    raw = line.encode()
    if len(raw) <= RECEIPT_MAX:
        return line
    return raw[: RECEIPT_MAX - 3].decode(errors="ignore") + "..."


def clock(turn: int) -> str:
    minutes = 9 * 60 + 4 * turn  # synthetic: the rig's turns carry no wall time
    return f"{minutes // 60:02d}:{minutes % 60:02d}"


def call_row(tool: dict) -> dict:
    result = str(tool.get("result", ""))
    return {"tool": tool["name"], "receipt": receipt(result), "ok": not result.startswith("ERROR")}


def ring_before(steps: list, index: int) -> list:
    ring = []
    for s in steps[:index]:
        for t in s.get("tools") or []:
            if t["name"] in READS or str(t.get("result", "")).startswith("ERROR"):
                continue
            ring.append({"at": clock(s.get("turn", 0)), "tool": t["name"], "receipt": receipt(str(t.get("result")))})
    return ring[-50:]


def clean(text: str) -> str:
    return CORRECTION.sub("\n\n", text).strip().replace(DASH, "-")


def seeds(evidence: Path) -> list:
    rows = []
    for f in sorted(evidence.glob("*/*/rep*/A/turns.json")):
        steps = json.loads(f.read_text())["steps"]
        model, scen, rep = f.parts[-5], f.parts[-4], f.parts[-3]
        for i, s in enumerate(steps):
            reply = s.get("reply") or ""
            if "(Correction:" not in reply:
                continue
            rows.append(
                {
                    "id": f"rig-{model}-{scen}-{rep}-t{s.get('turn')}",
                    "family": "rig-seed",
                    "owner": "\n".join(s.get("user") or []).replace(DASH, "-"),
                    "reply": clean(reply),
                    "this_turn": [call_row(t) for t in s.get("tools") or []],
                    "recent": ring_before(steps, i),
                    "claim": False,
                    "from": f"rig {model}/{scen}/{rep} turn {s.get('turn')}",
                }
            )
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--evidence", required=True, type=Path)
    ap.add_argument("--out", type=Path, default=HERE / "seeds_rig.json")
    args = ap.parse_args()
    rows = seeds(args.evidence)
    args.out.write_text(json.dumps(rows, indent=1, ensure_ascii=False) + "\n")
    print(f"{len(rows)} seed rows -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
