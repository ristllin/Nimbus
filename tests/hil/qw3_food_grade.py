"""Pure grading of the QW3 food-log leg (test_qw3_food_log_midnight.py): no device.

The leg replays 24-25 Oct 2026 on a bench unit set to Europe/London: two seeded
Saturday meals, Saturday's dinner, a clementine at 00:20 BST on Sunday, Sunday's
breakfast at 08:20 GMT, a correction to that breakfast, then a question about both
days' totals. The grade comes from the food log the device wrote and the final reply.
"""

from __future__ import annotations

import csv
import io
import re
from typing import Dict, List, NamedTuple

SEED = (
    "date,meal,items,kcal,protein_g\n"
    "2026-10-24,breakfast,sandwich + omelette + cappuccino,557,32\n"
    "2026-10-24,lunch,bread + salmon + beans,807,66\n"
)
SAT, SUN = "2026-10-24", "2026-10-25"
SAT_TOTAL = 557 + 807 + 990  # the seeded meals and the dinner
SUN_TOTAL = 35 + 910  # the clementine and the corrected breakfast
CLEMENTINE_KCAL, BREAKFAST_BEFORE, BREAKFAST_AFTER = 35, 945, 910


class Check(NamedTuple):
    name: str
    ok: bool
    detail: str


def data_rows(text: str) -> List[Dict[str, str]]:
    """The log's data rows (comment rows starting with '#' left out), header-keyed."""
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


def day_rows(rows: List[Dict[str, str]], day: str) -> List[Dict[str, str]]:
    return [r for r in rows if r.get("date", "")[:10] == day]


def numbers_in(reply: str) -> List[int]:
    """Whole numbers in a reply, thousands separators removed ("2,354" -> 2354)."""
    flat = re.sub(r"(?<=\d)[,  ](?=\d{3}\b)", "", reply or "")
    return [int(n) for n in re.findall(r"\d+", flat)]


def grade(after: str, final_reply: str) -> List[Check]:
    rows = data_rows(after)
    sat, sun = day_rows(rows, SAT), day_rows(rows, SUN)
    sat_sum, sun_sum = sum(kcal(r) for r in sat), sum(kcal(r) for r in sun)
    seeded = [ln for ln in SEED.splitlines()[1:]]
    later = [r for r in rows if r.get("date", "")[:10] > SUN]
    nums = numbers_in(final_reply)
    return [
        Check(
            "header kept", after.startswith(SEED.splitlines()[0] + "\n"), after.splitlines()[0] if after else "(empty)"
        ),
        Check("seeded rows byte-identical", all(ln + "\n" in after for ln in seeded), "the two Saturday rows"),
        Check("Saturday has 3 rows", len(sat) == 3, f"{len(sat)} rows dated {SAT}"),
        Check("Saturday total exact", abs(sat_sum - SAT_TOTAL) < 0.5, f"{sat_sum:g} (want {SAT_TOTAL})"),
        Check(
            "clementine on the local day (Sunday)",
            any(abs(kcal(r) - CLEMENTINE_KCAL) < 0.5 for r in sun),
            f"Sunday kcal values {[kcal(r) for r in sun]}",
        ),
        Check(
            "correction replaced, not appended",
            not any(abs(kcal(r) - BREAKFAST_BEFORE) < 0.5 for r in rows)
            and sum(abs(kcal(r) - BREAKFAST_AFTER) < 0.5 for r in rows) == 1,
            "no 945 row left, one 910 row",
        ),
        Check("Sunday has 2 rows", len(sun) == 2, f"{len(sun)} rows dated {SUN}"),
        Check("Sunday total exact", abs(sun_sum - SUN_TOTAL) < 0.5, f"{sun_sum:g} (want {SUN_TOTAL})"),
        Check("no row after Sunday", not later, f"{len(later)} later rows"),
        Check("no cut rows", "#cut:" not in after, ""),
        Check(
            "reply states both totals", SAT_TOTAL in nums and SUN_TOTAL in nums, f"numbers in the reply: {nums[:20]}"
        ),
    ]


def verdict(checks: List[Check]) -> str:
    return "PASS" if all(c.ok for c in checks) else "FAIL"


def table(checks: List[Check]) -> str:
    return "\n".join(f"| {c.name} | {'PASS' if c.ok else 'FAIL'} | {c.detail} |" for c in checks)
