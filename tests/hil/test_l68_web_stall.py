"""L68 - no web request can stall the device into a watchdog reset.

The owner's device reset with ``reason=crash(watchdog)`` on v4.6.0 (26 Oct 2026,
17:19:29Z): ``task=IDLE0``, ``crash-why: Task watchdog got triggered ... - async_tcp
(CPU 0)``. CPU 0 sat idle because the web server's task, which the task watchdog
watches, was blocked: a GET /api/state waited (``portMAX_DELAY``) for the shared card
lock (``agent::memory::lock``) in ``files::storageTruth`` while a turn's ``image.look``
held that lock across a photo caption request. The fix (``nimbus/sys/web_guard.h``): a
web request only tries for the card lock (``WebLock``) and answers degraded or 503 when
a tool holds it (a card download waits instead, up to its budget:
test_l68_card_downloads.py), and a tool that waits on the network runs outside the
dispatcher's lock.

All legs are ``hil`` + ``net`` (console verbs over serial, ``/api/chat`` and
``/api/state`` over the LAN). ``TOOLSTALL <ms>`` makes the next tool call the dispatcher
runs under its card lock hold the lock for ``<ms>``; ``PROVFAULT`` answers the head's
request with a canned round, so the stall legs bill nothing.

* ``test_state_answers_while_a_tool_holds_the_card`` (the fixed image; the bench runs it
  3 times): a 30 s hold (memory.scratchpad), /api/state polled every 1 s: no new boot,
  every poll answered within 1 s, at least one answer marked ``cardBusy``, ``WEBGUARD?``
  reads no blocking take and no network request under the lock.
* ``test_before_fix_image_resets_on_the_web_task`` (``NIMBUS_L68_BEFORE=1``, the v4.6.0
  test image): the same hold resets the board with the owner's signature (``async_tcp``).
* ``test_image_look_runs_outside_the_card_lock`` (both images): a photo on the card and a
  canned head round asking ``image.look`` about it; the caption falls through the canned
  provider to a real cheap one (one small request per round). The fixed image runs the
  look outside the card lock (the armed TOOLSTALL never fires for it, /api/state answers
  within 1 s throughout, no network request under the lock); the v4.6.0 image holds the
  lock across the look and resets when /api/state is polled (``NIMBUS_L68_BEFORE=1``).

The parsers and verdicts are host-tested in test_l68_web_stall_host.py.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import threading
import time
from typing import Dict, List, Optional

import pytest

from device import ExpectTimeout

HOLD_MS = 30000  # past the 8 s watchdog with room for many 1 s polls
LOOK_HOLD_MS = 20000  # armed during the image.look leg: fires only under the dispatcher's lock
LOOK_PHOTO_BYTES = 40000  # small enough to stage over the paced console in about 20 s
POLL_S = 1.0
ANSWER_MS = 1000  # the DoD: /api/state answers within 1 s, degraded while busy
MODEL = os.environ.get("NIMBUS_BENCH_MODEL", "claude-haiku-5-5")  # cosmetic in the canned round
STALL_TOOL = "memory.scratchpad"
LOOK_TOOL = "image.look"

WEBGUARD_RE = re.compile(
    r"WEBGUARD blocking=(?P<blocking>\d+) caller=0x(?P<caller>[0-9a-f]{8}) busy=(?P<busy>\d+) "
    r"netUnderLock=(?P<netUnderLock>\d+) netCaller=0x(?P<netCaller>[0-9a-f]{8}) releases=(?P<releases>\d+) "
    r"webTask=(?P<webTask>[01]) watched=(?P<watched>[01])"
)
HOLD_LINE_RE = re.compile(r"memory: tool (?P<tool>\S+) held the card lock for (?P<ms>\d+) ms")


# ---- pure parsers and verdicts (host-tested) -------------------------------------
def parse_webguard(line: str) -> Optional[Dict[str, object]]:
    """``WEBGUARD?`` reply -> counters (ints; the two callers as text)."""
    m = WEBGUARD_RE.search(line or "")
    if not m:
        return None
    out: Dict[str, object] = {k: int(v) for k, v in m.groupdict().items() if k not in ("caller", "netCaller")}
    out["caller"] = m.group("caller")
    out["netCaller"] = m.group("netCaller")
    return out


def tool_round(tool: str, args: dict, model: str = MODEL) -> str:
    """One canned Anthropic round that asks for one tool (and nothing else)."""
    body = {
        "id": "msg_l68",
        "type": "message",
        "role": "assistant",
        "model": model,
        "content": [{"type": "tool_use", "id": "toolu_l68", "name": tool.replace(".", "_"), "input": args}],
        "stop_reason": "tool_use",
        "usage": {"input_tokens": 10, "output_tokens": 10},
    }
    return json.dumps(body, separators=(",", ":"))


def photo_ref(data: bytes) -> str:
    """A 16-hex photo ref for the card copy (the owner's look reads it by name)."""
    return hashlib.sha256(data).hexdigest()[:16]


def state_verdict(samples: List[dict], expect_busy: bool) -> "tuple[bool, str]":
    """The /api/state polls of a leg: every one answered 200 within ANSWER_MS, and (when
    a hold was the stress) at least one answered degraded."""
    if not samples:
        return False, "no /api/state poll ran"
    late = [s for s in samples if s.get("status") != 200 or s.get("ms", ANSWER_MS + 1) > ANSWER_MS]
    if late:
        worst = late[0]
        return False, f"{len(late)} of {len(samples)} polls late or failed (first: {worst})"
    busy = sum(1 for s in samples if s.get("cardBusy"))
    if expect_busy and busy == 0:
        return False, "no answer marked cardBusy: the hold never met a poll"
    return True, f"{len(samples)} polls within {ANSWER_MS} ms, {busy} marked cardBusy"


def web_guard_verdict(before: Dict[str, object], after: Dict[str, object], expect_busy: bool) -> "tuple[bool, str]":
    """The web guard's counters across a leg on the fixed image."""
    if int(after["webTask"]) != 1 or int(after["watched"]) != 1:
        return False, "the web task is not recorded or not watched by the task watchdog"
    if int(after["blocking"]) != int(before["blocking"]):
        return False, f"a web request waited for the card lock (caller 0x{after['caller']})"
    if int(after["netUnderLock"]) != int(before["netUnderLock"]):
        return False, f"a network request started under the card lock (caller 0x{after['netCaller']})"
    if expect_busy and int(after["busy"]) <= int(before["busy"]):
        return False, "no busy answer counted: the hold never met a web request"
    return True, "no web wait, no network under the card lock"


def async_tcp_signature_verdict(rep: dict) -> "tuple[bool, str]":
    """A crash boot record (test_l39 ``boot_report``) with the owner's 26 Oct signature."""
    boot, crash, why = rep.get("boot") or {}, rep.get("crash") or {}, rep.get("why") or ""
    if boot.get("slug") != "crash(watchdog)":
        return False, f"boot reason {boot.get('slug')!r}, want crash(watchdog)"
    if crash.get("this") != "1":
        return False, f"crash record this-reset={crash.get('this')!r}, want 1"
    if "async_tcp" not in why:
        return False, f"crash-why {why!r} does not name async_tcp"
    return True, "watchdog reset with the web task blocked"


def boot_report_from_state(state: dict) -> dict:
    """``/api/state``'s boot object in the shape async_tcp_signature_verdict reads. The
    running boot's own record: a bench unit's durable log can hold several boot-number
    sequences (other lanes' images), so a search for "the first boot after n" can pick an
    old one; the boot the device is running is unambiguous."""
    boot = state.get("boot") or {}
    crash = boot.get("crash") or {}
    return {
        "boot": {"n": boot.get("n"), "slug": boot.get("reason")},
        "crash": {"this": "1" if crash.get("thisReset") else "0", "task": crash.get("task")},
        "why": crash.get("why") or "",
    }


def look_fallback(providers: dict) -> Optional[str]:
    """The keyed provider the photo caption falls through to after the canned one."""
    for slug in ("mistral", "openai"):
        if (providers.get(slug) or {}).get("hasKey"):
            return slug
    return None


# ---- device helpers ---------------------------------------------------------------
def _webguard(device) -> Dict[str, object]:
    """``WEBGUARD?``, asked up to 3 times (a line sent while the board settles after a
    reset can be lost); an image that never answers has no such verb."""
    line = ""
    for _ in range(3):
        device.drain(quiet=0.3)
        device.send("WEBGUARD?")
        try:
            line = device.expect("WEBGUARD", timeout=5.0)
        except ExpectTimeout:
            continue
        got = parse_webguard(line)
        if got is not None:
            return got
    pytest.skip(f"this image has no WEBGUARD? verb (last: {line!r})")


class StatePoller:
    """GET /api/state every POLL_S on a thread: status, latency, cardBusy per poll."""

    def __init__(self, net, ip: str):
        self.net, self.ip = net, ip
        self.samples: List[dict] = []
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        while not self._stop.is_set():
            t0 = time.time()
            s: dict = {"t": t0}
            try:
                r = self.net.get("/api/state", ip=self.ip, timeout=5.0)
                s["ms"] = int((time.time() - t0) * 1000)
                s["status"] = r.status_code
                s["len"] = len(r.content)
                if r.status_code == 200:
                    try:
                        s["cardBusy"] = bool(r.json().get("cardBusy"))
                    except ValueError:  # an answer that is not JSON: kept for the report
                        s["status"] = 0
                        s["error"] = "not JSON"
                        s["clen"] = r.headers.get("Content-Length")
                        s["tail"] = r.text[-120:]
            except Exception as exc:  # noqa: BLE001 - a stalled or rebooting board: recorded
                s["ms"] = int((time.time() - t0) * 1000)
                s["status"] = 0
                s["error"] = type(exc).__name__
            self.samples.append(s)
            self._stop.wait(max(0.0, POLL_S - (time.time() - t0)))

    def __enter__(self) -> "StatePoller":
        self._t.start()
        return self

    def __exit__(self, *exc) -> None:
        self._stop.set()
        self._t.join(timeout=10.0)


def _console(device, line: str, want: str) -> str:
    device.drain(quiet=0.2)
    device.send(line)
    try:
        return device.expect(want, timeout=10.0)
    except ExpectTimeout:
        pytest.skip(f"this image has no {line.split()[0]} console verb")


def _arm(device, hold_ms: int, round_body: str) -> None:
    got = _console(device, f"TOOLSTALL {hold_ms}", "TOOLSTALL")
    assert "armed" in got, f"TOOLSTALL refused: {got!r}"
    got = _console(device, "PROVFAULT api.anthropic.com 200 " + round_body, "PROVFAULT")
    if "not wired" in got:
        pytest.skip("PROVFAULT is not wired in this image")


def _cleanup(device, net, ip: str, prio: str, provfault_off: bool = True) -> None:
    steps = [("TOOLSTALL 0", "TOOLSTALL")]
    if provfault_off:
        steps.insert(0, ("PROVFAULT off", "PROVFAULT"))
    for line, want in steps:
        try:
            device.cmd(line, want, timeout=5.0)
        except Exception as exc:  # noqa: BLE001 - reported; the next leg checks its own state
            print(f"[L68] cleanup {line!r}: {exc}")
    try:
        net.post("/api/orch", {"provPrio": prio}, ip=ip, timeout=8.0)
    except Exception as exc:  # noqa: BLE001
        print(f"[L68] cleanup provPrio: {exc}")


def _start_turn(device, net, ip: str, prio: str, stall: "tuple[int, str]", text: str) -> str:
    """Pin the head to the canned provider, arm the stall (hold ms, canned round), start
    one web-chat turn. Returns the turn id."""
    net.post("/api/orch", {"provPrio": prio}, ip=ip, timeout=8.0)
    _arm(device, *stall)
    r = net.post("/api/chat", {"text": text}, ip=ip, timeout=10.0)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}: {r.text[:200]}"
    return str(r.json().get("turn") or "")


def _wait_turn_end(net, ip: str, turn: str, timeout: float = 240.0) -> bool:
    """The canned round repeats until the turn's round cap ends it; PROVFAULT is cleared
    only after that, so no later round reaches the real head and bills."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            page = net.get_json(f"/api/chat?turn={turn}", ip=ip, timeout=8.0)
            if not turn or page.get("done") or page.get("gone"):
                return True
        except Exception as exc:  # noqa: BLE001 - a busy answer: poll again
            print(f"[L68] turn poll: {exc}")
        time.sleep(3.0)
    return False


def _fsput_paced(device, data: bytes, path: str) -> None:
    """FSPUT in 256 B lines 100 ms apart (an unpaced burst overruns the console's 1.5 KB
    receive buffer: test_l70 measured it), paced on the device's FSACK, verified by size
    and sha256."""
    device.cmd(f"FSPUT {len(data)} {path}", "FSPUT READY", timeout=10.0)
    b64 = base64.b64encode(data).decode()
    acked = 0
    for i in range(0, len(b64), 256):
        device.send(b64[i : i + 256])
        time.sleep(0.1)
        while min(len(data), (i + 256) // 4 * 3) - acked > 8192 and acked < len(data):
            acked = int(device.expect_re(r"FSACK (\d+)", timeout=15.0).group(1))
    m = device.expect_re(r"FSPUT DONE bytes=(\d+) sha256=([0-9a-f]+)", timeout=30.0)
    assert int(m.group(1)) == len(data) and m.group(2) == hashlib.sha256(data).hexdigest(), "FSPUT did not verify"


def _console_until(device, pattern: str, timeout: float) -> List[str]:
    """Console lines until one matches ``pattern`` (or the timeout): the lines seen."""
    out, end = [], time.time() + timeout
    while time.time() < end:
        try:
            line = device.expect_re(r".", timeout=max(0.5, min(5.0, end - time.time()))).string
        except ExpectTimeout:
            continue
        out.append(line)
        if re.search(pattern, line):
            break
    return out


def _stall_arm() -> "tuple[int, str]":
    """The 30 s hold and the canned round that asks for the stall tool."""
    return HOLD_MS, tool_round(STALL_TOOL, {"action": "view"})


def _before() -> bool:
    return os.environ.get("NIMBUS_L68_BEFORE") == "1"


def _orch_prio(net, ip: str) -> str:
    return str(net.get_json("/api/orch", ip=ip, timeout=8.0).get("provPrio") or "")


# ---- HIL legs -----------------------------------------------------------------------
@pytest.mark.hil
@pytest.mark.net
def test_state_answers_while_a_tool_holds_the_card(device, net):
    from test_l39_crash_forensics import _boot_n, _lan_ip

    if _before():
        pytest.skip("NIMBUS_L68_BEFORE=1: the v4.6.0 image runs the reset leg instead")
    ip = _lan_ip(None)
    before_n = _boot_n(net, ip)
    wg0 = _webguard(device)
    prio = _orch_prio(net, ip)
    turn = ""
    try:
        with StatePoller(net, ip) as poll:
            turn = _start_turn(device, net, ip, "anthropic", _stall_arm(), "L68 stall probe: look at your scratchpad.")
            lines = _console_until(device, r"TOOLSTALL:", 60.0)
            assert any("TOOLSTALL:" in ln for ln in lines), "the stall never started (did the tool run?)"
            time.sleep(HOLD_MS / 1000.0 + 5.0)  # the whole hold, then a few free polls
        wg1 = _webguard(device)
        print(f"[L68] polls={len(poll.samples)} webguard before={wg0} after={wg1}")
        for s in poll.samples:
            print(f"[L68] poll {s}")
        assert _boot_n(net, ip) == before_n, "the board restarted during the hold"
        ok, why = state_verdict(poll.samples, expect_busy=True)
        assert ok, why
        ok, why = web_guard_verdict(wg0, wg1, expect_busy=True)
        assert ok, why
    finally:
        ended = _wait_turn_end(net, ip, turn)
        _cleanup(device, net, ip, prio, provfault_off=ended)
        assert ended, "the probe turn was still running; PROVFAULT is left armed (restart the unit to clear it)"


@pytest.mark.hil
@pytest.mark.net
def test_before_fix_image_resets_on_the_web_task(device, net):
    """Run only against the v4.6.0 test image (NIMBUS_L68_BEFORE=1)."""
    from test_l39_crash_forensics import NoCrash, _await_crash_reboot, _boot_n, _lan_ip

    if not _before():
        pytest.skip("set NIMBUS_L68_BEFORE=1 with the v4.6.0 image to reproduce the reset")
    ip = _lan_ip(None)
    before_n = _boot_n(net, ip)
    prio = _orch_prio(net, ip)
    try:
        with StatePoller(net, ip) as poll:
            _start_turn(device, net, ip, "anthropic", _stall_arm(), "L68 stall probe: look at your scratchpad.")
            device.expect("TOOLSTALL:", timeout=60.0)
            try:
                ip2 = _await_crash_reboot(device, net, ip, before_n, 60.0)
            except NoCrash as exc:
                pytest.fail(f"the hold did not reset the v4.6.0 image: {exc}")
        for s in poll.samples:
            print(f"[L68] poll {s}")
        ip = _lan_ip(ip2)
        rep = boot_report_from_state(net.get_json("/api/state", ip=ip, timeout=10.0))
        print(f"[L68] v4.6.0 reset: boot={rep['boot']} crash={rep['crash']} why={rep['why']!r} (before: n={before_n})")
        ok, why = async_tcp_signature_verdict(rep)
        assert ok, why
    finally:
        _cleanup(device, net, ip, prio)


def _assert_v460_look_reset(device, net, ip: str, before_n: int) -> None:
    """The v4.6.0 image: the look's stall resets the board on the web task."""
    from test_l39_crash_forensics import NoCrash, _await_crash_reboot, _lan_ip

    device.expect(f"TOOLSTALL: {LOOK_TOOL}", timeout=60.0)
    try:
        ip2 = _await_crash_reboot(device, net, ip, before_n, 60.0)
    except NoCrash as exc:
        pytest.fail(f"image.look under the card lock did not reset the v4.6.0 image: {exc}")
    rep = boot_report_from_state(net.get_json("/api/state", ip=_lan_ip(ip2), timeout=10.0))
    print(f"[L68] v4.6.0 look reset: boot={rep['boot']} crash={rep['crash']} why={rep['why']!r} (before: n={before_n})")
    ok, why = async_tcp_signature_verdict(rep)
    assert ok, why


def _assert_look_ran_free(lines: List[str], samples: List[dict], wg0, wg1) -> None:
    """The tip: the look ran outside the dispatcher's lock and the page kept answering."""
    assert not any(f"TOOLSTALL: {LOOK_TOOL}" in ln for ln in lines), (
        "image.look ran under the dispatcher's card lock (the armed stall fired for it)"
    )
    assert any(ln.startswith("vision:") or " vision: " in ln for ln in lines), "no caption request was seen"
    ok, why = state_verdict(samples, expect_busy=False)
    assert ok, why
    ok, why = web_guard_verdict(wg0, wg1, expect_busy=False)
    assert ok, why


@pytest.mark.hil
@pytest.mark.net
def test_image_look_runs_outside_the_card_lock(device, net):
    """The 26 Oct trigger: image.look on a stored photo while /api/state is polled."""
    from test_l39_crash_forensics import _boot_n, _errlog_file, _lan_ip

    ip = _lan_ip(None)
    providers = net.get_json("/api/orch", ip=ip, timeout=8.0).get("providers") or {}
    fallback = look_fallback(providers)
    if not fallback:
        pytest.skip("no keyed mistral or openai provider for the caption to fall through to")
    from test_l48_albums import photo_near

    photo = photo_near(LOOK_PHOTO_BYTES, seed=68)
    ref = photo_ref(photo)
    _fsput_paced(device, photo, f"/mem/blobs/{ref}.jpg")
    before_n = _boot_n(net, ip)
    wg0 = None if _before() else _webguard(device)
    prio = _orch_prio(net, ip)
    turn = ""
    ended = None
    lines: List[str] = []
    try:
        with StatePoller(net, ip) as poll:
            look = (LOOK_HOLD_MS, tool_round(LOOK_TOOL, {"ref": ref, "question": "What colors are in it?"}))
            turn = _start_turn(
                device, net, ip, f"anthropic,{fallback}", look, "L68 look probe: look at that photo again."
            )
            if _before():
                _assert_v460_look_reset(device, net, ip, before_n)
                return
            lines = _console_until(device, r"vision: \S+ \S+ read \d+ bytes", 90.0)
            time.sleep(5.0)
        wg1 = _webguard(device)
        for s in poll.samples:
            print(f"[L68] poll {s}")
        print(f"[L68] webguard before={wg0} after={wg1}")
        assert _boot_n(net, ip) == before_n, "the board restarted during the look"
        _assert_look_ran_free(lines, poll.samples, wg0, wg1)
        # The log is a big page: while the canned rounds still run, internal memory sits
        # under the big-response floor and it answers 503 (bench, 27 Oct). Read it after.
        ended = _wait_turn_end(net, ip, turn)
        assert ended, "the turn did not end"
        holds = [m for m in HOLD_LINE_RE.finditer(_errlog_file(net, ip)) if m.group("tool") == LOOK_TOOL]
        assert not holds, f"the durable log names image.look holding the card lock: {holds[-1].group(0)}"
    finally:
        if ended is None:
            ended = _wait_turn_end(net, ip, turn) if turn and not _before() else True
        _cleanup(device, net, ip, prio, provfault_off=ended)
