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
