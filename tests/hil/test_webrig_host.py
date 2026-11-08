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


def _chat_url_names(src: str, fn: ast.AST) -> set:
    """Names bound to a /api/chat URL inside one function."""
    names = set()
    for node in ast.walk(fn):
        if isinstance(node, ast.Assign) and "/api/chat" in (ast.get_source_segment(src, node.value) or ""):
            names |= {t.id for t in node.targets if isinstance(t, ast.Name)}
    return names


def _is_legacy_chat_get(src: str, node: ast.AST, chat_names: set) -> bool:
    """A .get/.get_json call on a /api/chat URL with no turn id or watch flag."""
    if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)):
        return False
    if node.func.attr not in ("get", "get_json") or not node.args:
        return False
    first = node.args[0]
    names_chat = isinstance(first, ast.Name) and first.id in chat_names
    if not names_chat and "/api/chat" not in (ast.get_source_segment(src, first) or ""):
        return False
    seg = ast.get_source_segment(src, node) or ""
    return "turn" not in seg and "watch" not in seg


def _chat_gets_without_turn(path: pathlib.Path) -> list:
    """Lines of `path` that GET /api/chat with no turn id or watch flag in the call."""
    src = path.read_text(encoding="utf-8")
    bad = []
    for fn in ast.walk(ast.parse(src)):
        if isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)):
            names = _chat_url_names(src, fn)
            bad += [f"{path.name}:{n.lineno}" for n in ast.walk(fn) if _is_legacy_chat_get(src, n, names)]
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


# ---- lane SNI: a failure notice is not an answer ---------------------------------------------------------------


class _Reply:
    def __init__(self, body, code=200):
        self.status_code, self._body, self.text = code, body, str(body)

    def json(self):
        return self._body


class _Session:
    """The two web-chat routes: POST answers with a turn id, GET with that turn done and `reply`."""

    def __init__(self, reply):
        self.reply = reply

    def post(self, url, data=None, timeout=None):
        return _Reply({"ok": True, "turn": "t1"})

    def get(self, url, params=None, timeout=None):
        return _Reply({"turn": "t1", "reply": self.reply, "pending": False, "done": True, "gone": False})


def test_chat_turn_returns_an_answer():
    from webrig import chat_turn

    assert chat_turn(_Session("391"), "10.0.0.9", "What is 17 times 23?", timeout_s=1, poll_s=0) == "391"


@pytest.mark.parametrize(
    "reply",
    [
        "That didn't finish - anthropic could not be reached; openai and mistral also failed.",
        "That didn't finish - the device ran out of working memory for this request.",
    ],
)
def test_chat_turn_fails_on_the_devices_failure_notice(reply):
    from webrig import chat_turn

    with pytest.raises(pytest.fail.Exception, match="failure notice"):
        chat_turn(_Session(reply), "10.0.0.9", "What is 17 times 23?", timeout_s=1, poll_s=0)
