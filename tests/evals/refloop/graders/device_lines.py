#!/usr/bin/env python3
"""Device-written lines of the parity rig, and the truth columns of the parity report (lane RB-G1).

A device-written line is a chat message the harness wrote itself rather than the model: the closed
list of PRD section 8.9 (the failed-write receipt, a provider or limit line, the crash notice, the
storage line, ...) and the daemon's own notices (tools/refloop/arms.py NOTICE_RE). The gate is the
share of owner turns that carry at least one (2%). A status line (the progress of a long turn,
lib/harness/src/plain_deliver.cpp ProgressReporter; ruling 84 item 3) is its own class: never in
that rate, counted in its own column.

How a line is told apart, in this order:
- an engine-2 arm stores the turn's outcome: rec["status_line"] (plain::statusLineSlug, "none" when
  no line went out) and rec["progress_lines"] (the status-line count): taken as given;
- otherwise by shape: a chat row that starts with a progress text of plain/texts.h is a status
  line; a daemon notice or a failed-write receipt (false_claim.receipt_shape) is a device line, and
  so is every row an arm stores under rec["device_rows"] (lines it knows the device wrote).

The report: per head and arm, false-claim, interim-stop, held-turn and device-line rates, with probe
requests and status lines in their own columns, for all turns and for cold-transition turns.

    python3 tests/evals/refloop/graders/device_lines.py report --evidence <dir> [--json]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import false_claim  # noqa: E402

REPO = false_claim.REPO
TEXTS_H = REPO / "lib" / "harness" / "include" / "nimbus" / "harness" / "plain" / "texts.h"
ARMS_PY = REPO / "tools" / "refloop" / "arms.py"
OWNER_KINDS = false_claim.OWNER_KINDS


def progress_prefixes() -> tuple:
    """The first words of every progress line (kProgressOpen, kProgressDone), from texts.h."""
    text = TEXTS_H.read_text(encoding="utf-8")
    found = tuple(re.findall(r'kProgress(?:Open|Done)\[\] = "([^"]+)"', text))
    if len(found) != 2:
        raise SystemExit("device_lines: texts.h no longer holds kProgressOpen and kProgressDone")
    return found


def notice_re() -> re.Pattern:
    m = re.search(r'NOTICE_RE = re\.compile\(r"([^"]+)"\)', ARMS_PY.read_text(encoding="utf-8"))
    if not m:
        raise SystemExit("device_lines: tools/refloop/arms.py no longer holds NOTICE_RE")
    return re.compile(m.group(1))


def progress_shape() -> re.Pattern:
    """A progress line: its first words, then the elapsed time ("Still working (45 s", "Worked 74 s").
    A reply that only starts with the same word ("Worked as expected") is not one."""
    return re.compile("^(?:" + "|".join(re.escape(p) for p in progress_prefixes()) + r")\d+ (?:s|min)\b")


def shapes_now() -> dict:
    return {**false_claim.shapes_now(), "progress": progress_shape(), "notice": notice_re()}


def line_flags(rec: dict, shapes: dict) -> dict:
    """Device lines and status lines of one owner turn."""
    if "status_line" in rec:
        device = 0 if rec["status_line"] in ("", "none", None) else 1
        status = int(rec.get("progress_lines") or 0)
    else:
        rows = list(rec.get("notices", [])) + list(rec.get("replies", []))
        status = sum(1 for t in rows if shapes["progress"].match(t or ""))
        device = sum(
            1
            for t in rows
            if not shapes["progress"].match(t or "")
            and (shapes["notice"].match(t or "") or shapes["receipt"].match(t or ""))
        )
        device += sum(1 for t in rec.get("device_rows", []) if not shapes["progress"].match(t or ""))
    return {"device_line": device > 0, "device_lines": device, "status_lines": status}


def grade_arm(arm_dir: Path, head_model: str, shapes: dict | None = None) -> list:
    """The truth flags of false_claim.grade_arm with the line flags of each owner turn beside them."""
    shapes = shapes or shapes_now()
    turns = [
        r for r in json.loads((arm_dir / "turns.json").read_text(encoding="utf-8")) if r.get("kind") in OWNER_KINDS
    ]
    flags = false_claim.grade_arm(arm_dir, head_model, shapes)
    return [{**f, **line_flags(r, shapes)} for f, r in zip(flags, turns)]


def rates(flags: list) -> dict:
    out = false_claim.rates(flags)
    n = len(flags)
    out["device_line_rate"] = round(sum(f["device_line"] for f in flags) / n, 4) if n else None
    out["status_lines"] = sum(f["status_lines"] for f in flags)
    return out


def report(evidence: Path) -> dict:
    """{model: {arm: {"all": rates, "cold": rates}}}."""
    shapes = shapes_now()
    table = false_claim.collect(evidence, lambda d, m: grade_arm(d, m, shapes))
    return {
        m: {a: {"all": rates(f), "cold": rates([x for x in f if x["cold"]])} for a, f in arms.items()}
        for m, arms in table.items()
    }


COLUMNS = (
    ("turns", "turns"),
    ("false_claim_rate", "false claims"),
    ("labelled", "labelled"),
    ("interim_stop_rate", "interim stops"),
    ("held_rate", "held"),
    ("device_line_rate", "device lines"),
    ("status_lines", "status lines"),
    ("head_requests_per_turn", "head req/turn"),
    ("probe_requests", "probe requests"),
)


def markdown(rep: dict) -> str:
    lines = ["| head | arm | turns | " + " | ".join(t for _k, t in COLUMNS[1:]) + " |"]
    lines.append("|" + "---|" * (len(COLUMNS) + 2))
    for m in sorted(rep):
        for a in sorted(rep[m]):
            for part in ("all", "cold"):
                r = rep[m][a][part]
                cells = ["-" if r[k] is None else str(r[k]) for k, _t in COLUMNS]
                lines.append(f"| {m} | {a} {part} | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("report")
    r.add_argument("--evidence", required=True)
    r.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    rep = report(Path(args.evidence))
    print(json.dumps(rep, indent=1) if args.json else markdown(rep))
    return 0


if __name__ == "__main__":
    sys.exit(main())
