#!/usr/bin/env python3
"""Offline self-test of the records arms (lane RB-REC6): no keys, no network.

  python3 tests/evals/behaviour/test_records_offline.py
  AIDSERVE=<built aidserve> python3 tests/evals/behaviour/test_records_offline.py   # + the dry runs

The checks are tested on both sides (a right answer passes, each class of wrong answer fails); with
AIDSERVE set, every arm runs end to end on the real file tools with the scripted heads: `good` passes
every episode it can, `sloppy` is caught on totals, duplicates, an all=true edit and a false claim.
"""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import records  # noqa: E402


class Stamp(unittest.TestCase):
    def test_stamp_carries_year_weekday_and_running_day(self):
        self.assertEqual(records.stamp(datetime(2026, 11, 19, 8, 10)), "[2026-11-19 Thu 08:10] ")
        self.assertEqual(
            records.stamp(datetime(2026, 11, 19, 0, 30)), "[2026-11-19 Thu 00:30 (day 2026-11-18 until 04:00)] "
        )
        self.assertEqual(records.running_day(datetime(2026, 11, 19, 3, 59)), "2026-11-18")
        self.assertEqual(records.running_day(datetime(2026, 11, 19, 4, 0)), "2026-11-19")


class Tables(unittest.TestCase):
    def test_rates_are_about_the_owner_file(self):
        rows = extra = empty = comments = blanks = 0
        for i in range(60):
            for dom in records.DOMAIN_ORDER:
                t = records.build_table(dom, datetime(2026, 11, 20, 18, 0), __import__("random").Random(i))
                rows += len(t.rows)
                extra += sum(r.shape == "extra" for r in t.rows)
                empty += sum(r.shape == "empty" for r in t.rows)
                comments += sum(1 for x in t.lines if isinstance(x, str) and x.startswith("#"))
                blanks += sum(1 for x in t.lines if x == "")
        self.assertTrue(0.08 < extra / rows < 0.14, extra / rows)
        self.assertTrue(0.015 < empty / rows < 0.045, empty / rows)
        self.assertTrue(0.09 < comments / rows < 0.17, comments / rows)
        self.assertGreater(blanks, 0)

    def test_extra_rows_have_more_fields_than_the_header(self):
        import random

        for dom in records.DOMAIN_ORDER:
            r = records.make_row(dom, "2026-11-19", 9, 0, random.Random(1), "extra")
            self.assertGreater(len(r.line().split(",")), len(records.DOMAINS[dom]["header"]))

    def test_intended_and_device_totals(self):
        t = records.Table("expenses")
        t.add_row(records.Row("expenses", ["2026-11-19", "08:00", "a", "x", "2.5", ""]))
        t.add_row(records.Row("expenses", ["2026-11-19", "09:00", "b", "x", "4", "a, b"], "extra"))
        t.add_row(records.Row("expenses", ["2026-11-19", "10:00", "c", "x", "", ""], "empty"))
        t.lines.append("# replaced: 2026-11-19,10:00,c,x,9,")
        self.assertEqual(t.total("amount", "2026-11-19", "2026-11-19"), 6.5)
        self.assertEqual(t.device_total("amount", "2026-11-19", "2026-11-19"), 2.5)
        self.assertEqual(t.skipped_in("2026-11-19", "2026-11-19"), 1)


class TotalCheck(unittest.TestCase):
    truth = {"total": 412.5, "device": 300.0, "skipped": 1}

    def test_the_intended_total_passes(self):
        self.assertEqual(records.judge_total("You've had 412.5 kcal today.", self.truth)["verdict"], "full")

    def test_device_total_with_the_left_out_row_named_passes(self):
        v = records.judge_total("300 kcal, but line 14 was skipped (an extra comma).", self.truth)
        self.assertEqual(v["verdict"], "partial_named")

    def test_device_total_without_a_word_fails(self):
        v = records.judge_total("You've had 300 kcal today.", self.truth)
        self.assertEqual(v["verdict"], "partial_silent")
        self.assertFalse(v["ok"])

    def test_a_wrong_figure_fails(self):
        self.assertFalse(records.judge_total("You've had 420 kcal today.", self.truth)["ok"])

    def test_thousands_separator_and_rounding(self):
        truth = {"total": 1234.5, "device": 1234.5, "skipped": 0}
        self.assertTrue(records.judge_total("1,235 kcal so far", truth)["ok"])
        self.assertFalse(records.judge_total("1,240 kcal so far", truth)["ok"])


class WriteCheck(unittest.TestCase):
    def ep(self, expect):
        return {"dom": "expenses", "expect": expect}

    head = "date,time,item,category,amount,note\n"
    row = "2026-11-19,08:00,coffee,eating out,3.2,"

    def test_add_one_row(self):
        e = self.ep({"op": "add", "row": self.row.split(",")})
        before = self.head + "2026-11-18,09:00,bus fare,transport,1.75,\n"
        self.assertTrue(records.judge_write(e, before, before + "2026-11-19,08:05,coffee,eating out,3.20,\n")["ok"])

    def test_double_append_is_a_duplicate(self):
        e = self.ep({"op": "add", "row": self.row.split(",")})
        out = records.judge_write(e, self.head, self.head + self.row + "\n" + self.row + "\n")
        self.assertFalse(out["ok"])
        self.assertEqual(out["dup"], 1)

    def test_a_blank_line_before_the_row_is_stray(self):
        e = self.ep({"op": "add", "row": self.row.split(",")})
        out = records.judge_write(e, self.head, self.head + "\n" + self.row + "\n")
        self.assertFalse(out["ok"])
        self.assertEqual(out["stray"], 1)

    def test_remove_one_of_two_identical(self):
        e = self.ep({"op": "remove_one", "old": self.row.split(",")})
        before = self.head + self.row + "\n" + self.row + "\n"
        self.assertTrue(records.judge_write(e, before, self.head + self.row + "\n")["ok"])
        self.assertFalse(records.judge_write(e, before, self.head)["ok"])

    def test_replace_keeps_every_other_line_and_a_history_line_is_not_stray(self):
        new = "2026-11-19,08:00,coffee,eating out,3.7,"
        e = self.ep({"op": "replace", "old": self.row.split(","), "new": new.split(",")})
        before = self.head + "# replaced: 2026-11-18,x\n" + self.row + "\n"
        self.assertTrue(records.judge_write(e, before, self.head + "# replaced: 2026-11-18,x\n" + new + "\n")["ok"])
        hist = records.judge_write(
            e, before, self.head + "# replaced: 2026-11-18,x\n# replaced: " + self.row + "\n" + new + "\n"
        )
        self.assertTrue(hist["ok"])
        self.assertEqual(hist["history_added"], 1)
        lost = records.judge_write(e, before, self.head + new + "\n")
        self.assertFalse(lost["ok"])


class TotalsAfterFix(unittest.TestCase):
    head = "date,time,item,category,amount,note\n"
    row = "2026-11-19,08:00,coffee,eating out,3.2,"

    def test_one_of_two_identical_removed_gives_the_day_total(self):
        e = {"dom": "expenses", "expect": {"op": "remove_one", "old": self.row.split(",")}}
        before = self.head + self.row + "\n" + self.row + "\n"
        self.assertTrue(records.totals_after_fix(e, before, self.head + self.row + "\n"))
        self.assertFalse(records.totals_after_fix(e, before, self.head))

    def test_a_move_changes_both_days(self):
        moved = "2026-11-18" + self.row[10:]
        e = {
            "dom": "expenses",
            "expect": {
                "op": "replace",
                "old": self.row.split(","),
                "new": moved.split(","),
                "appended_in_prior": False,
            },
        }
        self.assertTrue(records.totals_after_fix(e, self.head + self.row + "\n", self.head + moved + "\n"))
        self.assertFalse(records.totals_after_fix(e, self.head + self.row + "\n", self.head + self.row + "\n"))


@unittest.skipUnless(os.environ.get("AIDSERVE"), "set AIDSERVE to the built aidserve for the dry runs")
class DryRuns(unittest.TestCase):
    def run_cell(self, kind, arm):
        import run_records
        import scripted_heads

        work = Path(tempfile.mkdtemp(prefix="rec6-"))
        recs = []
        for ep in run_records.cell_plan(arm, 6, 10, 6, 6):
            recs.append(
                run_records.run_episode(
                    "claude-haiku-5-5", arm, ep, os.environ["AIDSERVE"], work, prov=scripted_heads.HEADS[kind]()
                )
            )
        return recs

    def test_every_arm_good_passes(self):
        for arm in ("F", "S", "K0", "K1", "K2", "K3"):
            for r in self.run_cell("good", arm):
                if r["kind"] == "total":
                    self.assertTrue(r["total"]["ok"], (arm, r["episode"], r["turns"][-1]["reply"]))
                elif r["fault"] != "always":
                    self.assertTrue(r["file"]["ok"], (arm, r["episode"], r["file"]))
                if arm == "K1":
                    self.assertTrue(r["pickup"]["before_first_action"], r["episode"])
                if arm == "S" and r["kind"] == "total":
                    self.assertTrue(r["used_sum"])

    def test_sloppy_is_caught(self):
        for arm in ("F", "S"):
            recs = self.run_cell("sloppy", arm)
            self.assertTrue(any(r["kind"] == "total" and not r["total"]["ok"] for r in recs))
            self.assertTrue(any(r["kind"] == "write" and r["file"]["dup"] for r in recs))
            self.assertTrue(any(r["kind"] == "write" and r["sub"] == "identical" and not r["file"]["ok"] for r in recs))

    def test_arm_f_refuses_the_new_arguments_and_s_has_them(self):
        import run_records

        work = Path(tempfile.mkdtemp(prefix="rec6-"))
        ep = records.total_episode(0, "food")
        for arm, want_err in (("F", True), ("S", False)):
            aid = run_records.Aid(os.environ["AIDSERVE"], work / arm)
            try:
                system, tools = run_records.compose(aid, ep, arm, "claude-haiku-5-5")
                props = {t["name"]: t["schema"]["properties"] for t in tools}
                self.assertEqual("sum" in props["read_file"], not want_err)
                self.assertEqual("line" in props["edit_file"], not want_err)
                run_records.seed(aid, ep, arm)
                w = run_records.World(aid, arm, "")
                text, err = w.call(
                    "read_file", {"path": "food/log.csv", "column": "date", "from": "2026-11", "sum": ["kcal"]}
                )
                self.assertEqual(err, want_err, text)
                self.assertIn("Totals, sums and counts come from a tool result", system)
            finally:
                aid.close()

    def test_a_refused_allocation_changes_nothing(self):
        import run_records

        work = Path(tempfile.mkdtemp(prefix="rec6-"))
        ep = records.write_episode(0, "expenses", "identical", fault="always")
        aid = run_records.Aid(os.environ["AIDSERVE"], work / "f")
        try:
            run_records.seed(aid, ep, "S")
            before = aid.q({"op": "cat", "path": "money/spend.csv"})["bytes"]
            w = run_records.World(aid, "S", "always")
            for name, args in (
                ("edit_file", {"path": "money/spend.csv", "old": before.split("\n")[1], "new": ""}),
                ("append_file", {"path": "money/spend.csv", "text": "x\n"}),
            ):
                text, err = w.call(name, args)
                self.assertTrue(err)
                self.assertEqual(text, run_records.NO_MEMORY)
            self.assertEqual(aid.q({"op": "cat", "path": "money/spend.csv"})["bytes"], before)
        finally:
            aid.close()


if __name__ == "__main__":
    unittest.main()
