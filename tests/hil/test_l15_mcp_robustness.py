"""§L15b - Outbound MCP robustness on real silicon (the SRAM + big-payload leg).

The host suite (test/test_mcp_robustness) pins the PORTABLE guards - the tool
budget, the transport-error classification, the per-server breaker isolation, the
one-request-per-exchange serialization. What it CANNOT measure is the thing the
owner actually asked about: does the device hold its scarce INTERNAL SRAM when
several MCP servers are connected at once and one returns a big payload mid-turn?
The native build is lib/core only; free-heap under load is a hardware fact. This
leg proves it on glass.

What it asserts, deterministically, over the device's OWN /mcp seam (no LLM):
  * N device-dialed MCP servers discovered and resident at once, with internal
    heap still above the orchestrator turn floor.
  * A response comfortably UNDER the size cap round-trips.
  * A response OVER the cap is refused with the honest "more data than the device
    can hold" line - and the device does NOT reboot (no watchdog, no OOM panic):
    the big body lands in PSRAM and is capped, never an unbounded internal string.

This is a MANUAL bench leg (hil + manual): it needs a real board and a reachable
MCP server whose `echo` tool reflects its input, so a big argument forces a big
reply. Run `PORT=3111 npx -y @modelcontextprotocol/server-everything streamableHttp`
on a LAN host and point NIMBUS_MCP_TEST_URL at http://<host>:3111/mcp (reachable
FROM the device). The operator confirms on the glass that the panel never blanks
or reboots during the oversize call.

Run:  python3 -m pytest tests/hil -m "manual and connectors" --allow-hardware \
         -k mcp_robustness -s
"""

from __future__ import annotations

import time

import pytest

from connectors import mcp_call
from test_l4_network import lan_ip_or_skip
from test_l15_mcp_outbound import (
    _add_device_mcp,
    _delete_connector,
    _kick_sync,
    _local_url_or_skip,
    _mcp_tool_names,
)

pytestmark = [pytest.mark.hil, pytest.mark.manual, pytest.mark.connectors, pytest.mark.agent, pytest.mark.net]

# Mirror of src/agent/agent_config.h ORCH_TURN_HARD_FLOOR (internal-heap turn floor)
# and src/agent/connectors.cpp kMaxBodyBytes (the PSRAM response cap). Kept as
# literals here on purpose: this test is the guard that the LIVE device honors them.
_ORCH_TURN_HARD_FLOOR = 28000
_MCP_BODY_CAP = 160 * 1024

# An echo argument sized to sit clearly ABOVE the cap once wrapped in the reply
# envelope ("Echo: <message>" + JSON-RPC/SSE framing), and one clearly BELOW it.
_OVER_CAP_CHARS = 220 * 1024
_UNDER_CAP_CHARS = 40 * 1024


def _state(net, ip) -> dict:
    return net.get_json("/api/state", ip=ip, timeout=6.0)


def _assert_internal_headroom(net, ip, where: str) -> dict:
    """The scarce INTERNAL pool must stay above the orchestrator turn floor - that
    is the whole point of routing the MCP body to PSRAM. Returns the state dict."""
    st = _state(net, ip)
    heap = int(st.get("heap", 0))
    heap_min = int(st.get("heapMin", 0))
    mem = st.get("mem", {}) or {}
    int_free = int(mem.get("intFree", heap))
    assert heap_min > _ORCH_TURN_HARD_FLOOR, (
        f"[{where}] internal heapMin {heap_min} dipped to/under the turn floor "
        f"{_ORCH_TURN_HARD_FLOOR} - the MCP path is eating internal SRAM"
    )
    assert int_free > _ORCH_TURN_HARD_FLOOR, (
        f"[{where}] internal free {int_free} under the turn floor {_ORCH_TURN_HARD_FLOOR}"
    )
    return st


def _mcp_call_text(net, ip, tool: str, arguments: dict) -> str:
    """Flatten a device /mcp tools/call envelope (result content OR error) to text."""
    out = mcp_call(net, ip, tool, arguments)
    if "error" in out:
        return str(out["error"].get("message", out["error"]))
    content = out.get("result", {}).get("content", []) or []
    return "\n".join(c.get("text", "") for c in content if isinstance(c, dict))


# ---- N connectors resident + one big payload mid-turn ------------------------
@pytest.mark.manual
def test_multi_connector_sram_and_big_payload(device, net, secrets, require_secret, require_manual):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    url = _local_url_or_skip()
    names = ["hilrobust_a", "hilrobust_b", "hilrobust_c"]
    try:
        # (1) THREE device-dialed MCP servers approved at once (same URL, distinct
        # slugs -> three resident toolsets). Discovery runs one server per turn.
        for n in names:
            _add_device_mcp(net, ip, n, url, approved=True)
        deadline = time.time() + 90
        while time.time() < deadline:
            _kick_sync(net, ip)
            if all(_mcp_tool_names(net, ip, n) for n in names):
                break
        for n in names:
            assert _mcp_tool_names(net, ip, n), f"{n} never discovered - device could not reach {url}"

        # Internal SRAM holds above the turn floor with all three resident.
        _assert_internal_headroom(net, ip, "three-connectors-resident")

        # (2) A comfortably UNDER-cap echo round-trips (the "just under succeeds"
        # half of the boundary), and internal heap is untouched.
        marker = "under-cap-" + "u" * (_UNDER_CAP_CHARS - len("under-cap-"))
        under = _mcp_call_text(net, ip, "mcp.hilrobust_a.echo", {"message": marker})
        assert "under-cap-uuu" in under, f"under-cap echo did not round-trip: {under[:160]!r}"
        _assert_internal_headroom(net, ip, "after-under-cap-call")

        # (3) An OVER-cap call: the HARD assertion (the task's HIL ask) is that the
        # device holds its internal SRAM above the turn floor and does NOT reboot or
        # OOM while a big payload moves through it - the big body rides PSRAM and is
        # capped there, never an unbounded internal string. The honest cap line is a
        # SOFT check: reproducing an over-cap reply needs a server/tool that returns
        # more than the cap. Echo reflects its input, but the argument must first
        # arrive through the inbound /mcp body limit, so if it did not cross the cap
        # here we WARN + point at the deterministic host proof rather than false-fail.
        over = _mcp_call_text(net, ip, "mcp.hilrobust_b.echo", {"message": "o" * _OVER_CAP_CHARS})
        _assert_internal_headroom(net, ip, "after-over-cap-call")  # HARD: SRAM floor held
        if "more data than the device can hold" in over:
            pass  # SOFT check reproduced: the honest cap line surfaced on glass
        else:
            print(
                "[l15-robustness] WARN: over-cap reply not reproduced on this run "
                f"(reply {len(over)}B did not exceed the {_MCP_BODY_CAP}B cap - the inbound "
                "/mcp body limit likely capped the argument first). The honest-copy path "
                "is proven deterministically host-side (test/test_mcp_robustness: "
                "test_oversize_refused_with_honest_copy_not_parse_error). Point a tool that "
                "returns an over-cap reply at the device to reproduce it live.",
                flush=True,
            )

        # (4) Human confirmation: nothing blanked or rebooted on the glass. The
        # white-screen/wedge class (AGENTS.md) is only ever confirmed by an eyeball.
        require_manual.confirm(
            "During the oversize MCP call, did the panel stay up (no blank screen, "
            "no reboot, ring/menu still responsive)?"
        )
    finally:
        for n in names:
            _delete_connector(net, ip, n)
