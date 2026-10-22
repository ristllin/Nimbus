"""webrig host tier: the turn-reading and log-stitching seams. No board.

Two classes of release-run failure came from the seams this file guards:
- a leg that polled the legacy single ``/api/chat`` slot read the PREVIOUS reply
  while its own message waited (L16 read an earlier file's "OK" as the /compact
  reply, L55 read turn 1 as turn 2), so no HIL leg may poll ``GET /api/chat``
  without a turn id (or ``watch``);
- a leg that read /api/log once after a turn saw only the ring's last 1,280 bytes,
  so legs follow and stitch it (webrig.stitch).
"""

from __future__ import annotations

import ast
import pathlib

import pytest

from webrig import stitch

pytestmark = pytest.mark.host

HIL = pathlib.Path(__file__).parent


def _chat_gets_without_turn(path: pathlib.Path) -> list:
    """Lines of `path` that GET /api/chat with no turn id or watch flag in the call."""
    src = path.read_text(encoding="utf-8")
    tree = ast.parse(src)
    bad = []
    for fn in ast.walk(tree):
        if not isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        chat_names = set()
        for node in ast.walk(fn):  # names bound to a /api/chat URL inside this function
            if isinstance(node, ast.Assign) and "/api/chat" in ast.get_source_segment(src, node.value or node):
                chat_names |= {t.id for t in node.targets if isinstance(t, ast.Name)}
        for node in ast.walk(fn):
            if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)):
                continue
            if node.func.attr not in ("get", "get_json") or not node.args:
                continue
            seg = ast.get_source_segment(src, node) or ""
            first = node.args[0]
            names_chat = isinstance(first, ast.Name) and first.id in chat_names
            if ("/api/chat" in ast.get_source_segment(src, first) or names_chat) and not (
                "turn" in seg or "watch" in seg
            ):
                bad.append(f"{path.name}:{node.lineno}")
    return bad


def test_no_leg_polls_the_legacy_chat_slot():
    files = sorted(HIL.glob("test_*.py")) + [HIL / "connectors.py", HIL / "webrig.py"]
    bad = [hit for f in files if f.exists() for hit in _chat_gets_without_turn(f)]
    assert not bad, f"GET /api/chat without a turn id (reads the previous reply): {bad}"


def test_the_lint_catches_the_legacy_shape(tmp_path):
    legacy = tmp_path / "test_legacy.py"
    legacy.write_text(
        "def _chat(s, ip, text):\n"
        "    url = f'http://{ip}/api/chat'\n"
        "    s.post(url, data={'text': text})\n"
        "    return s.get(url, timeout=10).json()\n"
    )
    assert _chat_gets_without_turn(legacy) == ["test_legacy.py:4"]
    by_turn = tmp_path / "test_turn.py"
    by_turn.write_text(
        "def _chat(s, ip, turn):\n"
        "    url = f'http://{ip}/api/chat'\n"
        "    return s.get(url, params={'turn': turn}, timeout=10).json()\n"
    )
    assert _chat_gets_without_turn(by_turn) == []


def test_stitch_appends_only_the_new_part():
    ring = "x" * 1000 + "line A\n"
    acc, gap = stitch("", ring)
    acc, gap = stitch(acc, ring[200:] + "line B\nline C\n")
    assert acc == ring + "line B\nline C\n" and not gap


def test_stitch_quiet_ring_adds_nothing():
    ring = "a" * 300 + "b" * 300
    acc, gap = stitch(ring, ring)
    assert acc == ring and not gap


def test_stitch_marks_a_turned_over_ring_as_a_gap():
    acc, gap = stitch("old " * 100, "entirely new text " * 20)
    assert gap and "[log gap]" in acc
