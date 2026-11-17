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

METER_WEEKS = [
    ("Week 1", "4 Oct", "18,204"),
    ("Week 2", "11 Oct", "18,251"),
    ("Week 3", "18 Oct", "18,317"),
    ("Week 4", "25 Oct", "18,362"),
]


def _meter(i: int) -> bytes:
    week, date, reading = METER_WEEKS[i]
    return _card(
        ["ELECTRICITY METER", f"{week}  -  {date} 2026", "", f"  {reading} kWh", "", "Serial MX-20417"],
        bg=(232, 238, 240),
        font_px=44,
    )


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
_TOPICS = [
    "compost bays",
    "water butts",
    "the polytunnel",
    "seed swaps",
    "the bee corner",
    "shared wheelbarrows",
    "path edging",
    "the notice board",
    "bonfire days",
    "the tool shed",
    "fruit cages",
    "the pond",
    "children's plots",
    "winter mulching",
    "hedge trimming",
    "the car park",
    "plot inspections",
    "open days",
]
_NAMES = ["Agnes", "Bilal", "Carys", "Dmitri", "Esme", "Farid", "Greta", "Hamza", "Ingrid", "Jonas", "Kirsty", "Lev"]
_DAYS = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]
_MONTHS = ["January", "February", "April", "May", "June", "July", "August", "September", "October", "November"]


def _filler(rng: random.Random, n: int) -> str:
    topic, who, day = rng.choice(_TOPICS), rng.choice(_NAMES), rng.choice(_DAYS)
    month, num, hour = rng.choice(_MONTHS), rng.randint(2, 29), rng.randint(8, 18)
    return (
        f"Section {n}. On {topic}. Members who use {topic} should sign the sheet kept by {who}, who looks "
        f"after this area on {day}s from {hour}:00. No more than {num} people may use it at one time, and "
        f"anything left out after dusk is tidied away by the committee. Problems go to {who} first, then "
        f"to the secretary. This rule was last reviewed in {month} and applies to plots {num} to "
        f"{num + rng.randint(3, 40)} as well as the shared beds."
    )


def handbook(target_bytes: int = 40_000) -> str:
    """A made-up members' handbook of about 40 KB with two details buried in the middle."""
    rng = random.Random(20261117)
    out = ["HARBOUR LANE COMMUNITY GARDEN - MEMBERS' HANDBOOK (2026 edition)", ""]
    n = 0
    while sum(len(s) + 1 for s in out) < target_bytes:
        n += 1
        if n == 23:
            out.append(
                "Section 23. Fees. The annual fee for a full plot is 64 pounds and for a half plot it is "
                "38 pounds, due by the last Friday of January. A raised bed for members with limited "
                "mobility is 15 pounds. Fees unpaid after six weeks mean the plot is offered to the "
                "waiting list."
            )
        elif n == 57:
            out.append(
                "Section 57. Tool shed access. The combination for the tool shed padlock is 5-1-8-3. "
                "It changes on the first Monday of March each year, and the new combination is given out "
                "at the spring meeting only, never by text or email."
            )
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
            out.append(
                f"Section 3. Gate. The code for the main gate at {site} is {code}. It is told to plot "
                f"holders in person and never written on the notice board."
            )
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

# The long-chat scenario: twenty-two travel journal entries of about 2.5 KB each, dictated one per
# turn. Together they pass the harness's 48 KB fold trigger; each stays under its 6 KB row clip.
JOURNAL_DAYS = 22
_TOWNS = ["Porto", "Aveiro", "Coimbra", "Nazare", "Obidos", "Sintra", "Lisbon", "Setubal", "Evora", "Beja", "Lagos"]
_TOWNS += ["Sagres", "Faro", "Tavira", "Mertola", "Elvas", "Marvao", "Tomar", "Viseu", "Guarda", "Braga", "Viana"]
_PEOPLE = ["Ines", "Rui", "Marta", "Tiago", "Sofia", "Duarte", "Leonor", "Nuno", "Beatriz", "Goncalo", "Carla"]
_PEOPLE += ["Vasco", "Helena", "Jorge", "Rita", "Paulo", "Teresa", "Miguel", "Clara", "Andre", "Filipa", "Hugo"]
_DISHES = ["grilled sardines", "octopus rice", "caldo verde", "custard tarts", "clam stew", "black pork", "salt cod"]
_DISHES += ["almond cake", "goat stew", "fig jam on toast", "razor clams", "chestnut soup", "orange sponge"]
_SIGHTS = ["the old cathedral", "the fish market", "a tiled chapel", "the river walk", "the castle walls"]
_SIGHTS += ["a cork workshop", "the lighthouse", "a hillside vineyard", "the covered market", "the Roman bridge"]
_WEATHER = ["bright and windy", "hot and still", "grey with a little drizzle", "cool and clear", "hazy and warm"]


def journal_facts(day: int) -> dict:
    """The checkable facts of one journal day: where, who, what they recommended, and the day's costs."""
    rng = random.Random(4100 + day)
    costs = {"room": rng.randint(38, 96), "food": rng.randint(14, 47), "transport": rng.randint(4, 33)}
    return {
        "town": _TOWNS[day - 1],
        "met": _PEOPLE[day - 1],
        "dish": rng.choice(_DISHES),
        "sight": rng.choice(_SIGHTS),
        "weather": rng.choice(_WEATHER),
        **costs,
        "total": sum(costs.values()),
        "woke": f"{rng.randint(6, 9)}:{rng.choice(['00', '15', '30', '45'])}",
        "steps": rng.randint(7, 24) * 1000,
    }


def journal(day: int) -> str:
    f = journal_facts(day)
    special = {
        1: "The flight out was fine. I was in seat 23F by the window and slept most of the way. ",
        2: "I took the small ferry across the estuary before lunch. The ferry ticket was 14 pounds 50, cash only. ",
    }.get(day, "")
    return (
        f"Journal, day {day}, {f['town']}. Woke at {f['woke']}. The weather was {f['weather']}. {special}"
        f"I spent the morning at {f['sight']} and walked far more than I meant to, about {f['steps']:,} steps by the "
        f"end of the day according to my watch. The streets here are steep and the pavements are those small "
        f"polished stones that look lovely and are slippery when wet, so I took it slowly. I stopped twice for "
        f"coffee and once just to sit on a wall and watch people go by. Around midday I got talking to {f['met']}, "
        f"who runs a small shop near the square and has lived in {f['town']} all their life. {f['met']} told me I "
        f"had to try the {f['dish']} while I was here and wrote the name of a place on the back of my map. I went "
        f"there in the evening and it was every bit as good as promised, a plain room with paper tablecloths and "
        f"a television in the corner that nobody was watching. In the afternoon I did some washing in the sink, "
        f"wrote two postcards, and tried to plan tomorrow without much success because the bus timetable on the "
        f"wall of the station does not match the one online. I asked at the desk and was told, kindly, that "
        f"neither is right and I should just turn up early. Money for the day: the room was {f['room']} pounds, "
        f"food came to {f['food']} pounds altogether, and transport was {f['transport']} pounds, which makes "
        f"{f['total']} pounds for day {day}. I am keeping an eye on that because the first week ran higher than "
        f"I planned. Small things I want to remember: the smell of woodsmoke in the lane behind the guest house, "
        f"a dog asleep in the doorway of the bakery, and the way the light came through the shutters at about "
        f"six in the evening. My feet hurt, but in the good way. I also noticed my left boot is rubbing at the "
        f"heel again, so I have put a plaster on it and will see how it is in the morning. Tomorrow I move on, "
        f"and I want to be up early enough to see the market before the bus. One more note to myself: I keep "
        f"meaning to ring home and keep forgetting until it is too late in the evening, so tomorrow I will do it "
        f"at lunch. I should also back up the photos from the camera, because the card is nearly full and I "
        f"would hate to lose the ones from {f['sight']}. The guest house has a shelf of books other travellers "
        f"left behind, and I swapped my finished paperback for a battered guide to the birds of the region, "
        f"which I will probably never open but could not resist. Dinner tonight is the last of the bread and "
        f"cheese from the market unless I find the energy to go back out. That is all for day {day}. "
        f"No need to comment on it, just say you have it."
    )


TEXTS = {
    "handbook": {
        "make": handbook,
        "truth": "A 40 KB handbook. Section 23: half plot fee is 38 pounds (full plot 64). "
        "Section 57: tool shed padlock combination 5-1-8-3, changes on the first Monday of March.",
    },
    "inventory": {
        "make": lambda: INVENTORY,
        "truth": "A 12-item garage inventory CSV; torque wrench: qty 2, shelf C3.",
    },
    **{
        f"journal{d:02d}": {"make": (lambda d=d: journal(d)), "truth": f"Journal day {d}: {journal_facts(d)}"}
        for d in range(1, JOURNAL_DAYS + 1)
    },
    **{
        f"volume{i:02d}": {
            "make": (lambda i=i: volume(i)),
            "truth": "A 40 KB minute book of {}: gate code {}, balance {} pounds.".format(*volume_facts(i)),
        }
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
