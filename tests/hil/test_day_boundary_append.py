"""The day boundary under plain appends, on hardware (the 24-25 Oct 2026 replay, generalized).

On 24-25 Oct 2026 the owner's assistant added Friday's dinner into Saturday's total, then took
Sunday's breakfast for Saturday's dinner. This leg replays the shape on a bench unit set to
Europe/London with real head calls (Anthropic, ``NIMBUS_DBA_MODEL``, default claude-haiku-5-5),
on three tables with identical expectations (owner ruling 41: a food log, an expenses log and a
training log are equal validation families; ``NIMBUS_DBA_TABLES`` narrows the run, for example
``food``):

1. the table is seeded (upload) with two Saturday rows;
2. 21:10 BST Saturday: a row;
3. 00:20 BST Sunday (23:20 UTC Saturday): a row - with the device's day start at 04:00 (RB-REC5,
   the setting day_starts, pinned for the leg by NIMBUS_DBA_DAY_START) the day still running is
   Saturday, so the row is Saturday's; with 00:00 it is Sunday's;
4. 08:20 GMT Sunday (BST ended at 01:00 UTC): a row;
5. a correction to that row;
6. both days' totals asked for.

Graded (day_append_grade.py) from the file the device wrote (``/api/files/dl``), its copy after
every turn and the final reply: the seeded rows untouched, every logging turn a pure append
(each earlier byte kept, rows added: files.append's log line, never a whole-file re-save), one
row per entry on its running day, the correction leaving one row, both totals exact in the file
and in the reply. The files before and after each turn, the replies and the log lines go to
``NIMBUS_DBA_EVIDENCE`` (a directory; default: no files written).

The device clock is placed with the console's CLOCK verb before each turn; the time zone
(``/api/config devTz``), the head model, the other providers' keys and the spend count are set
for the leg and restored after it (the storm kit). Markers: hil, net, agent. Needs
--allow-hardware, NIMBUS_PORT, NIMBUS_TEST_IP, NIMBUS_TEST_TOKEN, an SD card, and
ANTHROPIC_API_KEY on the unit (the bench .env restores the other keys).
"""

from __future__ import annotations

import contextlib
import json
import os
import re
import sys
import time

import pytest

import day_append_grade as dg

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "storm"))

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

LONDON = "GMT0BST,M3.5.0/1,M10.5.0"
MODEL = os.environ.get("NIMBUS_DBA_MODEL", "claude-haiku-5-5")
EVIDENCE = os.environ.get("NIMBUS_DBA_EVIDENCE", "")
TABLES = [d for d in os.environ.get("NIMBUS_DBA_TABLES", ",".join(sorted(dg.TABLES))).split(",") if d]
DAY_START = os.environ.get("NIMBUS_DBA_DAY_START", dg.DEFAULT_START)


def turns(t: dg.Table):
    """(UTC epoch, the owner's message, a pure append turn?) for one table."""
    where, i, col = f"{t.project}/{t.name}", t.items, t.col
    return [
        (dg.WHEN["evening"], f"Add a row to {where}: {i['evening']}, qty 1, {col} 990.", True),
        (dg.WHEN["night"], f"Add a row to {where}: {i['night']}, qty 1, {col} 35.", True),
        (dg.WHEN["morning"], f"Add a row to {where}: {i['morning']}, qty 1, {col} 945.", True),
        (
            1792917000,
            f"Correction in {where}: this morning's {i['morning']} row is {col} 910, not 945. Fix that row.",
            False,
        ),
        (
            1792917300,
            f"From {where}: what is the {col} total for yesterday (24 Oct) and for today? Give both numbers.",
            False,
        ),
    ]


def _save(name: str, text: str) -> None:
    if EVIDENCE:
        os.makedirs(EVIDENCE, exist_ok=True)
        with open(os.path.join(EVIDENCE, name), "w", encoding="utf-8") as f:
            f.write(text)


def _dump(net, t: dg.Table) -> str:
    r = net.get(f"/api/files/dl?project={t.project}&name={t.name}", timeout=15.0)
    return r.text if r.status_code == 200 else ""


def _seed(net, t: dg.Table) -> None:
    import requests

    net.post("/api/files/rm", {"project": t.project, "name": t.name}, timeout=10.0)  # 404 when absent
    r = requests.post(
        f"http://{net.ip}/api/files/upload?project={t.project}&name={t.name}",
        files={"file": (t.name, dg.seed(t).encode("utf-8"), "text/csv")},
        headers=net._auth(),
        timeout=30,
    )
    assert r.status_code == 200, f"seed upload: HTTP {r.status_code} {r.text[:120]}"
    assert _dump(net, t) == dg.seed(t), "the seeded table did not read back byte for byte"


@contextlib.contextmanager
def _london(net):
    """The unit on Europe/London for the leg; its own zone put back after."""
    before = str((net.get_json("/api/state", timeout=8.0).get("clock") or {}).get("tz") or "")
    r = net.post("/api/config", {"devTz": LONDON}, timeout=10.0)
    assert r.status_code == 200, f"devTz: HTTP {r.status_code}"
    time.sleep(3.0)  # the zone is applied on the poll task's next loop
    assert (net.get_json("/api/state", timeout=8.0).get("clock") or {}).get("tz") == LONDON, "devTz did not land"
    try:
        yield
    finally:
        net.post("/api/config", {"devTz": before}, timeout=10.0)


@contextlib.contextmanager
def _day_start(net, value: str):
    """The device's day start pinned for the leg (RB-REC5's day_starts); its own value put back after."""
    before = str((net.get_json("/api/state", timeout=8.0).get("clock") or {}).get("day_starts") or "")
    r = net.post("/api/config", {"day_starts": value}, timeout=10.0)
    assert r.status_code == 200, f"day_starts: HTTP {r.status_code}"
    try:
        yield
    finally:
        net.post("/api/config", {"day_starts": before}, timeout=10.0)


@contextlib.contextmanager
def _anthropic_key(bench):
    """The Anthropic key on the unit for the leg (pushed from the bench .env when the
    unit has none, and cleared again after)."""
    import kit

    has = ((bench.orch().get("providers") or {}).get("anthropic") or {}).get("hasKey")
    field, envname = kit.KEY_FIELDS["anthropic"]
    if not has:
        assert bench.env.get(envname), f"no {envname} in the bench .env (NIMBUS_ENV_FILE)"
        bench.post_orch({field: bench.env[envname]})
    try:
        yield
    finally:
        if not has:
            bench.post_orch({"clr_" + field: "1"}, ok=(200, 507))


@contextlib.contextmanager
def _clock(bench):
    """The leg moves the device clock; it goes back to the wall clock however the leg ends."""
    try:
        yield
    finally:
        bench.verb(f"CLOCK {int(time.time())}", "CLOCK set", timeout=6.0)


def _turn(bench, net, at: int, text: str):
    """One web-chat turn with the clock placed at `at`; returns (reply, log)."""
    from webrig import LogFollower, chat_turn, make_session

    bench.verb(f"CLOCK {at}", "CLOCK set", timeout=6.0)
    s = make_session(str(net.token()))
    with LogFollower(s, net.ip) as follow:
        reply = chat_turn(s, net.ip, text, timeout_s=240)
    return reply, follow.text


def _run_turns(bench, net, t: dg.Table, record: dict):
    """The five turns; returns the last reply and the (before, after) file of every append turn."""
    where = f"{t.project}/{t.name}"
    appended = re.compile(r"files: appended \d+ row\(s\) to " + re.escape(where) + r" \(")
    reply, appends = "", []
    for i, (at, text, is_append) in enumerate(turns(t), 1):
        before = _dump(net, t)
        reply, log = _turn(bench, net, at, text)
        after = _dump(net, t)
        _save(f"{t.project}.after-turn{i}.csv", after)
        _save(f"{t.project}.log-turn{i}.txt", log)
        resaved = f"files: artifact {where}" in log
        spend = re.findall(r"turn spend: ok \S+ \| (\S+) in=(\d+) out=(\d+) cR=(\d+) cW=(\d+)", log)
        record["turns"].append({"at": at, "text": text, "reply": reply, "resaved": resaved, "spend": spend})
        print(f"DBA {t.project} turn {i} resaved={resaved} spend={spend} reply={reply[:160]!r}")
        assert not resaved, f"turn {i}: {where} was re-saved whole"
        if is_append:
            assert appended.search(log), f"turn {i}: no files.append on {where}"
            appends.append((before, after))
    return reply, appends


@pytest.mark.parametrize("domain", TABLES)
def test_day_boundary_append(device, net, domain):
    import kit

    t = dg.TABLES[domain]
    kit.MODEL = MODEL
    bench = kit.StormBench(device, net, leg="dba")
    record = {"model": MODEL, "build": bench.build, "table": domain, "day_starts": DAY_START, "turns": []}
    with (
        bench.only_provider("anthropic"),
        _anthropic_key(bench),
        bench.spend_room("anthropic"),
        bench.head_model(),
        _london(net),
        _day_start(net, DAY_START),
        _clock(bench),
    ):
        _seed(net, t)
        _save(f"{t.project}.before.csv", _dump(net, t))
        reply, appends = _run_turns(bench, net, t, record)
    after = _dump(net, t)
    checks = dg.grade(t, after, reply, appends, DAY_START)
    record.update({"verdict": dg.verdict(checks), "checks": [c._asdict() for c in checks]})
    _save(f"{t.project}.after.csv", after)
    _save(f"{t.project}.record.json", json.dumps(record, indent=2))
    print(f"DBA {domain} grade:\n" + dg.table(checks))
    assert dg.verdict(checks) == "PASS", dg.table(checks)
