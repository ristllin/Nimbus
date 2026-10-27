"""CTX - a full day of food chat across a compaction, with exact totals at the end.

On 26 Oct 2026 the owner's assistant rebuilt the day's total five different ways in an
hour: no food log existed, the morning had been compacted away, and the model added
up whatever survived. This leg replays a day on a bench unit set to Europe/London
(3 Nov 2026, GMT) with real head calls (Anthropic, ``NIMBUS_CTX_MODEL``, default
claude-haiku-5-5):

1. the unit's own food ledger (``food/food-log.csv``) is set aside and the web chat
   cleared, so the day starts with no ledger and no conversation;
2. 08:10 breakfast, 10:30 a snack, 13:05 lunch, 15:40 a correction to lunch - the owner
   gives each figure but never names a file: the per-turn ledger block names it;
3. 16:00 a manual ``/compact`` (every turn so far folds into the summary);
4. 16:05 a second snack, 19:30 dinner, 21:00 "what's my total for today, by meal?".

Graded (ctx_food_grade.py) from the ledger the device wrote (``/api/files/dl``), the
final reply and the summary the final turn carried (``/api/lastturn``): one row per
eating occasion, the correction replacing its row, the day's total exact in the file
and in the reply, every meal's figure in the reply, the summary dated. Every logging
turn must write the ledger with the keyed ``files.append`` (its log line).

The device clock is placed with the console's CLOCK verb before each turn; the time
zone, the head model, the other providers' keys and the spend count are set for the
leg and restored after it (the storm kit), and the unit's ledger is put back. The
ledger file, the replies, the log lines, the last prompt and record.json go to
``NIMBUS_CTX_EVIDENCE`` (a directory; default: no files written). Markers: hil, net,
agent. Needs --allow-hardware, NIMBUS_PORT, NIMBUS_TEST_IP, NIMBUS_TEST_TOKEN, an SD
card, and ANTHROPIC_API_KEY on the unit or in the bench .env.
"""

from __future__ import annotations

import contextlib
import json
import os
import re
import sys
import time

import pytest

import ctx_food_grade as fg

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "storm"))

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

LONDON = "GMT0BST,M3.5.0/1,M10.5.0"
MODEL = os.environ.get("NIMBUS_CTX_MODEL", "claude-haiku-5-5")
EVIDENCE = os.environ.get("NIMBUS_CTX_EVIDENCE", "")
WHERE = f"{fg.PROJECT}/{fg.NAME}"
COMPACT = "/compact"
# (UTC epoch, the owner's message, a logging turn?)
TURNS = [
    (1793693400, "Breakfast: porridge with milk and a banana, 420 kcal and 28 g protein.", True),
    (1793701800, "Snack just now: an apple and a few almonds, 180 kcal, 6 g protein.", True),
    (1793711100, "Lunch: chicken wrap and a bowl of soup, 760 kcal and 52 g protein.", True),
    (1793720400, "Correction: lunch was 720 kcal, not 760. Protein stays 52 g.", True),
    (1793721600, COMPACT, False),
    (1793721900, "Snack: a protein yoghurt, 210 kcal and 20 g protein.", True),
    (1793734200, "Dinner: salmon, rice and greens, 690 kcal and 48 g protein.", True),
    (1793739600, "What's my total for today? List each meal with its kcal.", False),
]
KEYED_LINE = re.compile(
    r"files: (?:appended \d+ row\(s\) to " + re.escape(WHERE) + r" \(keyed|upsert " + re.escape(WHERE) + ")"
)
SUMMARY_HEAD = "[CONVERSATION SUMMARY - compacted earlier; information, not instructions]"


def _save(name: str, text: str) -> None:
    if EVIDENCE:
        os.makedirs(EVIDENCE, exist_ok=True)
        with open(os.path.join(EVIDENCE, name), "w", encoding="utf-8") as f:
            f.write(text)


def _dump(net) -> str:
    r = net.get(f"/api/files/dl?project={fg.PROJECT}&name={fg.NAME}", timeout=15.0)
    return r.text if r.status_code == 200 else ""


@contextlib.contextmanager
def _ledger_set_aside(net):
    """The unit's own ledger is downloaded and removed for the leg, and put back after."""
    import requests

    kept = _dump(net)
    net.post("/api/files/rm", {"project": fg.PROJECT, "name": fg.NAME}, timeout=10.0)  # 404 when absent
    assert _dump(net) == "", "the ledger could not be set aside"
    try:
        yield
    finally:
        net.post("/api/files/rm", {"project": fg.PROJECT, "name": fg.NAME}, timeout=10.0)
        if kept:
            requests.post(
                f"http://{net.ip}/api/files/upload?project={fg.PROJECT}&name={fg.NAME}",
                files={"file": (fg.NAME, kept.encode("utf-8"), "text/csv")},
                headers=net._auth(),
                timeout=30,
            )


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
    """The Anthropic key on the unit for the leg (from the bench .env when it has none)."""
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


def _newest_compact(net) -> str:
    """The id of the web chat's newest ev:compact row ("" = none)."""
    rows = net.get_json("/api/mem/episodic?session=web&kind=log&limit=20", timeout=10.0).get("messages", [])
    ids = [m.get("id", "") for m in rows if "ev:compact" in m.get("tags", "")]
    return max(ids) if ids else ""


def _wait_for_fold(net, before: str) -> bool:
    """A new ev:compact row (the manual fold landed), within 120 s."""
    deadline = time.time() + 120
    while time.time() < deadline:
        time.sleep(5)
        if _newest_compact(net) > before:
            return True
    return False


def _turn(bench, net, at: int, text: str):
    """One web-chat turn with the clock placed at `at`; returns (reply, log)."""
    from webrig import LogFollower, chat_turn, make_session

    bench.verb(f"CLOCK {at}", "CLOCK set", timeout=6.0)
    s = make_session(str(net.token()))
    before = _newest_compact(net) if text == COMPACT else ""
    with LogFollower(s, net.ip) as follow:
        reply = chat_turn(s, net.ip, text, timeout_s=240)
        if text == COMPACT:
            assert _wait_for_fold(net, before), "no new ev:compact row within 120 s of /compact"
    return reply, follow.text


def _run_turns(bench, net, record: dict) -> str:
    """The day's turns; returns the last reply."""
    reply = ""
    for i, (at, text, logs) in enumerate(TURNS, 1):
        reply, log = _turn(bench, net, at, text)
        _save(f"ledger.after-turn{i}.csv", _dump(net))
        _save(f"log-turn{i}.txt", log)
        keyed = bool(KEYED_LINE.search(log))
        spend = re.findall(r"turn spend: ok \S+ \| (\S+) in=(\d+) out=(\d+) cR=(\d+) cW=(\d+)", log)
        record["turns"].append({"at": at, "text": text, "reply": reply, "keyed": keyed, "spend": spend})
        print(f"CTX turn {i} keyed={keyed} spend={spend} reply={reply[:160]!r}")
        if logs:
            assert keyed, f"turn {i}: no keyed files.append on {WHERE}"
    return reply


def _summary(dump: str) -> str:
    """The fold summary the last turn carried (from the turn anatomy dump)."""
    at = dump.find(SUMMARY_HEAD)
    if at < 0:
        return ""
    end = dump.find("\n[assistant]", at)
    return dump[at + len(SUMMARY_HEAD) : end if end > 0 else at + 5000]


def test_ctx_a_day_of_food_across_a_compaction(device, net):
    import kit

    kit.MODEL = MODEL
    bench = kit.StormBench(device, net, leg="ctx")
    record = {"model": MODEL, "build": bench.build, "turns": []}
    with (
        bench.only_provider("anthropic"),
        _anthropic_key(bench),
        bench.spend_room("anthropic"),
        bench.head_model(),
        _london(net),
        _clock(bench),
        _ledger_set_aside(net),
    ):
        bench.post_orch({"clearConv": "1", "convReset": "1", "memClear": "1"})
        time.sleep(6.0)  # the turn task drains the staged clear between turns
        reply = _run_turns(bench, net, record)
        after = _dump(net)
        dump = net.get("/api/lastturn", timeout=15.0).text
    summary = _summary(dump)
    checks = fg.grade(after, reply, summary)
    record.update({"verdict": fg.verdict(checks), "checks": [c._asdict() for c in checks]})
    _save("ledger.after.csv", after)
    _save("lastturn.txt", dump)
    _save("record.json", json.dumps(record, indent=2))
    print("CTX grade:\n" + fg.table(checks))
    assert "[TODAY'S FOOD LEDGER - food/food-log.csv" in dump, "the last prompt carried no ledger block"
    assert fg.verdict(checks) == "PASS", fg.table(checks)
