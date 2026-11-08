"""Host tests for the owner-regression suite's ledger reading (tests/evals/owner_regressions/assertions.py).

Run: python3 -m pytest tools/release_gate

The suite grades the owner's outcome from the final on-device ledger. The device reads a ledger
row with a bare comma in its items (readWideRow, lib/core/src/orch_file_append.cpp), so the
assertions must too, or a correct row fails the figure checks (lane QFIX: glm-5.3 wrote such a
row in 7 of 200 reps and three scenarios graded the correct figure as a failure). The reading
must stay strict where the device is: a shifted figure is never words.
"""

import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ASSERTIONS_PATH = os.path.join(HERE, "..", "..", "tests", "evals", "owner_regressions", "assertions.py")

spec = importlib.util.spec_from_file_location("owner_regression_assertions", ASSERTIONS_PATH)
A = importlib.util.module_from_spec(spec)
spec.loader.exec_module(A)

HEADER = "date,meal,items,kcal,protein_g\n"


def ledger(*rows):
    return {"files": {A.FOOD_LEDGER: HEADER + "".join(r + "\n" for r in rows)}, "turns": []}


def near(t, item, expected):
    ok, _ = A.ASSERTIONS["ledger_row_kcal_near"]({}, t, {"item": item, "expected": expected, "tol": 5})
    return ok


def not_at(t, item, kcal):
    ok, _ = A.ASSERTIONS["ledger_row_kcal_ne"]({}, t, {"item": item, "kcal": kcal})
    return ok


def test_an_exact_width_row_is_read_as_before():
    t = ledger("2026-11-05,snack,boba tea 350 g,238,")
    (r,) = A.ledger_rows(t)
    assert (r["meal"], r["items"], r["kcal"], r["protein_g"]) == ("snack", "boba tea 350 g", "238", "")
    assert near(t, "boba", 238)


def test_a_bare_comma_in_the_items_is_read_like_the_device_reads_it():
    # The rows glm-5.3 wrote on 5 Nov (lc-grams-vs-kcal rep2, lc-photo-default rep2, lc-near-match rep2).
    boba = ledger("2026-11-05,snack-1600,boba tea 350 g (68 kcal/100 g, label),238,")
    (r,) = A.ledger_rows(boba)
    assert r["items"] == "boba tea 350 g (68 kcal/100 g, label)"
    assert (r["kcal"], r["protein_g"]) == ("238", "")
    assert near(boba, "boba", 238)
    bars = ledger("2026-11-05,snack-0815,protein bar pack (4 bars, 220 kcal each, owner's label figures),880,")
    assert near(bars, "protein bar", 880)
    lunch = ledger("2026-11-05,lunch,chicken (450 g, assumed cooked breast)+blueberries (250 g),886,141.3")
    (r,) = A.ledger_rows(lunch)
    assert "blueberries" in r["items"] and (r["kcal"], r["protein_g"]) == ("886", "141.3")
    # The raw text stays the file's own, for the forbid checks.
    assert "label)" in A.ledger_rows(boba)[0]["raw"]


def test_the_widened_reading_still_fails_a_wrong_figure():
    # Counter-test: the comma is forgiven, the figure is not (350 g logged as 350 kcal).
    wrong = ledger("2026-11-05,snack,boba tea 350 g (68 kcal/100 g, label),350,")
    assert not near(wrong, "boba", 238)
    assert not not_at(wrong, "boba", 350)


def test_a_shifted_figure_is_never_words():
    # "350" is a bare number among the absorbed fields: another schema or a shifted figure,
    # so the row stays wide and the figure checks read it as csv does (kcal 350, not 238).
    shifted = ledger("2026-11-05,snack,boba tea,350,238,")
    (r,) = A.ledger_rows(shifted)
    assert r["kcal"] == "350"
    assert not near(shifted, "boba", 238)
    assert not not_at(shifted, "boba", 350)


def test_a_wide_row_whose_last_fields_are_not_figures_stays_unread():
    unread = ledger("2026-11-05,snack,boba tea 350 g (68 kcal/100 g, label),238 kcal,")
    (r,) = A.ledger_rows(unread)
    assert r["items"] == "boba tea 350 g (68 kcal/100 g"
    assert not near(unread, "boba", 238)


def test_a_wide_row_with_no_day_stays_unread():
    other = ledger("not-a-day,snack,boba tea (68 kcal/100 g, label),238,")
    assert not near(other, "boba", 238)


def test_the_day_total_counts_a_widened_row():
    t = ledger("2026-11-05,breakfast,eggs,300,20", "2026-11-05,snack,boba (350 g, label),238,")
    total = sum(float(r["kcal"]) for r in A.ledger_rows(t))
    assert total == 538.0
