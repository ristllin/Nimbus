"""Pure grading of the day-boundary append leg (test_day_boundary_append.py): no device.

The leg replays the shape of 24-25 Oct 2026 on a bench unit set to Europe/London: two
seeded Saturday rows, a Saturday evening row, a row at 00:20 BST on Sunday (the local day
is already the 25th), a Sunday morning row at 08:20 GMT (BST ended at 01:00 UTC), a
correction to that row, then a question about both days' totals. The same leg runs on
three tables with identical expectations (owner ruling 41: food is one validation family
beside expenses and a training log, never a privileged path). The grade comes from the file
the device wrote, its copy after every turn, and the final reply.
"""

from __future__ import annotations

import csv
import io
import re
from typing import Dict, List, NamedTuple

SAT, SUN = "2026-10-24", "2026-10-25"
FIGURES = {"seed": (557, 807), "evening": 990, "night": 35, "morning": 945, "fixed": 910}
SAT_TOTAL = 557 + 807 + 990  # the seeded rows and the evening row
SUN_TOTAL = 35 + 910  # the 00:20 row and the corrected morning row


class Table(NamedTuple):
    project: str
    name: str
    col: str  # the figure column the totals add up
    header: str
    items: Dict[str, str]  # what each row holds, by row key


TABLES = {
    "food": Table(
        "food",
        "food-log.csv",
        "kcal",
        "date,time,item,qty,kcal",
        {"a": "sandwich", "b": "salmon salad", "evening": "beef and rice", "night": "clementine", "morning": "eggs"},
    ),
    "expenses": Table(
        "money",
        "expenses.csv",
        "amount",
        "date,time,item,qty,amount",
        {"a": "groceries", "b": "train pass", "evening": "dinner out", "night": "taxi", "morning": "hardware"},
    ),
    "training": Table(
        "gym",
        "training.csv",
        "kg",
        "date,time,item,qty,kg",
        {"a": "squat", "b": "deadlift", "evening": "bench press", "night": "curl", "morning": "row"},
    ),
}


def seed(t: Table) -> str:
    a, b = FIGURES["seed"]
    return f"{t.header}\n{SAT},08:30,{t.items['a']},1,{a}\n{SAT},12:40,{t.items['b']},1,{b}\n"


class Check(NamedTuple):
    name: str
    ok: bool
    detail: str


def data_rows(text: str) -> List[Dict[str, str]]:
    """The file's data rows, header-keyed."""
    lines = [ln for ln in text.splitlines() if ln.strip()]
    if not lines:
        return []
    return [
        {k.strip().lower(): (v or "").strip() for k, v in r.items() if k}
        for r in csv.DictReader(io.StringIO("\n".join(lines)))
    ]


def figure(row: Dict[str, str], col: str) -> float:
    m = re.match(r"\s*([0-9]+(?:\.[0-9]+)?)", row.get(col, ""))
    return float(m.group(1)) if m else float("nan")


def day_rows(rows: List[Dict[str, str]], day: str) -> List[Dict[str, str]]:
    return [r for r in rows if r.get("date", "")[:10] == day]


def numbers_in(reply: str) -> List[int]:
    """Whole numbers in a reply, thousands separators removed ("2,354" -> 2354)."""
    flat = re.sub(r"(?<=\d)[,  ](?=\d{3}\b)", "", reply or "")
    return [int(n) for n in re.findall(r"\d+", flat)]


def grade(t: Table, after: str, final_reply: str, appends: List[tuple]) -> List[Check]:
    """`appends`: (file before, file after) of every turn that only added rows."""
    rows = data_rows(after)
    sat, sun = day_rows(rows, SAT), day_rows(rows, SUN)
    sat_sum, sun_sum = sum(figure(r, t.col) for r in sat), sum(figure(r, t.col) for r in sun)
    seeded = seed(t).splitlines()
    later = [r for r in rows if r.get("date", "")[:10] > SUN]
    nums = numbers_in(final_reply)
    grown = [b.startswith(a) and len(b) > len(a) for a, b in appends]
    return [
        Check("header kept", after.startswith(seeded[0] + "\n"), after.splitlines()[0] if after else "(empty)"),
        Check("seeded rows byte-identical", all(ln + "\n" in after for ln in seeded[1:]), "the two Saturday rows"),
        Check(
            "every logging turn only appended",
            bool(grown) and all(grown),
            f"{sum(grown)} of {len(grown)} turns kept every earlier byte and added rows",
        ),
        Check("Saturday has 3 rows", len(sat) == 3, f"{len(sat)} rows dated {SAT}"),
        Check("Saturday total exact", abs(sat_sum - SAT_TOTAL) < 0.5, f"{sat_sum:g} (want {SAT_TOTAL})"),
        Check(
            "the 00:20 row on the local day (Sunday)",
            any(abs(figure(r, t.col) - FIGURES["night"]) < 0.5 for r in sun),
            f"Sunday figures {[figure(r, t.col) for r in sun]}",
        ),
        Check(
            "the correction left one row",
            not any(abs(figure(r, t.col) - FIGURES["morning"]) < 0.5 for r in rows)
            and sum(abs(figure(r, t.col) - FIGURES["fixed"]) < 0.5 for r in rows) == 1,
            f"no {FIGURES['morning']} row left, one {FIGURES['fixed']} row",
        ),
        Check("Sunday has 2 rows", len(sun) == 2, f"{len(sun)} rows dated {SUN}"),
        Check("Sunday total exact", abs(sun_sum - SUN_TOTAL) < 0.5, f"{sun_sum:g} (want {SUN_TOTAL})"),
        Check("no row after Sunday", not later, f"{len(later)} later rows"),
        Check(
            "reply states both totals", SAT_TOTAL in nums and SUN_TOTAL in nums, f"numbers in the reply: {nums[:20]}"
        ),
    ]


def verdict(checks: List[Check]) -> str:
    return "PASS" if all(c.ok for c in checks) else "FAIL"


def table(checks: List[Check]) -> str:
    return "\n".join(f"| {c.name} | {'PASS' if c.ok else 'FAIL'} | {c.detail} |" for c in checks)
