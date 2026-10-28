"""Host tests for the storm suite's grading (no device): the bound checks, the notice
counting and the before/after table, over recorded and scrubbed logs of the 19 and 21
Oct 2026 storms (``fixtures/``: only the line kinds the grader reads, every free-text
part replaced by a placeholder), plus a candidate-shaped log that must pass.

Run: python3 -m pytest tests/hil/storm/test_storm_grading_host.py -m host
"""

from __future__ import annotations

import json
import os
import re

import pytest

import grading as g
import kit
import settings_fill

pytestmark = pytest.mark.host

HERE = os.path.dirname(os.path.abspath(__file__))
REPLAY_BOUNDS = g.Bounds(max_paid_turns=1, max_provider_requests=8, max_identical=5, notices=(1, 1))


def fixture(name: str) -> list:
    with open(os.path.join(HERE, "fixtures", name), encoding="utf-8") as f:
        return g.parse_log(f.read())


# ---- the recorded storms grade FAIL --------------------------------------------------


def test_the_21_oct_replay_is_a_loop():
    """The 21 Oct page of six messages re-served once a cycle: every message comes back
    each cycle, each cycle is a paid turn, and nothing tells the owner."""
    lines = fixture("storm-2026-10-21.log")
    assert all(ln.seq > 0 for ln in lines)
    assert [ln.seq for ln in lines] == sorted(ln.seq for ln in lines)
    texts = g.inbound_texts(lines, 4242424242)
    assert len(set(texts)) == 6 and len(texts) == 72  # six messages, served twelve times
    peak, _ = g.identical_peak(lines)
    assert peak == 12  # each re-served message line, each reply send: once per cycle
    assert g.identical_peak(lines, ignore=("telegram: send", "orchestrator:", "turn end"))[1].endswith(
        "owner message 1"
    )
    m = g.Metrics(
        paid_turns=g.paid_turns(lines),
        provider_requests=g.paid_turns(lines),
        parked_requests=0,
        notices=len(g.log_notices(lines)),
        identical_peak=peak,
        usable=True,
        usable_s=10.0,
    )
    assert m.paid_turns == 12
    vs = g.grade(m, REPLAY_BOUNDS)
    assert vs["no_loop"].status == g.FAIL and "paid turns 12" in vs["no_loop"].detail
    assert vs["capped"].status == g.FAIL
    assert vs["told_once"].status == g.FAIL and "0 notice" in vs["told_once"].detail
    assert vs["usable"].status == g.PASS
    assert g.overall(vs) == g.FAIL


def test_the_19_oct_credit_outage_asked_a_refusing_provider_again_and_again():
    """19 Oct: the OpenAI account out of credit. The head and the embeddings kept asking
    it, and the device said nothing about it."""
    lines = fixture("storm-2026-10-19.log")
    refusals = [ln for ln in lines if "no credits remaining" in ln.text or "credit_balance_exhausted" in ln.text]
    assert len(refusals) >= 20
    zero = [ln for ln in lines if ln.text.startswith("transport: short body write 0/")]
    assert len(zero) == 2  # the low-memory sends of the same morning
    m = g.Metrics(
        paid_turns=g.paid_turns(lines),
        provider_requests=len(refusals),
        parked_requests=len(refusals) - 1,
        notices=len(g.log_notices(lines)),
        identical_peak=g.identical_peak(lines, ignore=("telegram: send ok",))[0],
        usable=True,
    )
    vs = g.grade(m, g.Bounds(max_paid_turns=2, max_provider_requests=1, max_identical=5, notices=(1, 1)))
    assert vs["capped"].status == g.FAIL and "parked provider" in vs["capped"].detail
    assert vs["told_once"].status == g.FAIL


# ---- a candidate-shaped episode grades PASS ------------------------------------------------

CANDIDATE_LOG = "\n".join(
    [
        "10 1000 - telegram: msg from Owner (chat 4747474747): storm b 1: reply pong",
        "11 1100 - orchestrator: turn host=anthropic (conv=0 heap=30000)",
        "12 2000 - transport: heaptrace api.anthropic.com/v1/messages body=9000 pre=1/1 conn=1/1 wrote=1/1 done=1/1 "
        "fail=none af=0 min=1 at=1 ms=1",
        "13 2050 - turn spend: ok owner | anthropic/claude-haiku-5-5 in=4 out=349 cR=0 cW=18049 n=1",
        "13 2100 - turn end: ok host=anthropic tokens in=900 out=4 tools=0 reply=4B src=owner",
        "13 2200 - turn spend: ok synthesis | anthropic/claude-haiku-5-5 in=10 out=2223 cR=21697 cW=37243 n=3",
        "13 2300 - turn end: ok host=anthropic tokens in=10 out=2223 tools=2 reply=720B src=synthesis",
        "14 2200 - telegram: injected page: 1 message batched into 1 turn",
    ]
    + [f"{15 + k} {3000 + k} - telegram: 1 duplicate dropped ({k + 1} since the guard was cleared)" for k in range(49)]
    + [
        "70 9000 - telegram: guard notice (duplicate) queued for the owner",
    ]
    + ["%d %d - telegram: injected page: 0 messages batched into 0 turns" % (71 + k, 9100 + k) for k in range(49)]
)


def test_a_candidate_shaped_replay_passes():
    lines = g.parse_log(CANDIDATE_LOG)
    peak, line = g.identical_peak(lines, ignore=kit.DRIVE_LINES)
    m = g.Metrics(
        paid_turns=g.paid_turns(lines),
        provider_requests=g.billed_requests(lines),
        parked_requests=0,
        notices=len(g.log_notices(lines)),
        identical_peak=peak,
        usable=True,
        usable_s=42.0,
    )
    assert (m.paid_turns, m.provider_requests, m.notices) == (1, 1, 1)
    assert peak == 1, line  # each duplicate line counts its own number; the drive's echo is ignored
    assert g.overall(g.grade(m, REPLAY_BOUNDS)) == g.PASS


def test_a_steady_heartbeat_is_not_a_loop():
    beat = "[sram] steady-orch    intFree=40408 intLargest=22516 heapMin=15460 psramFree=6821408 dIntFree=+0"
    lines = g.parse_log("\n".join(f"{k} {k * 20000} - {beat}" for k in range(1, 12)))
    assert g.identical_peak(lines) == (0, "")
    loop = g.parse_log("\n".join(f"{k} {k} - orchTurn(custom): chat HTTP 402: x" for k in range(1, 7)))
    assert g.identical_peak(loop)[0] == 6  # counter-test: a real repeat still counts


def test_without_the_drive_filter_the_drive_echo_is_a_repeat():
    """Counter-test for the ignore list: the injected-page echo repeats 49 times."""
    assert g.identical_peak(g.parse_log(CANDIDATE_LOG))[0] == 49


# ---- the counters --------------------------------------------------------------------------


def test_notice_lines():
    log = "\n".join(
        [
            "1 1 - telegram: guard notice (poison) queued for the owner",
            "2 2 - telegram: storage-full notice sent to the owner",
            "3 3 boot crash: owner notice queued (turn=m0000001a)",
            "4 4 boot crash: owner notice not queued (quiet hours)",
            "5 5 - telegram: send ok to 4747474747",
        ]
    )
    assert len(g.log_notices(g.parse_log(log))) == 3


def test_qw2_web_chat_notice_lines():
    """28 Oct re-baseline: QW2's owner_notice rework delivers a no-token (web-only)
    device's notices to the web chat and words the log line "captured for the web
    chat (no Telegram owner)" - a DELIVERED notice, so told_once must count it. The
    episode dedup ("already in the web chat today: not repeated") and a full reply
    queue ("dropped") are not deliveries and stay uncounted."""
    log = "\n".join(
        [
            "1 1 - telegram: storage-full notice captured for the web chat (no Telegram owner)",
            "2 2 - telegram: storage-full notice (file store) captured for the web chat (no Telegram owner)",
            "3 3 - telegram: guard notice (duplicate) captured for the web chat (no Telegram owner)",
            "4 4 - telegram: storage-full notice already in the web chat today: not repeated",
            "5 5 - telegram: guard notice (duplicate) dropped: reply queue full",
            "6 6 - telegram: storage-full notice sent to the owner",
        ]
    )
    found = g.log_notices(g.parse_log(log))
    assert len(found) == 4  # the two storage deliveries, the guard delivery, the pre-QW2 wording
    assert not any("not repeated" in t or "dropped" in t for t in found)


def test_turns_requests_and_parks():
    log = "\n".join(
        [
            "1 1 - turn end: ok host=anthropic tokens in=10 out=2 tools=0 reply=2B",
            "2 2 - turn end: fail host=none",
            "3 3 - provider: TEST override answered api.anthropic.com/v1/messages with HTTP 400",
            "4 4 - provider: anthropic parked for every seam (billing, HTTP 400)",
            "5 5 - provider: TEST override answered api.anthropic.com/v1/messages with HTTP 400",
            "6 6 - provider: TEST override answered api.openai.com/v1/responses with HTTP 429",
        ]
    )
    lines = g.parse_log(log)
    assert g.paid_turns(lines) == 1
    assert g.override_requests(lines) == {"api.anthropic.com": 2, "api.openai.com": 1}
    assert g.requests_after_park(lines, "anthropic", g.OVERRIDE_RE) == 2
    assert g.requests_after_park(lines, "mistral", g.OVERRIDE_RE) == 0


def test_replies_are_split_into_answers_notices_and_the_rest():
    rows = [
        {"role": "user", "text": "pong please"},
        {"role": "assistant", "text": "pong"},
        {"role": "assistant", "text": "Add credit on anthropic's billing page."},
        {"role": "assistant", "text": "Something else"},
    ]
    out = g.classify_replies(rows, re.compile("pong"), re.compile("credit"))
    assert (len(out["answers"]), len(out["notices"]), len(out["other"])) == (1, 1, 1)
    before = {"m1": rows[0]}
    after = {"m1": rows[0], "m3": rows[2], "m2": rows[1]}
    assert [r["text"] for r in g.new_rows(before, after)] == ["pong", "Add credit on anthropic's billing page."]


# ---- bounds and verdicts --------------------------------------------------------------------


def test_unmeasured_is_blocked_never_a_pass():
    vs = g.grade(g.Metrics(), REPLAY_BOUNDS)
    assert {v.status for v in vs.values()} == {g.BLOCKED}
    assert g.overall(vs) == g.BLOCKED


def test_a_late_answer_fails_usable():
    m = g.Metrics(paid_turns=1, provider_requests=1, parked_requests=0, notices=1, identical_peak=1, usable=True)
    m.usable_s = REPLAY_BOUNDS.usable_within_s + 1
    assert g.grade(m, REPLAY_BOUNDS)["usable"].status == g.FAIL
    m.usable = False
    assert g.grade(m, REPLAY_BOUNDS)["usable"].status == g.FAIL


def test_told_once_ranges():
    m = g.Metrics(paid_turns=1, provider_requests=1, parked_requests=0, notices=0, identical_peak=1, usable=True)
    assert g.grade(m, g.Bounds(1, 1, 5, notices=(0, 1)))["told_once"].status == g.PASS
    assert g.grade(m, g.Bounds(1, 1, 5, notices=(1, 1)))["told_once"].status == g.FAIL
    m.notices = 2
    assert g.grade(m, g.Bounds(1, 1, 5, notices=(0, 1)))["told_once"].status == g.FAIL
    assert g.grade(m, g.Bounds(1, 1, 5, notices=(0, 1)))["no_loop"].status == g.FAIL


def test_an_unmet_precondition_blocks_every_property():
    m = g.Metrics(paid_turns=0, provider_requests=0, parked_requests=0, notices=0, identical_peak=1, usable=False)
    assert g.overall(g.grade(m, REPLAY_BOUNDS)) == g.FAIL
    m.blocked_by = "the unit's hour was used up"
    vs = g.grade(m, REPLAY_BOUNDS)
    assert {v.status for v in vs.values()} == {g.BLOCKED} and "hour" in vs["usable"].detail


def test_the_ceiling_precondition():
    assert kit.at_ceiling_already({"ceiling": 3}, {"ceiling": 4})
    assert not kit.at_ceiling_already({"ceiling": 3}, {"ceiling": 3})
    assert not kit.at_ceiling_already(None, {"ceiling": 9})  # the control image has no guard
    assert kit.ceiling_blocked({"ceiling": 7}).blocked_by.endswith("(ceiling skips 7 before the leg)")


def test_a_fail_outranks_a_blocked():
    m = g.Metrics(paid_turns=9, usable=True)
    assert g.overall(g.grade(m, REPLAY_BOUNDS)) == g.FAIL


# ---- the table and the evidence ----------------------------------------------------------------


def test_table_shows_both_images_and_the_bounds():
    before = g.Metrics(paid_turns=20, provider_requests=20, notices=0, identical_peak=20, usable=True, usable_s=4.0)
    after = g.Metrics(paid_turns=1, provider_requests=1, parked_requests=0, notices=1, identical_peak=2, usable=True)
    text = g.table("b_page_reserved", before, after, REPLAY_BOUNDS)
    rows = {ln.split("  ")[0]: ln for ln in text.splitlines()}
    assert re.search(r"paid turns\s+20\s+1\s+<= 1", rows["paid turns"])
    assert re.search(r"grade\s+FAIL\s+PASS", rows["grade"])
    assert "v4.5.13 told_once: FAIL" in text and "candidate told_once: PASS" in text
    assert "not run" in g.table("b_page_reserved", None, after, REPLAY_BOUNDS)


def test_evidence_round_trip(tmp_path):
    m = g.Metrics(paid_turns=1, provider_requests=1, parked_requests=0, notices=1, identical_peak=1, usable=True)
    rec = g.evidence_record(
        g.RunId("b_page_reserved", g.CANDIDATE, "v4.5.16-26-geb50beb8", "eb50beb8", "L", 100.0, 200.0), m, REPLAY_BOUNDS
    )
    assert rec["grade"] == g.PASS and rec["verdicts"]["told_once"]["status"] == g.PASS
    ctl = g.evidence_record(
        g.RunId("b_page_reserved", g.CONTROL, "v4.5.13", "c1ab939e", "L", 1.0, 2.0), m, REPLAY_BOUNDS
    )
    assert ctl["grade"] == "RECORDED" and ctl["verdicts"] == {}
    g.write_evidence(str(tmp_path), rec)
    g.write_evidence(str(tmp_path), ctl)
    (tmp_path / "junk.json").write_text("{not json")
    (tmp_path / "other.json").write_text(json.dumps({"schema": 99}))
    records = g.load_evidence(str(tmp_path))
    assert len(records) == 2
    back = g.latest(records, "b_page_reserved", g.CANDIDATE)
    assert g.metrics_of(back) == m
    assert g.bounds_of(back) == REPLAY_BOUNDS
    assert g.latest(records, "a_settings_full", g.CANDIDATE) is None


def test_every_leg_file_is_listed():
    files = sorted(
        n[11:-3] for n in os.listdir(HERE) if n.startswith("test_storm_") and n != "test_storm_grading_host.py"
    )
    assert files == sorted(g.LEGS)


# ---- the drives' pure parts ------------------------------------------------------------------


def test_commit_of_a_build():
    assert kit.commit_of("v4.5.16-26-geb50beb8") == "eb50beb8"
    assert kit.commit_of("v4.5.13") == kit.CONTROL_COMMIT
    assert kit.commit_of("") == ""


def test_an_injected_page_fits_one_console_line():
    from device import command_line_problem

    page = kit.text_page(999999999, 9999999999, "storm a 999999999: reply with the single word pong", 1792000000)
    assert page == kit.text_page(
        999999999, 9999999999, "storm a 999999999: reply with the single word pong", 1792000000
    )
    body = json.loads(page)
    assert body["result"][0]["message"]["chat"]["id"] == 9999999999
    assert command_line_problem("TGINJECT " + page) is None


def test_filler_ids_are_distinct_and_page_sized():
    ids = [settings_fill.pad_id(k) for k in range(settings_fill.PAD_IDS)]
    assert len(set(ids)) == len(ids)
    step = len(ids[0]) + len(settings_fill.PAD_NAME) + 2
    assert 200 <= step <= 400  # a few hundred bytes per step: the names string grows gently
    assert len(settings_fill.DIRECTIVE_HEAD) < settings_fill.DIRECTIVE_MAX


def test_a_multi_episode_leg_is_graded_by_its_worst_episode_and_serializes(tmp_path):
    """Leg c runs three episodes: the record carries the worst one's counters and a
    summary of every episode, and still writes as JSON (an episode list that held the
    graded episode's own extra was a circular reference on the bench, 22 Oct)."""
    good = g.Metrics(paid_turns=2, provider_requests=1, parked_requests=0, notices=1, identical_peak=1, usable=True)
    bad = g.Metrics(paid_turns=2, provider_requests=4, parked_requests=3, notices=0, identical_peak=4, usable=False)
    bad.extra["shape"] = "quota-429"
    b = g.Bounds(max_paid_turns=2, max_provider_requests=1, max_identical=5)
    assert kit.worst_episode([good, bad, good], b) is bad
    assert kit.worst_episode([good, good], b) is good
    blocked = g.Metrics()
    assert kit.worst_episode([good, blocked], b) is blocked
    import test_storm_c_provider_refusing as legc  # noqa: F401 - the leg module imports cleanly

    bad.extra["episodes"] = [{k: v for k, v in vars(e).items() if k not in ("extra", "notes")} for e in (good, bad)]
    rec = g.evidence_record(g.RunId("c_provider_refusing", g.CANDIDATE, "v", "eb50beb8", "A", 1.0, 2.0), bad, b)
    assert g.write_evidence(str(tmp_path), rec).endswith(".json") and rec["grade"] == g.FAIL


def test_background_turns_are_not_the_storm():
    """A helper's synthesis that finishes inside a leg's window (an earlier lane's job on
    a shared unit, unit L 22 Oct) is neither a paid turn of the storm nor its requests."""
    log = "\n".join(
        [
            "1 1 - turn spend: ok owner | anthropic/m in=1 out=1 cR=0 cW=0 n=2 | mistral/s in=1 out=1 n=1",
            "2 2 - turn end: ok host=anthropic tokens in=1 out=1 tools=0 reply=2B src=owner",
            "3 3 - turn spend: ok synthesis | anthropic/m in=1 out=1 n=4",
            "4 4 - turn end: ok host=anthropic tokens in=1 out=1 tools=2 reply=9B src=synthesis",
            "5 5 - turn end: ok host=custom tokens in=0 out=0 tools=0 reply=12B",  # v4.5.13: no src
            "6 6 - turn spend: ok dream | anthropic/m in=1 out=1 n=7",
        ]
    )
    lines = g.parse_log(log)
    assert g.paid_turns(lines) == 2
    assert g.billed_requests(lines) == 3


def test_messages_per_turn():
    rows = [
        {"role": "assistant", "tags": "fx:none,out:reply,turn:m1"},
        {"role": "assistant", "tags": "out:ask,turn:m1"},
        {"role": "assistant", "tags": "turn:m2"},
        {"role": "user", "tags": "turn:m2"},
        {"role": "assistant"},
    ]
    assert g.rows_per_turn(rows) == {"turn:m1": 2, "turn:m2": 1, "turn:?": 1}


def test_the_log_tap_takes_only_newer_lines():
    """A read can return older lines when the log moves (the card dropped out and the
    log went to flash, whose tail holds earlier boots) or a line cut at its start; the
    tap keeps only lines newer than the last it took, and a restart starts it again."""
    tap = kit.LogTap(net=None)
    tap.last = (100, 400000)
    got = tap.take(
        [
            "101 400100 - telegram: msg from Owner (chat 1): a",
            "eady-orch    intFree=37160",  # cut at the start of the read
            "51 42112 - main: SD lost -> degraded (no reboot)",  # an earlier boot's tail
            "101 400100 - telegram: msg from Owner (chat 1): a",  # read twice
            "102 400200 - turn end: ok host=anthropic tokens in=1 out=1 tools=0 reply=2B src=owner",
        ]
    )
    assert [ln.split(" - ")[1][:9] for ln in got] == ["telegram:", "turn end:"]
    assert tap.skipped == 3
    tap.restarted(uptime_ms=5000)
    read = [
        "103 402167 - the previous boot's tail",
        "104 402300 - its last line",
        "3 900 - boot line of the new boot",
        "4 1200 - next line",
    ]
    assert tap.take(read) == ["3 900 - boot line of the new boot", "4 1200 - next line"]
    assert tap.take(read) == []  # read again: nothing new
    assert tap.take(read + ["5 1300 - a newer line"]) == ["5 1300 - a newer line"]


class _FakeDevice:
    """A console that yields queued lines through the device's reader."""

    def __init__(self, lines):
        self.queue = list(lines)
        self._ser = object()

    def _readline(self, deadline):
        return self.queue.pop(0) if self.queue else None


def test_the_console_tap_records_what_the_harness_reads_and_pumps_the_rest():
    dev = _FakeDevice(["[agent] turn end: ok host=anthropic tokens in=1 out=1 tools=0 reply=2B src=owner"])
    tap = kit.LogTap(net=None, console=kit.ConsoleTap(dev))
    tap._read = lambda: []  # no durable log over HTTP (a card that dropped out)
    tap.arm()
    dev.queue += ["TGINJECT parsed=1 queued", "[agent] telegram: guard notice (duplicate) queued for the owner"]
    assert dev._readline(0) == "TGINJECT parsed=1 queued"  # a command reads its reply: still recorded
    tap.poll()
    assert tap.source == "console"
    assert [ln.text for ln in tap.parsed()] == [
        "TGINJECT parsed=1 queued",
        "[agent] telegram: guard notice (duplicate) queued for the owner",
    ]
    assert len(g.log_notices(tap.parsed())) == 1
    tap.restarted(5000)  # a restart: the console missed the boot, the window reads the durable log
    assert tap.source == "http"
    tap.console.close()
    assert "_readline" not in vars(dev) or dev._readline.__name__ != "record"


def test_sends_and_failed_owner_turns_from_the_log():
    log = "\n".join(
        [
            "1 1 - turn end: FAIL host=anthropic tokens in=0 out=0 tools=0 reply=0B src=owner",
            "2 2 - telegram: send ok to 4747474705",
            "3 3 - telegram: send FAIL to 4747474705",
            "4 4 - telegram: send ok to 1234",
            "5 5 - turn end: FAIL host=anthropic tokens in=0 out=0 tools=0 reply=0B src=dream",
        ]
    )
    lines = g.parse_log(log)
    assert g.telegram_sends(lines, 4747474705) == 2
    assert g.owner_turn_failed(lines)
    ok = g.parse_log(
        "1 1 - turn end: ok host=anthropic tokens in=1 out=1 tools=0 reply=2B src=owner\n"
        "2 2 - turn end: FAIL host=anthropic tokens in=0 out=0 tools=0 reply=0B src=dream"
    )
    assert not g.owner_turn_failed(ok), "a failed background turn is not the owner's"


class _NetState:
    """/api/state answers in turn: unreachable, the old boot, then the new boot."""

    def __init__(self, answers):
        self.answers = list(answers)

    def get_json(self, path, timeout=None):
        a = self.answers.pop(0) if len(self.answers) > 1 else self.answers[0]
        if a is None:
            raise ConnectionError("booting")
        return a


def test_a_slow_crash_boot_is_seen_by_its_boot_number(monkeypatch):
    """A crash boot answered only after its uptime passed the previous one (unit A, 22
    Oct): the boot number moving on is what shows the restart."""
    monkeypatch.setattr(kit.time, "sleep", lambda s: None)
    bench = kit.StormBench.__new__(kit.StormBench)
    bench.net = _NetState([None, {"uptimeMs": 900000, "boot": {"n": 7}}, {"uptimeMs": 400000, "boot": {"n": 8}}])
    seen = []
    bench.tap = type("T", (), {"restarted": lambda self, up: seen.append(up), "poll": lambda self: []})()
    bench.reopen_console = lambda: None
    bench.settle = lambda: None
    bench.wait_back(timeout=5.0, before_ms=(300000, 7))
    assert seen == [400000]


def test_an_unfinished_run_is_a_fail_that_its_own_record_replaces(tmp_path):
    run = g.RunId("e_low_heap", g.CANDIDATE, "v", "eb50beb8", "A", 100.0, 100.0)
    first = g.write_evidence(str(tmp_path), g.unfinished_record(run))
    rec = g.load_evidence(str(tmp_path))[0]
    assert rec["grade"] == g.FAIL and g.UNFINISHED in rec["verdicts"]["usable"]["detail"]
    m = g.Metrics(paid_turns=1, provider_requests=1, parked_requests=0, notices=1, identical_peak=1, usable=True)
    done = g.evidence_record(
        g.RunId("e_low_heap", g.CANDIDATE, "v", "eb50beb8", "A", 100.0, 500.0), m, g.Bounds(3, 12, 5, notices=(0, 1))
    )
    assert g.write_evidence(str(tmp_path), done) == first  # the same file: replaced
    records = g.load_evidence(str(tmp_path))
    assert len(records) == 1 and records[0]["grade"] == g.PASS


def test_spend_limits_a_leg_needs_room_under():
    line = (
        'SPENDSET {"limitCents":2000,"spentCents":4444,"frozen":true,"ownerSet":false,"active":false,'
        '"month":202610,"resetsOn":"1 November","saved":true,"loaded":true,"dailyWarnCents":500,"todayCents":12,'
        '"providers":[{"p":"anthropic","capCents":1000,"spentCents":1200,"frozen":true,"resetsOn":"1 November"},'
        '{"p":"openai","capCents":500,"spentCents":0,"frozen":false,"resetsOn":"1 November"}]}'
    )
    st = kit.parse_spend_state(line)
    assert st["active"] is False and st["providers"]["anthropic"] == (1000, 1200, True)
    assert kit.spend_rows(st, "anthropic") == {"anthropic": (1000, 1200, True)}  # the inactive ceiling is no limit
    old = kit.parse_spend_state(
        'SPENDSET {"limitCents":2000,"spentCents":2509,"frozen":true,"ownerSet":false,"month":1}'
    )
    assert old["active"] is True and kit.spend_rows(old, "anthropic") == {"": (2000, 2509, True)}
    assert kit.spend_rows(None, "anthropic") == {}


def test_file_store_leg_allows_one_send_per_photo_plus_the_notice():
    """Leg g in bot mode: 5 replies and the one store-full notice log the same send line
    6 times, which is not a loop (lane FSN, 23 Oct); 10 sends for 5 photos is."""
    import test_storm_g_file_store_full as legg

    def run(sends: int) -> g.Verdict:
        lines = g.parse_log("\n".join(f"{k} {k * 100} - telegram: send ok to 4747474707" for k in range(1, sends + 1)))
        peak, _ = g.identical_peak(lines)
        m = g.Metrics(
            paid_turns=legg.PHOTOS,
            provider_requests=legg.PHOTOS,
            parked_requests=0,
            notices=1,
            identical_peak=peak,
            usable=True,
            usable_s=8.0,
        )
        return g.grade(m, legg.BOUNDS)["no_loop"]

    assert legg.MAX_SENDS == legg.PHOTOS + 1
    assert run(6).status == g.PASS
    assert run(10).status == g.FAIL and "identical log lines 10" in run(10).detail
