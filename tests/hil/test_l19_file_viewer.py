"""§L19 - the file explorer's preview, and the reason it is narrow.

Files reach this device from anyone it talks to: a Telegram member, a web
upload, the assistant itself. The web UI keeps the device access token in
localStorage. So an uploaded ``.html`` or ``.svg`` rendered inline would execute
on the device's own origin with that token in reach - stored XSS straight to
full device control.

The viewer therefore renders only what cannot execute. These tests assert the
server enforces that regardless of what the request asks for, and that the
client's "view" link list has not drifted away from the server's allowlist -
drift is precisely how a preview link would one day point at something that runs.

Markers: ``hil`` + ``net`` - pure LAN.
"""

from __future__ import annotations

import os
import re
import uuid

import pytest

try:
    import requests
except ImportError:  # pragma: no cover
    requests = None

pytestmark = [pytest.mark.hil, pytest.mark.net]

PROJECT = "l19"

# (filename, may it render inline?)
CASES = [
    ("shot.png", True),
    ("photo.jpg", True),
    ("photo.jpeg", True),
    ("anim.gif", True),
    ("pic.webp", True),
    ("notes.txt", True),
    ("rows.csv", True),
    ("page.html", False),  # executes on this origin
    ("vec.svg", False),  # SVG carries <script> too
    ("doc.pdf", False),  # the browser's PDF viewer is a scriptable surface
    ("blob.bin", False),
]


def _u(rig, path):
    ip, tok = rig
    sep = "&" if "?" in path else "?"
    return f"http://{ip}{path}{sep}t={tok}"


@pytest.fixture(scope="module")
def rig():
    if requests is None:
        pytest.skip("requests not installed")
    ip = os.environ.get("NIMBUS_TEST_IP")
    tok = os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        pytest.skip("set NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN to run L19")
    handle = (ip, tok)
    lst = requests.get(_u(handle, "/api/files/list"), timeout=10)
    if lst.status_code != 200:
        pytest.skip("file routes unavailable")
    if not lst.json().get("present"):
        pytest.skip("no SD card - the artifact store is unavailable on this board")
    yield handle
    for name, _ in CASES:
        try:
            requests.post(_u(handle, "/api/files/rm"), data={"project": PROJECT, "name": name}, timeout=10)
        except Exception:  # noqa: BLE001 - best effort cleanup
            pass


def _save(rig, name, text):
    import json

    body = json.dumps(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": "artifact.save", "arguments": {"project": PROJECT, "name": name, "text": text}},
        }
    )
    r = requests.post(_u(rig, "/api/test/astool"), data={"chat": "web", "body": body}, timeout=20)
    assert r.status_code == 200, r.text
    return "saved" in r.text


@pytest.mark.parametrize("name,viewable", CASES)
def test_only_non_executing_types_render_inline(rig, name, viewable):
    """?inline=1 is a REQUEST, not a decision - the server decides."""
    marker = f"L19-{uuid.uuid4().hex[:8]}"
    if not _save(rig, name, f"content {marker}"):
        pytest.skip(f"could not stage {name}")

    r = requests.get(_u(rig, f"/api/files/dl?inline=1&project={PROJECT}&name={name}"), timeout=15)
    assert r.status_code == 200, r.text
    disp = r.headers.get("Content-Disposition", "")
    if viewable:
        assert disp.startswith("inline"), f"{name} should preview inline, got {disp!r}"
    else:
        assert disp.startswith("attachment"), f"SECURITY: {name} rendered inline on the device's own origin ({disp!r})"

    # Never let the browser second-guess the declared type - a .txt sniffed as
    # HTML would defeat the allowlist entirely.
    assert r.headers.get("X-Content-Type-Options") == "nosniff", name


def test_a_plain_download_is_never_inline(rig):
    """Without ?inline=1 everything keeps the download disposition."""
    if not _save(rig, "notes.txt", "hello"):
        pytest.skip("could not stage the file")
    r = requests.get(_u(rig, f"/api/files/dl?project={PROJECT}&name=notes.txt"), timeout=15)
    assert r.headers.get("Content-Disposition", "").startswith("attachment")


def test_preview_response_carries_a_locked_down_csp(rig):
    """Defence in depth: whatever renders gets none of the page's privileges."""
    if not _save(rig, "notes.txt", "hello"):
        pytest.skip("could not stage the file")
    r = requests.get(_u(rig, f"/api/files/dl?inline=1&project={PROJECT}&name=notes.txt"), timeout=15)
    csp = r.headers.get("Content-Security-Policy", "")
    assert "sandbox" in csp, f"no sandbox in the preview CSP: {csp!r}"
    assert "default-src 'none'" in csp, csp


def test_client_view_links_match_the_server_allowlist(rig):
    """The UI must not offer "view" for anything the server refuses to inline.

    Both lists exist for the same reason and live in different files. If they
    drift, a preview link eventually points at something that executes. So this
    reads the SERVED page, evaluates its real matching rules, and compares them
    against what the device actually does for each extension - rather than
    trusting that whoever edits one list remembers the other.

    (This caught a real one: the UI offered a preview for .json, which the
    server sends as application/json and refuses to render inline.)
    """
    ip, tok = rig
    page = requests.get(f"http://{ip}/?t={tok}", timeout=30).text
    m = re.search(r"function _fviewable\(name\)\{(.*?)\n\}", page, re.S)
    assert m, "the file explorer's _fviewable() is not in the served page"

    # Pull the literal regexes out of the shipped function and evaluate them the
    # way the browser would, so an alternation like (jpe?g) is honoured exactly.
    patterns = re.findall(r"/(\\\.\([^/]+?\)\$)/", m.group(1))
    assert patterns, f"could not read the client's match rules from: {m.group(1)!r}"

    def client_offers(name: str) -> bool:
        n = name.lower()
        return any(re.search(p, n) for p in patterns)

    for name, server_inlines in CASES:
        assert client_offers(name) == server_inlines, (
            f"DRIFT on {name}: the UI "
            f"{'offers' if client_offers(name) else 'hides'} a preview link but the "
            f"device {'renders it inline' if server_inlines else 'refuses to'}"
        )
