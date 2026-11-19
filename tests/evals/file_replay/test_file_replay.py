#!/usr/bin/env python3
"""The file replay's own tests: the tree passes every case, and the grader catches each wrong
answer it exists for (a gate ships its counter-test). Needs a C++ compiler; no network.

    python3 -m pytest tests/evals/file_replay -q
"""

from __future__ import annotations

import copy
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay  # noqa: E402

CASES = replay.all_cases()
_RESULTS: dict = {}


def results() -> dict:
    if not _RESULTS:
        with tempfile.TemporaryDirectory() as tmp:
            _RESULTS.update(replay.run(replay.build(replay.REPO, Path(tmp) / "drv"), CASES))
    return _RESULTS


def case(cid: str) -> dict:
    return next(c for c in CASES if c["id"] == cid)


class ReplayTest(unittest.TestCase):
    def test_every_case_passes_on_this_tree(self):
        rows = replay.grade(CASES, results())
        self.assertEqual([r["id"] for r in rows if not r["ok"]], [])

    def test_three_domains_carry_the_same_calls(self):
        kinds = {}
        for c in CASES:
            dom, _, shape = c["id"].partition("-")
            if dom in ("food", "expenses", "training"):
                kinds.setdefault(shape, set()).add(dom)
        self.assertTrue(kinds)
        self.assertTrue(all(v == {"food", "expenses", "training"} for v in kinds.values()), kinds)

    def test_the_day_files_have_the_owner_row_classes(self):
        for dom in replay.domains.DOMAINS.values():
            day = dom.days["{D-1}"]
            self.assertEqual(len(day), 15)
            h = replay.domains.honest_day(dom, "{D-1}")
            self.assertEqual(len(h["named"]), 2, dom.name)  # about 11% of rows: more fields than the header
            self.assertEqual(len(h["blank"]), 1, dom.name)  # an unknown left empty, never 0
            self.assertNotEqual(h["full"], h["clean"], dom.name)


class CounterTest(unittest.TestCase):
    """Each wrong answer the replay exists to catch turns a case red."""

    def miss(self, cid: str, mutate) -> None:
        r = copy.deepcopy(results()[cid])
        mutate(r)
        ok, _ = replay.GRADERS[case(cid)["kind"]](case(cid), r)
        self.assertFalse(ok, cid)

    def test_a_total_that_silently_misses_a_named_row(self):
        def drop(r):
            r["field_count"] = {"count": 1, "lines": r["field_count"]["lines"][:1]}

        for d in ("food", "expenses", "training"):
            self.miss(f"{d}-day", drop)

    def test_a_total_that_counts_an_unknown_as_zero_and_says_nothing(self):
        for d in ("food", "expenses", "training"):
            self.miss(f"{d}-day", lambda r: r.update(blank={"count": 0, "lines": []}))

    def test_a_wrong_total(self):
        def bump(r):
            name = next(iter(r["sums"]))
            r["sums"][name] = str(replay.Decimal(r["sums"][name]) + 1)

        self.miss("expenses-day", bump)

    def test_an_append_written_twice(self):
        self.miss("training-relog", lambda r: r.update(file=r["file"] + case("training-relog")["text"]))

    def test_a_torn_tail_not_fenced(self):
        self.miss(
            "food-torn", lambda r: r.update(fence=False, file=case("food-torn")["file"] + case("food-torn")["text"])
        )

    def test_an_identical_line_not_named(self):
        self.miss("food-relog", lambda r: r.update(note=""))

    def test_a_stale_line_that_edits_another_row(self):
        self.miss("food-stale-line", lambda r: r.update(ok=True, changed=True))

    def test_a_refused_edit_that_changed_the_file(self):
        self.miss("food-refused-edit", lambda r: r.update(changed=True))


if __name__ == "__main__":
    unittest.main()
