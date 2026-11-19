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


# ---- the owner's own skill, one per domain (ruling 49 item 3, ruling 54 item 6) ------------
# Invented, the same form in all three: front matter, then what the file is, how a row is
# written, corrections, totals, unknowns, the owner's target, and how he likes replies. The
# size class of a real owner skill (3.4 to 4 KB with front matter); read through the skills
# index like any user skill, never attached by the device.
SKILL_FIELDS = {
    "food": {
        "id": "my-meals",
        "title": "My meal log",
        "desc": "Read before logging a meal or snack, or answering about calories, protein or what I ate.",
        "what": "meals and snacks I eat, one row per item I report",
        "columns": "date (YYYY-MM-DD), meal (breakfast, lunch, dinner, snack, drink; add a number for a second one: snack2), "
        "items (short free text), kcal (number), protein_g (number, grams)",
        "example": "{D0},lunch,lentil soup and rye bread,540,24",
        "unknown": "a figure I do not give and you cannot read from a label I sent: leave that cell empty",
        "target": "2,100 kcal and at least 120 g protein a day",
        "left": "what is left today is the target minus the day's total, computed with a tool",
        "labels": "When I send a pack label, use its per-pack figures unless I say how much I ate.",
    },
    "expenses": {
        "id": "my-spending",
        "title": "My spending log",
        "desc": "Read before logging a purchase or refund, or answering about what I spent or my budget.",
        "what": "money I spend and refunds I get, one row per payment",
        "columns": "date (YYYY-MM-DD), category (coffee, groceries, transport, eating out, household, refund, ...), "
        "note (short free text), amount (pounds, two decimals; a refund is negative), paid_with (card or cash)",
        "example": "{D0},groceries,weekly shop,48.37,card",
        "unknown": "an amount I do not give and you cannot read from a receipt I sent: leave that cell empty",
        "target": "a budget of 600.00 a month, everything included",
        "left": "what is left this month is the budget minus the month's total, computed with a tool",
        "labels": "When I send a receipt, log its total as one row unless I ask for the lines.",
    },
    "training": {
        "id": "my-training",
        "title": "My training log",
        "desc": "Read before logging a set or a workout, or answering about reps, weights or training volume.",
        "what": "the sets I do at the gym, one row per set",
        "columns": "date (YYYY-MM-DD), exercise (squat, bench press, row, deadlift, ...), note (short free text), "
        "reps (number), kg (number; 0 for bodyweight)",
        "example": "{D0},squat,top set,5,80",
        "unknown": "a weight I do not give and you cannot read from a board I sent: leave that cell empty",
        "target": "at least 12,000 kg of volume a week (reps times kg, summed)",
        "left": "volume is reps times kg per set; total it with a tool, never in your head",
        "labels": "When I send a photo of the gym board, log one row per set it lists (3 x 8 is three rows).",
    },
}

SKILL_TEMPLATE = """---
title: {title}
desc: {desc}
version: 3
---
# {title}

This is how I keep my {what_short} log. I wrote it; follow it whenever I log something or ask about it.

## The file
- It lives at `{path}` and is a plain CSV file I own. I also open it on my laptop, so keep it tidy.
- It holds {what}.
- Header (keep it exactly): `{header}`
- Columns: {columns}.
- Example row: `{example}`
- Some old rows have extra commas in the free text, and a few lines start with `#`: those are notes
  from an older version of my setup. Leave them where they are unless I ask you to clean up.

## Logging
1. One row per thing I report, appended at the end of the file. Never rewrite the whole file to add a row.
2. The date is the day that is running when I tell you, as YYYY-MM-DD with the year. My day ends at 04:00,
   so something I log at 00:30 belongs to the day before.
3. Free text has no commas. If I use one, put the text in double quotes or use a semicolon.
4. Numbers are plain: no units, no thousands separators, a point for decimals.
5. If I log the same thing twice on purpose ("another one, same again"), that is two rows. If I say a row
   was a mistake, remove exactly that row: when two rows are identical, remove the one I mean by its line,
   not the first one you find.
6. Do not log something I only ask about. A question is not an entry.
7. Unknowns: {unknown}. Never write 0 for something unknown.
8. {labels}

## Corrections
- When I correct a figure, a name or a date, change that row in place (an edit of that line). Do not append
  a second row and do not leave the old one behind.
- When I say a row belongs to another day, change its date; the row keeps everything else.
- After a correction, say in one line what the row says now.

## Totals and questions
- A day total covers every row of that day. Read the day from the file with the date filter and the sum,
  and use the figures the tool gives. Do not add up numbers in your head.
- If the tool says some rows were skipped (a wrong field count, an empty cell, a value that is not a number),
  either fix those rows with me first or give the total and say plainly which rows it leaves out and why.
  Never give a total that quietly misses rows.
- My target: {target}; {left}.
- If I ask whether something was logged, look in the file before you answer.

## Replies
- Short. Confirm what you logged in one line: the date and the figures.
- No lectures, no advice I did not ask for, no emoji.
- If a write fails, say so plainly and tell me what is in the file now.

## Things I do not want
- No second tracking file. Everything goes into `{path}`.
- No guessing at figures I did not give, and no rounding of the figures I did give.
- No summaries of my week unless I ask for them.
- No reminders unless I ask for one; when I do, the reminder only reminds me, it never logs anything itself.
"""


def skill_md(dom: Domain) -> str:
    f = SKILL_FIELDS[dom.name]
    return SKILL_TEMPLATE.format(
        title=f["title"], desc=f["desc"], what_short=dom.name if dom.name != "food" else "meal",
        path=dom.path, what=f["what"], header=",".join(dom.header), columns=f["columns"], example=f["example"],
        unknown=f["unknown"], target=f["target"], left=f["left"], labels=f["labels"],
    )
