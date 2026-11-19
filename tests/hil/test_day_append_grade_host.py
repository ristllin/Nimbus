"""Host tests of the day-boundary append leg's grading (day_append_grade.py): no device.
Every case runs on the three tables with identical expectations (owner ruling 41)."""

from __future__ import annotations

import pytest

import day_append_grade as dg

pytestmark = pytest.mark.host

DOMAINS = sorted(dg.TABLES)


def good(t: dg.Table) -> str:
    i = t.items
    return dg.seed(t) + (
        f"2026-10-24,21:10,{i['evening']},1,990\n2026-10-25,00:20,{i['night']},1,35\n2026-10-25,08:20,{i['morning']},1,910\n"
    )


def appends(t: dg.Table) -> list:
    """The three logging turns' files before and after (pure appends), then the edit."""
    lines = good(t).splitlines(keepends=True)
    s = "".join(lines[:3])
    return [(s, s + lines[3]), (s + lines[3], s + "".join(lines[3:5])), ("".join(lines[:5]), good(t))]


@pytest.mark.parametrize("domain", DOMAINS)
def test_the_right_file_and_reply_pass(domain):
    t = dg.TABLES[domain]
    checks = dg.grade(t, good(t), "24 Oct: 2,354. Today (25 Oct): 945 so far.", appends(t))
    assert dg.verdict(checks) == "PASS", dg.table(checks)


@pytest.mark.parametrize("domain", DOMAINS)
@pytest.mark.parametrize(
    "mutate, failing",
    [
        # the correction appended instead of edited: counted twice
        (lambda s, t: s + f"2026-10-25,08:25,{t.items['morning']},1,910\n", "Sunday total exact"),
        (lambda s, t: s.replace(",1,910\n", ",1,945\n"), "the correction left one row"),
        # the 00:20 row dated by the UTC day (still Saturday at 23:20 UTC)
        (lambda s, t: s.replace("2026-10-25,00:20", "2026-10-24,00:20"), "the 00:20 row on the local day (Sunday)"),
        # yesterday's evening row overwritten by this morning's (the 25 Oct mix-up)
        (
            lambda s, t: s.replace(f"2026-10-24,21:10,{t.items['evening']},1,990", "2026-10-24,21:10,x,1,910"),
            "Saturday total exact",
        ),
        # a seeded row lost
        (lambda s, t: s.replace(f"2026-10-24,12:40,{t.items['b']},1,807\n", ""), "seeded rows byte-identical"),
        (lambda s, t: s.replace(s.splitlines()[0], "date,item,figure"), "header kept"),
        (lambda s, t: s + "2026-10-26,09:00,x,1,1\n", "no row after Sunday"),
    ],
)
def test_each_failure_names_its_check(domain, mutate, failing):
    t = dg.TABLES[domain]
    checks = dg.grade(t, mutate(good(t), t), "2354 and 945", appends(t))
    bad = [c.name for c in checks if not c.ok]
    assert failing in bad, dg.table(checks)


@pytest.mark.parametrize("domain", DOMAINS)
def test_a_turn_that_rewrote_earlier_bytes_fails(domain):
    t = dg.TABLES[domain]
    before, after = appends(t)[0]
    rewritten = [(before, after.replace(",1,557\n", ",1,557.0\n"))]
    checks = dg.grade(t, good(t), "2354 and 945", rewritten)
    assert "every logging turn only appended" in [c.name for c in checks if not c.ok]
    assert "every logging turn only appended" in [c.name for c in dg.grade(t, good(t), "2354 945", []) if not c.ok]


@pytest.mark.parametrize("domain", DOMAINS)
def test_a_reply_without_both_totals_fails(domain):
    t = dg.TABLES[domain]
    checks = dg.grade(t, good(t), "Saturday was 2,354.", appends(t))
    assert [c.name for c in checks if not c.ok] == ["reply states both totals"]


def test_the_three_tables_share_every_expectation():
    """No table is privileged: the same rows, figures and checks, only names differ."""
    shapes = {
        (t.header.split(",")[:4] == ["date", "time", "item", "qty"], tuple(sorted(t.items))) for t in dg.TABLES.values()
    }
    assert len(shapes) == 1 and len(dg.TABLES) == 3
