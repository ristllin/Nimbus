"""Host tests of the QW3 food-log leg's grading (qw3_food_grade.py): no device."""

from __future__ import annotations

import pytest

import qw3_food_grade as fg

pytestmark = pytest.mark.host

GOOD = fg.SEED + (
    "2026-10-24,dinner,beef + skyr + chicken + eggplant,990,88\n"
    "2026-10-25,snack-0020,clementine,35,1\n"
    "2026-10-25,breakfast,chicken + eggs + skyr + berries,910,103\n"
)


def test_the_right_file_and_reply_pass():
    checks = fg.grade(GOOD, "24 Oct: 2,354 kcal. Today (25 Oct): 945 kcal so far.")
    assert fg.verdict(checks) == "PASS", fg.table(checks)


@pytest.mark.parametrize(
    "bad, failing",
    [
        # the correction appended instead of replacing: double-counted
        (GOOD + "2026-10-25,breakfast-fix,chicken,910,103\n", "Sunday total exact"),
        (GOOD.replace("910,103", "945,107"), "correction replaced, not appended"),
        # the clementine dated by the UTC day (still Saturday at 23:20 UTC)
        (GOOD.replace("2026-10-25,snack-0020", "2026-10-24,snack-0020"), "clementine on the local day (Sunday)"),
        # yesterday's dinner overwritten by this morning's breakfast (the 25 Oct mix-up)
        (
            GOOD.replace(
                "2026-10-24,dinner,beef + skyr + chicken + eggplant,990,88", "2026-10-24,dinner,chicken,910,103"
            ),
            "Saturday total exact",
        ),
        # a seeded row lost
        (GOOD.replace("2026-10-24,lunch,bread + salmon + beans,807,66\n", ""), "seeded rows byte-identical"),
        (GOOD + "#cut: 2026-10-25,lunch,4\n", "no cut rows"),
    ],
)
def test_each_failure_is_caught(bad, failing):
    checks = {c.name: c for c in fg.grade(bad, "2354 and 945")}
    assert not checks[failing].ok


def test_reply_numbers_ignore_thousands_separators():
    assert 2354 in fg.numbers_in("2,354 kcal")
    assert fg.numbers_in("no totals") == []
    checks = {c.name: c for c in fg.grade(GOOD, "Saturday 2354")}
    assert not checks["reply states both totals"].ok
