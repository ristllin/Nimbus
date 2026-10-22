"""Host checks for the turn record bench legs' parsers (no device).

The bench leg B2 once failed with the turn finished and its row stored, because the
EPIQ helper read only the summary line and the row preview is cut at 48 characters.
These pin the row reader (summary, then the counted rows, log lines skipped, never
reading past the last row), the reason a 48-character preview gives when the text is
past it, and the B1 storage and DREAM failure reasons.
"""

from __future__ import annotations

import pytest

from test_turn_record_bench import (
    EPIQ_PREVIEW_CHARS,
    NOT_MOUNTED,
    dream_loop,
    dream_never_fired_reason,
    drill_moved_loop,
    drill_status_reason,
    epiq_rows_from_lines,
    find_row,
    storage_down_reason,
)

pytestmark = pytest.mark.host

FULL_ROW = (
    '{"host":"anthropic","model":"claude-haiku-5-5","src":"serial","tools":0,"ok":true,"in":1200,"out":40,"reply":2}'
)
PREVIEW = FULL_ROW[:EPIQ_PREVIEW_CHARS]
MOUNT_ERR = f"[274822][E][vfs_api.cpp:28] open(): {NOT_MOUNTED}"


def test_full_rows_are_read_after_the_summary_with_log_lines_skipped():
    lines = [
        "EPIQ rows=2 ms=7 files=0 bytes=0 older=0 next=-",
        f"m00000142 {FULL_ROW}",
        MOUNT_ERR,
        "ORCH alive sta=1 ip=192.168.50.62 heap=44644",
        "m00000141 Turn FAILED (dream) on anthropic: invalid unicode code point",
    ]
    rows = epiq_rows_from_lines(lines)
    assert rows == [
        ("m00000142", FULL_ROW),
        ("m00000141", "Turn FAILED (dream) on anthropic: invalid unicode code point"),
    ]
    assert find_row(rows, '"src":"serial"') == FULL_ROW


def test_the_reader_stops_at_the_counted_rows():
    def source():
        yield "EPIQ rows=1 ms=7 files=0 bytes=0 older=0 next=-"
        yield f"m00000142 {FULL_ROW}"
        raise AssertionError("read past the last counted row")

    assert epiq_rows_from_lines(source()) == [("m00000142", FULL_ROW)]


def test_no_rows_and_a_missing_summary():
    assert epiq_rows_from_lines(["EPIQ rows=0 ms=3 files=0 bytes=0 older=0 next=-"]) == []
    with pytest.raises(AssertionError, match="no 'EPIQ rows=' summary"):
        epiq_rows_from_lines(["ORCH alive sta=1"])


def test_a_truncated_preview_names_why_the_text_is_unreachable():
    rows = epiq_rows_from_lines(["EPIQ rows=1 ms=7", f"m00000142 {PREVIEW}"])
    assert rows == [("m00000142", PREVIEW)]
    with pytest.raises(AssertionError, match="48-character previews"):
        find_row(rows, '"src":"serial"')
    # Counter-test: a short full row that lacks the text is a plain miss.
    with pytest.raises(AssertionError) as e:
        find_row([("m1", '{"host":"openai"}')], '"src":"serial"')
    assert "preview" not in str(e.value)


def test_storage_down_needs_a_burst():
    assert storage_down_reason([MOUNT_ERR, "ORCH alive", MOUNT_ERR]) is None
    why = storage_down_reason([MOUNT_ERR] * 3)
    assert why and "not mounted" in why and "3" in why


def test_dream_timeout_reason_names_the_cause_and_the_tail():
    tail = [f"line {i}" for i in range(30)] + [MOUNT_ERR] * 4
    why = dream_never_fired_reason(tail, 240)
    assert "DREAM did not answer within 240 s" in why
    assert "internal storage is not mounted" in why
    assert why.count("\n    ") == 20  # exactly the last 20 lines
    plain = dream_never_fired_reason(["ORCH alive sta=1"], 240)
    assert "no dream started" in plain and "not mounted" not in plain
    started = dream_never_fired_reason(["dream: maintenance - decayed, pruned=0"], 240)
    assert "no dream started" not in started
    # Old errors early in the session do not count once the log has moved on.
    stale = [MOUNT_ERR] * 5 + ["ORCH alive sta=1"] * 60
    assert "not mounted" not in dream_never_fired_reason(stale, 240)


# ---- B1 dream surfaces ---------------------------------------------------------------
# The record B1 read on unit L (22 Oct 2026): the 21 Oct scheduled fire, then a boot
# reconcile that marked it failed. The drill had moved nothing, and the leg read it as
# the drill's own record.
LOOP = {
    "id": "dream",
    "name": "Dream",
    "lastRun": 1792553405,
    "lastResult": "fail",
    "firesToday": 1,
    "tokensToday": 3254,
    "consecFails": 2,
    "nextRun": 1792639800,
}


def test_the_dream_routine_is_found_by_id_in_either_answer_shape():
    other = {"id": "lp1", "name": "Daydream digest"}
    assert dream_loop([other, LOOP]) == LOOP
    assert dream_loop({"loops": [other, LOOP]}) == LOOP
    assert dream_loop([other]) is None


def test_a_drill_that_leaves_the_routine_record_alone_passes():
    after = dict(LOOP, nextRun=LOOP["nextRun"] + 60)  # nextRun is the scheduler's, not a fire field
    assert drill_moved_loop(LOOP, after) is None


@pytest.mark.parametrize(
    "field,value",
    [
        ("lastRun", 1792612402),
        ("lastResult", "ok"),
        ("consecFails", 3),
        ("firesToday", 2),
        ("tokensToday", 4000),
        ("lastErr", "anthropic: messages HTTP 400"),
    ],
)
def test_a_drill_that_moves_the_routine_record_fails(field, value):
    why = drill_moved_loop(LOOP, dict(LOOP, **{field: value}))
    assert why and field in why


def test_a_missing_dream_routine_fails():
    assert "missing" in drill_moved_loop(None, LOOP)
    assert "missing" in drill_moved_loop(LOOP, None)


def test_the_dream_status_must_report_the_failed_drill():
    ok = {"known": True, "last": "failed", "cause": "provider-4xx", "err": "anthropic: messages HTTP 400: The request"}
    assert drill_status_reason(ok) is None
    assert "no dream since boot" in drill_status_reason({"known": False})
    assert "want 'failed'" in drill_status_reason(dict(ok, last="ok"))
    assert "empty" in drill_status_reason(dict(ok, err=""))
