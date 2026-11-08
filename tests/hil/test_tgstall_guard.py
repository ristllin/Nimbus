"""TGSTALL - a silent poll task restarts the device within minutes and says why.

On 4 Nov 2026 the owner's device (Lumi) stopped answering for 28 hours: the Telegram poll
task, which runs every turn, stopped inside a turn and nothing noticed (two more stalls of
the same shape followed). The liveness guard (lib/core ``nimbus/sys/poll_guard.h``,
``src/sys/poll_watch.cpp``) restarts the device when the poll task has been silent for
5 minutes, with a panic whose reason names the stall, so the poison guard counts a try and
the next boot's ``crash-why`` carries the stuck task's PC.

The test console's ``TGSTALL spin|block`` (NIMBUS_TEST only) stops the poll task at its
next loop top, on the CPU or blocked. Each leg arms one, waits out the guard and the
reboot, and judges the crash report of the next boot (``stall_verdict``, host-tested in
test_tgstall_guard_host.py): reason ``crash(panic)``, task ``loopTask`` (the guard runs on
the loop task), ``crash-why`` starting ``poll watchdog: tg_poll silent <s> s after test
stall`` with the silence at the 300 s limit and the spin or wait judgement right, and
``TGSTALL?`` reads ``afterStall=1 streak=1`` on the new boot.

Hardware legs: ``hil`` + ``net`` (the console verbs over serial, the report over the
LAN). Each takes about 6 minutes.
"""

from __future__ import annotations

import re
from typing import Optional

import pytest

from test_l39_crash_forensics import (
    _await_crash_reboot,
    _boot_n,
    _crash_report,
    _lan_ip,
)

GUARD_LIMIT_S = 300
GUARD_SLACK_S = 30  # the guard checks once a second; the beat before the stall is the loop top
STALL_WHY_RE = re.compile(
    r"^poll watchdog: tg_poll silent (?P<silent>\d+) s after (?P<step>[^,]+), "
    r"(?P<judge>on the CPU (?P<on>\d+)/(?P<n>\d+) \((?P<kind>a spin|waiting)\)|the task is gone)"
)


def stall_verdict(rep: dict, kind: str) -> "tuple[bool, str]":
    """The class rule for the boot right after a ``TGSTALL <kind>`` (spin or block)."""
    b, c, why = rep["boot"], rep["crash"], rep["why"]
    if b["slug"] != "crash(panic)":
        return False, f"boot reason {b['slug']!r}, want 'crash(panic)' (the guard panics on purpose)"
    if c is None or (c["new"], c["this"]) != ("1", "1"):
        return False, f"no new crash report for this reset: {c!r}"
    if c["task"] != "loopTask":
        return False, f"crash task {c['task']!r}, want 'loopTask' (the guard runs on the loop task)"
    m = STALL_WHY_RE.match(why or "")
    if not m:
        return False, f"crash-why is not the guard's: {why!r}"
    silent = int(m.group("silent"))
    if not GUARD_LIMIT_S <= silent <= GUARD_LIMIT_S + GUARD_SLACK_S:
        return False, f"silent {silent} s, want {GUARD_LIMIT_S} to {GUARD_LIMIT_S + GUARD_SLACK_S} s"
    if m.group("step") != "test stall":
        return False, f"last step {m.group('step')!r}, want 'test stall'"
    want = "a spin" if kind == "spin" else "waiting"
    if m.group("kind") != want:
        return False, f"judged {m.group('kind')!r}, want {want!r}"
    if "pc 0x" not in why:
        return False, "no PC of the stuck task in crash-why"
    return True, "ok"


def _require_tgstall_verb(device) -> None:
    device.drain(quiet=0.2)
    device.send("TGSTALL?")
    try:
        device.expect("TGSTALL silent=", timeout=3.0)
    except Exception:  # noqa: BLE001 - no answer means the verb is absent
        pytest.skip("the test console has no TGSTALL verb (needs a lane TGSTALL test image)")


def _tgstall_status(device) -> str:
    device.drain(quiet=0.2)
    device.send("TGSTALL?")
    return device.expect("TGSTALL silent=", timeout=10.0)


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.parametrize("kind", ("spin", "block"))
def test_silent_poll_task_restarts_and_says_why(device, net, kind):
    _require_tgstall_verb(device)
    ip = _lan_ip(None)
    before: Optional[int] = _boot_n(net, ip)
    device.drain(quiet=0.2)
    device.send(f"TGSTALL {kind}")
    line = device.expect("TGSTALL", timeout=5.0)
    assert f"armed={kind}" in line, f"TGSTALL {kind} refused: {line!r}"
    ip2 = _await_crash_reboot(device, net, ip, before, GUARD_LIMIT_S + 120.0)
    rep = _crash_report(net, _lan_ip(ip2), before)
    print(f"[TGSTALL] {kind}: boot={rep['boot']} crash={rep['crash']} why={rep['why']!r}")
    ok, why = stall_verdict(rep, kind)
    assert ok, f"TGSTALL {kind}: {why}"
    status = _tgstall_status(device)
    assert "afterStall=1" in status and "streak=1" in status, f"the new boot does not know: {status!r}"
