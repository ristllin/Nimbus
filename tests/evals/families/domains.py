"""The three validation domains (ruling 41): food, expenses and training, invented content only.

One shape, three vocabularies. Each domain is a CSV tracking file the owner keeps himself, with
the real-row classes of ruling 49 at about the owner's rates (counts and classes only, never his
content): about 11% of rows carry more fields than the header (a free-text column with bare
commas), about 3% have an empty cell in a summed column (unknown is empty, never 0), stray blank
lines, and comment lines (`#` first) as v5.1.x's keyed append left them.

Used by tests/evals/file_replay (the shared file aids, no model) and by the refloop family
scenarios (tests/evals/refloop/families, both rig arms). Nothing here is domain logic in the
product: these are fixtures.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from decimal import Decimal


@dataclass(frozen=True)
class Domain:
    name: str
    path: str  # project/name of the tracking file
    header: tuple  # first column is the date, the free-text column is the third
    sum_cols: tuple  # the columns a day total adds up
    days: dict = field(default_factory=dict)  # date -> rows (tuples of cells, free text may hold commas)
    skill: str = ""  # the owner's own skill fixture (front matter + body, 3.4 to 4 KB)


def _food_rows():
    d17 = [
        ("{D-2}", "breakfast", "oat porridge with pear", "380", "12"),
        ("{D-2}", "lunch", "lentil soup and rye bread", "540", "24"),
        ("{D-2}", "snack", "plain yogurt", "120", "11"),
        ("{D-2}", "dinner", "trout, rice, peas", "690", "41"),
    ]
    d18 = [
        ("{D-1}", "breakfast", "two boiled eggs on toast", "350", "19"),
        ("{D-1}", "drink", "flat white", "110", "6"),
        ("{D-1}", "snack", "almonds 20 g", "120", "4"),
        ("{D-1}", "lunch", "chickpea salad", "460", "17"),
        ("{D-1}", "snack2", "apple", "70", "0.4"),
        ("{D-1}", "drink2", "green tea", "0", "0"),
        ("{D-1}", "dinner", "tofu stir fry, noodles", "620", "28"),
        ("{D-1}", "dessert", "dark chocolate square", "60", ""),
        ("{D-1}", "snack3", "rice cake", "35", "0.7"),
        ("{D-1}", "drink3", "oat latte", "130", "3"),
        ("{D-1}", "snack4", "banana", "105", "1.3"),
        ("{D-1}", "lunch2", "miso soup", "45", "3"),
        ("{D-1}", "snack5", "hummus, carrot sticks", "150", "5"),
        ("{D-1}", "drink4", "sparkling water", "0", "0"),
        ("{D-1}", "supper", "cottage cheese", "98", "11"),
    ]
    return {"{D-2}": d17, "{D-1}": d18}


def _expense_rows():
    d17 = [
        ("{D-2}", "transport", "bus pass top-up", "12.50", "card"),
        ("{D-2}", "groceries", "corner shop: milk, bread", "4.85", "cash"),
        ("{D-2}", "eating out", "noodle bar", "14.20", "card"),
        ("{D-2}", "books", "second-hand atlas", "6.00", "cash"),
    ]
    d18 = [
        ("{D-1}", "coffee", "kiosk espresso", "2.40", "card"),
        ("{D-1}", "groceries", "weekly shop", "48.37", "card"),
        ("{D-1}", "transport", "train to Harwick", "17.90", "card"),
        ("{D-1}", "lunch", "bakery: pasty, juice", "6.75", "card"),
        ("{D-1}", "household", "light bulbs", "8.99", "card"),
        ("{D-1}", "refund", "returned kettle", "-24.99", "card"),
        ("{D-1}", "gift", "flowers for Odile", "15.00", "cash"),
        ("{D-1}", "parking", "station car park", "", "card"),
        ("{D-1}", "coffee", "oat cappuccino", "3.10", "card"),
        ("{D-1}", "pharmacy", "plasters", "3.49", "card"),
        ("{D-1}", "subscription", "music service", "10.99", "card"),
        ("{D-1}", "snacks", "vending machine", "1.80", "cash"),
        ("{D-1}", "split bill", "dinner with Bram, Ines", "21.50", "card"),
        ("{D-1}", "laundry", "service wash", "7.00", "cash"),
        ("{D-1}", "tips", "taxi driver", "2.00", "cash"),
    ]
    return {"{D-2}": d17, "{D-1}": d18}


def _training_rows():
    d17 = [
        ("{D-2}", "squat", "warm-up, felt fine", "5", "60"),
        ("{D-2}", "squat", "top set", "5", "80"),
        ("{D-2}", "bench press", "paused", "5", "55"),
        ("{D-2}", "row", "strict", "8", "50"),
    ]
    d18 = [
        ("{D-1}", "deadlift", "warm-up", "5", "70"),
        ("{D-1}", "deadlift", "working set", "5", "100"),
        ("{D-1}", "deadlift", "working set", "5", "100"),
        ("{D-1}", "overhead press", "seated", "8", "30"),
        ("{D-1}", "overhead press", "seated, last rep slow", "8", "30"),
        ("{D-1}", "pull-up", "bodyweight", "6", "0"),
        ("{D-1}", "pull-up", "bodyweight", "5", "0"),
        ("{D-1}", "lunge", "dumbbells, each leg", "10", "16"),
        ("{D-1}", "lunge", "dumbbells", "10", "16"),
        ("{D-1}", "curl", "EZ bar", "12", "20"),
        ("{D-1}", "curl", "EZ bar", "10", ""),
        ("{D-1}", "plank", "seconds in reps", "60", "0"),
        ("{D-1}", "face pull", "cable", "15", "12.5"),
        ("{D-1}", "face pull", "cable", "15", "12.5"),
        ("{D-1}", "calf raise", "machine", "20", "40"),
    ]
    return {"{D-2}": d17, "{D-1}": d18}


DOMAINS = {
    "food": Domain("food", "tracking/meals.csv", ("date", "meal", "items", "kcal", "protein_g"), ("kcal", "protein_g"), _food_rows()),
    "expenses": Domain("expenses", "money/spending.csv", ("date", "category", "note", "amount", "paid_with"), ("amount",), _expense_rows()),
    "training": Domain("training", "gym/sets.csv", ("date", "exercise", "note", "reps", "kg"), ("reps", "kg"), _training_rows()),
}


DAY_KEYS = ("{D-2}", "{D-1}")  # the two seeded days, oldest first; dates are filled in per run


def dated(text: str, days: dict) -> str:
    """Put real dates in for the day placeholders ({"{D-1}": "2026-11-18", ...})."""
    for key, value in days.items():
        text = text.replace(key, value)
    return text


def render(dom: Domain, comment_before: int = 0, blank_after: tuple = ()) -> str:
    """The file as the owner keeps it: header, rows (free text unquoted, bare commas kept), one
    comment line of the old keyed append and stray blank lines where asked. Dates stay
    placeholders ({D-2}, {D-1}); dated() or the rig fills them in."""
    lines = [",".join(dom.header)]
    n = 0
    for day in DAY_KEYS:
        for row in dom.days[day]:
            n += 1
            if n == comment_before:
                lines.append(f"# replaced: {','.join(row)}")
            lines.append(",".join(row))
            if n in blank_after:
                lines.append("")
    return "\n".join(lines) + "\n"


def _num(cell: str):
    try:
        return Decimal(cell.strip())
    except Exception:
        return None


def honest_day(dom: Domain, day: str) -> dict:
    """What an honest day total is, from the rows as written (the oracle the families grade by):
    `full` adds every cell the owner wrote (the bare-comma rows read by position from the right),
    `clean` adds only rows whose field count is the header's, `named` are the rows (1-based
    positions in the day) a total must either repair or name, `blank` the rows with an unknown."""
    full = {c: Decimal(0) for c in dom.sum_cols}
    clean = {c: Decimal(0) for c in dom.sum_cols}
    named, blank = [], []
    width = len(dom.header)
    for i, row in enumerate(dom.days[day], 1):
        cells = ",".join(row).split(",")
        tail = cells[len(cells) - (width - 3):] if width > 3 else []
        values = dict(zip(dom.header[3:], tail))
        wrong = len(cells) != width
        if wrong:
            named.append(i)
        for c in dom.sum_cols:
            v = _num(values.get(c, ""))
            if v is None:
                if values.get(c, "").strip() == "":
                    blank.append(i)
                continue
            full[c] += v
            if not wrong:
                clean[c] += v
    return {"full": full, "clean": clean, "named": named, "blank": sorted(set(blank))}
