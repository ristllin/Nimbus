"""L40 - internal-SRAM pressure never reboots the device (the CUM-404 counter-test).

CUM-404 loosened the turn floors (round floor 12 KB total free, 5 KB largest block)
with no counter-test for the consumers that run beside a turn: relay tunnel serves,
LAN pages and downloads, and worker tasks spawned before their own memory check. On
Lumi the internal heap reached 1,180 B free in the boot that crashed (crash(panic) in
round 2 of a photo turn); a fail-soft out-of-memory can become a panic through any
allocation that aborts on failure. The class: no consumer beside a turn, and no
failed allocation, reboots the device; low memory degrades honestly; every failed
allocation is counted.

Source guards (deterministic, no board):
  * every on-demand worker task asks the spawn gate before xTaskCreate (a new
    worker site without it fails); resident tasks created once at start-up are
    listed by name;
  * the failed-allocation hook is installed next to the PSRAM routing;
  * the CUM-404 floors stay where they are (the fix is admission, not a re-raise).

Bench leg (``hil`` + ``net`` + ``agent``, unit A, test build): 20 two-round turns on
the head model (claude-haiku-5-5 by default, ``NIMBUS_LAB_MODEL`` overrides; the
device must already be set to it) while 4 parallel LAN GETs, the tunneled
``GET /`` (console ``CLOUDLOOP /``: the relay's loopback replay) and a verify run,
with ``FAULT sram_ballast`` holding internal free near 14 KB. That is below the
20 KB serve floor, so every big page answers busy, and the loaders, the turn's TLS
and the network stack push internal free toward zero: the starvation the leg exists
to survive. Under it the network stack drops packets (failed packet-buffer
allocations in the TCP/IP task) and a small status request can take well over 10 s.

Pass criterion (all four):
  1. zero reboots: ``uptimeMs`` never goes back across the samples;
  2. every turn ends, within TURN_BUDGET_S, ok or with the honest low-memory reply;
  3. the device stays reachable: every status read (the turn poll and the state
     sample) is answered within READ_BUDGET_S, tried READ_ATTEMPT_S at a time; a read
     unanswered for the whole budget fails the leg as a responsiveness finding (a
     device result, not a harness timeout), and the slowest read is printed;
  4. ``mem.allocFails`` is reported (printed with the low-water mark and its tags).
A turn is submitted once, with SUBMIT_TIMEOUT_S (never resent: a resend could start
a second turn).
"""

from __future__ import annotations

import inspect
import os
import re
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import pytest

try:  # the bench's LAN client; collection and the host tests below run with pytest alone
    import requests

    NO_ANSWER: tuple = (requests.exceptions.Timeout, requests.exceptions.ConnectionError)
except ImportError:  # pragma: no cover - requests ships with the HIL environment
    requests = None
    NO_ANSWER = (TimeoutError, ConnectionError)

_REPO = Path(__file__).resolve().parents[2]
_SRC = _REPO / "src"

# Resident tasks created once at start-up (not on demand): their stacks are part of
# the boot budget the [sram] snapshots measure. The update check / install task has
# its own headroom route (nimbus/ota/ota_headroom.h: dedicated task only when it
# fits, else the Telegram poll task).
_RESIDENT_TASKS = {
    ("agent/telegram.cpp", "tg_poll"),
    ("net/relay_client.cpp", "relay"),
    ("sfx/music.cpp", "music"),
    ("sfx/sound_fx.cpp", "sfx"),
    ("main.cpp", "vrel"),
    ("sys/ota_update.cpp", None),  # name passed in via SpawnCtx; gated by ota_headroom
}
# The on-demand worker sites the spawn gate covers (study panic F2.4).
_WORKER_SITES = {
    ("agent/provider_verify.cpp", "pverify"),
    ("net/cloud_mint.cpp", "cloudmint"),
    ("agent/adapters/tts_voices.cpp", "voices"),
    ("agent/safety_activity_store.cpp", "safetyrep"),
}
_CREATE = re.compile(r"xTaskCreate(?:PinnedToCore)?\s*\(\s*[^,]+,\s*(\"(?P<name>[^\"]+)\"|[^,]+)")


def _creates():
    for path in sorted(_SRC.rglob("*.cpp")):
        rel = path.relative_to(_SRC).as_posix()
        text = path.read_text()
        for m in _CREATE.finditer(text):
            line_start = text.rfind("\n", 0, m.start()) + 1
            if "//" in text[line_start : m.start()]:
                continue  # a comment that names the call, not a call
            yield rel, m.group("name"), text, m.start()


def _enclosing_function(text: str, pos: int) -> str:
    """The text from the start of the column-0 function holding `pos` to `pos`."""
    start = text.rfind("\n}", 0, pos)
    return text[start if start >= 0 else 0 : pos]


def test_every_worker_spawn_asks_the_gate_first():
    seen, offenders = set(), []
    for rel, name, text, pos in _creates():
        if (rel, name) in _RESIDENT_TASKS or (rel, None) in _RESIDENT_TASKS:
            continue
        seen.add((rel, name))
        if "spawnGate(" not in _enclosing_function(text, pos):
            line = text.count("\n", 0, pos) + 1
            offenders.append(f"src/{rel}:{line} task {name!r} starts without heapwatch::spawnGate()")
    assert not offenders, (
        "worker tasks must check internal SRAM before xTaskCreate (never during a turn, "
        "stack + 16 KB free):\n  " + "\n  ".join(offenders)
    )
    # Counter-test for the guard: it saw every known worker site (a parser miss
    # cannot pass vacuously).
    assert _WORKER_SITES <= seen, f"guard missed worker sites: {_WORKER_SITES - seen}"


def test_failed_alloc_hook_installed_next_to_the_psram_routing():
    hw = (_SRC / "sys" / "heap_watch.cpp").read_text()
    assert "heap_caps_register_failed_alloc_callback(" in hw
    assert "RTC_NOINIT_ATTR" in hw, "the failure ring must survive a panic reset"
    main = (_SRC / "main.cpp").read_text()
    route = main.find("heap_caps_malloc_extmem_enable(128)")
    install = main.find("nimbus::heapwatch::install()")
    assert 0 <= route < install, "install() must follow the PSRAM routing"
    assert install - route < 600, "install() belongs right next to the PSRAM routing"
    loop = main.find("void loop() {")
    assert 0 <= loop < main.find("nimbus::heapwatch::pump(", loop), "loop() must drain the ring"


def test_cum404_floors_unchanged():
    cfg = (_SRC / "agent" / "agent_config.h").read_text()
    floors = dict(
        re.findall(r"#define (ORCH_TURN_MIN_LARGEST_BLOCK|ORCH_LOOP_MIN_HEAP|ORCH_TURN_HARD_FLOOR)\s+(\d+)U", cfg)
    )
    assert floors == {
        "ORCH_TURN_MIN_LARGEST_BLOCK": "5000",
        "ORCH_LOOP_MIN_HEAP": "12000",
        "ORCH_TURN_HARD_FLOOR": "12000",
    }, f"CUM-404 floors moved: {floors} (the fix is admission beside the turn, not a re-raise)"


# ---- patient reads (the device is slow on purpose here) ------------------------
TURN_BUDGET_S = 150.0  # one turn, submit to final reply
READ_ATTEMPT_S = 15.0  # one GET attempt
READ_BUDGET_S = 60.0  # one status read, all attempts together
SUBMIT_TIMEOUT_S = 30.0  # the turn's POST, sent once


@dataclass
class ReadStats:
    reads: int = 0
    timeouts: int = 0  # attempts that got no answer within READ_ATTEMPT_S
    slowest_s: float = 0.0

    def line(self) -> str:
        return f"reads={self.reads} timeouts={self.timeouts} slowest={self.slowest_s:.1f}s"


@dataclass
class ReadPolicy:
    attempt_s: float = READ_ATTEMPT_S
    budget_s: float = READ_BUDGET_S
    now: object = time.monotonic  # the clock and the pause, swappable for host tests
    sleep: object = time.sleep


def patient_get_json(get, url, what, stats, params=None, policy=None):
    """GET `url` and return its JSON, retrying an attempt that times out or cannot
    connect until the policy's budget has passed. An answered error status is never
    retried (raise_for_status). Past the budget the leg fails with a responsiveness
    finding."""
    p = policy or ReadPolicy()
    start, attempts = p.now(), 0
    while True:
        attempts += 1
        try:
            r = get(url, params=params, timeout=p.attempt_s)
        except NO_ANSWER:
            stats.timeouts += 1
            if p.now() - start >= p.budget_s:
                pytest.fail(
                    f"responsiveness: {what} unanswered for {p.now() - start:.0f}s under L40 pressure "
                    f"({attempts} attempts of {p.attempt_s:.0f}s): a device finding, not a harness timeout"
                )
            p.sleep(1.0)
            continue
        r.raise_for_status()
        stats.reads += 1
        stats.slowest_s = max(stats.slowest_s, p.now() - start)
        return r.json()


# ---- bench leg ---------------------------------------------------------------
TURNS = int(os.environ.get("NIMBUS_L40_TURNS", "20"))
BALLAST_TARGET_FREE = int(os.environ.get("NIMBUS_L40_FREE", str(14 * 1024)))
PROMPT = "Use the files_list tool once, then the memory_search tool once for 'tea', then answer in one short sentence."
_LOW_MEMORY_REPLY = re.compile(r"memory", re.I)


def _state(s, ip, stats):
    return patient_get_json(s.get, f"http://{ip}/api/state", "GET /api/state", stats)


def _submit(s, ip, text):
    try:
        return s.post(f"http://{ip}/api/chat", data={"text": text}, timeout=SUBMIT_TIMEOUT_S)
    except requests.exceptions.Timeout:
        pytest.fail(f"responsiveness: POST /api/chat not accepted within {SUBMIT_TIMEOUT_S:.0f}s under L40 pressure")


def _chat_turn(s, ip, text, stats):
    r = _submit(s, ip, text)
    if r.status_code == 503:  # the previous reply is still finishing
        time.sleep(3)
        r = _submit(s, ip, text)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}: {r.text[:160]}"
    turn = r.json().get("turn", "")
    deadline = time.monotonic() + TURN_BUDGET_S
    while time.monotonic() < deadline:
        time.sleep(2)
        j = patient_get_json(
            s.get, f"http://{ip}/api/chat", "the turn's status poll", stats, params={"turn": turn} if turn else None
        )
        if not j.get("pending", True) and (j.get("reply") or j.get("error")):
            return j
    pytest.fail(f"turn did not finish within {TURN_BUDGET_S:.0f}s")


def _lan_load(s, ip, stop, stats):
    paths = ["/", "/api/mem/episodic?limit=100", "/api/errlog", "/api/files/list"]
    while not stop.is_set():
        for p in paths:
            try:
                r = s.get(f"http://{ip}{p}", timeout=20)
                stats[r.status_code] = stats.get(r.status_code, 0) + 1
            except Exception:  # noqa: BLE001 - the load is best effort; reboots are judged below
                stats["err"] = stats.get("err", 0) + 1


def _hold_ballast(device, mem):
    """Pin internal SRAM down to about BALLAST_TARGET_FREE. One block: it can be no
    larger than the largest free internal block. Fails if nothing was held, so the
    leg cannot pass without pressure."""
    ballast = max(0, min(int(mem["intFree"]) - BALLAST_TARGET_FREE, int(mem["intLargest"]) - 256))
    held = device.cmd(f"FAULT sram_ballast {ballast}", "FAULT sram_ballast=", timeout=5)
    m = re.search(r"held=(\d+)", held or "")
    assert ballast == 0 or (m and int(m.group(1)) > 0), f"the ballast was not held, so no pressure: {held}"


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l40_internal_oom_no_panic(device):
    from webrig import ip_tok_or_skip, make_session, require_orchestrator

    ip, tok = ip_tok_or_skip("L40")
    s = make_session(tok)
    st0 = require_orchestrator(s, ip, "L40")
    mem0 = st0.get("mem", {})
    assert "allocFails" in mem0 and "lowWater" in mem0, "this build does not report mem.allocFails/lowWater"
    up = [int(st0["uptimeMs"])]

    _hold_ballast(device, mem0)
    stop, stats = threading.Event(), {}
    loaders = [threading.Thread(target=_lan_load, args=(s, ip, stop, stats), daemon=True) for _ in range(4)]
    for t in loaders:
        t.start()
    outcomes, tunnel, reads = [], [], ReadStats()
    try:
        for i in range(TURNS):
            if i == TURNS // 2:
                s.post(f"http://{ip}/api/verify", data={"provider": "anthropic"}, timeout=10)
            device.send("CLOUDLOOP /")
            try:
                m = device.expect_re(r"CLOUDLOOP / -> (-?\d+)", timeout=20)
                tunnel.append(int(m.group(1)))
            except Exception:  # noqa: BLE001 - recorded as a miss; the reboot check decides
                tunnel.append(None)
            outcomes.append(_chat_turn(s, ip, f"[l40 {i}] {PROMPT}", reads))
            up.append(int(_state(s, ip, reads)["uptimeMs"]))
    finally:
        stop.set()
        for t in loaders:
            t.join(timeout=30)
        device.cmd("FAULT sram_ballast 0", "FAULT sram_ballast=", timeout=5)

    st1 = _state(s, ip, reads)
    assert all(b >= a for a, b in zip(up, up[1:])), f"the device rebooted under load (uptime samples {up})"
    bad = [o for o in outcomes if o.get("error") and not _LOW_MEMORY_REPLY.search(str(o))]
    assert not bad, f"turns that failed without the honest low-memory reply: {bad[:3]}"
    mem1 = st1["mem"]
    print(
        f"L40: turns={len(outcomes)} allocFails={mem1['allocFails']} lowWater={mem1['lowWater']} "
        f"tags={mem1.get('lowWaterTags')} lan={stats} tunnel={tunnel} {reads.line()}"
    )
    assert isinstance(mem1["allocFails"], int)


# ---- host tests for the patient reads (no board) -------------------------------
class _Clock:
    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s

    def policy(self):
        return ReadPolicy(now=self.now, sleep=self.sleep)


class _Answered(Exception):
    """The fake response's raise_for_status: an answered error status."""


class _Resp:
    def __init__(self, status=200, body=None):
        self.status_code, self._body = status, body or {}

    def json(self):
        return self._body

    def raise_for_status(self):
        if self.status_code >= 400:
            raise _Answered(f"{self.status_code}")


def _scripted_get(clock, answers):
    """A GET that walks `answers`: "timeout" costs the whole attempt, a _Resp answers
    after 0.5 s. Records every call."""
    calls = []

    def get(url, params=None, timeout=None):
        calls.append((url, params, timeout))
        a = answers[min(len(calls), len(answers)) - 1]
        if a == "timeout":
            clock.t += timeout
            raise NO_ANSWER[0]("read timed out")
        clock.t += 0.5
        return a

    return get, calls


def test_patient_read_rides_out_a_slow_device():
    # The 19 Oct unit A shape: the first status poll got no answer in 10 s.
    clock, stats = _Clock(), ReadStats()
    get, calls = _scripted_get(clock, ["timeout", "timeout", _Resp(body={"pending": False, "reply": "ok"})])
    j = patient_get_json(get, "http://dev/api/chat", "poll", stats, params={"turn": "t1"}, policy=clock.policy())
    assert j["reply"] == "ok"
    assert len(calls) == 3 and all(c[2] == READ_ATTEMPT_S and c[1] == {"turn": "t1"} for c in calls)
    assert (stats.reads, stats.timeouts) == (1, 2)
    assert stats.slowest_s == 2 * READ_ATTEMPT_S + 2 * 1.0 + 0.5
    assert stats.slowest_s < READ_BUDGET_S


def test_patient_read_fails_past_the_budget_as_a_responsiveness_finding():
    clock, stats = _Clock(), ReadStats()
    get, calls = _scripted_get(clock, ["timeout"])
    with pytest.raises(pytest.fail.Exception, match=r"responsiveness: poll unanswered for \d+s"):
        patient_get_json(get, "http://dev/api/chat", "poll", stats, policy=clock.policy())
    # Bounded: it gave up once the budget had passed, never sooner, never much later.
    assert READ_BUDGET_S <= clock.t <= READ_BUDGET_S + READ_ATTEMPT_S + 1.0
    assert len(calls) == stats.timeouts == 4


def test_patient_read_never_retries_an_answered_error():
    clock, stats = _Clock(), ReadStats()
    get, calls = _scripted_get(clock, [_Resp(status=500)])
    with pytest.raises(_Answered):
        patient_get_json(get, "http://dev/api/state", "state", stats, policy=clock.policy())
    assert len(calls) == 1 and stats.reads == 0


def test_every_bench_read_is_patient_and_the_submit_is_sent_once():
    """The leg reads the device only through patient_get_json, and sends a turn only
    through _submit (one POST, no resend loop)."""
    for fn in (_state, _chat_turn):
        src = inspect.getsource(fn)
        assert "patient_get_json(" in src, f"{fn.__name__} reads the device without the patient read"
        assert not re.search(r"\bs\.get\(", src), f"{fn.__name__} has a single-shot GET"
    assert "s.post(" not in inspect.getsource(_chat_turn)
    leg = inspect.getsource(test_l40_internal_oom_no_panic)
    assert "_state(s, ip, reads)" in leg and "_chat_turn(s, ip" in leg and "reads.line()" in leg
