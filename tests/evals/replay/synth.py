#!/usr/bin/env python3
"""synth - synthetic owner turns with the numeric shape of a real day (shapes/*.json).

A shape holds only numbers: when each owner turn started, how many messages and photos it had,
how many bytes of text, how many tool calls the device made. This module writes made-up text of
about that size, generated photos, and a deterministic kind per turn chosen from the shape (a
photo turn is a photographed meal or a label, a turn with several tool calls is a log or a
correction, a turn with none is a chat or a question). The first logging turn plants exact
quantities and the last turn asks for them back, as on 30 Oct. Nothing here comes from an owner.
"""

from __future__ import annotations

import base64
import hashlib
import io
import json
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
SHAPES = HERE / "shapes"

MEALS = [
    ("breakfast", "2 eggs (110 g), 1 slice sourdough (45 g) and 8 g butter"),
    ("lunch", "chicken breast 160 g, basmati rice 180 g cooked, 70 g broccoli"),
    ("dinner", "salmon fillet 140 g, new potatoes 220 g, 60 g green beans"),
    ("snack", "a banana (120 g) and 25 g almonds"),
    ("lunch", "lentil soup 350 g and a 60 g wholemeal roll"),
    ("dinner", "beef chilli 300 g with 150 g cooked rice, no beans"),
    ("breakfast", "porridge: 50 g oats, 200 ml semi-skimmed milk, 15 g honey"),
    ("snack", "greek yoghurt 170 g with 30 g blueberries"),
]
PLANTED = ("breakfast", "135 g scrambled eggs, 35 g smoked salmon and 1 slice rye (32 g), no beans")
PLANTED_CHECK = r"135\s?g.*35\s?g|35\s?g.*135\s?g"
CORRECTIONS = [
    "Correction: the rice at lunch was 150 g, not 180 g. Fix that row.",
    "I didn't have the butter this morning after all, remove it.",
    "The potatoes were 180 g, change it please.",
    "Move the almonds to yesterday, I ate them last night.",
]
CHATS = [
    "Thanks. Any idea for a quick stretch for tight hamstrings?",
    "Ok cool. What should I keep in mind before a long run tomorrow?",
    "Nice. Remind me what a sensible protein spread across the day looks like?",
    "Ha, fair enough. How long should I rest between strength sessions?",
]
RESEARCH = [
    "Can you look up how much caffeine is in a double espresso versus a filter coffee?",
    "Search the web for the opening hours of public swimming pools in Leeds on Sundays.",
    "Find out whether creatine needs a loading phase, quick summary please.",
]
NOTES = [
    "Remember that I'm off dairy on weekdays from now on.",
    "Note for later: my gym sessions are Monday, Wednesday and Friday mornings.",
]
FILLER = [
    "Busy day at work today.",
    "Weather is grim but I walked anyway.",
    "Slept ok, about seven hours.",
    "Feeling good after the run.",
    "Bit hungrier than usual today.",
    "Back from the office now.",
]
LABELS = [
    (
        "PROTEIN BAR 60 g",
        ["Energy 214 kcal", "Protein 20.1 g", "Carbohydrate 21.4 g", "of which polyols 9.8 g", "Fat 6.9 g"],
    ),
    ("GREEK YOGHURT 170 g", ["Energy 158 kcal", "Protein 15.3 g", "Carbohydrate 6.1 g", "Fat 8.2 g"]),
    ("OAT DRINK 250 ml", ["Energy 115 kcal", "Protein 2.5 g", "Carbohydrate 16.3 g", "Fat 3.8 g"]),
    ("TUNA SALAD 220 g", ["Energy 296 kcal", "Protein 21.6 g", "Carbohydrate 14.0 g", "Fat 16.5 g"]),
]
PLATES = [
    ["PLATE NOTE", "grilled chicken thigh ~150 g", "roast potatoes ~200 g", "peas ~60 g"],
    ["PLATE NOTE", "pasta bolognese ~350 g", "parmesan ~10 g"],
    ["PLATE NOTE", "omelette 3 eggs", "cheddar ~25 g", "toast 1 slice"],
    ["PLATE NOTE", "stir fry tofu ~180 g", "noodles ~200 g cooked"],
]


def photo(kind: str, i: int) -> tuple:
    """(media type, base64, ref8): a 600 x 800 JPEG like a phone photo: a label card or a plate with a note."""
    img = Image.new("RGB", (600, 800), (238, 233, 224) if kind == "plate" else (250, 249, 244))
    d = ImageDraw.Draw(img)
    font = ImageFont.load_default(size=34)
    rng = random.Random(f"{kind}{i}")
    if kind == "plate":
        d.ellipse([70, 330, 530, 760], fill=(250, 250, 250), outline=(180, 180, 180), width=6)
        for _ in range(6):
            x, y = rng.randint(140, 400), rng.randint(400, 640)
            d.ellipse(
                [x, y, x + rng.randint(60, 120), y + rng.randint(40, 90)],
                fill=tuple(rng.randint(60, 200) for _ in range(3)),
            )
        lines = PLATES[i % len(PLATES)]
    else:
        title, rows = LABELS[i % len(LABELS)]
        lines = [title, "NUTRITION per pack", *rows]
    y = 40
    for line in lines:
        d.text((40, y), line, fill=(25, 25, 30), font=font)
        y += 52
    out = io.BytesIO()
    img.save(out, format="JPEG", quality=80)
    data = out.getvalue()
    # The ref names this photo, not its bytes: two sends of the same label are two photos.
    return "image/jpeg", base64.b64encode(data).decode(), hashlib.sha1(data + f"{kind}{i}".encode()).hexdigest()[:8]


def _pad(text: str, target: int, rng: random.Random) -> str:
    while len(text.encode()) + 20 < target:
        text = f"{rng.choice(FILLER)} {text}"
    return text


def load_shape(name: str) -> dict:
    return json.loads((SHAPES / f"{name}.json").read_text())


def turns_for(shape: dict, seed: str) -> list:
    """[{at_s, kind, text, photos: [(kind, i)], check}] for every owner turn of the shape."""
    rng = random.Random(seed)
    turns = shape["turns"]
    out, meal_i, cyc = [], 0, 0
    planted = False
    photo_i = 0
    for n, t in enumerate(turns):
        last = n == len(turns) - 1
        tools = len(t["tools"])
        check = None
        photos = []
        if last:
            kind, text, check = (
                "recall",
                "What exactly did I say I had for breakfast, with the weights?",
                ("reply", PLANTED_CHECK),
            )
        elif t["photos"]:
            kind = "photo"
            k = "label" if photo_i % 2 else "plate"
            photos = [(k, photo_i + j) for j in range(min(t["photos"], 4))]
            photo_i += len(photos)
            text = "Log this please." if k == "plate" else "Just had this, log it from the label."
            check = ("rows_added", 1)
        elif not planted and tools:
            kind, planted = "planted", True
            text = f"For {PLANTED[0]} I had {PLANTED[1]}. Log it."
            check = ("rows_added", 2)
        elif tools >= 2:
            kind = ["log", "correction", "total", "research", "log", "note"][cyc % 6]
            cyc += 1
            if kind == "log":
                meal, what = MEALS[meal_i % len(MEALS)]
                meal_i += 1
                text, check = f"{meal.capitalize()}: {what}. Log it.", ("rows_added", 1)
            elif kind == "correction":
                text = CORRECTIONS[cyc % len(CORRECTIONS)]
            elif kind == "total":
                text, check = "What's my total for today so far?", ("total", None)
            elif kind == "research":
                text = RESEARCH[cyc % len(RESEARCH)]
            else:
                text = NOTES[cyc % len(NOTES)]
        elif tools == 1:
            kind, text, check = "total", "How many calories have I got left today?", ("total", None)
        else:
            kind, text = "chat", CHATS[n % len(CHATS)]
        out.append(
            {
                "at_s": t["at_s"],
                "kind": kind,
                "text": _pad(text, t["text_bytes"], rng),
                "photos": photos,
                "check": check,
                "device_tools": tools,
            }
        )
    return out


AGED_DAY_TIMES = ("07:40", "08:15", "10:30", "12:45", "13:20", "15:10", "18:05", "19:30", "20:40", "22:10")


def prior_history(start: float, target_bytes: int, tz) -> list:
    """[(t, owner text, reply text)] of made-up earlier days, newest last, about target_bytes in all:
    ten food-and-chat turns a day going back from the day before start."""
    from datetime import datetime, timedelta

    out, size, day = [], 0, 1
    base = datetime.fromtimestamp(start, tz).replace(hour=0, minute=0, second=0, microsecond=0)
    while size < target_bytes:
        d = base - timedelta(days=day)
        for k, hhmm in enumerate(AGED_DAY_TIMES):
            h, m = map(int, hhmm.split(":"))
            t = d.replace(hour=h, minute=m)
            meal, what = MEALS[(day + k) % len(MEALS)]
            owner = t.strftime("[%a %d %b %Y %H:%M] ") + (
                f"{meal.capitalize()}: {what}. Log it." if k % 3 != 2 else CHATS[k % len(CHATS)]
            )
            reply = (
                f"Logged {meal}: {what}. That is about {300 + 37 * k} kcal and {12 + k} g protein; "
                f"the day is now at {900 + 210 * k} kcal with {1400 - 210 * k} kcal left."
                if k % 3 != 2
                else "Gentle hamstring stretches after a warm-up, held 30 seconds, two or three times, work well."
            )
            out.append((t.timestamp(), owner, reply))
            size += len(owner.encode()) + len(reply.encode())
            if size >= target_bytes:
                break
        day += 1
    return sorted(out)


def prior_digests(start: float, days: int, tz) -> list:
    """[(day label, digest)] of made-up older days, oldest first."""
    from datetime import datetime, timedelta

    out = []
    base = datetime.fromtimestamp(start, tz)
    for k in range(days, 0, -1):
        d = (base - timedelta(days=10 + k)).strftime("%a %d %b %Y")
        out.append(
            (
                d,
                f"Logged breakfast, lunch, dinner and two snacks, about {1900 + 23 * k} kcal and {120 + k} g protein. "
                "Asked about stretching and a long run; agreed an easy pace. Noted the gym moved to Wednesday this week. "
                "Nothing left open.",
            )
        )
    return out
