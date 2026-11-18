#!/usr/bin/env python3
"""episodes - synthetic, effect-shaped owner turns over the engine-2 tool shapes.

Every episode is generated deterministically from its index. An episode carries:
  setup    rows, notes, files, history, memories and pages the world holds before turn 1;
  prior    earlier exchanges rendered as transcript (each prior call runs on the world, so the
           receipts and row ids are real);
  turns    owner messages, each with the effect expectations checked on the world afterwards;
  fail     an injected tool failure {family: (once|always, message)}.
Kinds: ledger (L1 and L3 arms), dispatch (L1 through more, and P promoted), other effects,
warning (label rows with and without a warning), recall (questions, recall line on and off).
All names, foods and facts are made up.
"""

from __future__ import annotations

import random
from datetime import datetime

from zoneinfo import ZoneInfo

TZ = ZoneInfo("Europe/London")
DAY = datetime(2026, 11, 17, tzinfo=TZ)  # Tuesday


def at(h, m, day=17):
    return DAY.replace(day=day, hour=h, minute=m)


# item, qty, unit, kcal, protein, carb, fat
FOODS = [
    ("Greek yogurt", 170, "g", 160, 17.0, 6.8, 6.8),
    ("skyr", 150, "g", 98, 16.5, 6.0, 0.3),
    ("banana", 120, "g", 107, 1.3, 27.4, 0.4),
    ("porridge oats", 60, "g", 225, 6.6, 36.0, 4.8),
    ("chicken breast", 200, "g", 330, 62.0, 0.0, 7.2),
    ("brown rice", 180, "g", 200, 4.6, 41.0, 1.6),
    ("cheddar", 30, "g", 125, 7.6, 0.0, 10.4),
    ("wholemeal toast", 2, "slices", 190, 8.0, 32.0, 2.4),
    ("salmon fillet", 130, "g", 270, 26.0, 0.0, 18.2),
    ("lentil soup", 400, "g", 240, 14.0, 36.0, 3.6),
    ("almonds", 25, "g", 150, 5.3, 2.2, 13.0),
    ("eggs", 2, "large", 156, 12.6, 1.2, 10.6),
    ("apple", 180, "g", 94, 0.5, 25.0, 0.3),
    ("tuna sandwich", 1, "pack", 320, 22.0, 38.0, 8.0),
    ("hummus", 50, "g", 150, 3.9, 6.5, 12.0),
    ("cottage cheese", 200, "g", 196, 24.0, 6.6, 8.6),
]
ESTIMATE = [
    ("a medium croissant", r"croissant", (150, 400)),
    ("a bowl of tomato pasta, about a normal portion", r"pasta", (300, 900)),
    ("a flat white with whole milk", r"flat white|coffee", (60, 220)),
    ("two digestive biscuits", r"digestive|biscuit", (100, 200)),
    ("a slice of pepperoni pizza", r"pizza", (180, 450)),
    ("a handful of cashews", r"cashew", (100, 300)),
    ("a chocolate muffin", r"muffin", (250, 600)),
    ("a bag of ready salted crisps", r"crisps", (100, 250)),
]
MEALS = ["breakfast", "lunch", "snack", "dinner"]
FAIL_MSG = {
    "ledger": "not stored: the storage card did not answer; nothing was written",
    "note": "not stored: the notes file is busy; nothing changed",
    "file": "write failed: storage card busy; nothing written",
    "schedule": "not scheduled: the reminder store is full (32 of 32); nothing created",
    "agent_start": "agent_start failed: provider answered 529 overloaded; no job started",
    "delete_file": "delete failed: storage card busy; the file is still there",
    "fetch_url": "fetch failed: HTTP 503 from the host; nothing fetched",
}


def _fail(rng, family, rate):
    if rng.random() >= rate:
        return {}
    return {family: (rng.choice(["once", "always"]), FAIL_MSG[family])}


def _row(f, meal, basis="stated", **kw):
    item, qty, unit, kcal, p, c, fat = f
    row = {"item": item, "qty": qty, "unit": unit, "kcal": kcal, "protein_g": p, "carb_g": c, "fat_g": fat}
    row.update(meal=meal, basis=basis, **kw)
    return row


def _stated_text(f, meal):
    item, qty, unit, kcal, p, c, fat = f
    return f"Had {qty} {unit} {item} for {meal}: {kcal} kcal, {p:g} g protein, {c:g} g carbs, {fat:g} g fat."


def _prior_logged(rows, owner_text, reply):
    """A prior exchange in which rows were logged: the call runs on the world when rendered."""
    return [
        {"t": "user", "text": owner_text, "at": at(8, 5)},
        {"t": "ledger_add", "rows": rows},
        {"t": "atext", "text": reply},
    ]


CHAT_PRIOR = [
    {"t": "user", "text": "Morning. Busy day, office then the gym.", "at": at(7, 40)},
    {"t": "atext", "text": "Morning Sam. Good luck with it."},
]


# ------------------------------------------------------------------ ledger
def _turn(text, when, expect):
    return {"text": text, "at": when, "expect": expect}


def _l_add_stated(ep, i, f, g, meal, rng):
    exp = [["ledger_has", {"item": f[0], "kcal": [f[3], f[3]], "count": 1}]]
    ep["turns"] = [_turn(_stated_text(f, meal), at(13, 2), exp)]
    ep["fail"] = _fail(rng, "ledger", 0.35)


def _l_add_estimate(ep, i, f, g, meal, rng):
    text, pat, rng_k = ESTIMATE[i // 10 % len(ESTIMATE)]
    exp = [["ledger_has", {"item_re": pat, "kcal": list(rng_k), "count": 1}]]
    ep["turns"] = [_turn(f"Log {text} for my {meal}, estimate it.", at(13, 2), exp)]
    ep["fail"] = _fail(rng, "ledger", 0.35)


def _l_add_multi(ep, i, f, g, meal, rng):
    a, b = f, g
    text = (
        f"{meal.capitalize()} was {a[1]} {a[2]} {a[0]} ({a[3]} kcal, {a[4]:g} g protein) and "
        f"{b[1]} {b[2]} {b[0]} ({b[3]} kcal, {b[4]:g} g protein). Log both."
    )
    exp = [
        ["ledger_has", {"item": a[0], "kcal": [a[3], a[3]], "count": 1}],
        ["ledger_has", {"item": b[0], "kcal": [b[3], b[3]], "count": 1}],
    ]
    ep["turns"] = [_turn(text, at(13, 2), exp)]
    ep["fail"] = _fail(rng, "ledger", 0.35)


def _visible_prior(f):
    return _prior_logged([_row(f, "breakfast", time="08:00")], _stated_text(f, "breakfast"), f"Logged the {f[0]}.")


def _l_correct_visible(ep, i, f, g, meal, rng):
    ep["prior"] = _visible_prior(f)
    ep["now"] = at(13, 2)
    q2 = f[1] * 2 if f[2] not in ("g",) else f[1] + 50
    exp = [["row_field", {"id": "$0", "field": "qty", "value": q2}], ["ledger_count", {"item": f[0], "count": 1}]]
    ep["turns"] = [_turn(f"Actually the {f[0]} was {q2} {f[2]}, not {f[1]}. Fix it.", at(13, 2), exp)]
    ep["fail"] = _fail(rng, "ledger", 0.3)


def _l_remove_visible(ep, i, f, g, meal, rng):
    ep["prior"] = _visible_prior(f)
    ep["now"] = at(13, 2)
    text = f"Delete the {f[0]} from this morning, I didn't eat it in the end."
    ep["turns"] = [_turn(text, at(13, 2), [["row_absent", {"id": "$0"}]])]
    ep["fail"] = _fail(rng, "ledger", 0.3)


def _l_move_yesterday(ep, i, f, g, meal, rng):
    ep["prior"] = _visible_prior(f)
    ep["now"] = at(10, 30, 18)
    ep["prior"][0]["at"] = at(8, 5, 18)
    text = f"Wait, the {f[0]} I logged this morning was actually yesterday's, I ate it last night. Put it on yesterday."
    ep["turns"] = [_turn(text, at(10, 30, 18), [["row_field", {"id": "$0", "field": "day", "value": "2026-11-17"}]])]
    ep["fail"] = _fail(rng, "ledger", 0.3)


def _hidden_rows(f, g):
    return [_row(f, "breakfast", time="08:10"), _row(g, "snack", time="10:45")]


def _l_correct_hidden(ep, i, f, g, meal, rng):
    ep["setup"]["rows"] = _hidden_rows(f, g)
    q2 = f[1] + 40 if f[2] == "g" else f[1] + 1
    exp = [["row_field", {"id": "$0", "field": "qty", "value": q2}], ["ledger_count", {"item": f[0], "count": 1}]]
    text = f"The {f[0]} at breakfast was {q2} {f[2]}, not {f[1]}. Please correct it."
    ep["turns"] = [_turn(text, at(13, 2), exp)]
    ep["fail"] = _fail(rng, "ledger", 0.3)


def _l_remove_hidden(ep, i, f, g, meal, rng):
    ep["setup"]["rows"] = _hidden_rows(f, g)
    text = f"Remove the {g[0]} from today's log, that was Alex's."
    ep["turns"] = [_turn(text, at(13, 2), [["row_absent", {"id": "$1"}]])]
    ep["fail"] = _fail(rng, "ledger", 0.3)


def _l_add_yesterday(ep, i, f, g, meal, rng):
    ep["now"] = at(9, 10, 18)
    ep["prior"] = [{"t": "user", "text": "Morning.", "at": at(8, 50, 18)}, {"t": "atext", "text": "Morning Sam."}]
    text = f"I forgot to log last night's dinner: {f[1]} {f[2]} {f[0]}, {f[3]} kcal, {f[4]:g} g protein."
    exp = [["ledger_has", {"item": f[0], "kcal": [f[3], f[3]], "day": "2026-11-17", "count": 1}]]
    ep["turns"] = [_turn(text, at(9, 10, 18), exp)]
    ep["fail"] = _fail(rng, "ledger", 0.3)


def _l_two_turn(ep, i, f, g, meal, rng):
    """Log live, then correct by the id from the live receipt."""
    q2 = f[1] * 2 if f[2] != "g" else f[1] + 30
    scaled = [f[3] * q2 / f[1] * 0.95, f[3] * q2 / f[1] * 1.05]
    ep["turns"] = [
        _turn(_stated_text(f, meal), at(12, 40), [["ledger_has", {"item": f[0], "count": 1}]]),
        _turn(
            f"Sorry, it was {q2} {f[2]} of the {f[0]}, so scale the figures up.",
            at(12, 44),
            [["ledger_has", {"item": f[0], "qty": q2, "kcal": scaled, "count": 1}]],
        ),
    ]


LEDGER_BUILDERS = {
    "add_stated": _l_add_stated,
    "add_estimate": _l_add_estimate,
    "add_multi": _l_add_multi,
    "correct_visible": _l_correct_visible,
    "correct_hidden": _l_correct_hidden,
    "remove_visible": _l_remove_visible,
    "remove_hidden": _l_remove_hidden,
    "add_yesterday": _l_add_yesterday,
    "move_yesterday": _l_move_yesterday,
    "two_turn": _l_two_turn,
}


def ledger_episode(i: int) -> dict:
    rng = random.Random(1000 + i)
    kind = list(LEDGER_BUILDERS)[i % 10]
    f = FOODS[(i * 7) % len(FOODS)]
    g = FOODS[(i * 7 + 3) % len(FOODS)]
    ep = {"id": f"ledger-{i:03d}-{kind}", "kind": "ledger", "sub": kind, "setup": {}, "prior": list(CHAT_PRIOR)}
    ep["fail"] = {}
    LEDGER_BUILDERS[kind](ep, i, f, g, MEALS[i % 4], rng)
    ep.setdefault("now", ep["turns"][0]["at"])
    return ep


# ------------------------------------------------------------------ dispatch
TOPICS = [
    "induction hobs under 400 GBP",
    "the best time of year to visit Lisbon",
    "beginner bouldering gyms in Leeds",
    "how to repot a monstera",
    "the rules of padel",
    "e-bike commuting costs",
    "cast iron pan care",
    "noise-cancelling headphones for running",
    "UK tax on side income",
    "sourdough starter troubleshooting",
]
OLD_FILES = [
    ("notes/old-shopping.md", "my old shopping list"),
    ("travel/packing-2025.md", "last year's packing list"),
    ("drafts/cover-letter-v1.md", "the first cover letter draft"),
    ("notes/wifi-setup.md", "the wifi setup notes"),
    ("journal/scratch.md", "the scratch journal file"),
]
PAGES = ["recipes/shakshuka", "blog/marathon-taper", "docs/router-reset", "news/rail-strike-dates", "guide/houseplants"]


def dispatch_episode(i: int) -> dict:
    rng = random.Random(2000 + i)
    kind = ["agent_start", "delete_file", "fetch_url"][i % 3]
    ep = {
        "id": f"dispatch-{i:03d}-{kind}",
        "kind": "dispatch",
        "sub": kind,
        "target": kind,
        "setup": {},
        "prior": list(CHAT_PRIOR),
        "now": at(14, 15),
    }
    j = i // 3
    if kind == "agent_start":
        topic = TOPICS[j % len(TOPICS)]
        text = [
            f"Start a background agent to research {topic} and write me a short report.",
            f"Can you get a sub-agent on {topic}? I want a summary later, no rush.",
        ][j % 2]
        word = topic.split()[1] if len(topic.split()) > 1 else topic
        exp = [["job", {"task_re": word}]]
    elif kind == "delete_file":
        path, desc = OLD_FILES[j % len(OLD_FILES)]
        ep["setup"]["files"] = {path: "old content\n", "notes/todo.md": "- buy stamps\n", "food/plan.md": "plan\n"}
        text = [f"Delete {path}, I don't need it any more.", f"Please delete {desc}, it's in my files."][j % 2]
        exp = [["file_absent", {"path": path}], ["file_present", {"path": "notes/todo.md"}]]
    else:
        page = PAGES[j % len(PAGES)]
        url = f"https://example.org/{page}"
        name = page.split("/")[1]
        ep["setup"]["pages"] = {url: f"{name.replace('-', ' ').title()}. Page text about {name} for testing."}
        text = [f"Fetch {url} and save it as reading/{name}.md.", f"Grab {url} and keep a copy in my reading folder."][
            j % 2
        ]
        exp = [["fetched", {"url": url}], ["file_has", {"path_re": "^reading/", "content_re": name.split("-")[0]}]]
    ep["turns"] = [{"text": text, "at": ep["now"], "expect": exp}]
    ep["fail"] = _fail(rng, kind, 0.25)
    return ep


# ------------------------------------------------------------------ other effects
FACTS = [
    ("my sister Jo's birthday is 14 March", r"14 march|march 14|14/03|03-14"),
    ("the car is booked in for its MOT on 2 December", r"2 dec|december 2|12-02"),
    ("my gym locker code is 3391", r"3391"),
    ("Alex is allergic to peanuts", r"peanut"),
    ("my passport expires in August 2029", r"2029"),
]
RULES = [
    ("never suggest recipes with mushrooms", r"mushroom"),
    ("give me weights in grams, never ounces", r"gram"),
    ("always round kcal to the nearest 10", r"round|nearest 10"),
]


def other_episode(i: int) -> dict:
    rng = random.Random(3000 + i)
    kind = ["note_fact", "note_rule", "write_file", "append_file", "reminder"][i % 5]
    j = i // 5
    ep = {
        "id": f"other-{i:03d}-{kind}",
        "kind": "other",
        "sub": kind,
        "setup": {},
        "prior": list(CHAT_PRIOR),
        "now": at(16, 20),
    }
    if kind == "note_fact":
        fact, pat = FACTS[j % len(FACTS)]
        ep["turns"] = [{"text": f"Remember that {fact}.", "at": ep["now"], "expect": [["note_has", {"value_re": pat}]]}]
        fam = "note"
    elif kind == "note_rule":
        rule, pat = RULES[j % len(RULES)]
        ep["turns"] = [{"text": f"From now on, {rule}.", "at": ep["now"], "expect": [["note_has", {"value_re": pat}]]}]
        fam = "note"
    elif kind == "write_file":
        items = [["eggs", "spinach", "feta"], ["oat milk", "lemons", "rice"], ["bin bags", "soap", "batteries"]][j % 3]
        ep["turns"] = [
            {
                "text": f"Save a new shopping list file with {', '.join(items)}.",
                "at": ep["now"],
                "expect": [
                    ["file_has", {"path_re": ".", "content_re": items[0]}],
                    ["file_has", {"path_re": ".", "content_re": items[2]}],
                ],
            }
        ]
        fam = "file"
    elif kind == "append_file":
        task = ["renew the parking permit", "book the vet", "return the library books"][j % 3]
        ep["setup"]["files"] = {"notes/todo.md": "- buy stamps\n- call the bank\n"}
        ep["turns"] = [
            {
                "text": f"Add '{task}' to my todo list.",
                "at": ep["now"],
                "expect": [
                    ["file_has", {"path_re": "^notes/todo.md$", "content_re": task.split()[-1]}],
                    ["file_has", {"path_re": "^notes/todo.md$", "content_re": "buy stamps"}],
                ],
            }
        ]
        fam = "file"
    else:
        hh, what = [("18:30", "call the dentist"), ("07:15", "take the bins out"), ("21:00", "message Jo")][j % 3]
        day = "2026-11-17" if hh > "16:20" else "2026-11-18"
        word = "tomorrow at" if day == "2026-11-18" else "at"
        ep["turns"] = [
            {
                "text": f"Remind me {word} {hh} to {what}.",
                "at": ep["now"],
                "expect": [["reminder", {"when": f"{day} {hh}", "text_re": what.split()[-1]}]],
            }
        ]
        fam = "schedule"
    ep["fail"] = _fail(rng, fam, 0.3)
    return ep


# ------------------------------------------------------------------ label warnings
def bar(i):
    p, c, pol, fat, fib = [(20, 22, 10, 7, 3), (21, 19, 8, 8, 5), (15, 24, 12, 6, 4), (20, 18, 9, 9, 2)][i % 4]
    kcal = round(4 * p + 4 * (c - pol) + 9 * fat + 2 * fib + 2.4 * pol)
    name = ["salted caramel protein bar", "peanut protein bar", "cookie dough protein bar", "chocolate crisp bar"][
        i % 4
    ]
    text = (
        f"Protein bar, {name}, label per bar (60 g): energy {kcal} kcal, fat {fat} g, carbohydrate {c} g of "
        f"which polyols {pol} g, fibre {fib} g, protein {p} g. Log it as my afternoon snack."
    )
    return name, kcal, text


def beer(i):
    ml, abv, kcal, carb, prot = [
        (500, 4.6, 205, 15, 2),
        (330, 5.0, 142, 10.5, 1.3),
        (440, 4.2, 172, 13, 1.8),
        (568, 4.0, 210, 17, 2.3),
    ][i % 4]
    name = ["lager", "pale ale", "pilsner", "bitter"][i % 4]
    text = (
        f"Logging a beer: {ml} ml {name}, {abv}% ABV. The can says {kcal} kcal, carbohydrate {carb} g, protein "
        f"{prot} g, fat 0 g."
    )
    return name, kcal, text


def warning_episode(i: int, warn: bool) -> dict:
    which = "bar" if i % 2 == 0 else "beer"
    name, kcal, text = (bar if which == "bar" else beer)(i // 2)
    ep = {
        "id": f"warn-{'W' if warn else 'N'}-{i:03d}-{which}",
        "kind": "warning",
        "sub": which,
        "warn_label": warn,
        "label_kcal": kcal,
        "label_item": name.split()[-1],
        "setup": {},
        "prior": list(CHAT_PRIOR),
        "now": at(15, 40),
        "fail": {},
    }
    ep["turns"] = [
        {
            "text": text,
            "at": ep["now"],
            "expect": [["ledger_has", {"item_re": name.split()[-1], "kcal": [kcal, kcal], "count": 1}]],
        }
    ]
    return ep


# ------------------------------------------------------------------ recall
NOTE_FACTS = [
    ("sister-birthday", "Jo's birthday is 14 March", "When is my sister's birthday again?", r"14|march"),
    ("bike-lock", "combination 4471", "What's the code for my bicycle padlock?", r"4471"),
    ("dentist", "Dr Okafor at Kirkstall Dental, recall every 6 months", "What's my dentist called?", r"okafor"),
    (
        "boiler-service",
        "boiler last serviced 3 October 2026 by HeatRight",
        "When did the boiler last get serviced?",
        r"3 oct|october 3|heatright",
    ),
    ("shoe-size", "UK 9 in running shoes, 8.5 in boots", "What size running shoes do I take?", r"\b9\b"),
    ("alex-coffee", "Alex drinks oat flat whites, no sugar", "How does Alex take their coffee?", r"oat"),
    ("wifi-guest", "guest network Pebble-Guest, password on the fridge", "What's the guest wifi called?", r"pebble"),
    ("blood-type", "O negative", "Do you know my blood group?", r"o\s*neg|o-|o negative"),
    ("car-reg", "car registration YK21 ABC", "What's my number plate?", r"yk21"),
    ("landlord", "landlord is Priya Shah, 0113 496 0001", "Who's my landlord?", r"priya|shah"),
    ("allergy", "mild hay fever, cetirizine works", "Which antihistamine works for me?", r"cetirizine"),
    ("pension", "workplace pension with Hargrove, 6% matched", "Who is my pension with?", r"hargrove"),
    ("vet", "cat Miso sees Woodhouse Vets", "Where does Miso go to the vet?", r"woodhouse"),
    ("half-pb", "half marathon best 1:47:12 at Leeds 2025", "What's my half marathon PB?", r"1:47"),
    (
        "insurance",
        "home insurance renews 30 January with Clearsure",
        "When does the home insurance renew?",
        r"30 jan|january 30",
    ),
    ("locker", "work locker 112 on floor 3", "Which locker is mine at work?", r"112"),
    ("mum-phone", "mum's new number 07700 900456", "What's my mum's new phone number?", r"900456|900 456"),
    ("gift-alex", "Alex wants a ceramics class voucher", "What did Alex say they'd like as a present?", r"ceramic"),
    ("bins", "green bin every other Thursday", "When does the garden waste get collected?", r"thursday"),
    ("plant", "monstera gets watered every 9 days", "How often should the big leafy plant be watered?", r"\b9\b"),
]
HISTORY_FACTS = [
    (
        "The plumber who fixed the boiler was Dave Pritchard, he was great.",
        "What was the plumber's name, the one who did the boiler?",
        r"pritchard|dave",
    ),
    (
        "Booked the Lake District cottage for 12 to 15 February, it's called Fellside Barn.",
        "What was the cottage we booked in the Lakes called?",
        r"fellside",
    ),
    (
        "Parked on level 4, bay C17 at the station today.",
        "Remind me which bay I parked in at the station that time?",
        r"c17",
    ),
    ("Jo recommended the book The Salt Line, I should read it.", "What book did my sister recommend?", r"salt line"),
    ("My new desk at work is 3.14 by the window.", "What's my desk number at the office?", r"3\.14"),
    (
        "Tried the Ethiopian place Habesha on Call Lane, loved the injera.",
        "What was that Ethiopian restaurant called?",
        r"habesha",
    ),
    (
        "Ordered the replacement filter, part number HF-220.",
        "What was the part number of the filter I ordered?",
        r"hf-?220",
    ),
    ("Alex's work Christmas party is on 11 December.", "When is Alex's work Christmas do?", r"11 dec|december 11"),
    ("The quote for the new boiler was 2,450 pounds from HeatRight.", "How much was the new boiler quote?", r"2,?450"),
    ("Signed up for the Harrogate 10k on 8 March.", "Which 10k did I sign up for?", r"harrogate"),
]


def recall_episode(i: int, recall_line: bool) -> dict:
    ep = {
        "id": f"recall-{'R' if recall_line else 'X'}-{i:03d}",
        "kind": "recall",
        "recall_line": recall_line,
        "setup": {
            "unpinned": {k: v for k, v, _, _ in NOTE_FACTS},
            "history": [
                (at(19, 5, 26 - n % 4).replace(month=10), "owner", text) for n, (text, _, _) in enumerate(HISTORY_FACTS)
            ],
        },
        "prior": list(CHAT_PRIOR),
        "now": at(17, 30),
        "fail": {},
    }
    if i < len(NOTE_FACTS):
        _, _, q, pat = NOTE_FACTS[i]
        ep["sub"] = "notes"
    else:
        _, q, pat = HISTORY_FACTS[i - len(NOTE_FACTS)]
        ep["sub"] = "history"
    ep["turns"] = [{"text": q, "at": ep["now"], "expect": [["reply", {"re": pat}]]}]
    return ep


N_RECALL = len(NOTE_FACTS) + len(HISTORY_FACTS)


def plan(n_ledger=100, n_dispatch=60, n_other=40, n_warn=20, n_recall=N_RECALL) -> list:
    """(variant, episode, recall_line) for one head, interleaved so a spend stop keeps the arms balanced."""
    blocks = []
    for i in range(max(n_ledger, n_dispatch, n_other, n_warn, n_recall)):
        if i < n_ledger:
            e = ledger_episode(i)
            blocks += [("L1", e, True), ("L3", e, True)]
        if i < n_dispatch:
            e = dispatch_episode(i)
            blocks += [("L1", e, True), ("P", e, True)]
        if i < n_other:
            blocks.append(("L1", other_episode(i), True))
        if i < n_warn:
            blocks += [("L1", warning_episode(i, True), True), ("L1", warning_episode(i, False), True)]
        if i < n_recall:
            blocks += [("L1", recall_episode(i, True), True), ("L1", recall_episode(i, False), False)]
    return blocks
