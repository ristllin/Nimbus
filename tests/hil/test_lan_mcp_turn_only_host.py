"""Host tier: no HIL leg expects a result from a turn-only tool over LAN /mcp. No board.

Since v4.6.1 (HFW) the LAN endpoint answers every tool that waits on the network
(``nimbus::sys::toolWaitsOnNetwork``: image generation, a photo caption, the reply and
speech tools, web search, device.control, and every ``mcp.*`` tool of a connected
server) with JSON-RPC error -32003 before any request: such a tool runs only in the
device's own turns, never on the web task the watchdog watches. The test-only
/api/test/astool route runs on that same task and answers the same. Two legs (L65
test_2's image.generate and L15b's ``mcp.*`` echo) kept reading a result there and
could never pass.

The rule: a function that sends a turn-only tool to the LAN endpoint (a helper that
posts to ``/mcp`` or ``/api/test/astool``, found by what it posts, or a tools/call
body built inline next to such a post) checks the -32003 refusal in its own code (the
literal or a ``TURN_ONLY_RPC_ERROR`` constant; a comment or a string does not count).
A property that needs the tool to run moves to a turn.
"""

from __future__ import annotations

import ast
import functools
import re
from pathlib import Path
from typing import NamedTuple

import pytest

pytestmark = pytest.mark.host

HIL_DIR = Path(__file__).resolve().parent
GUARD_SRC = HIL_DIR.parents[1] / "lib" / "core" / "src" / "sys_web_guard.cpp"
# A string fragment that names the LAN tool endpoint or the test astool route (a plain
# http URL to a host, or the path itself); a third party's https .../mcp URL is not one.
_LAN_ROUTE = re.compile(r"(?:^|http://[^\s/]*)(?:/mcp(?:$|[?#])|/api/test/astool)")
_REFUSAL_NAMES = frozenset({"TURN_ONLY_RPC_ERROR", "_TURN_ONLY_RPC_ERROR"})


def turn_only_tools(cpp: str) -> frozenset:
    """The names toolWaitsOnNetwork lists (the mcp.* prefix is checked separately)."""
    body = cpp[cpp.index("bool toolWaitsOnNetwork(") :]
    body = body[: body.index("};")]
    return frozenset(re.findall(r'"([a-z_]+\.[a-z_]+)"', body))


def is_turn_only(name: str, listed: frozenset) -> bool:
    return name in listed or (len(name) > 4 and name.startswith("mcp."))


class FnFacts(NamedTuple):
    """What one function does, read in a single walk of its tree."""

    name: str
    posts: bool  # a string in it names the LAN endpoint or the astool route
    refuses: bool  # its CODE checks -32003 (comments are not in the tree)
    callees: frozenset
    call_args: tuple  # (callee, line, string constant) for each call's positional args
    body_names: tuple  # (line, string constant) for each {"name": ...} in it


def _callee(call: ast.Call) -> str:
    return call.func.attr if isinstance(call.func, ast.Attribute) else getattr(call.func, "id", "")


def _is_refusal(n: ast.AST) -> bool:
    """-32003 in code: the literal (32003 under a unary minus, or -32003) or a
    TURN_ONLY_RPC_ERROR constant. A string constant is not a number, so a quoted
    "-32003" does not count."""
    if isinstance(n, ast.UnaryOp) and isinstance(n.op, ast.USub) and isinstance(n.operand, ast.Constant):
        return type(n.operand.value) is int and n.operand.value == 32003
    if isinstance(n, ast.Constant):
        return type(n.value) is int and n.value == -32003
    return (isinstance(n, ast.Name) and n.id in _REFUSAL_NAMES) or (
        isinstance(n, ast.Attribute) and n.attr in _REFUSAL_NAMES
    )


def _strings(nodes) -> list:
    """(line, value) of the string constants among `nodes`."""
    return [(n.lineno, n.value) for n in nodes if isinstance(n, ast.Constant) and isinstance(n.value, str)]


def _body_names(d: ast.Dict) -> list:
    """The string values under a "name" key of one dict literal (a tools/call body)."""
    return _strings(v for k, v in zip(d.keys, d.values) if isinstance(k, ast.Constant) and k.value == "name")


def _facts(fn) -> FnFacts:
    posts = refuses = False
    callees, call_args, body_names = set(), [], []
    for n in ast.walk(fn):
        refuses = refuses or _is_refusal(n)
        if isinstance(n, ast.Constant) and isinstance(n.value, str) and _LAN_ROUTE.search(n.value):
            posts = True
        elif isinstance(n, ast.Call):
            callees.add(_callee(n))
            call_args += [(_callee(n), line, v) for line, v in _strings(n.args)]
        elif isinstance(n, ast.Dict):
            body_names += _body_names(n)
    return FnFacts(fn.name, posts, refuses, frozenset(callees), tuple(call_args), tuple(body_names))


def file_facts(source: str) -> list:
    """The facts of every function (nested ones included) in one source file."""
    return [_facts(n) for n in ast.walk(ast.parse(source)) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]


def lan_helpers(files: list) -> frozenset:
    """Names of the suite's functions that post a tools/call to the LAN endpoint, and
    of the wrappers that call one (``_call`` -> ``_rpc`` -> the astool route)."""
    fns = [f for facts in files for f in facts]
    helpers = {f.name for f in fns if f.posts}
    while True:
        more = {f.name for f in fns if f.name not in helpers and f.callees & helpers}
        if not more:
            return frozenset(helpers)
        helpers |= more


def lan_calls_without_refusal(facts: list, listed: frozenset, helpers: frozenset) -> list:
    """(line, tool) of each turn-only tool sent to the LAN endpoint in a function that
    never checks the -32003 refusal: as an argument of a LAN helper, or as the "name"
    of a tools/call body built in a function that posts to the endpoint itself."""
    found = []
    for f in facts:
        if f.refuses:
            continue
        found += [(line, v) for callee, line, v in f.call_args if callee in helpers and is_turn_only(v, listed)]
        if f.posts:
            found += [(line, v) for line, v in f.body_names if is_turn_only(v, listed)]
    return sorted(set(found))


@functools.lru_cache(maxsize=1)
def _suite() -> tuple:
    return tuple((path, file_facts(path.read_text(encoding="utf-8"))) for path in sorted(HIL_DIR.rglob("*.py")))


def test_the_guard_reads_the_firmware_list_and_finds_the_helpers():
    listed = turn_only_tools(GUARD_SRC.read_text(encoding="utf-8"))
    assert {"image.generate", "image.look", "reply.telegram", "web.search", "device.control"} <= listed
    helpers = lan_helpers([facts for _, facts in _suite()])
    assert {"mcp_call", "_mcp_envelope", "_mcp_as_owner", "_astool"} <= helpers, sorted(helpers)


def test_no_leg_expects_a_result_from_a_turn_only_tool_over_the_lan():
    listed = turn_only_tools(GUARD_SRC.read_text(encoding="utf-8"))
    suite = _suite()
    helpers = lan_helpers([facts for _, facts in suite])
    offenders = [
        f"{path.relative_to(HIL_DIR)}:{line} {tool}"
        for path, facts in suite
        for line, tool in lan_calls_without_refusal(facts, listed, helpers)
    ]
    assert not offenders, "a turn-only tool sent over LAN /mcp with no -32003 check:\n" + "\n".join(offenders)


_HELPERS_SRC = (
    "def _mcp(tool, args):\n"
    "    return _S.post(f'http://{_IP}/mcp', data=body(tool, args))\n"
    "def _astool(ip, chat, tool, args):\n"
    "    return _S.post(f'http://{ip}/api/test/astool', data={'chat': chat})\n"
    "def _call(rig, chat, tool):\n"
    "    return _astool(rig.ip, chat, tool, {})\n"
    "def _linear_url():\n"
    "    return 'https://mcp.linear.app/mcp'\n"
)


def test_the_guard_flags_the_old_shapes_and_passes_the_refusal():
    listed = frozenset({"image.generate", "web.search"})
    helpers = lan_helpers([file_facts(_HELPERS_SRC)])
    assert helpers == {"_mcp", "_astool", "_call"}
    old = (
        "def test_2():\n"
        "    img = _mcp('image.generate', {})\n"
        "    assert 'during a turn' in img\n"
        "def leg():\n"
        "    out = _astool(ip, 'web', 'mcp.srv.echo', {})\n"
        "def inline():\n"
        "    body = {'name': 'web.search', 'arguments': {}}\n"
        "    r = s.post(f'http://{ip}/mcp', data=body)\n"
        "def commented():\n"
        "    out = _mcp('web.search', {})  # answers -32003 since HFW\n"
        "    assert '-32003' in str(out)\n"
        "def wrapped():\n"
        "    assert _call(rig, 'web', 'image.generate')['result']\n"
    )
    assert lan_calls_without_refusal(file_facts(old), listed, helpers) == [
        (2, "image.generate"),
        (5, "mcp.srv.echo"),
        (7, "web.search"),
        (10, "web.search"),
        (13, "image.generate"),
    ]
    fixed = (
        "def test_2():\n"
        "    img = _mcp('image.generate', {})\n"
        "    assert img['error']['code'] == TURN_ONLY_RPC_ERROR\n"
        "def leg():\n"
        "    out = _astool(ip, 'web', 'mcp.srv.echo', {})\n"
        "    assert out['error']['code'] == -32003\n"
        "def other():\n"
        "    _mcp('session.spawn', {})\n"
        "    third_party = 'https://mcp.linear.app/mcp'\n"
    )
    assert lan_calls_without_refusal(file_facts(fixed), listed, helpers) == []
