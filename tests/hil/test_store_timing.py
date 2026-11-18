#!/usr/bin/env python3
"""Lane RB-M2: card timing on the bench, each operation with its card lock hold.

The rebuild PRD (section 2) gates the transcript on the card: every append goes through
the card lock, the longest lock hold is 150 ms, a logged meal is 5 transcript appends,
and a restart refills the transcript with one sequential read of 80 to 130 KB in 8 KB
slices inside 3 s of card time. PLAN.json asks for appends of 1, 4 and 16 KB, a 130 KB
sequential read and a 20 KB whole-file replace with its index write.

The device times each operation itself (console verb CARDTIME, src/test_console.cpp):
wait = ms to take the card lock, hold = ms it was held. `replace` is the file store's
own whole-file replace (agent::files::saveText: part file, rename, index rewrite);
`rawreplace` is the same bytes without the index, so their difference is the index
write. Driven by `python3 tests/hil/test_link_budget.py bench store <outdir>`.
"""

from __future__ import annotations

import json
import os
import re
from typing import Dict, List, Optional, Sequence

import pytest

CARD_RE = re.compile(r"^CARDTIME (?P<op>append|replace|rawreplace|read) (?P<kv>.*)$")
HOLD_GATE_MS = 150  # PRD section 2: the longest card lock hold
REFILL_GATE_MS = 3000  # PRD section 2: restart to full transcript in card time

APPEND_SIZES = (1024, 4096, 16384)
READ_BYTES, READ_SLICE = 133120, 8192
REPLACE_BYTES = 20480


def parse_card(line: str) -> Optional[Dict[str, object]]:
    m = CARD_RE.match(line.strip())
    if not m:
        return None
    out: Dict[str, object] = {"op": m.group("op")}
    for k, v in re.findall(r"(\w+)=(\S+)", m.group("kv")):
        out[k] = int(v) if re.fullmatch(r"-?\d+", v) else v
    return out


def parse_card_row(line: str) -> Dict[str, object]:
    """A row for every answer slot: a line that does not parse (no answer, CARDTIME ERR, a
    usage line) is a failed run, never dropped."""
    return parse_card(line or "") or {"op": "failed", "line": (line or "(no answer)")[:80]}


def card_ok(r: Dict[str, object]) -> bool:
    if r["op"] == "failed":
        return False
    if r["op"] == "append":
        return r.get("wrote") == r.get("bytes")
    if r["op"] == "read":
        return r.get("got") == r.get("bytes")
    return r.get("ok") == 1


def summarize_card(rows: Sequence[Dict[str, object]], key: str) -> Dict[str, object]:
    sys_path_fix()
    from test_link_budget import summary  # noqa: PLC0415

    good = [r for r in rows if card_ok(r)]
    return {"runs": len(rows), "ok": len(good), key: summary([r[key] for r in good if key in r])}


def sys_path_fix():
    import sys  # noqa: PLC0415

    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)


# ---- host tests -------------------------------------------------------------------------

pytestmark = pytest.mark.host


def test_card_lines_parse():
    r = parse_card("CARDTIME append bytes=4096 wrote=4096 wait=0 hold=23")
    assert r == {"op": "append", "bytes": 4096, "wrote": 4096, "wait": 0, "hold": 23} and card_ok(r)
    rd = parse_card("CARDTIME read bytes=133120 got=133120 slice=8192 slices=17 total=310 holdSum=290 holdMax=31")
    assert rd["slices"] == 17 and card_ok(rd)
    rp = parse_card("CARDTIME replace bytes=20480 ok=1 wait=0 hold=140 err=-")
    assert rp["hold"] == 140 and card_ok(rp)


def test_short_write_or_failed_replace_is_not_a_sample():
    # Counter-test: an append that landed fewer bytes, a short read and a refused replace
    # are failures, never timing samples.
    rows = [
        parse_card("CARDTIME append bytes=4096 wrote=1024 wait=0 hold=5"),
        parse_card("CARDTIME read bytes=133120 got=8192 slice=8192 slices=1 total=9 holdSum=9 holdMax=9"),
        parse_card("CARDTIME replace bytes=20480 ok=0 wait=0 hold=3 err=SD"),
        parse_card("CARDTIME append bytes=4096 wrote=4096 wait=0 hold=30"),
    ]
    assert [card_ok(r) for r in rows] == [False, False, False, True]
    s = summarize_card([rows[0], rows[3]], "hold")
    assert s["ok"] == 1 and s["hold"]["median"] == 30


def test_a_missing_or_refused_answer_is_a_failed_run():
    rows = [
        parse_card_row(""),
        parse_card_row("CARDTIME ERR no PSRAM"),
        parse_card_row("CARDTIME append bytes=1024 wrote=1024 wait=0 hold=6"),
    ]
    s = summarize_card(rows, "hold")
    assert s["runs"] == 3 and s["ok"] == 1


def test_malformed_card_line_is_refused():
    assert parse_card("CARDTIME usage: CARDTIME append|replace") is None
    assert parse_card("CARDTIME ERR no PSRAM") is None


# ---- the bench plan ---------------------------------------------------------------------


def plan_store(b, reps: int, out: str) -> None:
    """Every operation reps times (reads and replaces too); raw lines kept in store.raw.json."""
    res: Dict[str, List[str]] = {}

    def run(cmd: str, n: int) -> List[str]:
        got = []
        for i in range(n):
            ln = b.verb(f"CARDTIME {cmd}", "CARDTIME ", 30)
            b.say(f"> CARDTIME {cmd} [{i + 1}/{n}] -> {ln or '(no answer)'}")
            got.append(ln)
        return got

    for size in APPEND_SIZES:
        res[f"append_{size}"] = run(f"append {size}", reps)
    res["read_130k_8k"] = run(f"read {READ_BYTES} {READ_SLICE}", reps)
    res["replace_20k"] = run(f"replace {REPLACE_BYTES}", reps)
    res["rawreplace_20k"] = run(f"rawreplace {REPLACE_BYTES}", reps)
    with open(os.path.join(out, "store.raw.json"), "a") as f:
        f.write(json.dumps(res) + "\n")
