#!/usr/bin/env python3
"""capture_proxy - a local reverse proxy that records every provider exchange.

Both arms of the comparison send their provider requests through one of these, so
request counts, token usage and the bodies themselves are measured the same way:

  arm A: nimbusd + capture_shim (its libcurl URL is rewritten to this proxy)
  arm B: refloop, whose SDK base_url points at this proxy

A request to  http://127.0.0.1:<port>/<label>/<host>/<path>  is forwarded, with the
same method, headers and body bytes, to  https://<host>/<path>.  Only the hosts in
ALLOWED_HOSTS are forwarded. Each exchange becomes one JSON line in a gzip file:
the request body exactly as sent, the response body, the status, the timing, and
the provider-reported usage in one normalized shape.

Secrets: credential headers are forwarded and never written. Bodies carry no keys.
"""

from __future__ import annotations

import gzip
import http.client
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ALLOWED_HOSTS = ("api.anthropic.com", "api.openai.com", "api.mistral.ai")
# Request headers worth keeping in the record. Nothing that carries a credential.
KEPT_HEADERS = ("content-type", "anthropic-version", "anthropic-beta", "openai-beta", "user-agent")
HOP_HEADERS = {"host", "connection", "keep-alive", "proxy-connection", "transfer-encoding", "accept-encoding"}
UPSTREAM_TIMEOUT_S = 600


def normalize_usage(body) -> dict | None:
    """Provider usage as {input, cached, cache_write, output}: input is ALL prompt tokens."""
    usage = body.get("usage") if isinstance(body, dict) else None
    if not isinstance(usage, dict):
        return None
    if "cache_read_input_tokens" in usage or "cache_creation_input_tokens" in usage:  # Anthropic
        cached = usage.get("cache_read_input_tokens") or 0
        written = usage.get("cache_creation_input_tokens") or 0
        fresh = usage.get("input_tokens") or 0
        return {"input": fresh + cached + written, "cached": cached, "cache_write": written,
                "output": usage.get("output_tokens") or 0}
    details = usage.get("input_tokens_details") or usage.get("prompt_tokens_details") or {}
    total_in = usage.get("input_tokens", usage.get("prompt_tokens")) or 0
    out = usage.get("output_tokens", usage.get("completion_tokens")) or 0
    return {"input": total_in, "cached": details.get("cached_tokens") or 0, "cache_write": 0, "output": out}


def _parse(raw: bytes):
    """A body as JSON when it is JSON, else as text (a server-sent event stream, an error page)."""
    if not raw:
        return None
    try:
        return json.loads(raw)
    except ValueError:
        return raw.decode("utf-8", "replace")


class _Upstream:
    """One kept-alive TLS connection per (thread, host), so neither arm pays a handshake per request."""

    def __init__(self):
        self.local = threading.local()

    def send(self, host: str, method: str, path: str, body: bytes, headers: dict):
        conns = self.local.__dict__.setdefault("conns", {})
        for attempt in (0, 1):
            conn = conns.get(host) or http.client.HTTPSConnection(host, timeout=UPSTREAM_TIMEOUT_S)
            conns[host] = conn
            try:
                conn.request(method, path, body=body or None, headers=headers)
                resp = conn.getresponse()
                return resp.status, resp.getheaders(), resp.read()
            except (http.client.HTTPException, OSError):
                conn.close()
                conns.pop(host, None)
                # A kept-alive connection the server closed fails before any byte is
                # accepted, so one resend on a new connection repeats nothing.
                if attempt:
                    raise
        raise RuntimeError("unreachable")


class CaptureProxy:
    """Start with start(), point clients at url(), read the records from `path` after stop()."""

    def __init__(self, path: Path, label: str = "cap", keep_bodies: bool = False):
        self.path, self.label = Path(path), label
        self.ctx = {}
        self.records = []          # compact per-exchange rows (no bodies), for the runner
        self.keep_bodies = keep_bodies
        self.bodies = []           # full exchanges not yet taken (only with keep_bodies)
        self._lock = threading.Lock()
        self._seq = 0
        self._upstream = _Upstream()
        self._server = None
        self._file = None

    def start(self) -> int:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._file = gzip.open(self.path, "at", encoding="utf-8")
        proxy = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):   # quiet: the JSONL is the log
                pass

            def _serve(self):
                proxy._handle(self)

            do_GET = do_POST = do_PUT = do_DELETE = do_PATCH = _serve

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._server.daemon_threads = True
        threading.Thread(target=self._server.serve_forever, daemon=True).start()
        return self._server.server_address[1]

    def url(self, host: str | None = None) -> str:
        base = f"http://127.0.0.1:{self._server.server_address[1]}/{self.label}"
        return f"{base}/{host}" if host else base

    def mark(self, **ctx):
        """Context stamped on every later record (the scenario turn, usually)."""
        with self._lock:
            self.ctx = dict(ctx)

    def stop(self):
        if self._server:
            self._server.shutdown()
            self._server.server_close()
            self._server = None
        if self._file:
            self._file.close()
            self._file = None

    def rows(self, **match) -> list:
        with self._lock:
            return [r for r in self.records if all(r["ctx"].get(k) == v for k, v in match.items())]

    def take_bodies(self, **match) -> list:
        """The full exchanges (bodies included) recorded under this context, removed from memory."""
        with self._lock:
            hit = [r for r in self.bodies if all(r["ctx"].get(k) == v for k, v in match.items())]
            taken = {r["seq"] for r in hit}
            self.bodies = [r for r in self.bodies if r["seq"] not in taken]
            return hit

    def _reply(self, handler, status: int, headers, body: bytes):
        handler.send_response(status)
        for k, v in headers:
            if k.lower() not in ("transfer-encoding", "connection", "content-length", "content-encoding"):
                handler.send_header(k, v)
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    def _handle(self, handler):
        length = int(handler.headers.get("Content-Length") or 0)
        body = handler.rfile.read(length) if length else b""
        parts = handler.path.lstrip("/").split("/", 2)
        if len(parts) < 3 or parts[0] != self.label or parts[1] not in ALLOWED_HOSTS:
            self._reply(handler, 404, [("Content-Type", "application/json")], b'{"error":"not a captured host"}')
            return
        host, path = parts[1], "/" + parts[2]
        headers = {k: v for k, v in handler.headers.items() if k.lower() not in HOP_HEADERS}
        headers["Accept-Encoding"] = "identity"    # plain bodies, so the record is readable
        with self._lock:
            ctx = dict(self.ctx)
        started = time.time()
        try:
            status, resp_headers, resp_body = self._upstream.send(host, handler.command, path, body, headers)
        except (http.client.HTTPException, OSError) as e:
            status, resp_headers = 502, [("Content-Type", "application/json")]
            resp_body = json.dumps({"error": f"capture proxy: upstream failed: {type(e).__name__}"}).encode()
        ended = time.time()
        self._record(ctx, handler, host, path, body, status, resp_body, started, ended)
        self._reply(handler, status, resp_headers, resp_body)

    def _record(self, ctx, handler, host, path, body, status, resp_body, started, ended):
        request, response = _parse(body), _parse(resp_body)
        row = {
            "ctx": ctx, "host": host, "path": path.split("?")[0], "method": handler.command, "status": status,
            "t_start": round(started, 3), "ms": int((ended - started) * 1000),
            "req_bytes": len(body), "resp_bytes": len(resp_body),
            "model": request.get("model") if isinstance(request, dict) else None,
            "usage": normalize_usage(response),
        }
        full = dict(row)
        full["req_headers"] = {k.lower(): v for k, v in handler.headers.items() if k.lower() in KEPT_HEADERS}
        full["request"], full["response"] = request, response
        with self._lock:
            self._seq += 1
            row["seq"] = full["seq"] = self._seq
            self.records.append(row)
            if self.keep_bodies:
                self.bodies.append(full)
            if self._file:
                self._file.write(json.dumps(full, ensure_ascii=False) + "\n")
                self._file.flush()


def read_capture(path: Path) -> list:
    """Every recorded exchange of one capture file, bodies included."""
    with gzip.open(path, "rt", encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]
