"""Host tests for the L39 crash-forensics parsers and verdicts (no board).

The fixtures are synthetic lines in the exact shapes src/sys/crash_report.cpp and
src/sys/errlog_fs.cpp write (the formatters are pinned by test/test_crash_forensics and
test/test_errlog), so a renamed field fails here instead of passing a bench run vacuously.
"""

from __future__ import annotations

import pytest

import test_l39_crash_forensics as l39
from test_l39_crash_forensics import (
    boot_report,
    crash_verdict,
    every_boot_starts_at_seq0,
    first_boot_after,
    latest_turn_crash,
    offline_rows_verdict,
    parse_log,
    poll_until,
    split_boots,
    stale_verdict,
    starts_mid_boot,
    stitch_rotated,
)

pytestmark = pytest.mark.host

# The transient errors poll_until retries. With requests installed (the bench) they are
# its connect/read failures; CI's gate-logic job has only pytest, where the module falls
# back to OSError, so the builtin connect/timeout errors stand in for them.
try:
    import requests

    CONNECT_TIMEOUT = requests.exceptions.ConnectTimeout
    REFUSED = requests.exceptions.ConnectionError
    READ_TIMEOUT = requests.exceptions.ReadTimeout
except ImportError:
    CONNECT_TIMEOUT, REFUSED, READ_TIMEOUT = TimeoutError, ConnectionRefusedError, TimeoutError

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


def _mark_names(obj) -> set:
    marks = getattr(obj, "pytestmark", [])
    return {m.name for m in (marks if isinstance(marks, list) else [marks])}


def test_destructive_legs_stay_out_of_the_hil_and_net_selections():
    """The gate runs L39 with ``-m hil`` and the LAN suite with ``-m "net and not manual"``:
    neither may pick a leg that factory-resets the board or rolls back its firmware. Every
    leg is classified, so a new leg without its marks fails here."""
    module = _mark_names(l39)
    legs = {n: f for n, f in vars(l39).items() if n.startswith("test_") and callable(f)}
    assert len(legs) >= 9, f"expected the L39 legs, found {sorted(legs)}"
    destructive = []
    for name, fn in legs.items():
        marks = module | _mark_names(fn)
        if "destructive" in marks:
            destructive.append(name)
            assert not marks & {"hil", "net"}, f"{name}: a destructive leg also carries {marks & {'hil', 'net'}}"
        else:
            assert {"hil", "net"} <= marks, f"{name}: a non-destructive leg needs hil and net, has {marks}"
    assert sorted(destructive) == ["test_early_boot_abort_is_explained", "test_factory_reset_erases_the_dump"]


# ---- every PANIC kind, as the firmware writes it ------------------------------------
# kind, armed, boot slug (raw), crashing task, cause, cause name, crash-why. The reasons
# are the ones each kind really produces: abort() and the stack-overflow hook record a
# panic message, the task watchdog names the late tasks, and a CPU exception or the
# end-of-stack watchpoint records none, so the firmware states the exception cause.
KIND_TABLE = [
    (
        "abort",
        False,
        "crash(panic)(0x0c)",
        "loopTask",
        0,
        "IllegalInstruction",
        "abort() was called at PC 0x42001234 on core 1",
    ),
    (
        "nullptr",
        False,
        "crash(panic)(0x0c)",
        "loopTask",
        29,
        "StoreProhibited",
        "StoreProhibited in loopTask: a write to an invalid address (0x00000000); no panic message in the dump",
    ),
    (
        "canary",
        False,
        "crash(panic)(0x0c)",
        "loopTask",
        65,
        "DebugException",
        "DebugException in loopTask: the end-of-stack watchpoint fired (a stack overflow); no panic message in the dump",
    ),
    (
        "twdt",
        False,
        "crash(watchdog)(0x07)",
        "IDLE0",
        0,
        "IllegalInstruction",
        "Task watchdog got triggered. The following tasks did not reset the watchdog in time: loopTask (CPU 1)",
    ),
    (
        "oom",
        False,
        "crash(panic)(0x0c)",
        "loopTask",
        29,
        "StoreProhibited",
        "StoreProhibited in loopTask: a write to an invalid address (0x00000000); no panic message in the dump",
    ),
    (
        "nullptr",
        True,
        "crash(panic)(0x0c)",
        "tg_poll",
        29,
        "StoreProhibited",
        "StoreProhibited in tg_poll: a write to an invalid address (0x00000000); no panic message in the dump",
    ),
    (
        "abort",
        True,
        "crash(panic)(0x0c)",
        "tg_poll",
        0,
        "IllegalInstruction",
        "abort() was called at PC 0x42005678 on core 0",
    ),
    (
        "canary",
        True,
        "crash(panic)(0x0c)",
        "tg_poll",
        0,
        "IllegalInstruction",
        "***ERROR*** A stack overflow in task tg_poll has been detected.",
    ),
]
KIND_IDS = [f"{k[0]}{'-armed' if k[1] else ''}" for k in KIND_TABLE]
FP = "4b00ab64"


def _boot(n: int, reason: str) -> str:
    return f"0 800 boot boot: n={n} fw=v4.5.14 build=b reason={reason} wake=none mode=orch t=1792394441 prev=none"


def _crash(task: str, cause: int, cname: str, new: str) -> str:
    return (
        f"3 900 boot crash: fp={FP} new={new} this-reset={new} task={task} pc=0x4219718c cause={cause}({cname}) "
        "vaddr=0x00000000 size=46660/65536 elf=76226e4e0 fw-match=1"
    )


def _kind_log(row, harness_boots: int) -> str:
    """A boot before the panic (n=40), the crash boot (n=41), then the boots a host
    serial reopen adds on the native USB (reason usb, the dump already reported)."""
    _, _, reason, task, cause, cname, why = row
    lines = [
        _boot(40, "software-restart(0x0c)"),
        _boot(41, reason),
        _crash(task, cause, cname, "1"),
        "4 901 boot crash-bt: depth=3 corrupt=0 0x4219718c,0x4200f00d,0x4200beef",
        f"5 901 boot crash-why: {why}",
    ]
    for i in range(harness_boots):
        lines += [_boot(42 + i, "usb(0x15)"), _crash(task, cause, cname, "0")]
    return _log(*lines)


@pytest.mark.parametrize("row", KIND_TABLE, ids=KIND_IDS)
@pytest.mark.parametrize("harness_boots", [0, 1, 2])
def test_every_kind_is_judged_on_its_crash_boot(row, harness_boots):
    kind, armed = row[0], row[1]
    boots = split_boots(parse_log(_kind_log(row, harness_boots)))
    rep, later = first_boot_after(boots, 40)
    assert rep["boot"]["n"] == "41" and later == ["usb"] * harness_boots
    assert crash_verdict(rep, kind, armed=armed) == (True, "ok")
    if harness_boots:  # counter-check: the newest boot is the harness's usb reset
        ok, msg = crash_verdict(boot_report(boots[-1]), kind, armed=armed)
        assert not ok and "usb" in msg


@pytest.mark.parametrize("row", KIND_TABLE, ids=KIND_IDS)
def test_every_kind_needs_a_reason(row):
    kind, armed, why = row[0], row[1], row[6]
    text = _kind_log(row, 0).replace(why, "(no reason recorded)")
    rep, _ = first_boot_after(split_boots(parse_log(text)), 40)
    ok, msg = crash_verdict(rep, kind, armed=armed)
    assert not ok and "crash-why" in msg


def test_no_boot_after_the_panic_means_no_crash_record():
    boots = split_boots(parse_log(_log(_boot(40, "software-restart(0x0c)"))))
    assert first_boot_after(boots, 40) == (None, [])
    rep, later = first_boot_after(boots, None)  # nothing to compare: the newest boot
    assert rep["boot"]["n"] == "40" and later == []


def test_latest_turn_crash_skips_the_harness_boots():
    armed_canary = KIND_TABLE[-1]
    boots = split_boots(parse_log(_kind_log(armed_canary, 2)))
    assert latest_turn_crash(boots)["boot"]["n"] == "41"
    loop_only = split_boots(parse_log(_kind_log(KIND_TABLE[1], 1)))
    assert latest_turn_crash(loop_only) is None


class _Clock:
    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s


def test_poll_retries_connection_errors_within_the_budget():
    clock, calls = _Clock(), []

    def fetch():
        calls.append(clock.t)
        if len(calls) <= 2:
            raise CONNECT_TIMEOUT("rejoining Wi-Fi")
        if len(calls) == 3:
            raise REFUSED("refused")
        return "clock never synced, clock never synced"

    value, err = poll_until(fetch, lambda t: t.count("never synced") >= 2, 120.0, 5.0, clock.now, clock.sleep)
    assert value.count("never synced") == 2 and err is None and len(calls) == 4


def test_poll_gives_up_at_the_budget_with_the_last_error():
    clock, calls = _Clock(), []

    def fetch():
        calls.append(clock.t)
        raise READ_TIMEOUT("still booting")

    value, err = poll_until(fetch, lambda _: True, 120.0, 5.0, clock.now, clock.sleep)
    assert value is None and isinstance(err, READ_TIMEOUT)
    assert clock.t >= 120.0 and len(calls) == 25  # every 5 s from 0 through 120


def test_poll_lets_a_real_error_through():
    def fetch():
        raise ValueError("a bug in the test, not the network")

    with pytest.raises(ValueError):
        poll_until(fetch, lambda _: True, 10.0, 5.0, _Clock().now, _Clock().sleep)


# ---- reading the durable log: a busy answer and a rotation are not "no boot record" --
class _Resp:
    def __init__(self, status: int, text: str):
        self.status_code, self.text = status, text


class _LogNet:
    """GET /api/errlog as the device answers it: scripted answers per path, in order."""

    def __init__(self, answers: dict):
        self.answers = {k: list(v) for k, v in answers.items()}
        self.paths: list = []

    def get(self, path, ip=None, timeout=5.0):
        self.paths.append(path)
        queue = self.answers[path]
        return queue.pop(0) if len(queue) > 1 else queue[0]


ACTIVE, PREVIOUS = "/api/errlog", "/api/errlog?file=nimbus.log.1"
CRASH_BOOT = _log(BOOT_PANIC, CRASH_NEW, BT, WHY)
# The active file after a rotation inside the crash boot: the boot's later lines only.
MID_BOOT = _log("41 9000 - [sram] steady-orch intFree=34764", "42 9100 - relay: online=1 cause=hello-ack")


def test_a_file_that_starts_mid_boot_is_stitched_onto_the_previous_one():
    assert not starts_mid_boot(CRASH_BOOT)
    assert starts_mid_boot(MID_BOOT) and starts_mid_boot("")
    assert stitch_rotated("anything", CRASH_BOOT) == CRASH_BOOT  # a whole active file stands alone
    assert stitch_rotated("", MID_BOOT) == MID_BOOT  # nothing rotated yet
    stitched = stitch_rotated(CRASH_BOOT.rstrip("\n"), MID_BOOT)
    boots = split_boots(parse_log(stitched))
    assert len(boots) == 1 and len(boots[0]) == 6  # the crash boot, with the lines after the rotation


def test_boots_survive_a_busy_answer_and_a_rotation(monkeypatch):
    monkeypatch.setattr(l39, "ERRLOG_RETRY_S", 0.0)
    busy = _Resp(503, "Device busy, try again.")
    net = _LogNet({ACTIVE: [busy, busy, _Resp(200, MID_BOOT)], PREVIOUS: [_Resp(200, CRASH_BOOT)]})
    boots = l39._boots(net, "192.0.2.1")
    assert crash_verdict(boot_report(boots[-1]), "abort") == (True, "ok")
    assert net.paths == [ACTIVE, ACTIVE, ACTIVE, PREVIOUS]


def test_a_whole_active_file_needs_no_previous_read(monkeypatch):
    monkeypatch.setattr(l39, "ERRLOG_RETRY_S", 0.0)
    net = _LogNet({ACTIVE: [_Resp(200, CRASH_BOOT)]})
    assert len(l39._boots(net, "192.0.2.1")) == 1 and net.paths == [ACTIVE]


def test_a_refused_read_fails_with_its_status_not_as_an_empty_log(monkeypatch):
    monkeypatch.setattr(l39, "ERRLOG_RETRY_S", 0.0)
    net = _LogNet({ACTIVE: [_Resp(401, '{"error":"Access token required."}')]})
    with pytest.raises(AssertionError, match="-> 401"):
        l39._boots(net, "192.0.2.1")


def test_no_rotated_file_yet_reads_empty(monkeypatch):
    monkeypatch.setattr(l39, "ERRLOG_RETRY_S", 0.0)
    net = _LogNet({ACTIVE: [_Resp(200, MID_BOOT)], PREVIOUS: [_Resp(404, '{"error":"no such log file"}')]})
    with pytest.raises(AssertionError, match="no boot record"):
        l39._boots(net, "192.0.2.1")


# ---- offline boots: what the first sync writes ---------------------------------------
def _row(n: int, text: str) -> dict:
    return {"id": f"m{n:08x}", "session": "system", "role": "system", "kind": "log", "text": text, "tags": "ev:boot"}


BOOTED = (
    'Booted v4.5.13 (v4.5.13-112-gb0aefd52) reason=usb mode=orchestrator crash=tg_poll@0x421cb2fe "StoreProhibited"'
)
# The page the 22 Oct unit L run read after two cold boots (texts shortened): the boot
# that synced (f76) and the one earlier offline boot (f75), above the rows before the leg.
PAGE_22_OCT = {
    "messages": [
        _row(0xF76, BOOTED + " [pre-sync, stamped at first clock sync]"),
        _row(0xF75, BOOTED + " [from boot n=278, clock never synced; stamped at first clock sync]"),
        _row(0xF74, BOOTED.replace("reason=usb", "reason=crash(panic)")),
        _row(0xF71, "Booted v4.5.13 (v4.5.13-112-gb0aefd52) reason=crash(panic) mode=orchestrator"),
    ]
}
SEEN_22_OCT = {"m00000f74", "m00000f71"}


def test_two_cold_boots_give_one_earlier_row_and_the_syncing_boots_own():
    """The 22 Oct page is the device doing the right thing for two cold boots; the old
    leg counted both as "never synced" and failed it."""
    assert offline_rows_verdict(PAGE_22_OCT, SEEN_22_OCT, 1)[0]
    ok, why = offline_rows_verdict(PAGE_22_OCT, SEEN_22_OCT, 2)
    assert not ok and "1 earlier offline boot(s) [278]" in why


def test_three_cold_boots_need_two_distinct_earlier_boots_and_the_own_row():
    page = {
        "messages": [_row(0xF80, BOOTED + " [from boot n=280, clock never synced; stamped]")] + PAGE_22_OCT["messages"]
    }
    assert offline_rows_verdict(page, SEEN_22_OCT, 2)[0]
    twice = {"messages": [_row(0xF80, PAGE_22_OCT["messages"][1]["text"])] + PAGE_22_OCT["messages"]}
    assert not offline_rows_verdict(twice, SEEN_22_OCT, 2)[0]  # the same boot twice is one boot
    no_own = {"messages": [m for m in page["messages"] if "pre-sync, stamped" not in m["text"]]}
    ok, why = offline_rows_verdict(no_own, SEEN_22_OCT, 2)
    assert not ok and "no row of its own" in why


def test_rows_from_before_the_leg_never_count():
    """A previous run's "never synced" rows sit on the same 40-row page."""
    seen = SEEN_22_OCT | {"m00000f75", "m00000f76"}
    ok, why = offline_rows_verdict(PAGE_22_OCT, seen, 1)
    assert not ok and "0 earlier offline boot(s)" in why


# ---- a console that stays silent after a crash reset ---------------------------------
class _SilentDevice:
    """The ROM banner arrives, then the app's boot never reaches the host."""

    def __init__(self):
        self.calls: list = []

    def expect(self, substr, timeout=5.0):
        self.calls.append(f"expect {substr}")
        return "rst:0xc (RTC_SW_CPU_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)"

    def wait_ready(self, timeout=20.0, max_total=None):
        raise l39.ExpectTimeout("READY beacon", ["entry 0x403c88b8"])

    def close(self):
        self.calls.append("close")

    def bus_reset(self, *a, **k):
        self.calls.append("bus_reset")
        return True

    def reopen_after_reenumerate(self, drain_boot=True):
        self.calls.append("reopen")
        return self

    def ping(self, timeout=3.0):
        return True


class _StateNet:
    def __init__(self, n):
        self.n = n

    def get_json(self, path, ip=None, timeout=5.0):
        return {"boot": {"n": self.n}}


def test_a_crash_boot_the_console_missed_is_judged_over_the_lan_and_the_console_won_back():
    dev = _SilentDevice()
    assert l39._await_crash_reboot(dev, _StateNet(277), "192.0.2.1", 276, 10.0) is None
    assert dev.calls[-3:] == ["close", "bus_reset", "reopen"]


def test_a_console_that_does_not_come_back_fails_loudly_at_the_reboot():
    """The crash boot answered over the LAN but the bus reset did not win the console
    back: fail here, naming the wedge, not at the next leg's first command."""
    dev = _SilentDevice()
    dev._transcript = ["entry 0x403c88b8"]
    dev.ping = lambda timeout=3.0: False
    with pytest.raises(l39.ExpectTimeout, match="USB wedge"):
        l39._await_crash_reboot(dev, _StateNet(277), "192.0.2.1", 276, 10.0)


def test_a_silent_console_with_no_new_boot_is_still_a_failure(monkeypatch):
    monkeypatch.setattr(l39, "_new_boot_seen", lambda net, ip, before_n, budget_s: False)
    with pytest.raises(l39.ExpectTimeout):
        l39._await_crash_reboot(_SilentDevice(), _StateNet(276), "192.0.2.1", 276, 10.0)
