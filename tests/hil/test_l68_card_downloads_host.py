"""Host tests for the L68 card-download parsers and verdicts (no board).

The device's log line is read from its printf format in src/net/web_admission.cpp, so
a reworded line fails here instead of passing a bench run vacuously.
"""

from __future__ import annotations

import io
import pathlib
import re
import zipfile

import pytest

from test_l68_web_stall import HOLD_MS
from test_l68_card_downloads import (
    ARC_FILES,
    MID_GAP_S,
    past_budget_verdict,
    WAITED_RE,
    body_verdict,
    errlog_verdict,
    payload,
    run_verdict,
    sha,
    waited_lines,
    waited_verdict,
    zip_verdict,
)

pytestmark = pytest.mark.host

ROOT = pathlib.Path(__file__).resolve().parents[2]


def _zip(members: dict) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        for name, data in members.items():
            z.writestr(name, data)
    return buf.getvalue()


def test_payload_is_deterministic_and_sized():
    a, b = payload(100_000, 7), payload(100_000, 7)
    assert a == b and len(a) == 100_000
    assert payload(100_000, 8) != a
    assert len(payload(33, 1)) == 33


def test_body_verdict_wants_every_byte():
    data = payload(5000, 1)
    ok, _ = body_verdict({"status": 200, "bytes": 5000, "sha": sha(data), "ms": 10}, sha(data), 5000)
    assert ok
    short = data[:4000]
    ok, why = body_verdict({"status": 200, "bytes": 4000, "sha": sha(short)}, sha(data), 5000)
    assert not ok and "4000" in why
    ok, why = body_verdict({"status": 200, "bytes": 1200, "error": "ChunkedEncodingError"}, sha(data), 5000)
    assert not ok and "ended early" in why
    ok, why = body_verdict({"status": 503, "bytes": 30, "sha": "x"}, sha(data), 5000)
    assert not ok and "503" in why


def test_zip_verdict_checks_members_and_crcs():
    files = {name: payload(n // 100, i) for i, (name, n) in enumerate(ARC_FILES)}
    members = {k: sha(v) for k, v in files.items()}
    whole = _zip(dict(files, **{"index.jsonl": b"{}\n"}))
    ok, why = zip_verdict({"status": 200, "ms": 5}, whole, members)
    assert ok, why
    ok, why = zip_verdict({"status": 200}, whole[: len(whole) // 2], members)
    assert not ok and "not a ZIP" in why
    first = next(iter(files))
    ok, why = zip_verdict({"status": 200}, _zip({first: files[first]}), members)
    assert not ok and "missing" in why
    other = _zip({k: (v[:-1] + b"x") for k, v in files.items()})
    ok, why = zip_verdict({"status": 200}, other, members)
    assert not ok and "differs" in why
    # A flipped data byte with the old CRC: the archive opens, the member fails it.
    raw = bytearray(whole)
    at = raw.find(files[first][:32])
    raw[at + 40] ^= 0xFF
    ok, why = zip_verdict({"status": 200}, bytes(raw), members)
    assert not ok and ("CRC" in why or "differs" in why)


def test_errlog_verdict_allows_growth_not_loss():
    ref = b"0 1 boot: n=4\n" * 400
    grown = ref + b"9 9 more\n" * 10
    ok, why = errlog_verdict({"status": 200, "fileBytes": str(len(ref) + 20), "ms": 3}, grown, ref)
    assert ok, why
    ok, why = errlog_verdict({"status": 200, "fileBytes": str(len(grown))}, grown[:-50], ref)
    assert not ok and "less than" in why
    ok, why = errlog_verdict({"status": 200, "fileBytes": str(len(ref))}, b"X" + ref[1:], ref)
    assert not ok and "start" in why
    # Rotated meanwhile (it opened smaller than the quiet read): only the size counts.
    rotated = b"1 1 new file\n" * 20
    ok, why = errlog_verdict({"status": 200, "fileBytes": str(len(rotated))}, rotated, ref)
    assert ok, why
    ok, why = errlog_verdict({"status": 200, "error": "ConnectionError", "bytes": 10}, b"x", ref)
    assert not ok and "ended early" in why


def test_the_device_line_matches_the_parser():
    src = (ROOT / "src/net/web_admission.cpp").read_text()
    m = re.search(r'"(web: a card download waited [^"]*)"', src)
    assert m, "the waited line is gone from web_admission.cpp"
    line = m.group(1).replace("%u", "{}").format(31250, 2, 1500000)
    got = waited_lines("noise\n" + line + "\nmore")
    assert got == [{"ms": 31250, "holds": 2, "bytes": 1500000}]
    assert WAITED_RE.search(line)
    assert 'gave up after waiting %u ms' in src


def test_run_verdict_names_each_failure():
    ok, why = run_verdict([("file", True, "whole"), ("zip", True, "ok")])
    assert ok and "2 checks" in why
    ok, why = run_verdict([("file", True, "whole"), ("zip", False, "member missing"), ("waited", False, "0")])
    assert not ok and "zip: member missing" in why and "waited: 0" in why


def test_waited_verdict_rejects_a_hold_that_missed_the_download():
    hold_s = HOLD_MS / 1000.0
    # Started 3 s into the hold: the head may come only after the rest of the hold.
    ok, why = waited_verdict("file", {"headMs": int((hold_s - 3) * 1000)}, 3.0)
    assert ok, why
    ok, why = waited_verdict("file", {"headMs": 400}, 3.0)
    assert not ok and "400" in why
    # The slow read: a pause of the hold's size mid-file, not a steady trickle.
    ok, why = waited_verdict("file-mid", {"gapMs": 24000}, None)
    assert ok, why
    ok, why = waited_verdict("file-mid", {"gapMs": int(MID_GAP_S * 1000) - 1}, None)
    assert not ok


def test_past_budget_verdict_wants_a_clear_failure():
    door = {"status": 503, "retryAfter": "5", "headMs": 60100}
    cut = {"error": "ChunkedEncodingError", "bytes": 300000}
    ok, why = past_budget_verdict(door, cut, 58.0, 1_500_000)
    assert ok, why
    ok, why = past_budget_verdict(dict(door, retryAfter=None), cut, 58.0, 1_500_000)
    assert not ok and "retryAfter" in why
    ok, why = past_budget_verdict(dict(door, headMs=900), cut, 58.0, 1_500_000)
    assert not ok and "before the budget" in why
    ok, why = past_budget_verdict(door, {"bytes": 300000}, 58.0, 1_500_000)
    assert not ok and "short" in why
    ok, why = past_budget_verdict(door, {"bytes": 1_500_000}, 58.0, 1_500_000)
    assert not ok and "not cut" in why


def test_seeded_archive_names_pass_the_device_rule():
    """The import route takes only restored names: "r", 16 lowercase hex digits, ".",
    a short lowercase extension (validRestoredName in lib/core/src/tg_archive_engine.cpp)."""
    rule = re.compile(r"^r[0-9a-f]{16}\.[a-z0-9]{2,5}$")
    for name, _ in ARC_FILES:
        assert rule.match(name), name
