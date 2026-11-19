"""Day boundary - last night's turns are still in the request just after midnight (v5.1.2).

Bench row B09 and owner-regression row OIR-54 (lc-day-boundary), on hardware. Up to
v5.1.1 the owner's day switched at local midnight and the turns before it fell to the
small recent-history share (5,242 B at the Mistral table, 8,192 B at Claude's, then down to
half of it, the newest two turns always kept): a 23:30 estimate followed by three late turns
was gone from the 00:05 request. From v5.1.2 nothing moves at midnight (lane H): the history
grows to its soft mark whatever the day.

The leg places the device clock with the console's CLOCK verb on Europe/London (GMT in
November), sends the estimate turn at 23:30, three long late turns at 23:40, 23:50 and 23:55,
then the question at 00:05 the next day, and reads the history out of that turn's prompt
(/api/lastturn). Pass: the 23:30 owner row (its run marker) is in the 00:05 request. The
reply is not graded here (a head may search the log; the live row lc-day-boundary grades
both). The zone, the head model, the other providers' keys, the spend count and the clock
are restored after the leg, and the web conversation is cleared.

Markers: hil, net, agent. Needs --allow-hardware, NIMBUS_PORT, NIMBUS_TEST_IP,
NIMBUS_TEST_TOKEN, an SD card and ANTHROPIC_API_KEY on the unit (the bench .env restores
the other keys). Five claude-haiku-5-5 turns (a few cents). The dedicated bench board only.
"""

from __future__ import annotations

import calendar
import os
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "storm"))

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

MODEL = os.environ.get("NIMBUS_DAYB_MODEL", "claude-haiku-5-5")
RUN = f"DB{int(time.time()) % 100000}"
ESTIMATE = f"ESTIMATE-{RUN}"

# Long enough that the three late turns alone pass half of Claude's 8,192 B pre-today share.
NOTES = (
    "Notes for tomorrow so I don't forget: the plumber comes between 9 and 11, the parcel for "
    "the neighbour is in the hall, the bins go out on Tuesday, the car needs air in the front "
    "tyres, my sister's birthday card has to be posted by Thursday, and I want to try the "
    "new running route by the river on Sunday morning before the shops open. "
)


def at_london(year: int, month: int, day: int, hour: int, minute: int) -> int:
    """A November wall time in London (GMT, no offset) as a UTC epoch."""
    return calendar.timegm((year, month, day, hour, minute, 0))


EVENING = [
    (at_london(2026, 11, 20, 23, 30), f"{ESTIMATE}: late snack, a bowl of blueberries with skyr, about a cereal "
                                      "bowl. Estimate it in one line, don't log it."),
    (at_london(2026, 11, 20, 23, 40), "Reply with just OK. " + NOTES * 3),
    (at_london(2026, 11, 20, 23, 50), "Reply with just OK. " + NOTES * 3),
    (at_london(2026, 11, 20, 23, 55), "Reply with just OK. Good night. " + NOTES * 2),
]  # fmt: skip
QUESTION_AT = at_london(2026, 11, 21, 0, 5)
QUESTION = f"{RUN}: what did you estimate for the blueberries last night? One line."


def _turn(bench, net, at: int, text: str) -> str:
    from webrig import chat_turn, make_session

    bench.verb(f"CLOCK {at}", "CLOCK set", timeout=6.0)
    return chat_turn(make_session(str(net.token())), net.ip, text, timeout_s=240)


def test_last_nights_estimate_is_in_the_request_after_midnight(device, net):
    import kit
    from test_l52_history_cache import _history
    from test_day_boundary_append import _anthropic_key, _clock, _london

    kit.MODEL = MODEL
    bench = kit.StormBench(device, net, leg="dayb")
    with (
        bench.only_provider("anthropic"),
        _anthropic_key(bench),
        bench.spend_room("anthropic"),
        bench.head_model(),
        _london(net),
        _clock(bench),
    ):
        try:
            for at, text in EVENING:
                assert _turn(bench, net, at, text), f"no reply to the turn at {at}"
            assert _turn(bench, net, QUESTION_AT, QUESTION), "no reply to the 00:05 question"
            dump = net.get("/api/lastturn", timeout=15.0).text
        finally:
            bench.post_orch({"convReset": "1"}, ok=(200, 507))
    hist = _history(dump)
    assert RUN in dump, "the 00:05 turn's prompt is not the one read back"
    assert ESTIMATE in hist, (
        "the 23:30 estimate turn is missing from the 00:05 request: the history moved at midnight\n" + hist[-2500:]
    )
