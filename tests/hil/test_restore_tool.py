"""Host-side tests for tools/restore_device.py (CUM-406, no board needed).

Drives the real tool as a subprocess against a mock device HTTP server so the
failure-handling contract is actually exercised: a MANIFEST that disagrees with the
folder must refuse before pushing anything; an ok:false / lossy device reply must
never be reported as a clean success. Marked `host` so it runs in a plain
`pytest tests/hil` without --allow-hardware.
"""

import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "restore_device.py"

pytestmark = pytest.mark.host


class _Handler(BaseHTTPRequestHandler):
    # Behaviour is injected per-server via .cfg on the server instance.
    def log_message(self, *a):  # silence
        pass

    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        cfg = self.server.cfg
        if self.path.startswith("/api/state"):
            self._send(200, {"devName": "TestDev"})
        elif self.path.startswith("/api/mem/stats"):
            self._send(200, {"episodicMsgs": cfg.get("episodicMsgs", 0), "vectors": 0})
        elif self.path.startswith("/api/files/list"):
            self._send(200, {"files": []})
        else:
            self._send(404, {"error": "nope"})

    def do_POST(self):
        cfg = self.server.cfg
        n = int(self.headers.get("Content-Length", 0))
        payload = json.loads(self.rfile.read(n)) if n else {}
        self.server.imports.append(payload)  # record what was pushed
        kind = payload.get("kind")
        count = payload.get("count", 0)
        if cfg.get("ok_false"):
            self._send(200, {"ok": False, "error": "simulated device rejection"})
            return
        if kind == "vectors":
            self._send(
                200,
                {
                    "ok": True,
                    "added": count,
                    "replaced": 0,
                    "skippedNoVec": 0,
                    "widthErrors": 0,
                    "badRows": 0,
                    "evicted": cfg.get("evicted", 0),
                    "stored": count,
                    "dims": 4,
                },
            )
        elif kind == "episodic":
            self._send(
                200,
                {
                    "ok": True,
                    "added": count,
                    "dupSkipped": 0,
                    "badRows": 0,
                    "stored": count,
                    "truncated": 0,
                    "total": count,
                },
            )
        elif kind == "scratchpad":
            self._send(200, {"ok": True, "active": 1, "short": 0, "mid": 0, "long": 0})
        else:
            self._send(200, {"ok": False, "error": "bad kind"})


def _serve(cfg):
    srv = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    srv.cfg = cfg
    srv.imports = []
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def _backup(tmp, vectors=2, episodic=1, manifest_vectors=None, manifest_episodic=None):
    """Write a minimal backup folder. manifest_* override the counts to force a mismatch."""
    d = tmp / "TestDev" / "2026-09-13_1200"
    d.mkdir(parents=True)
    entries = [{"id": f"v{i}", "content": f"m{i}", "importance": 0.5, "vec": "AQIDBA=="} for i in range(vectors)]
    (d / "vectors.json").write_text(json.dumps({"total": len(entries), "entries": entries}))
    (d / "episodic.jsonl").write_text(
        "".join(
            json.dumps(
                {"id": f"m{i:08x}", "session": "web", "ts": i, "role": "user", "kind": "message", "text": f"hi {i}"}
            )
            + "\n"
            for i in range(episodic)
        )
    )
    (d / "scratchpad.json").write_text(json.dumps({"active": "x", "short": [], "mid": [], "long": []}))
    (d / "MANIFEST.json").write_text(
        json.dumps(
            {
                "device": "TestDev",
                "vectorCount": vectors if manifest_vectors is None else manifest_vectors,
                "episodicCount": episodic if manifest_episodic is None else manifest_episodic,
                "files": [],
                "errors": [],
            }
        )
    )
    return d


def _run(src, ip, extra=()):
    return subprocess.run(
        [sys.executable, str(TOOL), "--ip", ip, "--token", "tok", "--src", str(src), *extra],
        capture_output=True,
        text=True,
    )


def test_manifest_mismatch_refused_before_push(tmp_path):
    # vectors.json has 2 entries but the MANIFEST claims 5 -> refuse, push nothing.
    src = _backup(tmp_path, vectors=2, manifest_vectors=5)
    srv = _serve({})
    try:
        r = _run(src, f"127.0.0.1:{srv.server_address[1]}")
    finally:
        srv.shutdown()
    assert r.returncode == 2, r.stderr
    assert "MANIFEST verification FAILED" in r.stderr
    assert srv.imports == []  # nothing was pushed


def test_ok_false_is_hard_error(tmp_path):
    src = _backup(tmp_path)
    srv = _serve({"ok_false": True})
    try:
        r = _run(src, f"127.0.0.1:{srv.server_address[1]}")
    finally:
        srv.shutdown()
    assert r.returncode == 2, (r.stdout, r.stderr)
    assert "rejected" in r.stderr.lower() or "error" in r.stderr.lower()


def test_evicted_is_warning_exit_3(tmp_path):
    src = _backup(tmp_path)
    srv = _serve({"evicted": 1})
    try:
        r = _run(src, f"127.0.0.1:{srv.server_address[1]}")
    finally:
        srv.shutdown()
    assert r.returncode == 3, (r.stdout, r.stderr)
    assert "evicted" in r.stderr.lower()


def test_clean_restore_exit_0(tmp_path):
    src = _backup(tmp_path)
    srv = _serve({})
    try:
        r = _run(src, f"127.0.0.1:{srv.server_address[1]}")
    finally:
        srv.shutdown()
    assert r.returncode == 0, (r.stdout, r.stderr)
    # vectors + episodic + scratchpad were each pushed.
    kinds = {p["kind"] for p in srv.imports}
    assert {"vectors", "episodic", "scratchpad"} <= kinds
