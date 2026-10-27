"""Host tests of the daily-ledger leg's grading (ctx_food_grade.py): no device."""

from __future__ import annotations

import pytest

import ctx_food_grade as fg

pytestmark = pytest.mark.host

GOOD = (
    "date,meal,items,kcal,protein_g\n"
    "2026-11-03,breakfast,porridge + milk + banana,420,28\n"
    "2026-11-03,snack-1030,apple + almonds,180,6\n"
    "2026-11-03,lunch,chicken wrap + soup,720,52\n"
    "2026-11-03,snack-1605,protein yoghurt,210,20\n"
    "2026-11-03,dinner,salmon + rice + greens,690,48\n"
)
REPLY = "Today: 2,220 kcal. Breakfast 420, snack 180, lunch 720, snack 210, dinner 690."
SUMMARY = "2. Pending items & next steps\n- Food ledger for 2026-11-03: 1320 kcal from 3 rows."


def test_the_right_ledger_reply_and_summary_pass():
    checks = fg.grade(GOOD, REPLY, SUMMARY)
    assert fg.verdict(checks) == "PASS", fg.table(checks)
    assert fg.TOTAL == 2220


@pytest.mark.parametrize(
    "ledger, reply, summary, failing",
    [
        # the correction appended instead of replacing: double-counted
        (GOOD + "2026-11-03,lunch-fix,chicken wrap,720,52\n", REPLY, SUMMARY, "the day's total exact in the ledger"),
        (GOOD.replace("720,52", "760,52"), REPLY, SUMMARY, "the correction replaced the lunch row"),
        # a meal logged on another day
        (GOOD.replace("2026-11-03,dinner", "2026-11-02,dinner"), REPLY, SUMMARY, "no row on another day"),
        # two snacks under one meal name (the second replaced the first)
        (GOOD.replace("snack-1605", "snack-1030"), REPLY, SUMMARY, "one row per eating occasion"),
        # the total rebuilt from memory (the 26 Oct class)
        (GOOD, REPLY.replace("2,220", "2,402"), SUMMARY, "the final reply states the total"),
        (GOOD, "Today: 2,220 kcal.", SUMMARY, "the final reply lists every meal's kcal"),
        # a summary that says "today" instead of a date
        (GOOD, REPLY, "2. Pending items\n- Today's food: 1320 kcal.", "the fold summary is dated"),
        # no ledger at all
        ("", REPLY, SUMMARY, "the canonical header"),
    ],
)
def test_each_failure_is_caught(ledger, reply, summary, failing):
    checks = fg.grade(ledger, reply, summary)
    assert fg.verdict(checks) == "FAIL"
    assert failing in [c.name for c in checks if not c.ok], fg.table(checks)


def test_numbers_read_thousands_separators():
    assert fg.numbers_in("2,220 kcal and 2 220 and 2 220") == [2220, 2220, 2220]
