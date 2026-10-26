"""QW3 - the food log across a London midnight on hardware (the 24-25 Oct 2026 replay).

On 24-25 Oct 2026 the owner's assistant added Friday's dinner into Saturday's total,
then took Sunday's breakfast for Saturday's dinner. This leg replays the shape on a
bench unit set to Europe/London, with real head calls (Anthropic, ``NIMBUS_QW3_MODEL``,
default claude-haiku-5-5):

1. the food log ``qw3-bench/food-log.csv`` is seeded (upload) with Saturday's breakfast
   and lunch;
2. 21:10 BST Saturday: dinner;
3. 00:20 BST Sunday (23:20 UTC Saturday): a clementine - the local day is already the 25th;
4. 08:20 GMT Sunday (BST ended at 01:00 UTC): breakfast;
5. a correction to that breakfast;
6. both days' totals asked for.

Graded (qw3_food_grade.py) from the file the device wrote (``/api/files/dl``) and the
final reply: the seeded rows untouched, one row per meal on its local day, the
correction replacing its row (counted once), both totals exact in the file and in the
reply. Every logging turn must run the keyed files.append (its log line), never a
whole-file re-save. The file before and after each turn, the replies and the log
lines go to ``NIMBUS_QW3_EVIDENCE`` (a directory; default: no files written).

The device clock is placed with the console's CLOCK verb before each turn; the time
zone (``/api/config devTz``), the head model, the other providers' keys and the spend
count are set for the leg and restored after it (the storm kit). Markers: hil, net,
agent. Needs --allow-hardware, NIMBUS_PORT, NIMBUS_TEST_IP, NIMBUS_TEST_TOKEN, an SD
card, and ANTHROPIC_API_KEY on the unit (the bench .env restores the other keys).
"""

from __future__ import annotations

import contextlib
import json
import os
import re
import sys
import time

import pytest

import qw3_food_grade as fg

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "storm"))

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

LONDON = "GMT0BST,M3.5.0/1,M10.5.0"
PROJECT, NAME = "qw3-bench", "food-log.csv"
MODEL = os.environ.get("NIMBUS_QW3_MODEL", "claude-haiku-5-5")
EVIDENCE = os.environ.get("NIMBUS_QW3_EVIDENCE", "")
WHERE = f"{PROJECT}/{NAME}"
# (UTC epoch, the owner's message, a logging turn?)
TURNS = [
    (
        1792872600,
        f"Log my dinner to {WHERE}: beef fillet 100 g, skyr with berries, chicken 90 g, roast "
        "eggplant, carrot, tofu and salsa - 990 kcal and 88 g protein in total.",
        True,
    ),
    (1792884000, f"I just had a clementine before bed: 35 kcal, 1 g protein. Log it to {WHERE}.", True),
    (
        1792916400,
        "Breakfast this morning: two small chicken breasts (225 g), 2 eggs, 200 g skyr, berries "
        f"and salad - 945 kcal and 107 g protein. Log it to {WHERE}.",
        True,
    ),
    (
        1792917000,
        "Correction to this morning's breakfast: it was 910 kcal and 103 g protein, not 945 "
        f"and 107. Fix it in {WHERE}.",
        True,
    ),
    (
        1792917300,
        f"From {WHERE}: what are my calorie totals for yesterday (24 Oct) and for today? Give both numbers.",
        False,
    ),
]
KEYED_LINE = re.compile(
    r"files: (?:appended \d+ row\(s\) to " + re.escape(WHERE) + r" \(keyed|upsert " + re.escape(WHERE) + ")"
)


def _save(name: str, text: str) -> None:
    if EVIDENCE:
        os.makedirs(EVIDENCE, exist_ok=True)
        with open(os.path.join(EVIDENCE, name), "w", encoding="utf-8") as f:
            f.write(text)


def _dump(net) -> str:
    r = net.get(f"/api/files/dl?project={PROJECT}&name={NAME}", timeout=15.0)
    return r.text if r.status_code == 200 else ""


def _seed(net) -> None:
    import requests

    net.post("/api/files/rm", {"project": PROJECT, "name": NAME}, timeout=10.0)  # 404 when absent
    r = requests.post(
        f"http://{net.ip}/api/files/upload?project={PROJECT}&name={NAME}",
        files={"file": (NAME, fg.SEED.encode("utf-8"), "text/csv")},
        headers=net._auth(),
        timeout=30,
    )
    assert r.status_code == 200, f"seed upload: HTTP {r.status_code} {r.text[:120]}"
    assert _dump(net) == fg.SEED, "the seeded food log did not read back byte for byte"


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


def _run_turns(bench, net, record: dict) -> str:
    """The five turns; returns the last reply."""
    reply = ""
    for i, (at, text, logs) in enumerate(TURNS, 1):
        reply, log = _turn(bench, net, at, text)
        _save(f"food-log.after-turn{i}.csv", _dump(net))
        _save(f"log-turn{i}.txt", log)
        keyed = bool(KEYED_LINE.search(log))
        resaved = f"files: artifact {WHERE}" in log
        spend = re.findall(r"turn spend: ok \S+ \| (\S+) in=(\d+) out=(\d+) cR=(\d+) cW=(\d+)", log)
        record["turns"].append(
            {"at": at, "text": text, "reply": reply, "keyed": keyed, "resaved": resaved, "spend": spend}
        )
        print(f"QW3 turn {i} keyed={keyed} resaved={resaved} spend={spend} reply={reply[:160]!r}")
        assert not resaved, f"turn {i}: the food log was re-saved whole"
        if logs:
            assert keyed, f"turn {i}: no keyed files.append on {WHERE}"
    return reply


def test_qw3_food_log_across_a_london_midnight(device, net):
    import kit

    kit.MODEL = MODEL
    bench = kit.StormBench(device, net, leg="qw3")
    record = {"model": MODEL, "build": bench.build, "turns": []}
    with (
        bench.only_provider("anthropic"),
        _anthropic_key(bench),
        bench.spend_room("anthropic"),
        bench.head_model(),
        _london(net),
        _clock(bench),
    ):
        _seed(net)
        _save("food-log.before.csv", _dump(net))
        reply = _run_turns(bench, net, record)
    after = _dump(net)
    checks = fg.grade(after, reply)
    record.update({"verdict": fg.verdict(checks), "checks": [c._asdict() for c in checks]})
    _save("food-log.after.csv", after)
    _save("record.json", json.dumps(record, indent=2))
    print("QW3 grade:\n" + fg.table(checks))
    assert fg.verdict(checks) == "PASS", fg.table(checks)
