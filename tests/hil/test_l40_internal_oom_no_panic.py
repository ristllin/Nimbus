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
with ``FAULT sram_ballast`` holding internal free near 14 KB. Pass: zero reboots,
every turn ends ok or with the honest low-memory reply, ``mem.allocFails`` is
reported.
"""

from __future__ import annotations

import os
import re
import threading
import time
from pathlib import Path

import pytest

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


# ---- bench leg ---------------------------------------------------------------
TURNS = int(os.environ.get("NIMBUS_L40_TURNS", "20"))
BALLAST_TARGET_FREE = int(os.environ.get("NIMBUS_L40_FREE", str(14 * 1024)))
PROMPT = "Use the files_list tool once, then the memory_search tool once for 'tea', then answer in one short sentence."
_LOW_MEMORY_REPLY = re.compile(r"memory", re.I)


def _state(s, ip):
    r = s.get(f"http://{ip}/api/state", timeout=10)
    r.raise_for_status()
    return r.json()


def _chat_turn(s, ip, text, timeout_s=150):
    r = s.post(f"http://{ip}/api/chat", data={"text": text}, timeout=10)
    if r.status_code == 503:  # the previous reply is still finishing
        time.sleep(3)
        r = s.post(f"http://{ip}/api/chat", data={"text": text}, timeout=10)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}: {r.text[:160]}"
    turn = r.json().get("turn", "")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        time.sleep(2)
        j = s.get(f"http://{ip}/api/chat", params={"turn": turn} if turn else None, timeout=10).json()
        if not j.get("pending", True) and (j.get("reply") or j.get("error")):
            return j
    pytest.fail(f"turn did not finish within {timeout_s}s")


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
    outcomes, tunnel = [], []
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
            outcomes.append(_chat_turn(s, ip, f"[l40 {i}] {PROMPT}"))
            up.append(int(_state(s, ip)["uptimeMs"]))
    finally:
        stop.set()
        for t in loaders:
            t.join(timeout=30)
        device.cmd("FAULT sram_ballast 0", "FAULT sram_ballast=", timeout=5)

    st1 = _state(s, ip)
    assert all(b >= a for a, b in zip(up, up[1:])), f"the device rebooted under load (uptime samples {up})"
    bad = [o for o in outcomes if o.get("error") and not _LOW_MEMORY_REPLY.search(str(o))]
    assert not bad, f"turns that failed without the honest low-memory reply: {bad[:3]}"
    mem1 = st1["mem"]
    print(
        f"L40: turns={len(outcomes)} allocFails={mem1['allocFails']} lowWater={mem1['lowWater']} "
        f"tags={mem1.get('lowWaterTags')} lan={stats} tunnel={tunnel}"
    )
    assert isinstance(mem1["allocFails"], int)
