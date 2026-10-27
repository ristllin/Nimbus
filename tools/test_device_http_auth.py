#!/usr/bin/env python3
"""Host tests for how the tools that talk to a device sign in (CUM-514, the CUM-45 class).

Since CUM-45 the device API takes the access token only from the X-Nimbus-Token header
(any method) or a `t` form field on POST; a ?t= URL query is ignored (a URL lands in
logs and history). tools/connectors_setup.py still built `/api/connectors?t=<token>` and
could no longer sign in at all. These tests drive every device client in tools/ against
a fake device that enforces the firmware rule (src/net/webui.cpp webAuthOk), and scan
tools/ statically so a new tool that puts the token in a URL, or talks to the device
API without the header, fails here. Nothing here reaches a real device or reads .env."""

from __future__ import annotations

import ast
import contextlib
import http.server
import importlib.util
import io
import json
import re
import threading
import urllib.parse
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
TOKEN = "test-token-0123456789abcdef"  # a fake; the fake device accepts only this


def _load(name):
    spec = importlib.util.spec_from_file_location(f"{name}_under_test", TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CONNECTORS = _load("connectors_setup")
BACKUP = _load("backup_device")
RESTORE = _load("restore_device")


class _FakeDevice(http.server.BaseHTTPRequestHandler):
    """The firmware's auth rule: header first, then a POST form `t`; a query `t` never
    authenticates. Every request is recorded for the assertions."""

    requests: list = []

    def log_message(self, *args):  # keep test output quiet
        pass

    def _handle(self, method):
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        form = {}
        if (self.headers.get("Content-Type") or "").startswith("application/x-www-form-urlencoded"):
            form = urllib.parse.parse_qs(body.decode())
        header = self.headers.get("X-Nimbus-Token")
        authed = header == TOKEN if header is not None else form.get("t") == [TOKEN]
        self.requests.append(
            {"method": method, "path": url.path, "query": query, "header": header, "form": form, "authed": authed}
        )
        reply = (
            {"ok": True, "configured": [], "host": "fake", "keyed": 1}
            if authed
            else {"error": "Signed-in link required"}
        )
        data = json.dumps(reply).encode()
        self.send_response(200 if authed else 401)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")


@contextlib.contextmanager
def _device():
    _FakeDevice.requests = []
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _FakeDevice)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"127.0.0.1:{server.server_address[1]}", _FakeDevice.requests
    finally:
        server.shutdown()
        server.server_close()


def _assert_header_auth(requests, expected_count):
    assert len(requests) == expected_count, requests
    for req in requests:
        assert req["header"] == TOKEN, req  # the header, on every method
        assert "t" not in req["query"], req  # never the URL
        assert req["authed"], req


# ---- connectors_setup.py (the CUM-514 instance) -------------------------------


def test_connectors_api_signs_in_with_the_header_on_get_and_post():
    with _device() as (host, requests):
        state = CONNECTORS.api(host, TOKEN, "GET")
        ok = CONNECTORS.patch(host, TOKEN, {"name": "github", "type": "github", "prov": "openai"})
    assert state["host"] == "fake" and ok is True
    _assert_header_auth(requests, 2)
    assert [r["method"] for r in requests] == ["GET", "POST"]
    assert json.loads(requests[1]["form"]["patch"][0])["name"] == "github"


def test_connectors_main_end_to_end_never_prints_a_secret():
    env = {"GITHUB_PAT": "ghp_fake_pat_value"}  # .env is never read in this test
    out = io.StringIO()
    with _device() as (host, requests):
        saved = CONNECTORS.load_env
        CONNECTORS.load_env = lambda: env
        try:
            with contextlib.redirect_stdout(out):
                CONNECTORS.main(["--ip", host, "--token", TOKEN, "--only", "github"])
        finally:
            CONNECTORS.load_env = saved
    _assert_header_auth(requests, 2)
    assert "OK  github" in out.getvalue()
    assert TOKEN not in out.getvalue() and env["GITHUB_PAT"] not in out.getvalue()


def test_connectors_main_wrong_token_stops_with_a_message_not_a_traceback():
    with _device() as (host, requests):
        saved = CONNECTORS.load_env
        CONNECTORS.load_env = lambda: {}
        try:
            CONNECTORS.main(["--ip", host, "--token", "not-the-token", "--list"])
        except SystemExit as exc:
            message = str(exc.code)
        else:
            raise AssertionError("a refused token must stop the tool")
        finally:
            CONNECTORS.load_env = saved
    assert "401" in message and "--token" in message
    assert len(requests) == 1 and not requests[0]["authed"]


# ---- the other device clients in tools/ (the class) ----------------------------


def test_backup_device_get_signs_in_with_the_header():
    with _device() as (host, requests):
        raw = BACKUP.get(f"http://{host}", TOKEN, "/api/mem/episodic", {"limit": 200, "cold": 1})
    assert json.loads(raw)["ok"] is True
    _assert_header_auth(requests, 1)
    assert requests[0]["query"] == {"limit": ["200"], "cold": ["1"]}


def test_restore_device_requests_sign_in_with_the_header():
    with _device() as (host, requests):
        base = f"http://{host}"
        RESTORE.get_json(base, TOKEN, "/api/state")
        RESTORE.post_import(base, TOKEN, "vectors", "rows", [{"id": 1}], dry_run=True)
        RESTORE.upload_file(base, TOKEN, "proj", "a.txt", b"hello")
    _assert_header_auth(requests, 3)
    assert [r["method"] for r in requests] == ["GET", "POST", "POST"]


# ---- static class guard over every tool -----------------------------------------


def _string_fragments(tree):
    """Every string literal (and literal part of an f-string), minus docstrings."""
    docstrings = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and node.body:
            first = node.body[0]
            if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant):
                docstrings.add(id(first.value))
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in docstrings:
            yield node.value


def _python_tools():
    """Every Python file under tools/, except vendored trees and this guard itself
    (its counter-tests quote the forbidden shapes on purpose)."""
    skip = ("harness-lab", "webui_harness", "node_modules")
    me = Path(__file__).resolve()
    return sorted(p for p in TOOLS.rglob("*.py") if not any(part in skip for part in p.parts) and p.resolve() != me)


_QUERY_TOKEN = re.compile(r"[?&]t=")
# A device API path: a fragment that starts the path (`base + "/api/..."`, or the literal
# tail of f"http://{ip}/api/...", optionally after a port) or a plain-http URL to a host
# (the device's LAN API is http only). A third party's https API that happens to have an
# /api/ segment (Z.ai's model list) and prose that mentions a route are not device calls.
_DEVICE_API = re.compile(r"^(?::\d+)?/api/|http://[^\s/]*/api/")


def _device_api_fragments(source):
    return [text for text in _string_fragments(ast.parse(source)) if _DEVICE_API.search(text)]


def _query_token_fragments(source):
    return [text for text in _string_fragments(ast.parse(source)) if _QUERY_TOKEN.search(text)]


def test_guard_catches_the_old_connectors_url_shape():
    # Counter-test: the exact pre-fix line from connectors_setup.py must be flagged.
    assert _query_token_fragments('url = f"http://{ip}/api/connectors?t={token}"\n')
    assert _query_token_fragments('url = base + "/api/state?x=1&t=" + tok\n')
    # A docstring that explains the rule is not a URL and is not flagged.
    assert not _query_token_fragments('"""The device ignores a ?t= query."""\n')


def test_no_tool_puts_the_device_token_in_a_url():
    offenders = []
    for path in _python_tools():
        for text in _query_token_fragments(path.read_text(encoding="utf-8")):
            offenders.append(f"{path.relative_to(TOOLS)}: {text!r}")
    assert not offenders, "token in a URL query (the device ignores a query token since CUM-45): " + "; ".join(
        offenders
    )


def test_device_api_detection_ignores_third_party_apis_and_prose():
    # Counter-test: every shape a device client builds is seen as one.
    assert _device_api_fragments('url = f"http://{ip}/api/state"\n')
    assert _device_api_fragments('url = f"http://{ip}:80/api/state"\n')
    assert _device_api_fragments('get(base + "/api/files/upload", tok)\n')
    assert _device_api_fragments('url = "http://192.168.4.1/api/state"\n')
    # A third-party https API and a route named in prose are not device calls.
    assert not _device_api_fragments('url = "https://api.z.ai/api/paas/v4/models"\n')
    assert not _device_api_fragments('PLAN = """\n  # raising it over\n  #   /api/orch lets it run\n"""\n')


def test_every_tool_that_calls_the_device_api_sends_the_token_header():
    clients, missing = [], []
    for path in _python_tools():
        fragments = list(_string_fragments(ast.parse(path.read_text(encoding="utf-8"))))
        if not any(_DEVICE_API.search(text) for text in fragments):
            continue
        clients.append(path.name)
        if "X-Nimbus-Token" not in fragments:
            missing.append(path.name)
    assert not missing, f"device API clients without the X-Nimbus-Token header: {missing}"
    # The guard must actually be guarding something: the known clients are found.
    assert {"connectors_setup.py", "backup_device.py", "restore_device.py"} <= set(clients), clients


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("device HTTP auth: header-only sign-in across every tools/ device client passed")
