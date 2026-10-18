#!/usr/bin/env python3
"""Self-test for tg_archive_import.py against a simulated device (no board, no network).

Runnable directly (`python3 tools/test_tg_archive_import.py`, as nimbusd's `make test`
does) or under pytest. The fake device implements the two archive routes the tool uses
with the device's own rules: the token header, the r<16 hex>.<ext> name pattern, one
upload at a time (429), never overwriting a file, and paged index reads.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import re
import sys
import tempfile
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = "test-" + "token-1234567890"


def _load():
    p = pathlib.Path(__file__).with_name("tg_archive_import.py")
    spec = importlib.util.spec_from_file_location("tg_archive_import", p)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod  # dataclasses resolve annotations through sys.modules
    spec.loader.exec_module(mod)
    return mod


M = _load()
NAME_RE = re.compile(r"^r[0-9a-f]{16}\.[a-z0-9]{1,5}$")


class FakeDevice:
    def __init__(self, root: pathlib.Path):
        self.root = root
        self.rows: dict[tuple[str, str], list[dict]] = {}
        self.busy_once = True
        self.uploads = 0
        self.overwrites = 0


def make_handler(dev: FakeDevice):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _reply(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _q(self):
            u = urllib.parse.urlparse(self.path)
            return u.path, {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}

        def do_POST(self):
            path, q = self._q()
            data = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            if self.headers.get("X-Nimbus-Token") != TOKEN:
                return self._reply(401, {"error": "auth required"})
            if path != "/api/tg/archive/import" or not NAME_RE.match(q.get("name", "")):
                return self._reply(400, {"error": "bad name"})
            if dev.busy_once:
                dev.busy_once = False
                return self._reply(429, {"error": "another import is in progress"})
            f = dev.root / q["chat"] / q["day"] / q["name"]
            if f.exists():
                return self._reply(200, {"ok": True, "skipped": True})
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(data)
            dev.uploads += 1
            dev.rows.setdefault((q["chat"], q["day"]), []).append(
                {
                    "v": 1,
                    "dir": "restored",
                    "file": q["name"],
                    "kind": q.get("kind"),
                    "role": q.get("role"),
                    "bytes": len(data),
                }
            )
            return self._reply(200, {"ok": True, "bytes": len(data)})

        def do_GET(self):
            path, q = self._q()
            if self.headers.get("X-Nimbus-Token") != TOKEN:
                return self._reply(401, {"error": "auth required"})
            rows = dev.rows.get((q.get("chat"), q.get("day")), [])
            start, limit = int(q.get("from", 0)), int(q.get("limit", 50))
            page = rows[start : start + limit]
            return self._reply(200, {"rows": page, "next": start + len(page), "more": start + len(page) < len(rows)})

    return H


@contextlib.contextmanager
def fake_device():
    with tempfile.TemporaryDirectory() as d:
        dev = FakeDevice(pathlib.Path(d))
        srv = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(dev))
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        try:
            yield dev, f"http://127.0.0.1:{srv.server_address[1]}"
        finally:
            srv.shutdown()


def make_backup(tmp: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    b = tmp / "2026-10-18_blobs"
    b.mkdir()
    (b / "0779d8d2849f5e2b.jpg").write_bytes(b"\xff\xd8" + b"a" * 1000)
    (b / "5012d1411b7f028c.ogg").write_bytes(b"OggS" + b"b" * 500)
    (b / "002d5cf5cb9567fe.txt").write_bytes(b"tool output")
    (b / "aaaaaaaaaaaaaaaa.txt").write_bytes(b"no row names me")
    (b / "MANIFEST.md").write_text("notes")
    (b / "bbbbbbbbbbbbbbbb.txt").write_bytes(b"")  # empty: skipped
    rows = [
        {
            "id": "m1",
            "session": "6098532481",
            "ts": "497802",
            "role": "user",
            "kind": "image",
            "blob": "/mem/blobs/0779d8d2849f5e2b.jpg",
        },
        {
            "id": "m0",
            "session": "6098532481",
            "ts": "497700",
            "role": "user",
            "kind": "image",
            "blob": "/mem/blobs/0779d8d2849f5e2b.jpg",
        },
        {
            "id": "m2",
            "session": "6098532481",
            "ts": "497790",
            "role": "user",
            "kind": "audio",
            "blob": "/mem/blobs/5012d1411b7f028c.ogg",
        },
        {
            "id": "m3",
            "session": "system",
            "ts": "497795",
            "role": "system",
            "kind": "tool_output",
            "blob": "/mem/blobs/002d5cf5cb9567fe.txt",
        },
    ]
    rp = tmp / "rows.jsonl"
    rp.write_text("\n".join(json.dumps(r) for r in rows))
    return b, rp


def test_plan_places_each_sidecar_by_its_row():
    with tempfile.TemporaryDirectory() as d:
        b, rp = make_backup(pathlib.Path(d))
        items, skipped = M.plan(b, M.load_rows(str(rp)))
        by = {it.name: it for it in items}
        assert sorted(skipped) == ["MANIFEST.md", "bbbbbbbbbbbbbbbb.txt"]
        photo = by["r0779d8d2849f5e2b.jpg"]
        assert (photo.chat, photo.kind, photo.role) == ("6098532481", "photo", "user")
        assert photo.date == 497700 * 3600  # the earliest row that names it
        assert photo.day == M.day_of(497700 * 3600)
        assert by["r5012d1411b7f028c.ogg"].kind == "voice"
        # A row from the system session is not a chat: the file goes under "restored".
        assert by["r002d5cf5cb9567fe.txt"].chat == "restored"
        assert by["r002d5cf5cb9567fe.txt"].kind == "tool_output"
        unmapped = by["raaaaaaaaaaaaaaaa.txt"]
        assert (unmapped.chat, unmapped.day, unmapped.date) == ("restored", "2026-10-18", 0)
        for it in items:
            assert NAME_RE.match(it.name)


def test_dry_run_and_the_owner_gate():
    with tempfile.TemporaryDirectory() as d, fake_device() as (dev, url):
        b, rp = make_backup(pathlib.Path(d))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            assert M.main(["--ip", url, "--backup", str(b), "--rows", str(rp)]) == 0
        assert dev.uploads == 0 and "plan: 4 file(s)" in out.getvalue()
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            assert M.main(["--ip", url, "--backup", str(b), "--write"]) == 2
        assert "owner-approved" in err.getvalue() and dev.uploads == 0


def test_write_verify_and_rerun_skips():
    with tempfile.TemporaryDirectory() as d, fake_device() as (dev, url):
        tmp = pathlib.Path(d)
        b, rp = make_backup(tmp)
        tf = tmp / "token"
        tf.write_text(TOKEN)
        args = ["--ip", url, "--backup", str(b), "--rows", str(rp), "--token-file", str(tf)]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            assert M.main(args + ["--write", "--owner-approved", "--verify"]) == 0
        assert dev.uploads == 4  # the 429 was retried
        assert "4 uploaded" in out.getvalue() and "verify: 4 of 4 recorded" in out.getvalue()
        assert TOKEN not in out.getvalue() + err.getvalue()
        f = dev.root / "6098532481" / M.day_of(497700 * 3600) / "r0779d8d2849f5e2b.jpg"
        assert f.read_bytes() == (b / "0779d8d2849f5e2b.jpg").read_bytes()
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            assert M.main(args + ["--write", "--owner-approved"]) == 0
        assert dev.uploads == 4 and "4 already there" in out.getvalue()


def test_rows_formats():
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "one.jsonl"
        p.write_text(json.dumps({"id": "m1", "blob": "/mem/blobs/0779d8d2849f5e2b.jpg"}))
        assert len(M.load_rows(str(p))) == 1  # a one-line JSON-lines file is one row
        p.write_text(json.dumps({"rows": [{"id": "a"}, {"id": "b"}, 3]}))
        assert [r["id"] for r in M.load_rows(str(p))] == ["a", "b"]
        p.write_text('{"id": "a"}\n7\n{"id": "b"}\n')
        assert [r["id"] for r in M.load_rows(str(p))] == ["a", "b"]


def test_missing_files_are_reported():
    with tempfile.TemporaryDirectory() as d, fake_device() as (dev, url):
        b, rp = make_backup(pathlib.Path(d))
        items, _ = M.plan(b, M.load_rows(str(rp)))
        missing = M.verify(M.Device(url, TOKEN), items)
        assert sorted(missing) == sorted(it.name for it in items)


def test_a_wrong_token_fails_loudly():
    with tempfile.TemporaryDirectory() as d, fake_device() as (dev, url):
        b, rp = make_backup(pathlib.Path(d))
        items, _ = M.plan(b, M.load_rows(str(rp)))
        dev.busy_once = False
        written, skipped, failures = M.push(M.Device(url, "wrong"), items, "t", sleep=lambda s: None)
        assert (written, skipped, len(failures)) == (0, 0, 4)
        assert all("HTTP 401" in f for f in failures)


if __name__ == "__main__":
    test_plan_places_each_sidecar_by_its_row()
    test_dry_run_and_the_owner_gate()
    test_write_verify_and_rerun_skips()
    test_rows_formats()
    test_missing_files_are_reported()
    test_a_wrong_token_fails_loudly()
    print("tg_archive_import: plan, owner gate, write, verify, rerun and auth failure OK")
