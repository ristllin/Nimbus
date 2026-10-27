"""Pure grading of the daily-ledger leg (test_ctx_food_day.py): no device.

The leg replays a full day of food chat on a bench unit set to Europe/London
(3 Nov 2026, GMT): breakfast, a snack, lunch, a correction to lunch, a manual
/compact, a second snack, dinner, then "what's my total for today, by meal?". The
owner never names a file: the device's per-turn ledger block names it. The grade
comes from the ledger the device wrote, the final reply and the fold summary.
"""

from __future__ import annotations

import csv
import io
import re
from typing import Dict, List, NamedTuple

PROJECT, NAME = "food", "food-log.csv"
DAY = "2026-11-03"
# (meal kcal) after the correction; the lunch was first logged at 760.
MEALS = {"breakfast": 420, "lunch": 720, "dinner": 690}
SNACKS = (180, 210)
LUNCH_BEFORE = 760
TOTAL = sum(MEALS.values()) + sum(SNACKS)  # 2220


class Check(NamedTuple):
    name: str
    ok: bool
    detail: str


def data_rows(text: str) -> List[Dict[str, str]]:
    """The ledger's data rows (comment rows starting with '#' left out), header-keyed."""
    lines = [ln for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
    if not lines:
        return []
    return [
        {k.strip().lower(): (v or "").strip() for k, v in r.items() if k}
        for r in csv.DictReader(io.StringIO("\n".join(lines)))
    ]


def kcal(row: Dict[str, str]) -> float:
    m = re.match(r"\s*([0-9]+(?:\.[0-9]+)?)", row.get("kcal", ""))
    return float(m.group(1)) if m else float("nan")


def numbers_in(text: str) -> List[int]:
    """Whole numbers in a text, thousands separators removed ("2,220" -> 2220)."""
    flat = re.sub(r"(?<=\d)[,   ](?=\d{3}\b)", "", text or "")
    return [int(n) for n in re.findall(r"\d+", flat)]


def grade(after: str, final_reply: str, summary: str) -> List[Check]:
    rows = data_rows(after)
    day = [r for r in rows if r.get("date", "")[:10] == DAY]
    total = sum(kcal(r) for r in day)
    meals = [r.get("meal", "").lower() for r in day]
    nums = numbers_in(final_reply)
    figures = sorted(int(kcal(r)) for r in day if kcal(r) == kcal(r))
    return [
        Check(
            "the canonical header",
            after.startswith("date,meal,items,kcal,protein_g\n"),
            after.splitlines()[0] if after else "(no ledger)",
        ),
        Check(
            "one row per eating occasion",
            len(day) == 5 and len(set(meals)) == 5,
            f"{len(day)} rows dated {DAY}: {meals}",
        ),
        Check("the day's total exact in the ledger", abs(total - TOTAL) < 0.5, f"{total:g} (want {TOTAL})"),
        Check(
            "the correction replaced the lunch row",
            not any(abs(kcal(r) - LUNCH_BEFORE) < 0.5 for r in rows),
            f"kcal values {figures}",
        ),
        Check("no row on another day", len(day) == len(rows), f"{len(rows) - len(day)} other rows"),
        Check("the final reply states the total", TOTAL in nums, f"numbers in the reply: {nums[:20]}"),
        Check(
            "the final reply lists every meal's kcal",
            all(v in nums for v in list(MEALS.values()) + list(SNACKS)),
            f"numbers in the reply: {nums[:20]}",
        ),
        Check(
            "the fold summary is dated",
            DAY in (summary or ""),
            "the summary names the day" if DAY in (summary or "") else "no YYYY-MM-DD date in the summary",
        ),
    ]


def verdict(checks: List[Check]) -> str:
    return "PASS" if all(c.ok for c in checks) else "FAIL"


def table(checks: List[Check]) -> str:
    return "\n".join(f"{'PASS' if c.ok else 'FAIL'}  {c.name}: {c.detail}" for c in checks)
