"""L66 - a slow tool never stalls the main loop into the task watchdog.

The owner's device reset with ``reason=crash(watchdog)`` about 3.6 h after every boot
on 21-22 Oct 2026. The core dump it kept (read back after its v4.5.15 update) says
``task=IDLE1`` with ``crash-why: Task watchdog got triggered ... - loopTask (CPU 1)``:
CPU 1 sat idle because the main loop, the only task the watchdog watches, was blocked.
Every one of its 21 logged boots ended inside a turn's tool dispatch, which holds the
shared card lock (``agent::memory::lock``) for the whole dispatch, and the loop took
that lock with an unbounded wait on every pass (``tickSdHealth``). The fix: the loop
only tries for the lock (``LoopLock``, ``nimbus/sys/loop_guard.h``) and skips or parks
its card work while a tool holds it.

Both legs are ``hil`` + ``net`` (the console verbs over serial, ``/api/chat``,
``/api/state`` and ``/api/errlog`` over the LAN) and bill nothing: a canned provider
round (``PROVFAULT``) asks for ``memory.scratchpad`` and ``TOOLSTALL <ms>`` makes that
one dispatch hold the card lock for ``<ms>``, the way a slow tool (an embedding over a
slow link, a long card scan) does.

* ``test_slow_tool_leaves_the_loop_running`` (the fixed image): the console answers while
  the tool holds the lock (the loop runs), no new boot starts, ``LOOPGUARD?`` reads
  ``blocking=0`` with ``busy`` skips counted, the longest loop pass under the slow-pass
  bar and the hold named; the durable log carries the hold line.
* ``test_before_fix_image_resets_like_lumi`` (``NIMBUS_L66_BEFORE=1``, the v4.5.15 base
  with only the TOOLSTALL seam): the same stall resets the board through the task
  watchdog with the owner's signature: ``crash(watchdog)``, ``this-reset=1`` and a
  crash-why naming ``loopTask (CPU 1)``.

The parsers and verdicts are host-tested in test_l66_loop_lock_stall_host.py.
"""

from __future__ import annotations

import json
import os
import re
import time
from typing import Dict, Optional

import pytest

from device import ExpectTimeout

STALL_MS = 15000  # past the 8 s watchdog with room for the 20 s probe cadence
SLOW_PASS_MS = 2000  # nimbus::sys::kSlowPassMs
LONG_HOLD_MS = 3000  # nimbus::sys::kLongHoldMs
MODEL = os.environ.get("NIMBUS_BENCH_MODEL", "claude-haiku-5-5")  # cosmetic in the canned round
STALL_TOOL = "memory.scratchpad"

LOOPGUARD_RE = re.compile(
    r"LOOPGUARD blocking=(?P<blocking>\d+) caller=0x(?P<caller>[0-9a-f]{8}) busy=(?P<busy>\d+) "
    r"parked=(?P<parked>\d+) dropped=(?P<dropped>\d+) longHolds=(?P<longHolds>\d+) "
    r"longestHoldMs=(?P<longestHoldMs>\d+) tool=(?P<tool>\S+) maxPassMs=(?P<maxPassMs>\d+) "
    r"slowPasses=(?P<slowPasses>\d+)"
)
HOLD_LINE_RE = re.compile(r"memory: tool (?P<tool>\S+) held the card lock for (?P<ms>\d+) ms")


# ---- pure parsers and verdicts (host-tested) -------------------------------------
def parse_loopguard(line: str) -> Optional[Dict[str, object]]:
    """``LOOPGUARD? `` reply -> counters (ints, ``tool`` and ``caller`` as text)."""
    m = LOOPGUARD_RE.search(line or "")
    if not m:
        return None
    out: Dict[str, object] = {k: int(v) for k, v in m.groupdict().items() if k not in ("tool", "caller")}
    out["tool"] = m.group("tool")
    out["caller"] = m.group("caller")
    return out


def tool_round(model: str = MODEL) -> str:
    """One canned Anthropic round that asks for the stall tool (and nothing else)."""
    body = {
        "id": "msg_l66",
        "type": "message",
        "role": "assistant",
        "model": model,
        "content": [
            {"type": "tool_use", "id": "toolu_l66", "name": STALL_TOOL.replace(".", "_"), "input": {"action": "view"}}
        ],
        "stop_reason": "tool_use",
        "usage": {"input_tokens": 10, "output_tokens": 10},
    }
    return json.dumps(body, separators=(",", ":"))


def loop_verdict(before: Dict[str, object], after: Dict[str, object], stall_ms: int) -> "tuple[bool, str]":
    """The fixed image while a tool held the card lock for ``stall_ms``."""
    if int(after["blocking"]) != int(before["blocking"]):
        return False, f"the loop waited for the card lock {int(after['blocking']) - int(before['blocking'])} time(s)"
    if int(after["busy"]) <= int(before["busy"]):
        return False, "no busy skip counted: the stall never met the loop (did the tool run?)"
    if int(after["maxPassMs"]) >= SLOW_PASS_MS:
        return False, f"a loop pass took {after['maxPassMs']} ms (the loop stalled)"
    if int(after["longHolds"]) <= int(before["longHolds"]):
        return False, "the long hold was not recorded"
    if int(after["longestHoldMs"]) < stall_ms or after["tool"] != STALL_TOOL:
        return False, f"longest hold {after['longestHoldMs']} ms by {after['tool']}, want >= {stall_ms} by {STALL_TOOL}"
    return True, "loop kept running"


def lumi_signature_verdict(rep: dict) -> "tuple[bool, str]":
    """A crash boot record (test_l39 ``boot_report``) with the owner's 22 Oct signature."""
    boot, crash, why = rep.get("boot") or {}, rep.get("crash") or {}, rep.get("why") or ""
    if boot.get("slug") != "crash(watchdog)":
        return False, f"boot reason {boot.get('slug')!r}, want crash(watchdog)"
    if crash.get("this") != "1":
        return False, f"crash record this-reset={crash.get('this')!r}, want 1"
    if "loopTask (CPU 1)" not in why:
        return False, f"crash-why {why!r} does not name loopTask (CPU 1)"
    return True, "watchdog reset with the loop blocked"


# ---- device helpers ---------------------------------------------------------------
def _loopguard(device) -> Dict[str, object]:
    device.drain(quiet=0.2)
    device.send("LOOPGUARD?")
    try:
        line = device.expect("LOOPGUARD", timeout=5.0)
    except ExpectTimeout:
        pytest.skip("this image has no LOOPGUARD? verb (the lane WDT test image)")
    got = parse_loopguard(line)
    if got is None:
        pytest.skip(f"LOOPGUARD? reply not understood: {line!r}")
    return got


def _arm(device, stall_ms: int) -> None:
    device.drain(quiet=0.2)
    device.send(f"TOOLSTALL {stall_ms}")
    try:
        line = device.expect("TOOLSTALL", timeout=5.0)
    except ExpectTimeout:
        pytest.skip("this image has no TOOLSTALL verb (the lane WDT test image)")
    assert "armed" in line, f"TOOLSTALL refused: {line!r}"


def _provfault_on(device) -> None:
    device.drain(quiet=0.2)
    device.send("PROVFAULT api.anthropic.com 200 " + tool_round())
    try:
        armed = device.expect_re(r"PROVFAULT (api\.anthropic\.com 200 armed|not wired)", timeout=10.0)
    except ExpectTimeout:
        pytest.skip("this image has no PROVFAULT console verb")
    if "not wired" in armed.group(0):
        pytest.skip("PROVFAULT is not wired in this image")


def _cleanup(device, net, ip: str, prio: str) -> None:
    for line, want in (("PROVFAULT off", "PROVFAULT"), ("TOOLSTALL 0", "TOOLSTALL")):
        try:
            device.cmd(line, want, timeout=5.0)
        except Exception as exc:  # noqa: BLE001 - reported; the next leg checks its own state
            print(f"[L66] cleanup {line!r}: {exc}")
    try:
        net.post("/api/orch", {"provPrio": prio}, ip=ip, timeout=8.0)
    except Exception as exc:  # noqa: BLE001
        print(f"[L66] cleanup provPrio: {exc}")


def _start_stall_turn(device, net, ip: str) -> str:
    """Arm the stall and the canned round, then start one web-chat turn."""
    prio = str(net.get_json("/api/orch", ip=ip, timeout=8.0).get("provPrio") or "")
    net.post("/api/orch", {"provPrio": "anthropic"}, ip=ip, timeout=8.0)
    _arm(device, STALL_MS)
    _provfault_on(device)
    r = net.post("/api/chat", {"text": "L66 stall probe: look at your scratchpad."}, ip=ip, timeout=10.0)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}: {r.text[:200]}"
    return prio


def _wait_for_stall_start(device, timeout: float = 60.0) -> None:
    device.expect("TOOLSTALL:", timeout=timeout)  # the dispatch took the lock and holds it


def _wait_turn_end(device, timeout: float = 90.0) -> None:
    """The canned round repeats until the turn's round cap ends it; clear PROVFAULT only
    after that, so no later round of this turn reaches the real provider (and bills)."""
    try:
        device.expect_re(r"turn end: (ok|FAIL)", timeout=timeout)
    except ExpectTimeout:
        print(f"[L66] no turn end within {timeout:.0f} s; clearing PROVFAULT anyway")


# ---- HIL legs -----------------------------------------------------------------------
@pytest.mark.hil
@pytest.mark.net
def test_slow_tool_leaves_the_loop_running(device, net):
    from test_l39_crash_forensics import _boot_n, _errlog_file, _lan_ip

    if os.environ.get("NIMBUS_L66_BEFORE") == "1":
        pytest.skip("NIMBUS_L66_BEFORE=1: the before-fix image runs the reset leg instead")
    ip = _lan_ip(None)
    before_n = _boot_n(net, ip)
    before = _loopguard(device)
    prio = _start_stall_turn(device, net, ip)
    try:
        _wait_for_stall_start(device)
        # Inside the hold, past the 8 s watchdog: the loop still runs the console.
        time.sleep(10.0)
        assert device.ping(timeout=3.0), "the console stopped answering while the tool held the card lock"
        time.sleep(STALL_MS / 1000.0)  # the hold ends; the next pass takes the lock again
        after = _loopguard(device)
        print(f"[L66] before={before} after={after}")
        assert _boot_n(net, ip) == before_n, "the board restarted during the stall"
        ok, why = loop_verdict(before, after, STALL_MS)
        assert ok, why
        log = _errlog_file(net, ip)
        holds = [m for m in HOLD_LINE_RE.finditer(log) if m.group("tool") == STALL_TOOL]
        assert holds and int(holds[-1].group("ms")) >= STALL_MS, "the durable log does not name the long hold"
    finally:
        _wait_turn_end(device)
        _cleanup(device, net, ip, prio)


@pytest.mark.hil
@pytest.mark.net
def test_before_fix_image_resets_like_lumi(device, net):
    """Run only against the before-fix image (v4.5.15 + the TOOLSTALL seam alone)."""
    from test_l39_crash_forensics import NoCrash, _await_crash_reboot, _boot_n, _crash_report, _lan_ip

    if os.environ.get("NIMBUS_L66_BEFORE") != "1":
        pytest.skip("set NIMBUS_L66_BEFORE=1 with the before-fix image to reproduce the reset")
    ip = _lan_ip(None)
    before_n = _boot_n(net, ip)
    prio = _start_stall_turn(device, net, ip)
    try:
        _wait_for_stall_start(device)
        try:
            ip2 = _await_crash_reboot(device, net, ip, before_n, 60.0)
        except NoCrash as exc:
            pytest.fail(f"the stall did not reset the before-fix image: {exc}")
        ip = _lan_ip(ip2)
        rep = _crash_report(net, ip, before_n)
        print(f"[L66] before-fix reset: boot={rep['boot']} crash={rep['crash']} why={rep['why']!r}")
        ok, why = lumi_signature_verdict(rep)
        assert ok, why
    finally:
        _cleanup(device, net, ip, prio)
