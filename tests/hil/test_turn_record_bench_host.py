"""Host checks for the turn record bench legs' parsers (no device).

The bench leg B2 once failed with the turn finished and its row stored, because the
EPIQ helper read only the summary line and the row preview is cut at 48 characters.
These pin the row reader (summary, then the counted rows, log lines skipped, never
reading past the last row) and the reason a 48-character preview gives when the text
is past it.
"""

from __future__ import annotations

import pytest

from test_turn_record_bench import (
    EPIQ_PREVIEW_CHARS,
    epiq_rows_from_lines,
    find_row,
)

pytestmark = pytest.mark.host

FULL_ROW = (
    '{"host":"anthropic","model":"claude-haiku-5-5","src":"serial","tools":0,"ok":true,"in":1200,"out":40,"reply":2}'
)
PREVIEW = FULL_ROW[:EPIQ_PREVIEW_CHARS]
MOUNT_ERR = "[274822][E][vfs_api.cpp:28] open(): File system is not mounted"


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
