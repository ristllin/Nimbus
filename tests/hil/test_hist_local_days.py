"""HIST - the conversation history's day markers across London's clock change, on hardware.

The history the head reads opens each local day with a marker like ``(Sun 25 Oct)``. A
marker used to come from the row's stored hour and the offset in force NOW, so a clock
change (or a half-hour zone) put rows on the wrong day. Now each turn is told from the
second the device stored its owner row (the ``at:`` tag) and the offset in force then.

This leg places the device clock with the console's CLOCK verb across the end of British
Summer Time on 25 Oct 2026 (01:00 UTC), with the zone set to Europe/London
(``/api/config devTz``), sends five web-chat turns and one probe turn on a cheap head
(``NIMBUS_HIST_MODEL``, default claude-haiku-5-5), and reads the history out of the
probe turn's prompt (``/api/lastturn``):

  1. Sat 24 Oct 22:30 UTC = 23:30 BST   -> opens (Sat 24 Oct)
  2. Sat 24 Oct 23:30 UTC = 00:30 BST   -> opens (Sun 25 Oct); the offset now (GMT)
                                           would have kept it on Saturday
  3. Sun 25 Oct 01:30 UTC = 01:30 GMT   -> no marker (still Sunday)
  4. Sun 25 Oct 23:30 UTC = 23:30 GMT   -> no marker
  5. Mon 26 Oct 00:10 UTC = 00:10 GMT   -> opens (Mon 26 Oct)

It also reads the five owner rows back (``/api/mem/episodic``) and checks each carries the
``at:`` stamp of the second it was stored. The zone, the head model, the other providers'
keys, the spend count and the clock are restored after the leg, and the web conversation
is cleared so the rows stored at the placed times stay out of the live history.

Markers: hil, net, agent. Needs --allow-hardware, NIMBUS_PORT, NIMBUS_TEST_IP,
NIMBUS_TEST_TOKEN, an SD card and ANTHROPIC_API_KEY on the unit (the bench .env restores
the other keys). About six haiku turns (a few cents).
"""

from __future__ import annotations

import calendar
import os
import re
import sys
import time
from typing import List, Optional, Tuple

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "storm"))

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

MODEL = os.environ.get("NIMBUS_HIST_MODEL", "claude-haiku-5-5")
RUN = f"H{int(time.time()) % 100000}"
DAY = r"\((?:Mon|Tue|Wed|Thu|Fri|Sat|Sun) \d{1,2} (?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)\)"
NO_MARKER = "no marker"


def utc(day: int, hour: int, minute: int, month: int = 10, year: int = 2026) -> int:
    return calendar.timegm((year, month, day, hour, minute, 0))


# (UTC epoch the clock is placed at, the marker that opens the turn in the history)
TURNS: List[Tuple[int, str]] = [
    (utc(24, 22, 30), "Sat 24 Oct"),  # 23:30 BST
    (utc(24, 23, 30), "Sun 25 Oct"),  # 00:30 BST
    (utc(25, 1, 30), NO_MARKER),  # 01:30 GMT, after BST ended at 01:00 UTC
    (utc(25, 23, 30), NO_MARKER),  # 23:30 GMT
    (utc(26, 0, 10), "Mon 26 Oct"),  # 00:10 GMT
]
PROBE_AT = utc(26, 0, 20)


def mark(i: int) -> str:
    return f"HISTMARK{i} {RUN}"


def marker_before(history: str, text: str) -> Optional[str]:
    """The day marker that directly opens `text` in a rendered history ("Sun 25 Oct"),
    NO_MARKER when the text opens without one, None when the text is not in the history.
    Reads the block form ("(Sun 25 Oct)\\n- user: text") and the message form
    ("(Sun 25 Oct) text")."""
    at = history.find(text)
    if at < 0:
        return None
    head = history[:at].rstrip()
    if head.endswith("- user:"):
        head = head[: -len("- user:")].rstrip()
    found = re.search(DAY + r"$", head)
    return found.group(0)[1:-1] if found else NO_MARKER


def stamps_ok(tags: str, clock_at: int, window_s: int = 600) -> bool:
    """The row carries an at: stamp within `window_s` seconds after the clock was placed."""
    found = re.search(r"(?:^|,)at:(\d+)(?:,|$)", tags or "")
    return bool(found) and 0 <= int(found.group(1)) - clock_at <= window_s


def _turn(bench, net, at: int, text: str) -> str:
    from webrig import chat_turn, make_session

    bench.verb(f"CLOCK {at}", "CLOCK set", timeout=6.0)
    return chat_turn(make_session(str(net.token())), net.ip, text, timeout_s=240)


def _owner_rows(net) -> dict:
    page = net.get_json("/api/mem/episodic?session=web&limit=80", timeout=10.0)
    return {m.get("text", ""): m for m in page.get("messages", []) if m.get("role") == "user"}


def test_history_days_across_the_london_clock_change(device, net):
    import kit
    from test_l52_history_cache import _history
    from test_qw3_food_log_midnight import _anthropic_key, _clock, _london

    kit.MODEL = MODEL
    bench = kit.StormBench(device, net, leg="hist")
    with (
        bench.only_provider("anthropic"),
        _anthropic_key(bench),
        bench.spend_room("anthropic"),
        bench.head_model(),
        _london(net),
        _clock(bench),
    ):
        try:
            for i, (at, _) in enumerate(TURNS, 1):
                reply = _turn(bench, net, at, f"{mark(i)}: reply with just OK.")
                assert reply, f"turn {i} got no reply"
            _turn(bench, net, PROBE_AT, f"{RUN}: say OK.")
            dump = net.get("/api/lastturn", timeout=15.0).text
            rows = _owner_rows(net)
        finally:
            bench.post_orch({"convReset": "1"}, ok=(200, 507))
    hist = _history(dump)
    seen = [(i, marker_before(hist, mark(i)), want) for i, (_, want) in enumerate(TURNS, 1)]
    print("HIST day markers (turn, seen, wanted):", seen)
    wrong = [s for s in seen if s[1] != s[2]]
    assert not wrong, f"day markers wrong (turn, seen, wanted): {wrong}\n{hist[-2500:]}"
    for i, (at, _) in enumerate(TURNS, 1):
        row = next((m for t, m in rows.items() if mark(i) in t), None)
        assert row is not None, f"turn {i}: no owner row in the web chat"
        assert stamps_ok(row.get("tags", ""), at), f"turn {i}: no at: stamp near {at}: {row.get('tags')!r}"
