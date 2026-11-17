#!/usr/bin/env python3
"""fixtures - synthetic photos and files for the scenarios.

Everything is generated here, deterministically, from made-up content: no owner
conversation text and no owner photo is ever used. A scenario names a fixture; the
runner asks this module for its bytes, and the judge gets its `truth` line.
"""

from __future__ import annotations

import io
import random

from PIL import Image, ImageDraw, ImageFont


def _card(lines: list, size=(900, 640), bg=(250, 249, 244), ink=(25, 25, 30), font_px: int = 34) -> bytes:
    """A plain card with large printed lines: a receipt, a meter display, a label."""
    img = Image.new("RGB", size, bg)
    draw = ImageDraw.Draw(img)
    font = ImageFont.load_default(size=font_px)
    y = 36
    for line in lines:
        draw.text((48, y), line, fill=ink, font=font)
        y += int(font_px * 1.55)
    draw.rectangle([12, 12, size[0] - 12, size[1] - 12], outline=(120, 120, 120), width=3)
    out = io.BytesIO()
    img.save(out, format="PNG", optimize=True)
    return out.getvalue()


RECEIPT_LINES = [
    "GREENLEAF MARKET",
    "14 Nov 2026   18:42",
    "Cashier: Tomasz",
    "Sourdough loaf          3.40",
    "Oat milk 1L             1.95",
    "Cherry tomatoes         2.25",
    "Dark chocolate 85%      2.80",
    "TOTAL                  10.40",
    "Paid: card ending 4417",
]

METER_WEEKS = [("Week 1", "4 Oct", "18,204"), ("Week 2", "11 Oct", "18,251"),
               ("Week 3", "18 Oct", "18,317"), ("Week 4", "25 Oct", "18,362")]


def _meter(i: int) -> bytes:
    week, date, reading = METER_WEEKS[i]
    return _card(["ELECTRICITY METER", f"{week}  -  {date} 2026", "", f"  {reading} kWh", "", "Serial MX-20417"],
                 bg=(232, 238, 240), font_px=44)


PHOTOS = {
    "receipt": {
        "make": lambda: _card(RECEIPT_LINES),
        "truth": "A shop receipt: " + "; ".join(RECEIPT_LINES),
    },
    **{
        f"meter{i + 1}": {
            "make": (lambda i=i: _meter(i)),
            "truth": f"Electricity meter photo, {METER_WEEKS[i][0]} ({METER_WEEKS[i][1]} 2026): {METER_WEEKS[i][2]} kWh",
        }
        for i in range(4)
    },
}


def photo(name: str) -> bytes:
    return PHOTOS[name]["make"]()


def photo_truth(name: str) -> str:
    return PHOTOS[name]["truth"]


# ----------------------------------------------------------------- text files
_TOPICS = ["compost bays", "water butts", "the polytunnel", "seed swaps", "the bee corner", "shared wheelbarrows",
           "path edging", "the notice board", "bonfire days", "the tool shed", "fruit cages", "the pond",
           "children's plots", "winter mulching", "hedge trimming", "the car park", "plot inspections", "open days"]
_NAMES = ["Agnes", "Bilal", "Carys", "Dmitri", "Esme", "Farid", "Greta", "Hamza", "Ingrid", "Jonas", "Kirsty", "Lev"]
_DAYS = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]
_MONTHS = ["January", "February", "April", "May", "June", "July", "August", "September", "October", "November"]


def _filler(rng: random.Random, n: int) -> str:
    topic, who, day = rng.choice(_TOPICS), rng.choice(_NAMES), rng.choice(_DAYS)
    month, num, hour = rng.choice(_MONTHS), rng.randint(2, 29), rng.randint(8, 18)
    return (f"Section {n}. On {topic}. Members who use {topic} should sign the sheet kept by {who}, who looks "
            f"after this area on {day}s from {hour}:00. No more than {num} people may use it at one time, and "
            f"anything left out after dusk is tidied away by the committee. Problems go to {who} first, then "
            f"to the secretary. This rule was last reviewed in {month} and applies to plots {num} to "
            f"{num + rng.randint(3, 40)} as well as the shared beds.")


def handbook(target_bytes: int = 40_000) -> str:
    """A made-up members' handbook of about 40 KB with two details buried in the middle."""
    rng = random.Random(20261117)
    out = ["HARBOUR LANE COMMUNITY GARDEN - MEMBERS' HANDBOOK (2026 edition)", ""]
    n = 0
    while sum(len(s) + 1 for s in out) < target_bytes:
        n += 1
        if n == 23:
            out.append("Section 23. Fees. The annual fee for a full plot is 64 pounds and for a half plot it is "
                       "38 pounds, due by the last Friday of January. A raised bed for members with limited "
                       "mobility is 15 pounds. Fees unpaid after six weeks mean the plot is offered to the "
                       "waiting list.")
        elif n == 57:
            out.append("Section 57. Tool shed access. The combination for the tool shed padlock is 5-1-8-3. "
                       "It changes on the first Monday of March each year, and the new combination is given out "
                       "at the spring meeting only, never by text or email.")
        else:
            out.append(_filler(rng, n))
        out.append("")
    return "\n".join(out)


# The long-haul scenario: twelve 40 KB minute books, each with a gate code near the top and the
# treasurer's balance on its last line. Reading them all takes a conversation past 150k tokens.
VOLUMES = 12
_STREETS = ["Alder", "Birch", "Cedar", "Dove", "Elm", "Fern", "Gorse", "Hazel", "Iris", "Juniper", "Kestrel", "Larch"]


def volume_facts(i: int) -> tuple:
    """(site name, gate code, treasurer's balance in pounds) of minute book i (1-based)."""
    rng = random.Random(7000 + i)
    return f"{_STREETS[i - 1]} Road allotments", f"{rng.randint(1000, 9999)}", rng.randint(200, 1900)


def volume(i: int, target_bytes: int = 40_000) -> str:
    site, code, balance = volume_facts(i)
    rng = random.Random(9000 + i)
    out = [f"{site.upper()} - COMMITTEE MINUTE BOOK, VOLUME {i}", ""]
    n = 0
    while sum(len(s) + 1 for s in out) < target_bytes:
        n += 1
        if n == 3:
            out.append(f"Section 3. Gate. The code for the main gate at {site} is {code}. It is told to plot "
                       f"holders in person and never written on the notice board.")
        else:
            out.append(_filler(rng, n))
        out.append("")
    out.append(f"Closing note from the treasurer: the balance carried forward for {site} is {balance} pounds.")
    return "\n".join(out)


INVENTORY = """item,qty,shelf
cordless drill,1,A1
drill bit set,2,A1
spirit level,1,A2
claw hammer,3,A2
torque wrench,2,C3
socket set,1,C3
jump leads,1,C4
tyre inflator,1,C4
wood glue,4,B1
sandpaper packs,6,B1
paint roller,2,B2
dust sheets,5,B2
"""

TEXTS = {
    "handbook": {"make": handbook, "truth": "A 40 KB handbook. Section 23: half plot fee is 38 pounds (full plot 64). "
                 "Section 57: tool shed padlock combination 5-1-8-3, changes on the first Monday of March."},
    "inventory": {"make": lambda: INVENTORY, "truth": "A 12-item garage inventory CSV; torque wrench: qty 2, shelf C3."},
    **{
        f"volume{i:02d}": {"make": (lambda i=i: volume(i)),
                           "truth": "A 40 KB minute book of {}: gate code {}, balance {} pounds.".format(*volume_facts(i))}
        for i in range(1, VOLUMES + 1)
    },
}


def text(name: str) -> str:
    return TEXTS[name]["make"]()


def text_truth(name: str) -> str:
    return TEXTS[name]["truth"]


if __name__ == "__main__":
    for name in PHOTOS:
        print(name, len(photo(name)), "bytes")
    for name in TEXTS:
        print(name, len(text(name).encode()), "bytes")
