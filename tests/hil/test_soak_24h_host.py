"""Host tier: the 24 h soak driver against a fake device, and its grade against recorded runs. No board.

The driver runs for real on a compressed clock (a few hundred milliseconds), over HTTP to a fake on localhost. The grade
is pure over the files a run leaves, so each way a soak can go wrong is one small test: a green grade here could also
have gone red.
"""

from __future__ import annotations

import json
import os
import time

import pytest

import bench_driver as bd
import fake_bench_device as fake
import soak_24h as soak
import sram_sampler

pytestmark = pytest.mark.host


# ---- the scanner: the 28 Oct false zero --------------------------------------------------------------------

# The classes the 28 Oct console held in the final run's window, in the firmware's own format.
OCT_28 = (
    ["heap: alloc FAILED size=1626 caps=DMA|INTERNAL|8BIT task=async_t free=14000 largest=1400 up=100s"] * 12
    + ["heap: alloc FAILED size=1626 caps=DMA|INTERNAL|8BIT task=tiT free=14000 largest=1400 up=200s"] * 8
    + ["heap: alloc FAILED size=622 caps=DMA|INTERNAL|8BIT task=tg_poll free=14000 largest=500 up=300s"] * 2
    + ["heap: alloc FAILED size=190 caps=DMA|INTERNAL|8BIT task=tg_poll free=14000 largest=150 up=400s"] * 2
)


def test_the_scanner_counts_the_24_lines_the_old_one_read_as_zero():
    old_scanner = [line for line in OCT_28 if "alloc failed" in line]  # the bug, kept here to show what it missed
    assert old_scanner == []
    found = bd.scan_lines(OCT_28 + ["all fine", "turn end: ok host=x"])
    assert found["total"] == 24 and found["allocFailed"] == 24
    assert found["heapBySize"]["size=1626 caps=DMA|INTERNAL|8BIT task=async_t"] == 12
    assert found["turnEnds"] == {"ok": 1, "fail": 0}


@pytest.mark.parametrize(
    "line,name",
    [
        ("heap: ALLOC failed size=1 caps=X", "allocFailed"),
        ("truncated: write failed: low memory (alloc failed 1600 B DMA|INTERNAL)", "allocFailed"),
        ("esp-aes: Failed to allocate memory", "failedToAllocate"),
        ("sdmmc_read_sectors: not enough mem", "notEnoughMem"),
        ("turn failed (low memory, 900 B)", "turnFailedLowMemory"),
    ],
)
def test_every_memory_failure_class_counts_in_any_case(line, name):
    for spelling in (line, line.upper(), line.lower()):
        found = bd.scan_lines([spelling])
        assert found[name] == 1 and found["total"] == 1, spelling


def test_one_line_in_two_classes_is_one_line():
    assert bd.scan_lines(["esp-aes: failed to allocate, alloc failed"])["total"] == 1


# ---- the grade over a recorded run -------------------------------------------------------------------------


HOUR = 3600.0
START = 1_000_000.0


def poll(t, **overrides):
    row = {
        "t": START + t,
        "http": 200,
        "s": 0.1,
        "build": "v5.0.0",
        "uptimeMs": int(t * 1000) + 5000,
        "bootN": 7,
        "crashFp": None,
        "crashNew": None,
        "heap": 40000,
        "heapMin": 20000,
        "intFree": 40000,
        "intLargest": 16000,
        "lowWater": 14000,
        "lowWaterTags": "boot",
        "allocFails": 3,
        "pollStackMin": 9000,
        "nvsFree": 7000,
        "nvsFails": 0,
        "dreamKnown": True,
        "dreamLast": "ran",
        "dreamStateSaved": True,
    }
    row.update(overrides)
    return row


def run_polls(**overrides):
    return [poll(float(t), **overrides) for t in range(0, int(24 * HOUR) + 1, 300)]


def turns():
    rows = []
    for hour in range(24):
        rows.append({"t": START + hour * HOUR, "kind": "text", "ok": True, "s": 8.0, "error": ""})
        rows.append({"t": START + hour * HOUR, "kind": "food", "ok": True, "s": 12.0, "error": ""})
    return rows


PLAN = {"t": START, "hours": 24.0, "albums": False, "pollOnly": False}


def clean_scan():
    empty = bd.scan_lines([])
    return {"errlog": empty, "console": empty, "total": 0, "turnFailedLowMemory": 0}


def grade(polls=None, turn_rows=None, scan=None, plan=None):
    return soak.grade_run(
        plan or PLAN,
        run_polls() if polls is None else polls,
        turns() if turn_rows is None else turn_rows,
        scan or clean_scan(),
    )


def test_a_clean_24_hours_passes():
    verdict = grade()
    assert verdict["grade"] == "PASS", verdict["reasons"]
    assert verdict["numbers"]["lowWaterAfterFirstHour"] == 14000
    assert verdict["numbers"]["polls"] == 289


def _with(row_index, **overrides):
    polls = run_polls()
    polls[row_index] = {**polls[row_index], **overrides}
    return polls


@pytest.mark.parametrize(
    "polls,needle",
    [
        ([poll(float(t), bootN=7 if t < 40000 else 8) for t in range(0, int(24 * HOUR) + 1, 300)], "restarted"),
        (_with(200, uptimeMs=5), "uptime went back"),
        (
            [poll(float(t), crashFp=None if t < 40000 else "deadbeef") for t in range(0, int(24 * HOUR) + 1, 300)],
            "crash",
        ),
        ([poll(float(t), crashNew=t >= 40000) for t in range(0, int(24 * HOUR) + 1, 300)], "crash record"),
        (
            [poll(float(t), lowWater=14000 if t < 2 * HOUR else 7000) for t in range(0, int(24 * HOUR) + 1, 300)],
            "low water",
        ),
        ([poll(float(t), nvsFree=7000 - t // 300) for t in range(0, int(24 * HOUR) + 1, 300)], "settings store"),
        (_with(10, http=0, err="timed out"), "no answer"),
        (_with(10, s=6.0), "slowest poll"),
        ([poll(float(t), s=0.4) for t in range(0, int(24 * HOUR) + 1, 300)], "median poll"),
        ([poll(float(t), dreamLast="failed") for t in range(0, int(24 * HOUR) + 1, 300)], "dream ended"),
        ([poll(float(t), dreamStateSaved=False) for t in range(0, int(24 * HOUR) + 1, 300)], "state was not saved"),
    ],
)
def test_each_way_a_soak_goes_wrong_is_a_fail(polls, needle):
    verdict = grade(polls=polls)
    assert verdict["grade"] == "FAIL", verdict
    assert any(needle in reason for reason in verdict["reasons"]["FAIL"]), verdict["reasons"]


def test_a_low_water_in_the_first_hour_alone_is_not_a_fail():
    polls = [poll(float(t), lowWater=7000 if t < HOUR else 14000) for t in range(0, int(24 * HOUR) + 1, 300)]
    assert grade(polls=polls)["grade"] == "PASS"


def test_a_failed_or_slow_turn_is_a_fail():
    bad = turns()
    bad[3] = {**bad[3], "ok": False, "error": "no reply within 90 s"}
    assert grade(turn_rows=bad)["grade"] == "FAIL"
    slow = turns()
    slow[0] = {**slow[0], "s": 31.0}
    assert grade(turn_rows=slow)["grade"] == "FAIL"


def test_a_low_memory_turn_in_the_log_is_a_fail():
    found = bd.scan_lines(["turn failed (low memory)"])
    assert grade(scan={"errlog": found, "console": found, "total": 1, "turnFailedLowMemory": 1})["grade"] == "FAIL"


@pytest.mark.parametrize(
    "polls,turn_rows,needle",
    [
        ([poll(float(t)) for t in range(0, int(6 * HOUR), 300)], None, "watched"),
        ([poll(float(t)) for t in range(0, int(24 * HOUR) + 1, 3600)], None, "hole"),
        ([], None, "no poll"),
        ([{**poll(float(t)), "lowWater": None} for t in range(0, int(24 * HOUR) + 1, 300)], None, "low-water"),
        (
            [{**poll(float(t)), "allocFails": None} for t in range(0, int(24 * HOUR) + 1, 300)],
            None,
            "failed-allocation",
        ),
        ([{**poll(float(t)), "bootN": None} for t in range(0, int(24 * HOUR) + 1, 300)], None, "boot count"),
        ([{**poll(float(t)), "nvsFree": None} for t in range(0, int(24 * HOUR) + 1, 300)], None, "settings-store"),
        ([{**poll(float(t)), "dreamKnown": False} for t in range(0, int(24 * HOUR) + 1, 300)], None, "dream"),
        ([{**poll(float(t)), "dreamLast": "skipped"} for t in range(0, int(24 * HOUR) + 1, 300)], None, "unproven"),
        (None, [], "no text turn"),
    ],
)
def test_a_missing_measurement_is_blocked_never_pass(polls, turn_rows, needle):
    verdict = grade(polls=polls, turn_rows=turn_rows)
    assert verdict["grade"] == "BLOCKED", verdict
    assert any(needle in reason for reason in verdict["reasons"]["BLOCKED"]), verdict["reasons"]


@pytest.mark.parametrize(
    "field",
    [
        "uptimeMs",
        "bootN",
        "heap",
        "heapMin",
        "intFree",
        "intLargest",
        "lowWater",
        "allocFails",
        "pollStackMin",
        "nvsFree",
        "nvsFails",
    ],
)
def test_a_measurement_that_disappears_mid_run_is_blocked_never_pass(field):
    """A number measured in the first hour and None afterwards is a missing measurement, not a fine one."""
    polls = [
        poll(float(t)) if t < 2 * HOUR else {**poll(float(t)), field: None} for t in range(0, int(24 * HOUR) + 1, 300)
    ]
    verdict = grade(polls=polls)
    assert verdict["grade"] == "BLOCKED", verdict
    assert any(field in reason for reason in verdict["reasons"]["BLOCKED"]), verdict["reasons"]


def test_a_measurement_missing_only_at_the_start_is_blocked_too():
    polls = [
        {**poll(float(t)), "lowWater": None} if t < 2 * HOUR else poll(float(t))
        for t in range(0, int(24 * HOUR) + 1, 300)
    ]
    verdict = grade(polls=polls)
    assert verdict["grade"] == "BLOCKED", verdict
    assert any("lowWater" in reason for reason in verdict["reasons"]["BLOCKED"]), verdict["reasons"]


def test_a_planned_album_that_never_ran_is_blocked():
    assert grade(plan={**PLAN, "albums": True})["grade"] == "BLOCKED"


def test_counted_memory_failures_are_open_for_a_person_to_read():
    found = bd.scan_lines(OCT_28)
    verdict = grade(scan={"errlog": found, "console": bd.scan_lines([]), "total": 24, "turnFailedLowMemory": 0})
    assert verdict["grade"] == "OPEN"
    assert "24 log lines" in verdict["reasons"]["OPEN"][0] and "size=1626" in verdict["reasons"]["OPEN"][0]


def test_the_device_counter_rising_is_open_even_with_a_silent_log():
    polls = [poll(float(t), allocFails=3 if t < 40000 else 5) for t in range(0, int(24 * HOUR) + 1, 300)]
    verdict = grade(polls=polls)
    assert verdict["grade"] == "OPEN" and verdict["numbers"]["allocFailsDelta"] == 2


# ---- the driver, end to end on a compressed clock -----------------------------------------------------------


def args_for(out, ip, token_file, **overrides):
    values = {
        "--ip": ip,
        "--token-file": token_file,
        "--out": out,
        "--hours": str(0.6 / 3600),
        "--poll-s": "0.03",
        "--raw-s": "0.1",
        "--text-every": "0.2",
        "--food-every": "0.2",
        "--errlog-s": "0.2",
        "--turn-poll-s": "0.01",
    }
    values.update(overrides)
    flat = []
    for key, value in values.items():
        flat += [key] if value is True else [key, value]
    return soak.parse_args(flat)


@pytest.fixture
def run(tmp_path):
    token_file = tmp_path / "token"
    token_file.write_text(fake.TOKEN)
    out = tmp_path / "run"
    with fake.FakeDevice() as device:
        device.state.update(needsOnboarding=False, sta=True)

        def go(**overrides):
            args = args_for(str(out), device.ip, str(token_file), **overrides)
            return soak.run_soak(args, bd.Api(args.ip, bd.read_token(args.token_file)))

        yield device, out, go


def test_the_driver_keeps_raw_polls_and_turn_outcomes_on_disk(run):
    device, out, go = run
    verdict = go()
    polls = soak.load_jsonl(out / "polls.jsonl")
    assert len(polls) >= 5
    assert polls[0]["heap"] == 40000 and polls[0]["heapMin"] == 20000 and polls[0]["intLargest"] == 16000
    assert polls[0]["bootN"] == 7 and "crashFp" in polls[0] and polls[0]["allocFails"] == 0
    raw = sorted(os.listdir(out / "polls"))
    assert raw and json.loads((out / "polls" / raw[0]).read_text())["mem"]["intLargest"] == 16000
    kinds = {t["kind"] for t in soak.load_jsonl(out / "turns.jsonl")}
    assert kinds == {"text", "food"}
    assert os.listdir(out / "errlog")
    assert (out / "summary.md").exists() and json.loads((out / "verdict.json").read_text())["grade"] == verdict["grade"]
    assert verdict["grade"] == "PASS", verdict["reasons"]


def test_the_driver_sends_only_the_token_header_and_writes_no_token(run):
    device, out, go = run
    go()
    assert all(h.get("X-Nimbus-Token") == fake.TOKEN and "Authorization" not in h for h in device.auth_headers)
    for root, _, names in os.walk(out):
        for name in names:
            assert fake.TOKEN not in open(os.path.join(root, name), encoding="utf-8", errors="replace").read(), name


def test_the_grade_can_be_recomputed_from_the_files_alone(run):
    device, out, go = run
    first = go()
    again = soak.grade_dir(str(out))
    assert again["grade"] == first["grade"]
    assert soak.main(["--grade", str(out)]) == 0


def test_alloc_failures_in_the_real_log_are_counted_end_to_end(run):
    device, out, go = run
    device.errlog.extend(OCT_28)
    verdict = go()
    assert verdict["grade"] == "OPEN"
    assert verdict["numbers"]["allocFailureLines"] == 24


def test_a_restart_during_the_run_fails_it(run):
    device, out, go = run
    import threading

    threading.Timer(0.25, device.restart).start()
    verdict = go()
    assert verdict["grade"] == "FAIL"
    assert any("restarted" in reason for reason in verdict["reasons"]["FAIL"])


def test_a_wrong_build_refuses_to_start(run):
    device, out, go = run
    verdict = go(**{"--expect-build": "v9.9.9"})
    assert verdict["grade"] == "BLOCKED" and "runs" in verdict["reasons"]["BLOCKED"][0]
    assert not (out / "polls.jsonl").exists()


def test_a_second_run_in_the_same_folder_refuses_and_resume_keeps_the_start(run):
    device, out, go = run
    go()
    started = json.loads((out / "start.json").read_text())["t"]
    assert go()["grade"] == "BLOCKED"
    time.sleep(0.01)
    go(**{"--resume": True})
    assert json.loads((out / "start.json").read_text())["t"] == started


def test_poll_only_runs_no_turn_and_is_not_blocked_for_it(run):
    device, out, go = run
    verdict = go(**{"--poll-only": True})
    assert not (out / "turns.jsonl").exists()
    assert verdict["grade"] == "PASS", verdict["reasons"]


def test_a_stop_file_ends_a_run_early_and_it_grades_blocked(run):
    device, out, go = run
    os.makedirs(out)
    (out / "STOP").write_text("")
    verdict = go(**{"--hours": "1"})
    assert verdict["grade"] == "BLOCKED"


def test_a_failed_turn_end_to_end_is_recorded_and_fails_the_run(run):
    device, out, go = run
    device.chat_code = 503
    verdict = go(**{"--turn-poll-s": "0.001"})
    assert verdict["grade"] == "FAIL"
    assert soak.load_jsonl(out / "turns.jsonl")[0]["ok"] is False


def test_metrics_read_a_real_state_shape_and_keep_the_mem_block():
    row = soak.metrics(fake.fresh_state())
    assert row["lowWater"] == 15000 and row["allocFails"] == 0 and row["nvsFree"] == 7712
    assert soak.metrics({})["lowWater"] is None


# ---- the other samplers -------------------------------------------------------------------------------------


def test_sram_sampler_sends_the_header_the_device_accepts(tmp_path):
    with fake.FakeDevice() as device:
        row = sram_sampler.sample_once(device.ip, fake.TOKEN)
    assert row["intFree"] == 40000
    assert all("Authorization" not in h for h in device.auth_headers)
    assert device.auth_headers[0]["X-Nimbus-Token"] == fake.TOKEN


def test_sram_sampler_takes_its_token_from_a_file_not_the_command_line(tmp_path, capsys):
    token_file = tmp_path / "tok"
    token_file.write_text(fake.TOKEN)
    with pytest.raises(SystemExit):
        sram_sampler.main(["--ip", "192.0.2.1", "--token", "x", "--once"])
    capsys.readouterr()
    with fake.FakeDevice() as device:
        assert sram_sampler.main(["--ip", device.ip, "--token-file", str(token_file), "--once"]) == 0
    assert fake.TOKEN not in capsys.readouterr().out


# ---- lane SNI: the soak that passed a build whose every exchange failed ------------------------------------------

K01 = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "k01-c168797b")
NOTICES = [
    "That didn't finish - anthropic could not be reached; openai and mistral also failed. Your message was not "
    "answered. Nothing is still running; ask again to retry.",
    "That didn't finish - the device ran out of working memory for this request.",
    "That didn’t finish - mistral did not write an answer.",
    "Couldn't start that: mistral could not be reached. Check the connection and try again.",
]
ANSWERS = ["391", "Logged lunch: grilled chicken breast and rice, 640 kcal.", "Finished: 3 rows saved.", ""]


def test_the_stored_k01_soak_fails_on_its_turns_and_its_exchanges():
    """K01 on c168797b: the build dialed TLS by address, so every exchange failed at connect and every reply was the
    failure notice. The driver recorded each turn ok, and the run's own verdict counted 0 failed turns. The fixture is
    the run's start.json and turns.jsonl as recorded, the first and last three polls, and every distinct heaptrace
    line of its 16 durable-log reads."""
    verdict = soak.grade_dir(K01)
    assert verdict["grade"] == "FAIL"
    fails = verdict["reasons"]["FAIL"]
    assert any(reason.startswith("32 of 32 turns failed") for reason in fails), fails
    assert any(reason.startswith("212 provider exchanges failed at connect") for reason in fails), fails
    assert verdict["numbers"]["failedTurns"] == 32
    assert verdict["numbers"]["connectFailedExchanges"] == 212


@pytest.mark.parametrize("reply", NOTICES)
def test_a_failure_notice_is_a_failed_turn_even_when_it_was_recorded_ok(reply):
    rows = turns()
    rows[5] = {**rows[5], "ok": True, "reply": reply}
    verdict = grade(turn_rows=rows)
    assert verdict["grade"] == "FAIL"
    assert verdict["numbers"]["failedTurns"] == 1
    assert bd.failure_notice(reply)


@pytest.mark.parametrize("reply", ANSWERS)
def test_an_answer_is_not_a_failure_notice(reply):
    rows = [{**row, "reply": reply} for row in turns()]
    assert not bd.failure_notice(reply)
    assert grade(turn_rows=rows)["grade"] == "PASS"


def _trace(host, stage):
    return (
        f"47 96105 - transport: heaptrace {host}/v1/messages body=67131 pre=38676/14324 conn=38584/14324 "
        f"wrote=0/0 done=38628/14324 fail={stage} af=0 min=37952 at=95006 ms=1097"
    )


def test_the_scanner_counts_exchanges_by_the_stage_they_stopped_at():
    found = bd.scan_lines(
        [_trace("api.mistral.ai", "none")] * 3
        + [_trace("api.openai.com", "connect")] * 2
        + [_trace("api.mistral.ai", "connect"), _trace("api.anthropic.com", "read")]
    )
    assert found["exchanges"] == {"none": 3, "connect": 3, "read": 1}
    assert found["connectFailed"] == 3
    assert found["connectFailedByHost"] == {"api.openai.com": 2, "api.mistral.ai": 1}
    assert found["total"] == 0  # an exchange record is not a memory failure


def test_an_exchange_that_failed_at_connect_fails_the_soak():
    found = bd.scan_lines([_trace("api.mistral.ai", "none"), _trace("api.openai.com", "connect")])
    verdict = grade(
        scan={
            "errlog": found,
            "console": bd.scan_lines([]),
            "total": 0,
            "turnFailedLowMemory": 0,
            "connectFailed": found["connectFailed"],
        }
    )
    assert verdict["grade"] == "FAIL"
    assert any("1 provider exchanges failed at connect" in reason for reason in verdict["reasons"]["FAIL"])
    healthy = bd.scan_lines([_trace("api.mistral.ai", "none")] * 5)
    assert (
        grade(scan={"errlog": healthy, "console": healthy, "total": 0, "turnFailedLowMemory": 0, "connectFailed": 0})[
            "grade"
        ]
        == "PASS"
    )


def test_a_failure_notice_from_the_device_fails_the_run_end_to_end(run):
    device, out, go = run
    device.chat_reply = lambda text: NOTICES[0]
    device.errlog.extend([_trace("api.anthropic.com", "connect")] * 3)
    verdict = go()
    assert verdict["grade"] == "FAIL"
    recorded = soak.load_jsonl(out / "turns.jsonl")
    assert recorded and all(row["ok"] is False and "failure notice" in row["error"] for row in recorded)
    assert verdict["numbers"]["connectFailedExchanges"] == 3
