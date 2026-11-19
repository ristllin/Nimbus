#!/usr/bin/env python3
"""records - the general-records arms of the behaviour eval (lane RB-REC6, ruling 42 item 5).

Tracking is an ordinary CSV file and the plain file tools (ruling 42). Three domains of equal weight
(food, expenses, training), invented content, each file carrying the owner file's real-row classes at
about his rates (ruling 49): about 11% of rows with more fields than the header (a bare comma in a
free-text field), about 3% with an empty cell in a summed column (unknown is empty, never 0), stray
blank lines, and about 12% '#' comment lines (the history lines v5.1.x wrote).

Arms (tools and system text are the REAL engine-2 bytes from aidserve, except where an arm says):
  F   files only: read_file and edit_file as declared before RB-REC4 (no column/from/to/sum, no line);
      the model sums.
  S   rb-main's declarations: read_file column/from/to/sum, edit_file line.
  K0  skill pickup, S tools, a fresh chat, no skill: the table's path is in no note or skill.
  K1  K0 plus the owner's skill in the skills index (read_file skills/<id>.md to open it).
  K2  K0 plus the same skill marked inject: context: the old engine's [ACTIVE SKILLS] block rides the
      owner's message (agent::skillsAttachText bytes).
  K3  K2 with two user skills attached at once (one 4,096 B block, sorted by id, the second cut).
In F and S the pinned notes name the table's path and header (the owner's own note, ruling 42 item 8).

Episode kinds: total (a day, yesterday, a week, a month so far; late at night the running day), add
(some between midnight and the 04:00 day start), correct (a row from earlier in this chat, or one
only in the file), identical (one of two identical lines removed), move (a row to the day before).
A fault episode injects the real 'not enough memory for this call; nothing changed' (aids::kAidNoMemory)
into the first or every write and sum: the refusing-allocator leg.

Nothing here is the owner's data: every row, item and skill is invented.
"""

from __future__ import annotations

import random
import re
from datetime import datetime, timedelta

DAY_START_MIN = 240  # RB-REC5's default, 04:00

# ------------------------------------------------------------------ domains
DOMAINS = {
    "food": {
        "path": "food/log.csv",
        "header": ["date", "time", "item", "qty", "unit", "kcal", "protein_g", "note"],
        "sum": "kcal",
        "noun": "kcal",
        "label": "food log",
        "skill_id": "my-food-log",
        "items": [
            ("porridge", 1, "bowl", 310, 11),
            ("banana", 1, "piece", 105, 1.3),
            ("flat white", 1, "cup", 120, 7),
            ("lentil soup", 1, "bowl", 260, 14),
            ("rye toast", 2, "slice", 170, 6),
            ("greek yogurt", 150, "g", 146, 15),
            ("chicken wrap", 1, "piece", 480, 31),
            ("apple", 1, "piece", 80, 0.4),
            ("almonds", 30, "g", 174, 6.3),
            ("salmon fillet", 1, "piece", 360, 34),
            ("rice", 180, "g", 234, 4.8),
            ("dark chocolate", 20, "g", 120, 1.6),
            ("egg fried rice", 1, "box", 520, 13),
            ("hummus", 50, "g", 140, 4),
            ("oat bar", 1, "bar", 190, 4.5),
        ],
    },
    "expenses": {
        "path": "money/spend.csv",
        "header": ["date", "time", "item", "category", "amount", "note"],
        "sum": "amount",
        "noun": "spent",
        "label": "spending log",
        "skill_id": "my-spending",
        "items": [
            ("coffee", "eating out", 3.2),
            ("bus fare", "transport", 1.75),
            ("groceries", "groceries", 23.48),
            ("lunch deal", "eating out", 6.4),
            ("phone top-up", "bills", 10),
            ("paperback", "leisure", 8.99),
            ("cinema ticket", "leisure", 11.5),
            ("train ticket", "transport", 14.2),
            ("bakery", "eating out", 4.35),
            ("hardware store", "home", 17.06),
            ("pharmacy", "health", 7.89),
            ("parking", "transport", 3.6),
            ("takeaway", "eating out", 18.75),
            ("plants", "home", 12.3),
            ("gym day pass", "health", 9),
        ],
    },
    "training": {
        "path": "gym/sets.csv",
        "header": ["date", "time", "exercise", "sets", "reps", "kg", "note"],
        "sum": "sets",
        "noun": "sets",
        "label": "training log",
        "skill_id": "my-training",
        "items": [
            ("back squat", 3, 8, 60),
            ("bench press", 3, 6, 50),
            ("deadlift", 2, 5, 90),
            ("pull-up", 3, 6, 0),
            ("overhead press", 3, 8, 32.5),
            ("barbell row", 3, 10, 45),
            ("lunge", 2, 12, 16),
            ("dip", 3, 8, 0),
            ("leg press", 3, 12, 110),
            ("plank", 3, 1, 0),
            ("curl", 2, 12, 12.5),
            ("hip thrust", 3, 10, 70),
        ],
    },
}
DOMAIN_ORDER = ("food", "expenses", "training")
# Free text with a bare comma: the owner file's commonest wrong-field-count shape.
COMMA_NOTES = [
    "with milk, no sugar",
    "split, paid half",
    "felt good, slow tempo",
    "late, rushed",
    "left, right",
]
NOTES = ["", "", "", "", "quick", "home", "work", "weekend"]


def money(v: float) -> str:
    return f"{v:.2f}".rstrip("0").rstrip(".") if v != int(v) else str(int(v))


def num(v) -> str:
    if isinstance(v, float) and v != int(v):
        return f"{v:.3f}".rstrip("0").rstrip(".")
    return str(int(v))


# ------------------------------------------------------------------ the clock and the stamp
def running_day(t: datetime) -> str:
    """RB-REC5: before the day start the day still running is the one before."""
    if t.hour * 60 + t.minute < DAY_START_MIN:
        return (t - timedelta(days=1)).strftime("%Y-%m-%d")
    return t.strftime("%Y-%m-%d")


def stamp(t: datetime) -> str:
    """RB-REC5's engine-2 message stamp, '[2026-11-19 Thu 08:10]' (+ the day clause before 04:00)."""
    base = t.strftime("%Y-%m-%d %a %H:%M")
    if t.hour * 60 + t.minute < DAY_START_MIN:
        return f"[{base} (day {running_day(t)} until 04:00)] "
    return f"[{base}] "


# ------------------------------------------------------------------ rows and files
class Row:
    """One logical row: its fields as intended, how it is written, and what it counts for."""

    def __init__(self, dom: str, fields: list, shape: str = "ok"):
        self.dom, self.fields, self.shape = (
            dom,
            fields,
            shape,
        )  # shape: ok | extra | empty

    def line(self) -> str:
        return ",".join(self.fields)

    @property
    def date(self) -> str:
        return self.fields[0]

    def value(self, col: str):
        """The intended number in col (None when the cell is empty: unknown)."""
        i = DOMAINS[self.dom]["header"].index(col)
        v = self.fields[i]
        return None if v == "" else float(v)


def make_row(dom: str, day: str, minute: int, rng: random.Random, shape: str = "ok", pick=None) -> Row:
    """One row at `minute` after midnight; shape ok, extra (a bare comma in free text) or empty (summed cell)."""
    d = DOMAINS[dom]
    it = pick if pick is not None else rng.choice(d["items"])
    note = rng.choice(COMMA_NOTES) if shape == "extra" else rng.choice(NOTES)
    t = f"{minute // 60:02d}:{minute % 60:02d}"
    if dom == "food":
        name, qty, unit, kcal, prot = it
        f = [day, t, name, num(qty), unit, num(kcal), num(float(prot)), note]
        if shape == "empty":
            f[5] = ""
    elif dom == "expenses":
        name, cat, amt = it
        f = [day, t, name, cat, money(amt), note]
        if shape == "empty":
            f[4] = ""
    else:
        name, sets, reps, kg = it
        f = [day, t, name, num(sets), num(reps), num(float(kg)), note]
        if shape == "empty":
            f[3] = ""
    return Row(dom, f, shape)


def day_list(end: datetime, days: int) -> list:
    return [(end - timedelta(days=k)).strftime("%Y-%m-%d") for k in range(days - 1, -1, -1)]


class Table:
    """The seeded file: rows, comment lines and blank lines in file order."""

    def __init__(self, dom: str):
        self.dom = dom
        self.lines: list = [",".join(DOMAINS[dom]["header"])]  # str (raw) or Row
        self.rows: list = []

    def add_row(self, r: Row):
        self.lines.append(r)
        self.rows.append(r)

    def text(self) -> str:
        return "".join((x.line() if isinstance(x, Row) else x) + "\n" for x in self.lines)

    def total(self, col: str, lo: str, hi: str) -> float:
        """The intended total over the rows of [lo, hi]: wrong-field-count rows count (a person reads
        them), empty cells do not (unknown), comment lines never."""
        return round(sum((r.value(col) or 0.0) for r in self.rows if lo <= r.date <= hi), 3)

    def device_total(self, col: str, lo: str, hi: str) -> float:
        """What read_file's sum states: the rows with the header's field count only."""
        return round(
            sum((r.value(col) or 0.0) for r in self.rows if lo <= r.date <= hi and r.shape != "extra"),
            3,
        )

    def skipped_in(self, lo: str, hi: str) -> int:
        return sum(1 for r in self.rows if lo <= r.date <= hi and r.shape == "extra")

    def skipped_rows(self, lo: str, hi: str) -> list:
        """(file line number, item) of each wrong-field-count row of [lo, hi]: what the device names."""
        return [
            [k + 1, x.fields[2]]
            for k, x in enumerate(self.lines)
            if isinstance(x, Row) and x.shape == "extra" and lo <= x.date <= hi
        ]


def build_table(
    dom: str,
    now: datetime,
    rng: random.Random,
    days: int = 30,
    today_rows: int | None = None,
) -> Table:
    """A month of rows up to now: about 11% wrong field count, 3% empty summed cell, 12% comment lines,
    a few blank lines. No identical lines unless an episode adds them."""
    t = Table(dom)
    today = running_day(now)
    for day in day_list(datetime.strptime(today, "%Y-%m-%d"), days):
        n = rng.randint(3, 7) if day != today else (today_rows if today_rows is not None else rng.randint(2, 5))
        mins = sorted(rng.sample(range(6 * 60, 22 * 60), n))
        if day == today and now.strftime("%Y-%m-%d") == today:
            cap = now.hour * 60 + now.minute - 5
            mins = sorted(rng.sample(range(6 * 60, max(cap, 6 * 60 + n + 1)), n))
        used = set()
        for m in mins:
            u = rng.random()
            shape = "extra" if u < 0.11 else "empty" if u < 0.14 else "ok"
            items = [x for x in DOMAINS[dom]["items"] if x[0] not in used]
            pick = rng.choice(items)
            used.add(pick[0])
            r = make_row(dom, day, m, rng, shape, pick)
            if rng.random() < 0.13:  # a history line of v5.1.x's keyed append: '#', a word, a colon, the old row
                old = list(r.fields)
                old[1] = f"{max(0, m // 60 - 1):02d}:{m % 60:02d}"
                t.lines.append("# replaced: " + ",".join(old))
            t.add_row(r)
            if rng.random() < 0.02:
                t.lines.append("")
    return t


# ------------------------------------------------------------------ the owner's own note and skill
def note_lines(dom: str) -> list:
    d = DOMAINS[dom]
    return [
        "name: Sam (the owner)",
        "units: metric; money in GBP",
        f"{d['label'].replace(' ', '-')}: {d['path']} with header {','.join(d['header'])}; one row per entry",
    ]


def skill_body(dom: str) -> str:
    """An invented user skill of the owner's size class (about 3.4 to 4 KB with front matter)."""
    d = DOMAINS[dom]
    cols = ",".join(d["header"])
    what = {
        "food": "what I eat and drink",
        "expenses": "what I spend",
        "training": "my gym sets",
    }[dom]
    num_cols = {
        "food": "qty, kcal and protein_g",
        "expenses": "amount",
        "training": "sets, reps and kg",
    }[dom]
    example = {
        "food": "2026-11-18,08:05,porridge,1,bowl,310,11,",
        "expenses": "2026-11-18,08:05,coffee,eating out,3.2,",
        "training": "2026-11-18,18:30,back squat,3,8,60,",
    }[dom]
    body = f"""---
title: My {d["label"]}
desc: how I keep {what}
version: 3
---
# My {d["label"]}

I keep {what} in one CSV file that I own: `{d["path"]}`. Use this skill every time I log, correct,
move, remove or total anything in it. The file is mine; never create a second file for the same thing.

## The file
- Path: `{d["path"]}`. Header (line 1, never change it): `{cols}`.
- One row per entry, one line per row, fields in header order, plain commas between fields.
- `date` is YYYY-MM-DD and `time` is HH:MM, 24-hour, local time.
- {num_cols} are plain numbers: digits and a point, no units and no thousands separators.
- An unknown number is left empty, never written as 0. Free text goes in `note`; avoid commas in
  free text (write "and" instead), because a bare comma adds a field and the row stops adding up.
- Lines starting with `#` are old history lines from an earlier version. They are not rows: leave
  them alone and never count them.
- Example row: `{example}`

## Which day
- Every message carries its time. Before 04:00 the stamp names the day still running: an entry
  sent at 00:30 belongs to that running day, not to the new calendar date.
- When I say "yesterday" or name a weekday, work out the date from the stamp, not from memory.

## Logging
- Append new rows with append_file, one line per entry, ending with a newline.
- If I give several entries in one message, append them in one call, one line each.
- Use the numbers I give. If I give none, use a sensible typical figure and say so in the note.
- After logging, reply in one short line with what was added. Do not repeat the whole day.

## Corrections, moves and removals
- Read the rows first (read_file with column date and from/to for the day) so you see the line
  numbers, then change the one line with edit_file: old is the whole line as it stands, new is
  the corrected line. A move to another day is an edit of the date field.
- To remove a row, edit its whole line to an empty string.
- If the same line appears twice (I logged it twice), pass `line` with the number of the copy
  to change, so only that copy changes.
- Never rewrite the whole file to change one row.

## Totals
- Totals come from the device, never from adding numbers up in your head: read_file with
  column `date`, from and to for the range, and `sum` naming the columns. A day is from=to=the
  date; a month is from=YYYY-MM.
- The result says which rows it could not count (a wrong field count, a blank cell) by line.
  Look at those lines: if a row is clearly mine and only has a stray comma, fix the line, then
  total again; otherwise tell me which entries are not in the total.
- Use calc for anything that is not a plain total (what is left, an average, a difference).

## Habits
- I usually log within an hour of the entry; a message with no time means "now" by the stamp.
- If I write "same as yesterday", read yesterday's rows and copy the matching one with today's
  date and time, so the row is mine and not a guess.
- I sometimes send two entries in one line ("{example.split(",")[2]} and a second one"): one row each.
- A week is the last 7 days including today unless I name the days. A month so far starts on the 1st.

## Replies
- Short and plain. Say a figure once, with its unit. Name entries that were left out.
- If a tool fails, say it failed and that nothing was changed; never say it was logged.
- Do not ask me to confirm a plain log; ask only when the entry cannot be written without an answer.
"""
    return body


def other_skill_body(dom: str) -> tuple:
    """The second user skill of the two-skill arm: another domain's skill (both attached at once)."""
    nxt = {"food": "expenses", "expenses": "training", "training": "food"}[dom]
    return nxt, skill_body(nxt)


# ------------------------------------------------------------------ owner phrasing
def _ask_total(dom: str, rng: random.Random, kind: str) -> str:
    q = {
        ("food", "day"): [
            "How many kcal have I had today?",
            "What's my kcal total for today so far?",
        ],
        ("food", "yesterday"): [
            "How many kcal did I eat yesterday?",
            "What was my kcal total yesterday?",
        ],
        ("food", "week"): ["How many kcal in total over the last 7 days, today included?"],
        ("food", "month"): ["Total kcal this month so far?"],
        ("expenses", "day"): [
            "How much have I spent today?",
            "What's today's spending total?",
        ],
        ("expenses", "yesterday"): [
            "How much did I spend yesterday?",
            "What did yesterday's spending come to?",
        ],
        ("expenses", "week"): ["How much have I spent over the last 7 days, today included?"],
        ("expenses", "month"): ["How much have I spent this month so far?"],
        ("training", "day"): ["How many sets have I done today?", "Total sets today?"],
        ("training", "yesterday"): ["How many sets did I do yesterday?"],
        ("training", "week"): ["How many sets in the last 7 days, today included?"],
        ("training", "month"): ["How many sets this month so far?"],
    }[(dom, kind)]
    return rng.choice(q)


def _range(kind: str, today: str) -> tuple:
    t = datetime.strptime(today, "%Y-%m-%d")
    if kind == "day":
        return today, today
    if kind == "yesterday":
        y = (t - timedelta(days=1)).strftime("%Y-%m-%d")
        return y, y
    if kind == "week":
        return (t - timedelta(days=6)).strftime("%Y-%m-%d"), today
    return t.strftime("%Y-%m-01"), today


def _add_text(dom: str, r: Row) -> str:
    f = r.fields
    if dom == "food":
        return f"Log {f[3]} {f[4]} {f[2]}: {f[5]} kcal, {f[6]} g protein."
    if dom == "expenses":
        return f"Spent {f[4]} on {f[2]} ({f[3]})."
    return f"Log {f[2]}: {f[3]} sets of {f[4]} at {f[5]} kg."


def _value_col(dom: str) -> str:
    return {"food": "kcal", "expenses": "amount", "training": "reps"}[dom]


def _with_value(dom: str, r: Row, v: str) -> Row:
    f = list(r.fields)
    f[DOMAINS[dom]["header"].index(_value_col(dom))] = v
    return Row(dom, f, r.shape)


def _new_value(dom: str, r: Row, rng: random.Random) -> str:
    v = r.value(_value_col(dom)) or 1.0
    if dom == "expenses":
        return money(round(v + rng.choice([0.5, 1.1, -0.4, 2.25]), 2))
    if dom == "food":
        return num(int(v + rng.choice([40, 60, -30, 90])))
    return num(max(1, int(v + rng.choice([2, -2, 4]))))  # never fewer than 1 rep


def _correct_text(dom: str, r: Row, new: str, when: str) -> str:
    f = r.fields
    if dom == "food":
        return f"Correction: the {f[2]} {when} was {new} kcal, not {f[5]}."
    if dom == "expenses":
        return f"The {f[2]} {when} was actually {new}, not {f[4]}. Please fix it."
    return f"Fix the {f[2]} {when}: it was {new} reps per set, not {f[4]}."


# ------------------------------------------------------------------ episodes
def _now(rng: random.Random, late: bool) -> datetime:
    base = datetime(2026, 11, 2) + timedelta(days=rng.randint(0, 26))
    if late:
        return base.replace(hour=rng.randint(0, 3), minute=rng.randint(0, 59))
    return base.replace(hour=rng.randint(12, 22), minute=rng.randint(0, 59))


def late_total(i: int) -> bool:
    """Every seventh total question is asked between midnight and the 04:00 day start."""
    return i % 7 == 3


def total_episode(i: int, dom: str) -> dict:
    rng = random.Random(f"total-{dom}-{i}")
    kind = ["day", "day", "yesterday", "week", "month", "day"][i % 6]
    now = _now(rng, late=late_total(i))
    t = build_table(dom, now, rng)
    today = running_day(now)
    lo, hi = _range(kind, today)
    col = DOMAINS[dom]["sum"]
    return {
        "id": f"total-{dom}-{i:03d}",
        "kind": "total",
        "sub": kind,
        "dom": dom,
        "now": now,
        "table": t,
        "turns": [{"at": now, "text": _ask_total(dom, rng, kind)}],
        "truth": {
            "col": col,
            "lo": lo,
            "hi": hi,
            "total": t.total(col, lo, hi),
            "device": t.device_total(col, lo, hi),
            "skipped": t.skipped_in(lo, hi),
            "skipped_rows": t.skipped_rows(lo, hi),
        },
    }


def total_truth(episode_id: str, dom: str) -> dict:
    """The truth of a stored total episode, rebuilt from its id (episodes are deterministic)."""
    return total_episode(int(episode_id.rsplit("-", 1)[1]), dom)["truth"]


def _unused_pick(dom: str, t: "Table", today: str, rng: random.Random):
    used = {r.fields[2] for r in t.rows if r.date == today}
    return rng.choice([x for x in DOMAINS[dom]["items"] if x[0] not in used])


def _ep_add(ep: dict, ok_today: list, rng: random.Random):
    dom, now, today = ep["dom"], ep["now"], running_day(ep["now"])
    new = make_row(
        dom,
        today,
        now.hour * 60 + now.minute,
        rng,
        "ok",
        _unused_pick(dom, ep["table"], today, rng),
    )
    new.fields[-1] = ""
    ep["turns"] = [{"at": now, "text": _add_text(dom, new)}]
    ep["expect"] = {"op": "add", "row": new.fields}


def _ep_correct(ep: dict, ok_today: list, rng: random.Random):
    dom, now, today = ep["dom"], ep["now"], running_day(ep["now"])
    if ep["sub"] == "correct_visible":
        # logged earlier in this chat: the prior turn appended it for real
        pick = _unused_pick(dom, ep["table"], today, rng)
        t_prior = now - timedelta(minutes=rng.randint(20, 90))
        target = make_row(dom, today, t_prior.hour * 60 + t_prior.minute, rng, "ok", pick)
        target.fields[-1] = ""
        ep["prior"] = [
            {
                "at": t_prior,
                "text": _add_text(dom, target),
                "append": target.line() + "\n",
            }
        ]
        when = "I just logged"
    else:
        target = rng.choice(ok_today)
        when = f"at {target.fields[1]} today"
    new_v = _new_value(dom, target, rng)
    ep["turns"] = [{"at": now, "text": _correct_text(dom, target, new_v, when)}]
    ep["expect"] = {
        "op": "replace",
        "old": target.fields,
        "new": _with_value(dom, target, new_v).fields,
        "appended_in_prior": ep["sub"] == "correct_visible",
    }


def _ep_identical(ep: dict, ok_today: list, rng: random.Random):
    target = rng.choice(ok_today)
    # the owner logged it twice: an identical line right after it
    lines = ep["table"].lines
    lines.insert(lines.index(target) + 1, Row(ep["dom"], list(target.fields), "ok"))
    text = f"I logged the {target.fields[2]} at {target.fields[1]} twice. Remove one of them."
    ep["turns"] = [{"at": ep["now"], "text": text}]
    ep["expect"] = {"op": "remove_one", "old": target.fields}


def _ep_move(ep: dict, ok_today: list, rng: random.Random):
    target = rng.choice(ok_today)
    y = (datetime.strptime(running_day(ep["now"]), "%Y-%m-%d") - timedelta(days=1)).strftime("%Y-%m-%d")
    moved = [y, *target.fields[1:]]
    text = f"The {target.fields[2]} at {target.fields[1]} today was really yesterday. Move it."
    ep["turns"] = [{"at": ep["now"], "text": text}]
    ep["expect"] = {
        "op": "replace",
        "old": target.fields,
        "new": moved,
        "appended_in_prior": False,
    }


EPISODE_BUILDERS = {
    "add": _ep_add,
    "correct_visible": _ep_correct,
    "correct_hidden": _ep_correct,
    "identical": _ep_identical,
    "move": _ep_move,
}


def write_episode(i: int, dom: str, sub: str, fault: str = "") -> dict:
    """add | correct_visible | correct_hidden | identical | move."""
    rng = random.Random(f"write-{dom}-{sub}-{i}-{fault}")
    now = _now(rng, late=sub == "add" and i % 4 == 1)
    t = build_table(dom, now, rng, today_rows=rng.randint(3, 5))
    today = running_day(now)
    ok_today = [r for r in t.rows if r.date == today and r.shape == "ok"]
    ep = {
        "id": f"{sub}-{dom}-{i:03d}{('-' + fault) if fault else ''}",
        "kind": "write",
        "sub": sub,
        "dom": dom,
        "now": now,
        "table": t,
        "prior": [],
        "fault": fault,
    }
    EPISODE_BUILDERS[sub](ep, ok_today, rng)
    return ep


def skill_episode(i: int, dom: str, arm: str) -> dict:
    """A fresh chat whose first message is a tracking action: an add, a correction or a total."""
    sub = ["add", "correct_hidden", "total"][i % 3]
    if sub == "total":
        ep = total_episode(1000 + i, dom)
    else:
        ep = write_episode(1000 + i, dom, sub)
    ep["id"] = f"{arm}-{ep['id']}"
    ep["skill_arm"] = arm
    return ep


WRITE_SUBS = ("add", "correct_visible", "correct_hidden", "identical", "move")


def plan(n_total: int, n_write: int, n_fault: int = 0) -> list:
    """Episodes of one cell, the three domains interleaved so any prefix is balanced."""
    out = []
    for i in range(n_total):
        out.append(total_episode(i // 3, DOMAIN_ORDER[i % 3]))
    for i in range(n_write):
        k = i // 3
        out.append(write_episode(k, DOMAIN_ORDER[i % 3], WRITE_SUBS[k % len(WRITE_SUBS)]))
    for i in range(n_fault):
        k = i // 3
        sub = ("add", "correct_hidden", "identical")[k % 3]
        mode = "always" if k % 2 == 0 else "once"
        out.append(write_episode(500 + k, DOMAIN_ORDER[i % 3], sub, fault=mode))
    return out


def plan_skills(arm: str, n: int) -> list:
    return [skill_episode(i // 3, DOMAIN_ORDER[i % 3], arm) for i in range(n)]


# ------------------------------------------------------------------ checks
NUM_RE = re.compile(r"(?<![\w.])-?\d{1,3}(?:,\d{3})+(?:\.\d+)?(?![\w])|(?<![\w.])-?\d+(?:\.\d+)?")
# Numbers that are no figure: dates, clock times, day-of-month mentions and line references.
MONTHS = r"(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)[a-z]*"
NOT_FIGURES = re.compile(
    r"\d{4}-\d{2}-\d{2}|\b\d{1,2}:\d{2}\b|"
    rf"\b\d{{1,2}}(?:st|nd|rd|th)?(?:\s*[-\u2013\u2014]\s*\d{{1,2}}(?:st|nd|rd|th)?)?\s+{MONTHS}\b|"
    rf"\b{MONTHS}\s+\d{{1,2}}(?:st|nd|rd|th)?\b|"
    r"\blines?\s+\d+(?:\s*(?:,|and|to|-)\s*\d+)*",
    re.I,
)
# What a reply says about rows a total left out, beyond naming their line or item.
LEFT_OUT_RE = re.compile(
    r"not (?:be )?(?:counted|included|summed|in (?:the|that|this) (?:total|sum))|left out|leaves? out|exclud|"
    r"(?:couldn't|could not|can't|cannot|didn't|did not|wasn't|weren't) (?:be )?(?:count|add|includ|read|pars|sum)|"
    r"field count|wrong (?:number of )?fields?|extra (?:comma|field)|stray comma|malformed|unquoted comma",
    re.I,
)


def numbers(text: str) -> list:
    out = []
    for m in NUM_RE.findall(NOT_FIGURES.sub(" ", text or "")):
        try:
            out.append(float(m.replace(",", "")))
        except ValueError:
            pass
    return out


def names_left_out(reply: str, rows: list) -> bool:
    """The reply points at a row the total left out: its line number, or what is wrong with it. An item's name
    alone is not enough (the same item is often counted in another row), nor is 'skipped' (the device's tail
    says comment lines were skipped on every read)."""
    text = reply or ""
    for line_no, _item in rows:
        if re.search(rf"\blines?\b[^.\n]{{0,40}}\b{line_no}\b", text, re.I):
            return True
    return bool(LEFT_OUT_RE.search(text))


def _close(x: float, want: float) -> bool:
    return abs(x - want) < 0.0051 or (abs(want) >= 100 and abs(x - want) <= 0.5)


def judge_total(reply: str, truth: dict) -> dict:
    """PASS when the reply states the intended total, or states the device's total and says which rows
    it left out. A total that silently misses the rows the result named is a FAIL (ruling 49)."""
    ns = numbers(reply)
    full = any(_close(x, truth["total"]) for x in ns)
    partial = truth["skipped"] > 0 and any(_close(x, truth["device"]) for x in ns)
    named = names_left_out(reply, truth.get("skipped_rows") or [])
    if full:
        verdict = "full"
    elif partial and named:
        verdict = "partial_named"
    elif partial:
        verdict = "partial_silent"
    else:
        verdict = "wrong"
    return {"ok": verdict in ("full", "partial_named"), "verdict": verdict}


def _data_lines(text: str) -> list:
    """The lines of a file as a person reads it: no header, no comments, no blank lines."""
    out = []
    for raw in (text or "").split("\n")[1:]:
        if raw.strip() == "" or raw.lstrip().startswith("#"):
            continue
        out.append(raw)
    return out


def _fields(line: str) -> list:
    return [x.strip() for x in line.split(",")]


def _num_eq(a: str, b: str) -> bool:
    try:
        return abs(float(a) - float(b)) < 1e-6
    except ValueError:
        return a.strip().lower() == b.strip().lower()


def _row_matches(line: str, want: list, header: list, loose_time: bool = True) -> bool:
    f = _fields(line)
    if len(f) != len(header):
        return False
    for k, col in enumerate(header):
        if col == "note":
            continue
        if col == "time" and loose_time:
            continue
        if col in ("item", "exercise"):
            if want[k].lower() not in f[k].lower() and f[k].lower() not in want[k].lower():
                return False
            continue
        if col in ("unit", "category"):
            continue  # the owner's words, not the model's to match exactly
        if not _num_eq(f[k], want[k]):
            return False
    return True


def multiset_diff(before: list, after: list) -> tuple:
    from collections import Counter

    b, a = Counter(before), Counter(after)
    return list((b - a).elements()), list((a - b).elements())


def _frame(before_text: str, after_text: str) -> tuple:
    """(data lines removed, data lines added, stray count and facts) of everything but the rows themselves."""
    removed, added = multiset_diff(_data_lines(before_text), _data_lines(after_text))
    raw_b, raw_a = (before_text or "").split("\n"), (after_text or "").split("\n")
    is_comment = lambda x: x.lstrip().startswith("#")  # noqa: E731
    blanks = lambda raw: sum(1 for x in raw[:-1] if x.strip() == "")  # noqa: E731
    lost_comments, new_comments = multiset_diff(
        [x for x in raw_b if is_comment(x)], [x for x in raw_a if is_comment(x)]
    )
    header_ok = raw_a[:1] == raw_b[:1]
    blank_added = max(0, blanks(raw_a) - blanks(raw_b))
    # A '#' history line the head adds beside a correction follows the file's own convention: it is
    # no row and no total reads it (ruling 49), so it is counted apart, never as a stray line.
    facts = {
        "removed": removed,
        "added": added,
        "blank_added": blank_added,
        "header_ok": header_ok,
        "comments_lost": len(lost_comments),
        "history_added": len(new_comments),
    }
    return (
        removed,
        added,
        blank_added + (0 if header_ok else 1) + len(lost_comments),
        facts,
    )


def _check_add(exp, header, removed, added) -> tuple:
    good = [x for x in added if _row_matches(x, exp["row"], header)]
    ok = not removed and len(added) == 1 and len(good) == 1
    dup = max(0, len(good) - 1)
    return ok, len(removed) + max(0, len(added) - len(good)) + dup, dup


def _check_replace(exp, header, removed, added) -> tuple:
    old_line = ",".join(exp["old"])
    good = [x for x in added if _row_matches(x, exp["new"], header, loose_time=False)]
    ok = removed == [old_line] and len(added) == 1 and len(good) == 1
    dup = max(0, len(good) - 1)
    stray = max(0, len(removed) - 1) + max(0, len(added) - len(good)) + dup
    return ok, stray + (1 if removed and removed != [old_line] else 0), dup


def _check_remove_one(exp, header, removed, added) -> tuple:
    old_line = ",".join(exp["old"])
    ok = removed == [old_line] and not added
    return (
        ok,
        len(added) + max(0, len(removed) - 1) + (1 if removed and removed[0] != old_line else 0),
        0,
    )


CHECKS = {"add": _check_add, "replace": _check_replace, "remove_one": _check_remove_one}


def judge_write(ep: dict, before_text: str, after_text: str) -> dict:
    """The file after the turn against what the owner asked: exact on every other line."""
    removed, added, stray, out = _frame(before_text, after_text)
    exp = ep["expect"]
    ok, more, dup = CHECKS[exp["op"]](exp, DOMAINS[ep["dom"]]["header"], removed, added)
    stray += more
    out.update({"ok": ok and stray == 0, "effect_ok": ok, "stray": stray, "dup": dup})
    return out


NUMERIC = {
    "food": ("qty", "kcal", "protein_g"),
    "expenses": ("amount",),
    "training": ("sets", "reps", "kg"),
}


def _day_totals(text: str, dom: str, days: set) -> dict:
    """Per day and numeric column, the total of the rows a device sum counts (the header's field count)."""
    header = DOMAINS[dom]["header"]
    out = {}
    for line in _data_lines(text):
        f = line.split(",")
        if len(f) != len(header) or f[0] not in days:
            continue
        for col in NUMERIC[dom]:
            v = f[header.index(col)]
            try:
                out[(f[0], col)] = round(out.get((f[0], col), 0.0) + (float(v) if v else 0.0), 3)
            except ValueError:
                out[(f[0], col)] = None
    return out


def totals_after_fix(ep: dict, before_text: str, after_text: str) -> bool:
    """Correction, move and identical-row episodes: every device total of the days the change touches
    (each numeric column) equals the total the owner's request leads to."""
    exp, dom = ep["expect"], ep["dom"]
    if exp["op"] == "add":
        return True
    old = ",".join(exp["old"])
    days = {exp["old"][0]} | ({exp["new"][0]} if exp["op"] == "replace" else set())
    lines = (before_text or "").split("\n")
    if exp["op"] == "replace" and exp.get("appended_in_prior") is not None:
        k = lines.index(old)
        lines[k] = ",".join(exp["new"])
    else:
        lines.remove(old)
    want = _day_totals("\n".join(lines), dom, days)
    return _day_totals(after_text, dom, days) == want


# ------------------------------------------------------------------ calls
def tool_name(name: str, args) -> str:
    """The tool a call runs: a call through more names its target."""
    if name == "more" and isinstance(args, dict) and args.get("name"):
        return str(args["name"]).split(".")[-1]
    return name


def _target(c: dict) -> tuple:
    """(tool, path or listing prefix) of one call, a call through more unwrapped."""
    args = c["args"] if isinstance(c["args"], dict) else {}
    name = tool_name(c["name"], args)
    if c["name"] == "more" and isinstance(args.get("args"), dict):
        args = args["args"]
    return name, str(args.get("path") or args.get("prefix") or "")


def pickup(calls: list, dom: str) -> dict:
    """Did the head read its skill before its first tracking action (a call on a table, or a listing
    outside skills/)?"""
    sid = DOMAINS[dom]["skill_id"]
    skill_at = track_at = None
    for k, c in enumerate(calls):
        name, where = _target(c)
        in_skills = where.startswith("skills/") or where == "skills"
        if name == "read_file" and in_skills and sid in where and skill_at is None:
            skill_at = k
        elif name in ("read_file", "append_file", "edit_file", "write_file", "list_files") and not in_skills:
            track_at = k if track_at is None else track_at
    return {
        "skill_read": skill_at is not None,
        "tracked": track_at is not None,
        "before_first_action": skill_at is not None and (track_at is None or skill_at < track_at),
    }


def needs_label(r: dict) -> bool:
    """A write turn that can hold a false success claim: a fault was injected or the file is not as asked."""
    return r["kind"] == "write" and (bool(r.get("fault")) or not r["file"]["effect_ok"])
