"""Host tests for the TGSTALL verdict (no board): the crash-why shape is the one
lib/core nimbus/sys/poll_guard.h pollStallReason writes (pinned by test/test_poll_guard)."""

from __future__ import annotations

import pytest

from test_tgstall_guard import stall_verdict

pytestmark = pytest.mark.host

SPIN_WHY = (
    "poll watchdog: tg_poll silent 301 s after test stall, on the CPU 290/291 (a spin), "
    "pc 0x4200a1b2 bt 0x4200a1b2 0x42011c3d"
)
BLOCK_WHY = (
    "poll watchdog: tg_poll silent 300 s after test stall, on the CPU 0/291 (waiting), "
    "pc 0x40380abc bt 0x40380abc 0x4200f00d"
)


def report(why, slug="crash(panic)", task="loopTask", new="1"):
    return {
        "boot": {"slug": slug},
        "crash": {"new": new, "this": new, "task": task},
        "why": why,
    }


def test_spin_and_block_pass():
    assert stall_verdict(report(SPIN_WHY), "spin") == (True, "ok")
    assert stall_verdict(report(BLOCK_WHY), "block") == (True, "ok")


@pytest.mark.parametrize(
    "rep, kind, needle",
    [
        (report(SPIN_WHY), "block", "judged"),
        (report(BLOCK_WHY), "spin", "judged"),
        (report(SPIN_WHY, slug="crash(watchdog)"), "spin", "boot reason"),
        (report(SPIN_WHY, task="tg_poll"), "spin", "crash task"),
        (report(SPIN_WHY, new="0"), "spin", "no new crash"),
        (report("abort() was called at PC 0x40377853 on core 0"), "spin", "not the guard"),
        (report(SPIN_WHY.replace("silent 301 s", "silent 299 s")), "spin", "silent 299"),
        (report(SPIN_WHY.replace("silent 301 s", "silent 900 s")), "spin", "silent 900"),
        (report(SPIN_WHY.replace("after test stall", "after poll loop")), "spin", "last step"),
        (report(SPIN_WHY.split(", pc")[0]), "spin", "no PC"),
        (report(None), "spin", "not the guard"),
    ],
)
def test_each_wrong_report_fails_with_its_reason(rep, kind, needle):
    ok, why = stall_verdict(rep, kind)
    assert not ok
    assert needle in why
