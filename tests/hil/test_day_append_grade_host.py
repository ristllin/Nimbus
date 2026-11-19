"""Host tests of the day-boundary append leg's grading (day_append_grade.py): no device.
Every case runs on the three tables with identical expectations (owner ruling 41), under the
default day start (04:00, RB-REC5) and a midnight one."""

from __future__ import annotations

import pytest

import day_append_grade as dg

DOMAINS = sorted(dg.TABLES)
STARTS = ["04:00", "00:00"]

pytestmark = pytest.mark.host


def good(t: dg.Table, start: str) -> str:
    i, d = t.items, lambda key: dg.running_date(dg.WHEN[key], start)
    return dg.seed(t) + (
        f"{d('evening')},21:10,{i['evening']},1,990\n{d('night')},00:20,{i['night']},1,35\n"
        f"{d('morning')},08:20,{i['morning']},1,910\n"
    )


def appends(t: dg.Table, start: str) -> list:
    """The three logging turns' files before and after (pure appends), then the edit."""
    lines = good(t, start).splitlines(keepends=True)
    s = "".join(lines[:3])
    return [(s, s + lines[3]), (s + lines[3], s + "".join(lines[3:5])), ("".join(lines[:5]), good(t, start))]


def reply(start: str) -> str:
    want = dg.day_totals(start)
    return f"24 Oct: {int(want[dg.SAT][0]):,}. Today (25 Oct): {int(want[dg.SUN][0])} so far."


def test_the_running_day_follows_the_setting():
    assert dg.running_date(dg.WHEN["night"], "04:00") == dg.SAT  # 00:20 BST: Saturday is still running
    assert dg.running_date(dg.WHEN["night"], "00:00") == dg.SUN
    assert dg.running_date(dg.WHEN["morning"], "04:00") == dg.SUN  # 08:20 GMT, after BST ended
    assert dg.running_date(dg.WHEN["evening"], "04:00") == dg.SAT
    assert dg.day_totals("04:00") == {dg.SAT: (2389.0, 4), dg.SUN: (910.0, 1)}
    assert dg.day_totals("00:00") == {dg.SAT: (2354.0, 3), dg.SUN: (945.0, 2)}


@pytest.mark.parametrize("start", STARTS)
@pytest.mark.parametrize("domain", DOMAINS)
def test_the_right_file_and_reply_pass(domain, start):
    t = dg.TABLES[domain]
    checks = dg.grade(t, good(t, start), reply(start), appends(t, start), start)
    assert dg.verdict(checks) == "PASS", dg.table(checks)


@pytest.mark.parametrize("start", STARTS)
@pytest.mark.parametrize("domain", DOMAINS)
@pytest.mark.parametrize(
    "mutate, failing",
    [
        # the correction appended instead of edited: counted twice
        (lambda s, t: s + f"2026-10-25,08:25,{t.items['morning']},1,910\n", "2026-10-25 total exact"),
        (lambda s, t: s.replace(",1,910\n", ",1,945\n"), "the correction left one row"),
        # yesterday's evening row overwritten by this morning's (the 25 Oct mix-up)
        (
            lambda s, t: s.replace(f"2026-10-24,21:10,{t.items['evening']},1,990", "2026-10-24,21:10,x,1,910"),
            "2026-10-24 total exact",
        ),
        # a seeded row lost
        (lambda s, t: s.replace(f"2026-10-24,12:40,{t.items['b']},1,807\n", ""), "seeded rows byte-identical"),
        (lambda s, t: s.replace(s.splitlines()[0], "date,item,figure"), "header kept"),
        (lambda s, t: s + "2026-10-26,09:00,x,1,1\n", "no row after Sunday"),
    ],
)
def test_each_failure_names_its_check(domain, start, mutate, failing):
    t = dg.TABLES[domain]
    checks = dg.grade(t, mutate(good(t, start), t), reply(start), appends(t, start), start)
    assert failing in [c.name for c in checks if not c.ok], dg.table(checks)


@pytest.mark.parametrize("domain", DOMAINS)
def test_the_00_20_row_on_the_wrong_day_fails_under_either_setting(domain):
    t = dg.TABLES[domain]
    for start, wrong in (("04:00", dg.SUN), ("00:00", dg.SAT)):
        right = dg.running_date(dg.WHEN["night"], start)
        bad = good(t, start).replace(f"{right},00:20", f"{wrong},00:20")
        failed = [c.name for c in dg.grade(t, bad, reply(start), appends(t, start), start) if not c.ok]
        assert f"the 00:20 row on its running day ({right})" in failed, (start, failed)


@pytest.mark.parametrize("domain", DOMAINS)
def test_a_turn_that_rewrote_earlier_bytes_fails(domain):
    t = dg.TABLES[domain]
    before, after = appends(t, "04:00")[0]
    rewritten = [(before, after.replace(",1,557\n", ",1,557.0\n"))]
    checks = dg.grade(t, good(t, "04:00"), reply("04:00"), rewritten)
    assert "every logging turn only appended" in [c.name for c in checks if not c.ok]
    assert "every logging turn only appended" in [c.name for c in dg.grade(t, good(t, "04:00"), "", []) if not c.ok]


@pytest.mark.parametrize("domain", DOMAINS)
def test_a_reply_without_both_totals_fails(domain):
    t = dg.TABLES[domain]
    checks = dg.grade(t, good(t, "04:00"), "Saturday was 2,389.", appends(t, "04:00"))
    assert [c.name for c in checks if not c.ok] == ["reply states both totals"]


def test_the_three_tables_share_every_expectation():
    """No table is privileged: the same rows, figures and checks, only names differ."""
    shapes = {
        (t.header.split(",")[:4] == ["date", "time", "item", "qty"], tuple(sorted(t.items))) for t in dg.TABLES.values()
    }
    assert len(shapes) == 1 and len(dg.TABLES) == 3
