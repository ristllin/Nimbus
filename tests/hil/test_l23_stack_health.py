"""§L23 - FreeRTOS task-stack sizing regression guards.

Two task stacks were retuned the hard way (each paid for in a crash(panic) or an
on-device measurement), and both are a single constant that a careless edit could
silently revert:

  * tg_poll   = 24576 B  (telegram.cpp POLL_STACK_BYTES) - the deep tool-loop +
    mbedTLS chain peaks at 16156 B; the old 16384 left 228 B and overflowed. This
    was the recurring crash(panic).
  * async_tcp = 12288 B  (platformio.ini CONFIG_ASYNC_TCP_STACK_SIZE) - the task is
    NOT TLS-free: GET /api/mem/vector and POST /mcp memory.write run a blocking
    mbedTLS embed INLINE on it, measured on-device peaking at 6536 B used. The
    earlier 8192 left only ~1656 B (80% used) - too tight; raised to 12288 (~5748 B).

The two SOURCE guards below are deterministic and need NO hardware - they fail loud
if either constant is edited back below its measured-safe floor. The runtime guard
(net-marked, skip-and-warn) confirms the live async_tcp high-water still has real
headroom on a reachable board.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]

# Measured-safe floors. Not the exact current values - a floor, so a deliberate
# *raise* stays green while a revert to the crashing/too-tight value goes red.
ASYNC_TCP_FLOOR = 12288
POLL_STACK_FLOOR = 24576
# Live free-stack danger floor (bytes). mbedTLS scratch alone is multiple KB; under
# ~2 KB free is the "about to overflow" zone the retune exists to avoid.
RUNTIME_MIN_FREE = 2048


# ---- deterministic source guards (no device) -------------------------------
def test_async_tcp_stack_not_reverted():
    """platformio.ini CONFIG_ASYNC_TCP_STACK_SIZE must stay >= the measured-safe
    12288 (the task runs blocking mbedTLS embeds; 8192 left only 1656 B headroom)."""
    ini = (_REPO / "platformio.ini").read_text()
    m = re.search(r"CONFIG_ASYNC_TCP_STACK_SIZE\s*=\s*(\d+)", ini)
    assert m, "CONFIG_ASYNC_TCP_STACK_SIZE not found in platformio.ini"
    size = int(m.group(1))
    assert size >= ASYNC_TCP_FLOOR, (
        f"async_tcp stack {size} < {ASYNC_TCP_FLOOR}: the task runs a blocking "
        f"mbedTLS embed (GET /api/mem/vector, POST /mcp memory.write) that peaks "
        f"at ~6536 B; reverting starves it. See the platformio.ini comment."
    )


def test_tg_poll_stack_not_reverted():
    """telegram.cpp POLL_STACK_BYTES must stay >= 24576 - the deep tool-loop +
    mbedTLS chain peaks at 16156 B; 16384 overflowed (the recurring crash(panic))."""
    src = (_REPO / "src" / "agent" / "telegram.cpp").read_text()
    m = re.search(r"POLL_STACK_BYTES\s*=\s*(\d+)", src)
    assert m, "POLL_STACK_BYTES not found in telegram.cpp"
    size = int(m.group(1))
    assert size >= POLL_STACK_FLOOR, (
        f"tg_poll stack {size} < {POLL_STACK_FLOOR}: the tool-loop + TLS chain peaks "
        f"at ~16156 B and the smaller stack overflowed (crash(panic))."
    )


# ---- live runtime headroom (net; skip+warn, never silent-pass) -------------
@pytest.mark.net
@pytest.mark.agent
def test_async_stack_runtime_headroom(device, net, secrets, require_secret):
    """Drive the deepest async_tcp path (vector search embeds the query inline) and
    confirm the task's worst-ever free stack stays above the danger floor. Skips
    LOUD if the board is unreachable - never a silent pass."""
    from test_l4_network import lan_ip_or_skip

    ip = lan_ip_or_skip(device, net, secrets, require_secret)

    # Best-effort: exercise the inline-embed path a few times. Vector search embeds
    # the query on the async_tcp task; tolerate any per-call error (empty VDB, no
    # embedder) - the point is to deepen the stack, then read its high-water.
    for _ in range(4):
        try:
            net.get_json("/api/mem/vector", params={"query": "stack headroom probe", "limit": "3"}, ip=ip, timeout=15.0)
        except Exception:
            pass

    st = net.get_json("/api/state", ip=ip, timeout=8.0)
    mem = st.get("mem", {})
    if "asyncStackMin" not in mem:
        pytest.skip("device build does not surface mem.asyncStackMin (async_tcp task absent)")
    free = int(mem["asyncStackMin"])
    assert free > RUNTIME_MIN_FREE, (
        f"async_tcp worst free stack {free} B <= {RUNTIME_MIN_FREE} B danger floor - "
        f"raise CONFIG_ASYNC_TCP_STACK_SIZE (see platformio.ini)."
    )

    # Opportunistic: if the tg_poll task has run a turn, its high-water must be sane
    # too (UINT32_MAX = never ran → nothing to assert).
    poll = int(mem.get("pollStackMin", 0xFFFFFFFF))
    if poll != 0xFFFFFFFF:
        assert poll > RUNTIME_MIN_FREE, f"tg_poll worst free stack {poll} B <= {RUNTIME_MIN_FREE} B danger floor."


# ---- O2 (row 41): more stack marks, and the photo turn's tg_poll headroom -------
# The 18 Oct crash(panic) came in round 2 of a photo turn. A photo turn runs one frame
# deeper than a text turn (handleAttachment calls the turn inline), and the loopTask,
# display render and voice-release stacks were not surfaced at all. Owner rule (row
# 41): OTA and photo turns leave pollStackMin at or above the text-turn baseline less
# 512 B (a looser floor such as 2 KB would hide a 4.9 KB regression).
PHOTO_STACK_SLACK = 512
PHOTO_PROMPT = (
    "Describe this photo, then use memory_search for 'tea' and then for 'car seat', then answer in one sentence."
)
TEXT_PROMPT = "Use memory_search for 'tea', then for 'car seat', then answer in one sentence."


@pytest.mark.net
def test_more_stack_marks_surfaced(device, net, secrets, require_secret):
    """loopTask, the display render task and the voice-release watcher report their
    worst free stack in /api/state.mem, each above the danger floor."""
    from test_l4_network import lan_ip_or_skip

    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    mem = net.get_json("/api/state", ip=ip, timeout=8.0).get("mem", {})
    for name in ("loopStackMin", "vrelStackMin"):
        assert name in mem, f"/api/state.mem.{name} missing (row 41 stack marks)"
        assert int(mem[name]) > 512, f"{name} {mem[name]} B: the task is about to overflow"
    if "panelStackMin" in mem:  # absent when the board runs without the colour display
        assert int(mem["panelStackMin"]) > 512


def _chat_and_wait(s, ip, text, timeout_s=180):
    import time

    r = s.post(f"http://{ip}/api/chat", data={"text": text}, timeout=10)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}"
    turn = r.json().get("turn", "")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        time.sleep(2)
        j = s.get(f"http://{ip}/api/chat", params={"turn": turn} if turn else None, timeout=10).json()
        if not j.get("pending", True):
            return j
    pytest.fail("turn did not finish")


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_photo_turn_keeps_tg_poll_headroom(device):
    """A 3-round photo turn (injected as a raw Telegram update) leaves pollStackMin at
    or above the 3-round text-turn baseline less 512 B."""
    import json
    import os
    import time

    from webrig import ip_tok_or_skip, make_session, require_orchestrator

    file_id = os.environ.get("NIMBUS_TEST_TG_PHOTO_FILE_ID")
    chat_id = os.environ.get("NIMBUS_TEST_TG_CHAT_ID")
    if not file_id or not chat_id:
        pytest.skip("set NIMBUS_TEST_TG_PHOTO_FILE_ID (a photo the bench bot can fetch) and NIMBUS_TEST_TG_CHAT_ID")
    ip, tok = ip_tok_or_skip("L23 photo")
    s = make_session(tok)
    require_orchestrator(s, ip, "L23 photo")

    _chat_and_wait(s, ip, TEXT_PROMPT)
    base = int(s.get(f"http://{ip}/api/state", timeout=10).json()["mem"]["pollStackMin"])
    upd = int(os.environ.get("NIMBUS_TEST_TG_UPDATE_ID", str(int(time.time()))))
    page = {
        "ok": True,
        "result": [
            {
                "update_id": upd,
                "message": {
                    "message_id": upd % 100000,
                    "from": {"id": int(chat_id), "first_name": "Bench"},
                    "chat": {"id": int(chat_id), "type": "private"},
                    "date": int(time.time()),
                    "photo": [{"file_id": file_id, "file_unique_id": "bench", "width": 1280, "height": 960}],
                    "caption": PHOTO_PROMPT,
                },
            }
        ],
    }
    line = device.cmd(f"TGINJECT {json.dumps(page, separators=(',', ':'))}", "TGINJECT", timeout=10)
    if "not wired" in line or "injected=0" in line:
        pytest.skip(f"this build's TGINJECT does not dispatch photo updates yet: {line}")
    deadline = time.time() + 240
    while time.time() < deadline:
        time.sleep(5)
        st = s.get(f"http://{ip}/api/chat", params={"watch": "1"}, timeout=10).json()
        if not st.get("busy", st.get("pending", False)):
            break
    after = int(s.get(f"http://{ip}/api/state", timeout=10).json()["mem"]["pollStackMin"])
    assert after >= base - PHOTO_STACK_SLACK, (
        f"the photo turn took tg_poll {base - after} B deeper than the text-turn baseline "
        f"({after} B free vs {base} B): over the {PHOTO_STACK_SLACK} B budget (row 41)"
    )


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_ota_check_keeps_tg_poll_headroom(device):
    """An update check on a loaded device (the check may run on the Telegram poll task
    when its own task does not fit) leaves pollStackMin at or above the 3-round
    text-turn baseline less 512 B (row 41)."""
    import time

    from webrig import ip_tok_or_skip, make_session, require_orchestrator

    ip, tok = ip_tok_or_skip("L23 OTA")
    s = make_session(tok)
    require_orchestrator(s, ip, "L23 OTA")

    _chat_and_wait(s, ip, TEXT_PROMPT)
    base = int(s.get(f"http://{ip}/api/state", timeout=10).json()["mem"]["pollStackMin"])
    r = s.post(f"http://{ip}/api/ota/check", timeout=10)
    if r.status_code == 409:
        pytest.skip(f"the device refused the update check: {r.text[:120]}")
    assert r.status_code == 202, f"/api/ota/check -> {r.status_code}"
    deadline = time.time() + 120
    st = {}
    while time.time() < deadline:
        time.sleep(3)
        st = s.get(f"http://{ip}/api/state", timeout=10).json()
        if st.get("ota") != "checking":
            break
    assert st.get("ota") != "checking", "the update check did not settle within 120 s"
    after = int(st["mem"]["pollStackMin"])
    assert after >= base - PHOTO_STACK_SLACK, (
        f"the update check took tg_poll {base - after} B deeper than the text-turn baseline "
        f"({after} B free vs {base} B): over the {PHOTO_STACK_SLACK} B budget (row 41)"
    )
