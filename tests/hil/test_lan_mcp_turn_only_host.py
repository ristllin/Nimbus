"""Host tier: no HIL leg expects a result from a turn-only tool over LAN /mcp. No board.

Since v4.6.1 (HFW) the LAN endpoint answers every tool that waits on the network
(``nimbus::sys::toolWaitsOnNetwork``: image generation, a photo caption, the reply and
speech tools, web search, device.control, and every ``mcp.*`` tool of a connected
server) with JSON-RPC error -32003 before any request: such a tool runs only in the
device's own turns, never on the web task the watchdog watches. Two legs (L65 test_2's
image.generate and L15b's ``mcp.*`` echo) kept reading a result there and could never
pass. A leg that sends such a tool to a LAN /mcp helper must assert the -32003 refusal
in the same function; a property that needs the tool to run moves to a turn.
"""

from __future__ import annotations

import ast
import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.host

HIL_DIR = Path(__file__).resolve().parent
GUARD_SRC = HIL_DIR.parents[1] / "lib" / "core" / "src" / "sys_web_guard.cpp"
# The suite's helpers that post one tools/call to the LAN /mcp endpoint (or the test
# astool route, which runs on the same web task).
LAN_MCP_HELPERS = frozenset({"_mcp", "_mcp_envelope", "mcp_call", "_mcp_as_owner", "_mcp_call_text"})


def turn_only_tools(cpp: str) -> frozenset:
    """The names toolWaitsOnNetwork lists (the mcp.* prefix is checked separately)."""
    body = cpp[cpp.index("bool toolWaitsOnNetwork(") :]
    body = body[: body.index("};")]
    return frozenset(re.findall(r'"([a-z_]+\.[a-z_]+)"', body))


def is_turn_only(name: str, listed: frozenset) -> bool:
    return name in listed or (len(name) > 4 and name.startswith("mcp."))


def lan_calls_without_refusal(source: str, listed: frozenset) -> list:
    """(line, tool) of each LAN /mcp helper call naming a turn-only tool in a function
    that never checks the -32003 refusal."""
    found = []
    tree = ast.parse(source)
    for fn in ast.walk(tree):
        if not isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        seg = ast.get_source_segment(source, fn) or ""
        checks_refusal = "32003" in seg or "TURN_ONLY_RPC_ERROR" in seg
        for node in ast.walk(fn):
            if not isinstance(node, ast.Call):
                continue
            callee = node.func.attr if isinstance(node.func, ast.Attribute) else getattr(node.func, "id", "")
            if callee not in LAN_MCP_HELPERS:
                continue
            if checks_refusal:
                continue
            for arg in node.args:
                if isinstance(arg, ast.Constant) and isinstance(arg.value, str) and is_turn_only(arg.value, listed):
                    found.append((node.lineno, arg.value))
    return found


def test_the_guard_reads_the_firmware_list():
    listed = turn_only_tools(GUARD_SRC.read_text(encoding="utf-8"))
    assert {"image.generate", "image.look", "reply.telegram", "web.search", "device.control"} <= listed


def test_no_leg_expects_a_result_from_a_turn_only_tool_over_the_lan():
    listed = turn_only_tools(GUARD_SRC.read_text(encoding="utf-8"))
    offenders = [
        f"{path.relative_to(HIL_DIR)}:{line} {tool}"
        for path in sorted(HIL_DIR.rglob("*.py"))
        for line, tool in lan_calls_without_refusal(path.read_text(encoding="utf-8"), listed)
    ]
    assert not offenders, "a turn-only tool sent over LAN /mcp with no -32003 check:\n" + "\n".join(offenders)


def test_the_guard_flags_the_old_shapes_and_passes_the_refusal():
    listed = frozenset({"image.generate"})
    old = (
        "def test_2():\n"
        "    img = _mcp('image.generate', {})\n"
        "    assert 'during a turn' in img\n"
        "def leg():\n"
        "    out = mcp_call(net, ip, 'mcp.srv.echo', {})\n"
    )
    assert lan_calls_without_refusal(old, listed) == [(2, "image.generate"), (5, "mcp.srv.echo")]
    fixed = (
        "def test_2():\n"
        "    img = _mcp_envelope('image.generate', {})\n"
        "    assert img['error']['code'] == TURN_ONLY_RPC_ERROR\n"
        "def leg():\n"
        "    _mcp('session.spawn', {})\n"
    )
    assert lan_calls_without_refusal(fixed, listed) == []
