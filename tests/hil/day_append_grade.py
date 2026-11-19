"""Pure grading of the day-boundary append leg (test_day_boundary_append.py): no device.

The leg replays the shape of 24-25 Oct 2026 on a bench unit set to Europe/London: two
seeded Saturday rows, a Saturday evening row, a row at 00:20 BST on Sunday, a Sunday morning
row at 08:20 GMT (BST ended at 01:00 UTC), a correction to that row, then a question about
both days' totals. Each row belongs to the device's running day (RB-REC5, the setting
day_starts, default 04:00: before it the day still running is yesterday, so the 00:20 row
is Saturday's; with a 00:00 start it is Sunday's): the expectations follow the setting. The same leg runs on
three tables with identical expectations (owner ruling 41: food is one validation family
beside expenses and a training log, never a privileged path). The grade comes from the file
the device wrote, its copy after every turn, and the final reply.
"""

from __future__ import annotations

import csv
import io
import re
from datetime import datetime, timedelta, timezone
from typing import Dict, List, NamedTuple
from zoneinfo import ZoneInfo

SAT, SUN = "2026-10-24", "2026-10-25"
FIGURES = {"seed": (557, 807), "evening": 990, "night": 35, "morning": 945, "fixed": 910}
ZONE = "Europe/London"
# When each logging turn happens (UTC epoch): 21:10 BST Saturday, 00:20 BST Sunday, 08:20 GMT Sunday.
WHEN = {"evening": 1792872600, "night": 1792884000, "morning": 1792916400}
DEFAULT_START = "04:00"


def running_date(epoch: int, start: str = DEFAULT_START, zone: str = ZONE) -> str:
    """The device's running day of `epoch` (lib/core day_start.h runningDay): the local date, one
    day less while the local time is before the day start."""
    local = datetime.fromtimestamp(epoch, timezone.utc).astimezone(ZoneInfo(zone))
    hh, mm = (int(x) for x in start.split(":"))
    if local.hour * 60 + local.minute < hh * 60 + mm:
        local -= timedelta(days=1)
    return local.date().isoformat()


def day_totals(start: str = DEFAULT_START) -> Dict[str, float]:
    """Each day's expected total and row count under a day start: the seeded rows are Saturday's."""
    out = {SAT: [float(sum(FIGURES["seed"])), 2], SUN: [0.0, 0]}
    for key, fig in (("evening", FIGURES["evening"]), ("night", FIGURES["night"]), ("morning", FIGURES["fixed"])):
        day = out.setdefault(running_date(WHEN[key], start), [0.0, 0])
        day[0] += fig
        day[1] += 1
    return {d: (v[0], v[1]) for d, v in out.items()}


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


def _file_checks(t: Table, after: str, appends: List[tuple]) -> List[Check]:
    """The file as the device left it: the header, the seeded rows, every append a pure one."""
    seeded = seed(t).splitlines()
    grown = [b.startswith(a) and len(b) > len(a) for a, b in appends]
    return [
        Check("header kept", after.startswith(seeded[0] + "\n"), after.splitlines()[0] if after else "(empty)"),
        Check("seeded rows byte-identical", all(ln + "\n" in after for ln in seeded[1:]), "the two Saturday rows"),
        Check(
            "every logging turn only appended",
            bool(grown) and all(grown),
            f"{sum(grown)} of {len(grown)} turns kept every earlier byte and added rows",
        ),
    ]


def _day_checks(t: Table, rows: List[Dict[str, str]], start: str) -> List[Check]:
    """Each row on its running day, the correction leaving one row, both days' totals exact."""
    want = day_totals(start)
    near = lambda r, v: abs(figure(r, t.col) - v) < 0.5  # noqa: E731
    night_day = running_date(WHEN["night"], start)
    checks = [
        Check(
            f"the 00:20 row on its running day ({night_day})",
            any(near(r, FIGURES["night"]) for r in day_rows(rows, night_day)),
            f"figures dated {night_day}: {[figure(r, t.col) for r in day_rows(rows, night_day)]}",
        ),
        Check(
            "the correction left one row",
            not any(near(r, FIGURES["morning"]) for r in rows) and sum(near(r, FIGURES["fixed"]) for r in rows) == 1,
            f"no {FIGURES['morning']} row left, one {FIGURES['fixed']} row",
        ),
    ]
    for day, (total, count) in sorted(want.items()):
        got = day_rows(rows, day)
        got_sum = sum(figure(r, t.col) for r in got)
        checks.append(Check(f"{day} has {count} rows", len(got) == count, f"{len(got)} rows dated {day}"))
        checks.append(Check(f"{day} total exact", abs(got_sum - total) < 0.5, f"{got_sum:g} (want {total:g})"))
    later = [r for r in rows if r.get("date", "")[:10] > SUN]
    checks.append(Check("no row after Sunday", not later, f"{len(later)} later rows"))
    return checks


def grade(t: Table, after: str, final_reply: str, appends: List[tuple], start: str = DEFAULT_START) -> List[Check]:
    """`appends`: (file before, file after) of every turn that only added rows; `start` the device's
    day_starts setting during the leg."""
    nums = numbers_in(final_reply)
    want = day_totals(start)
    sat, sun = int(want[SAT][0]), int(want[SUN][0])
    return (
        _file_checks(t, after, appends)
        + _day_checks(t, data_rows(after), start)
        + [Check("reply states both totals", sat in nums and sun in nums, f"numbers in the reply: {nums[:20]}")]
    )


def verdict(checks: List[Check]) -> str:
    return "PASS" if all(c.ok for c in checks) else "FAIL"


def table(checks: List[Check]) -> str:
    return "\n".join(f"| {c.name} | {'PASS' if c.ok else 'FAIL'} | {c.detail} |" for c in checks)
