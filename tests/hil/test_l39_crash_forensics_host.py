"""Host tests for the L39 crash-forensics parsers and verdicts (no board).

The fixtures are synthetic lines in the exact shapes src/sys/crash_report.cpp and
src/sys/errlog_fs.cpp write (the formatters are pinned by test/test_crash_forensics and
test/test_errlog), so a renamed field fails here instead of passing a bench run vacuously.
"""

from __future__ import annotations

import pytest

from test_l39_crash_forensics import (
    boot_report,
    crash_verdict,
    every_boot_starts_at_seq0,
    parse_log,
    split_boots,
    stale_verdict,
)

pytestmark = pytest.mark.host

BOOT_PANIC = "0 812 boot boot: n=17 fw=v4.5.14 build=v4.5.14-3-gabc1234 reason=crash(panic)(0x0c) wake=none mode=orch t=1792336105 ota=ok prev=none"
CRASH_NEW = (
    "5 1031 boot crash: fp=1a2b3c4d new=1 this-reset=1 task=loopTask pc=0x4037a2b0 "
    "cause=29(StoreProhibited) vaddr=0x00000000 size=23456/65536 elf=0badf00d1 fw-match=1"
)
BT = "6 1032 boot crash-bt: depth=3 corrupt=0 0x4037a2b0,0x4200f00d,0x4200beef"
WHY = "7 1032 boot crash-why: Unhandled debug exception"


def _log(*lines: str) -> str:
    return "\n".join(lines) + "\n"


def test_parse_and_split_boots():
    text = _log(
        "9 99 - a line from an older firmware with no boot record",
        BOOT_PANIC,
        "1 900 - [sram] boot-hal intFree=200000",
        CRASH_NEW,
        BT,
        WHY,
        "0 500 boot boot: n=18 fw=v4.5.14 build=b reason=software-restart(0x0c) wake=none mode=orch t=1792336200 prev=none",
        "1 600 boot crash: fp=1a2b3c4d new=0 this-reset=0 task=loopTask pc=0x4037a2b0 cause=29(StoreProhibited) vaddr=0x00000000 size=23456/65536 elf=0badf00d1 fw-match=1",
        "not a log line",
    )
    boots = split_boots(parse_log(text))
    assert len(boots) == 2
    first, second = boot_report(boots[0]), boot_report(boots[1])
    assert first["boot"]["n"] == "17" and first["boot"]["slug"] == "crash(panic)" and first["boot"]["raw"] == "0c"
    assert first["boot"]["ota"] == "ok"
    assert first["crash"]["task"] == "loopTask" and first["bt"]["depth"] == "3"
    assert first["why"] == "Unhandled debug exception"
    assert second["boot"]["slug"] == "software-restart" and second["boot"]["ota"] is None
    assert second["crash"]["new"] == "0" and second["why"] is None


def test_crash_verdict_accepts_a_good_report():
    rep = boot_report(split_boots(parse_log(_log(BOOT_PANIC, CRASH_NEW, BT, WHY)))[0])
    assert crash_verdict(rep, "abort") == (True, "ok")


@pytest.mark.parametrize(
    "mutate, why",
    [
        (lambda t: t.replace("0 812 boot boot:", "6 812 boot boot:"), "pre-mount hole"),
        (lambda t: t.replace("crash(panic)(0x0c)", "crash(watchdog)(0x07)"), "boot reason"),
        (lambda t: t.replace("new=1 this-reset=1", "new=0 this-reset=1"), "new=0"),
        (lambda t: t.replace("task=loopTask", "task=tg_poll"), "crash task"),
        (lambda t: t.replace("size=23456/65536", "size=65536/65536"), "64 KB"),
        (lambda t: t.replace("Unhandled debug exception", "(no reason recorded)"), "crash-why"),
        (lambda t: t.replace(BT + "\n", ""), "crash-bt"),
    ],
)
def test_crash_verdict_rejects_each_missing_fact(mutate, why):
    text = mutate(_log(BOOT_PANIC, CRASH_NEW, BT, WHY))
    rep = boot_report(split_boots(parse_log(text))[0])
    ok, msg = crash_verdict(rep, "abort")
    assert not ok and why in msg


def test_crash_verdict_needs_the_crash_line():
    rep = boot_report(split_boots(parse_log(_log(BOOT_PANIC, BT, WHY)))[0])
    ok, msg = crash_verdict(rep, "nullptr")
    assert not ok and "no crash: line" in msg


def test_twdt_expects_the_watchdog_slug():
    text = _log(BOOT_PANIC.replace("crash(panic)(0x0c)", "crash(watchdog)(0x07)"), CRASH_NEW, BT, WHY)
    rep = boot_report(split_boots(parse_log(text))[0])
    assert crash_verdict(rep, "twdt")[0]
    assert not crash_verdict(rep, "abort")[0]


def test_armed_kinds_expect_the_turn_task():
    text = _log(BOOT_PANIC, CRASH_NEW.replace("task=loopTask", "task=tg_poll"), BT, WHY)
    rep = boot_report(split_boots(parse_log(text))[0])
    assert crash_verdict(rep, "nullptr", armed=True)[0]
    assert not crash_verdict(rep, "nullptr")[0]  # the immediate form runs on the console task
    rep_loop = boot_report(split_boots(parse_log(_log(BOOT_PANIC, CRASH_NEW, BT, WHY)))[0])
    assert not crash_verdict(rep_loop, "canary", armed=True)[0]


def test_stale_verdict():
    stale_boot = "0 500 boot boot: n=18 fw=v build=b reason=software-restart(0x0c) wake=none mode=orch t=0 prev=none"
    stale_crash = CRASH_NEW.replace("new=1 this-reset=1", "new=0 this-reset=0")
    rep = boot_report(split_boots(parse_log(_log(stale_boot, stale_crash)))[0])
    assert stale_verdict(rep) == (True, "ok")
    rep = boot_report(split_boots(parse_log(_log(stale_boot, stale_crash, BT, WHY)))[0])
    assert not stale_verdict(rep)[0]  # a stale dump must not repeat its backtrace
    rep = boot_report(split_boots(parse_log(_log(stale_boot)))[0])
    assert not stale_verdict(rep)[0]  # a stored dump must be mentioned


def test_bad_image_line_parses():
    bad = "4 900 boot crash: image=bad-crc fp=deadbeef new=1 this-reset=1 size=65536/65536"
    rep = boot_report(split_boots(parse_log(_log(BOOT_PANIC, bad)))[0])
    assert rep["crash"] is None and rep["bad"]["state"] == "bad-crc"


def test_every_boot_starts_at_seq0():
    good = split_boots(parse_log(_log(BOOT_PANIC, CRASH_NEW, BOOT_PANIC)))
    assert every_boot_starts_at_seq0(good)[0]
    hole = split_boots(parse_log(_log(BOOT_PANIC, BOOT_PANIC.replace("0 812", "6 812", 1))))
    assert not every_boot_starts_at_seq0(hole)[0]


def test_boot_record_with_unknown_number_parses():
    rep = boot_report(split_boots(parse_log(_log(BOOT_PANIC.replace("n=17", "n=?"))))[0])
    assert rep["boot"]["n"] == "?"


def test_not_a_boot_record_is_loud():
    with pytest.raises(ValueError):
        boot_report([{"seq": 0, "ms": 0, "cat": "boot", "msg": "boot: garbage"}])
