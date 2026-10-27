"""§L15b - Outbound MCP robustness on real silicon (the SRAM leg).

The host suite (test/test_mcp_robustness) pins the PORTABLE guards - the tool
budget, the transport-error classification, the per-server breaker isolation, the
one-request-per-exchange serialization, and the over-cap reply refused with the
honest "more data than the device can hold" line instead of a parse error
(test_oversize_refused_with_honest_copy_not_parse_error). What it CANNOT measure is
the thing the owner actually asked about: does the device hold its scarce INTERNAL
SRAM when several MCP servers are connected at once and one of them is called
mid-turn? The native build is lib/core only; free-heap under load is a hardware
fact. This leg proves it on glass.

What it asserts:
  * N device-dialed MCP servers discovered and resident at once, with internal
    heap still above the orchestrator turn floor.
  * The LAN /mcp endpoint refuses a call to a connected server's tool (``mcp.*``)
    with JSON-RPC error -32003 and never dials the server: since v4.6.1 a tool that
    waits on the network runs only in the device's own turns, never on the web task
    the watchdog watches (lib/core/src/sys_web_guard.cpp toolWaitsOnNetwork).
  * A real device turn calls a connected server's echo tool with all N resident
    (proved from the turn's own tool-loop transcript, not the reply: the marker is
    in the prompt), and internal heap is still above the turn floor after it.

The oversize half moved to the host suite: the LAN endpoint can no longer carry a
220 KB argument to the server (it refuses the call), and a model cannot write one
into a tool call, so no seam on the device can produce an over-cap reply on demand.

This is a MANUAL bench leg (hil + manual): it needs a real board with a live head
(the echo runs in a turn) and a reachable MCP server whose `echo` tool reflects its
input. Run `PORT=3111 npx -y @modelcontextprotocol/server-everything streamableHttp`
on a LAN host and point NIMBUS_MCP_TEST_URL at http://<host>:3111/mcp (reachable
FROM the device). The operator confirms on the glass that the panel never blanks
or reboots during the leg.

Run:  python3 -m pytest tests/hil -m "manual and connectors" --allow-hardware \
         -k mcp_robustness -s
"""

from __future__ import annotations

import time

import pytest

from connectors import mcp_call, mcp_result_line, new_marker, reply_text, run_turn, skip_if_unavailable
from test_l4_network import lan_ip_or_skip
from test_l15_mcp_outbound import (
    _add_device_mcp,
    _delete_connector,
    _kick_sync,
    _local_url_or_skip,
    _mcp_tool_names,
)

pytestmark = [pytest.mark.hil, pytest.mark.manual, pytest.mark.connectors, pytest.mark.agent, pytest.mark.net]

# Mirror of src/agent/agent_config.h ORCH_TURN_HARD_FLOOR (internal-heap turn floor).
# Kept as a literal here on purpose: this test is the guard that the LIVE device
# honors it.
_ORCH_TURN_HARD_FLOOR = 28000
# The LAN endpoint's answer to a tool that waits on the network
# (nimbus::sys::turnOnlyToolRpcError).
_TURN_ONLY_RPC_ERROR = -32003


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


# ---- N connectors resident + one call mid-turn --------------------------------
@pytest.mark.manual
def test_multi_connector_sram_and_an_mcp_call_mid_turn(device, net, secrets, require_secret, require_manual):
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

        # (2) The LAN endpoint refuses a connected server's tool with -32003: it
        # never dials the server from the web task (HFW, v4.6.1).
        lan = mcp_call(net, ip, "mcp.hilrobust_a.echo", {"message": "from the LAN"})
        assert (lan.get("error") or {}).get("code") == _TURN_ONLY_RPC_ERROR, f"LAN /mcp mcp.* call: {lan!r}"[:300]

        # (3) A real device turn calls another server's echo with all three resident:
        # the turn's tool-loop transcript holds the call's result carrying the marker
        # (the reply alone proves nothing: the prompt carries the marker). Internal
        # heap still holds above the turn floor after the call.
        marker = new_marker("l15b-echo")
        turn = run_turn(
            net,
            ip,
            f"Use the echo tool from the hilrobust_b MCP server to echo exactly this text: {marker}",
        )
        skip_if_unavailable(turn, "device MCP (everything)")
        called = mcp_result_line(turn, "hilrobust_b", "echo", marker)
        assert called, (
            "the turn never called mcp.hilrobust_b.echo (no result line with the marker in its "
            f"tool-loop transcript); reply: {reply_text(turn)[:200]!r}"
        )
        print(f"[l15b] {called[:160]}", flush=True)
        _assert_internal_headroom(net, ip, "after-the-mcp-call")

        # (4) Human confirmation: nothing blanked or rebooted on the glass. The
        # white-screen/wedge class (AGENTS.md) is only ever confirmed by an eyeball.
        require_manual.confirm(
            "During the three-server discovery and the MCP call turn, did the panel stay up "
            "(no blank screen, no reboot, ring/menu still responsive)?"
        )
    finally:
        for n in names:
            _delete_connector(net, ip, n)
