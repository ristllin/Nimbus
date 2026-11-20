"""The v5.2.0 round trip on the bench unit (lane RB-R1; rulings 41, 42 item 7 and 87).

Up, down to released v5.1.1 and v5.1.2, ten owner turns on the old images, up again; nothing is lost:

  * every file the turns wrote is there byte for byte after the candidate is back (what the old image itself
    stored is the reference, read back over the download route before the next flash);
  * the table: every row the candidate seeded and every row an old image appended is byte-identical, in order
    (ruling 42: "ledger id" reads "untouched rows byte-identical"; "a meal" is a row added to a table);
  * the standing rules: the rule each old image's turn added is in the candidate's /prefs list, and the rule the
    candidate seeded before the trip is still there;
  * attribution: each owner line the old images answered is found again in the candidate's stored conversation
    (GET /api/mem/episodic?text=<code word>) with role "user", the owner's line.

The flashes are app-only and MAC-verified (tests/hil/test_files_bench.py's Unit; never OTA, never another port);
sound 0 and voice replies off are set and read back after every flash. The old images are released v5.1.1 and
v5.1.2 esp32s3-cyd builds; the candidate is its test-cyd build (the tool route is a NIMBUS_TEST route).

Paid: ten web turns on claude-haiku-5-5 (the unit's head is set to it for the trip and put back after).

    NIMBUS_PORT=<port> NIMBUS_BENCH_MAC=<mac> NIMBUS_TEST_IP=<ip> NIMBUS_TEST_TOKEN=<token> \
    NIMBUS_ROLLBACK_IMAGES=<dir with candidate.bin, v5.1.1.bin, v5.1.2.bin> \
        pytest tests/hil/test_roundtrip_v52.py -m hil --allow-hardware

A leg that cannot run FAILS with "BLOCKED: ...": the row never passes by a skip. The pure checks run on the host.
"""

from __future__ import annotations

import os
import time
import urllib.parse

import pytest

import bench_driver as bd
from test_files_bench import EVIDENCE, Unit, record, sha8

PROJECT = "rt52"
TABLE = "table.csv"
HEADER = b"date,item,qty,note\n"
OLD = ("v5.1.1", "v5.1.2")
# Two rules that cannot conflict, so no image has a reason to merge or replace the other one (on the first bench run
# v5.1.2's head rightly replaced v5.1.1's "sign replies with <word>" rule with its own, since the two contradicted).
RULES = {
    "v5.1.1": "Make this a standing rule: in answers about the bench, give lengths in metres (tag v511rule).",
    "v5.1.2": "Make this a standing rule: in answers about the bench, write dates as YYYY-MM-DD (tag v512rule).",
}


# ---- the pure checks (host) ---------------------------------------------------------------------------------


def turns_for(image: str) -> list:
    """The five owner turns one old image answers: a row added to the table, a file write, a rule, two plain
    lines with a code word each. Each carries a code word that is unique to the image and the turn."""
    tag = image.replace(".", "")
    row = f"2026-11-20,{tag} row,1,added on {image}"
    return [
        ("row", f"Append exactly this one line to the file {PROJECT}/{TABLE}, nothing else: {row}", row),
        (
            "file",
            f"Create the file {PROJECT}/note-{tag}.txt containing exactly this one line and nothing else: "
            f"written on {image} code {tag}-file",
            f"note-{tag}.txt",
        ),
        (
            "rule",
            RULES[image],
            f"{tag}rule",
        ),
        (
            "line",
            f"Just so you know, the code word for my locker on {image} is {tag}locker. No action needed.",
            f"{tag}locker",
        ),
        (
            "line",
            f"One more thing to keep in mind: my bike is parked at stand {tag}bike. A short ok is enough.",
            f"{tag}bike",
        ),
    ]


def rule_lines(prefs: str) -> list:
    """The rule lines of a /prefs answer, without the header (it names the version) or the footer."""
    return [ln for ln in prefs.splitlines() if ln.startswith("(p")]


def rows_kept(before: bytes, after: bytes) -> bool:
    """Every line of `before` is in `after` at the same place, byte for byte; `after` only adds lines at the end."""
    return after.startswith(before)


def test_ten_turns_and_the_classes_they_cover():
    turns = [t for img in OLD for t in turns_for(img)]
    assert len(turns) == 10
    kinds = {k for k, _, _ in turns}
    assert {"row", "file", "rule", "line"} <= kinds
    words = [w for _, _, w in turns]
    assert len(set(words)) == len(words), "every turn's code word is unique"


def test_rule_lines_ignore_the_version_header():
    a = "Nimbus · v5.1.2: Standing rules (2):\n(p1) one [added today]\n(p2) two\nAdd: /prefs add <rule>."
    b = "Nimbus · v5.2.0: Standing rules (2):\n(p1) one [added today]\n(p2) two\nAdd: /prefs add <rule>."
    assert rule_lines(a) == rule_lines(b) == ["(p1) one [added today]", "(p2) two"]
    assert rule_lines(a) != rule_lines(b.replace("(p2) two\n", ""))


def test_rows_kept_is_byte_exact():
    a = HEADER + b"2026-11-20,x,1,a\n"
    assert rows_kept(a, a + b"2026-11-20,y,1,b\n")
    assert not rows_kept(a, HEADER + b"2026-11-20,x,1,A\n")
    assert not rows_kept(a, HEADER)
    assert not rows_kept(a, b"2026-11-20,y,1,b\n" + a)


# ---- the leg (hardware) -------------------------------------------------------------------------------------


@pytest.fixture()
def unit(allow_hardware):
    port, mac = os.environ.get("NIMBUS_PORT", ""), os.environ.get("NIMBUS_BENCH_MAC", "")
    ip, token = os.environ.get("NIMBUS_TEST_IP", ""), os.environ.get("NIMBUS_TEST_TOKEN", "")
    if not (port and mac and ip and token):
        pytest.fail("BLOCKED: set NIMBUS_PORT, NIMBUS_BENCH_MAC, NIMBUS_TEST_IP and NIMBUS_TEST_TOKEN")
    import port_guard

    port_guard.check_mac(mac, "test_roundtrip_v52")
    u = Unit(port, mac, bd.Api(ip, token))
    if u.api.tool("files.list", {"project": PROJECT}).code == 404:
        pytest.fail("BLOCKED: this image has no /api/test/astool (flash the candidate's test-cyd image)")
    yield u
    u.detach()


def _file(api: bd.Api, name: str) -> bytes:
    for _ in range(24):  # a unit busy with its boot answers 503 for a while
        r = api.download(PROJECT, name)
        if r.code not in (0, 503):
            break
        time.sleep(5)
    record("roundtrip.log", f"download {name}: HTTP {r.code}")
    return r.body if r.code == 200 else b""


def _turn(api: bd.Api, text: str) -> bd.TurnResult:
    for _ in range(12):
        turn = bd.chat_turn(api, text, 180)
        if turn.http != 503 and "503" not in turn.error:
            return turn
        time.sleep(10)
    return turn


def _command(api: bd.Api, text: str) -> str:
    """An owner command over the web chat (no model call)."""
    return _turn(api, text).reply


class Trip:
    """What the old images wrote, as each stored it: the table, the files, the rules and the owner's code words."""

    def __init__(self, table: bytes):
        self.table = table
        self.files: dict = {}
        self.rules: list = []
        self.prefs = ""
        self.lines: list = []


def _seed(api: bd.Api) -> Trip:
    """On the candidate: the table and one standing rule."""
    seed = HEADER + b"".join(f"2026-11-1{i},seed {i},{i},candidate\n".encode() for i in range(1, 7))
    assert api.upload(PROJECT, TABLE, seed).code == 200
    assert _file(api, TABLE) == seed
    rule = _turn(api, "Make this a standing rule: always answer bench questions in English. (rt52seed)")
    prefs = _command(api, "/prefs")
    record(
        "roundtrip.log",
        f"candidate seeded {TABLE} {len(seed)} B sha8 {sha8(seed)}; rule ok={rule.ok}; /prefs {prefs[:600]!r}",
    )
    assert "English" in prefs, f"the seeded rule is not in /prefs: {prefs[:300]!r}"
    return Trip(seed)


def _old_turn(api: bd.Api, old: str, kind: str, want: str, trip: Trip) -> None:
    if kind == "row":
        now = _file(api, TABLE)
        assert rows_kept(trip.table, now), f"{old}: the append rewrote a row"
        assert want.encode() in now, f"{old}: the row did not land"
        trip.table = now
    elif kind == "file":
        body = _file(api, want)
        assert body, f"{old}: {want} was not written"
        trip.files[want] = body
    elif kind == "rule":
        prefs = _command(api, "/prefs")
        record("roundtrip.log", f"{old} /prefs: {prefs[:600]!r}")
        assert want in prefs, f"{old}: the rule is not in /prefs"
        trip.rules.append(want)
        trip.prefs = prefs
    else:
        trip.lines.append(want)


def _old_image(unit: Unit, image: str, old: str, trip: Trip) -> None:
    """Flash one released image, check the table came through with no write, answer its five turns."""
    unit.flash(image, old)
    unit.rezero(old, console=False)  # a released image has no test console; the API sets and reads back
    assert _file(unit.api, TABLE) == trip.table, f"{old}: the table changed with no write"
    for kind, text, want in turns_for(old):
        turn = _turn(unit.api, text)
        record("roundtrip.log", f"{old} {kind}: ok={turn.ok} {turn.seconds:.1f}s reply {turn.reply[:200]!r}")
        assert turn.ok, f"{old} {kind}: {turn.error}"
        _old_turn(unit.api, old, kind, want, trip)
    record(
        "roundtrip.log", f"{old} done: table {len(trip.table)} B sha8 {sha8(trip.table)}; files {sorted(trip.files)}"
    )


def _owner_line_kept(api: bd.Api, word: str) -> None:
    got = api.get_json("/api/mem/episodic?" + urllib.parse.urlencode({"text": word, "limit": 20}), 30.0)
    rows = [m for m in got.get("messages") or [] if word in (m.get("text") or "")]
    record("roundtrip.log", f"candidate episodic {word}: {[(m.get('id'), m.get('role'), m.get('tags')) for m in rows]}")
    assert rows, f"the owner's line with {word} is not in the stored conversation"
    assert any(m.get("role") == "user" for m in rows), f"the line with {word} is not stored as the owner's"


def _up_again(unit: Unit, image: str, trip: Trip) -> None:
    """The candidate again: every file, rule and owner line the old images left is there, byte for byte."""
    api = unit.api
    unit.flash(image, "candidate")
    unit.attach()
    unit.rezero("candidate again")
    unit.detach()
    now = _file(api, TABLE)
    record("roundtrip.log", f"candidate again: table {len(now)} B sha8 {sha8(now)} (was {sha8(trip.table)})")
    assert now == trip.table, "the candidate changed the table with no write"
    for name, body in trip.files.items():
        back = _file(api, name)
        record("roundtrip.log", f"candidate again: {name} {len(back)} B sha8 {sha8(back)} (was {sha8(body)})")
        assert back == body, f"{name} changed across the trip"
    prefs = _command(api, "/prefs")
    record("roundtrip.log", f"candidate /prefs after: {prefs[:800]!r}")
    assert "English" in prefs, "the rule seeded before the trip is gone"
    assert rule_lines(prefs) == rule_lines(trip.prefs), "the standing rules changed across the update"
    for w in trip.rules:
        assert w in prefs, f"the rule {w} added on an old image is gone"
    for w in trip.lines:
        _owner_line_kept(api, w)
    # One paid turn on the candidate: it reads the old images' rows like any other.
    last = _turn(api, f"How many rows below the header does {PROJECT}/{TABLE} have now? Reply with the number only.")
    record("roundtrip.log", f"candidate turn: ok={last.ok} reply {last.reply[:200]!r}")
    assert last.ok, last.error
    n = trip.table.count(b"\n") - 1
    assert str(n) in last.reply, f"the candidate counted the table wrong: {last.reply[:120]!r} (want {n})"


@pytest.mark.hil
def test_round_trip_through_v511_and_v512_loses_nothing(request, unit):
    images = request.config.getoption("--rollback-images", default=None) or os.environ.get("NIMBUS_ROLLBACK_IMAGES")
    if not images:
        pytest.fail("BLOCKED: no --rollback-images DIR (candidate.bin, v5.1.1.bin, v5.1.2.bin)")
    imgs = {k: os.path.join(images, f"{k}.bin") for k in ("candidate", *OLD)}
    missing = [p for p in imgs.values() if not os.path.exists(p)]
    assert not missing, f"BLOCKED: missing images {missing}"
    head_before = unit.api.get_json("/api/orch").get("orchHost") or "zai"
    unit.api.post_form("/api/orch", {"orchHost": "anthropic", "orchM_anthropic": "claude-haiku-5-5"})
    try:
        trip = _seed(unit.api)
        for old in OLD:
            _old_image(unit, imgs[old], old, trip)
        _up_again(unit, imgs["candidate"], trip)
    finally:
        unit.api.post_form("/api/orch", {"orchHost": head_before})
        record("roundtrip.log", f"head put back to {head_before}; evidence {EVIDENCE}")
