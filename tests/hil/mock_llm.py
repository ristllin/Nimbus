"""Mock OpenAI-compatible LLM server for deterministic, keyless on-device e2e.

The device's EXISTING custom adapter (``src/agent/adapters/custom_adapter.cpp``,
READ-ONLY contract) is the only firmware wire that can reach a LAN endpoint:

  * transport: HTTP/1.0, ``Connection: close``, plain HTTP when the configured
    base (``store::customBase()``, NVS ``custBase``) starts with ``http://``
    (default port 80; ``host:port`` honoured). The base's PATH is STRIPPED by the
    adapter - the request path is always the hardcoded ``/v1/chat/completions``
    (openai/mistral conv) or ``/v1/messages`` (anthropic conv).
  * auth: an empty ``custKey`` sends NO auth header, and a key is NEVER sent over
    ``http://`` (cleartext guard) - so the keyless LAN path is header-free.
  * request body (``custConv=openai``):
        {"model": <custModel>,
         "messages": [
           {"role":"system","content":"You are an autonomous <category> agent. ..."},
           {"role":"user","content": <task>}]}
    NO tools, NO response_format, NO temperature/max_tokens on this wire.
  * response parsing: ArduinoJson stream-parse with a filter that reads ONLY
    ``choices[0].message.content`` (and ``error.message``). Empty content ->
    ParseFail; HTTP 401/403 -> Auth; connect-fail/timeout -> Network (code 0);
    any other non-200 -> RemoteFail (NO retry at the adapter - only the TCP
    connect itself is retried 3x).
  * timeout: ``CUST_TIMEOUT_MS = 30000`` - a compile-time firmware constant, NOT
    runtime-configurable. The mock's slow-scenario delay is configurable instead
    (``slow_delay_s`` attr / ``NIMBUS_MOCK_SLOW_S`` env, default 35 s > 30 s).

The mock also honours structured-output/tool requests IF a future head path ever
sends them (``response_format.json_schema`` -> the content is the orch_turn JSON;
advertised ``tools`` -> a ``tool_calls`` round) - but nothing in today's firmware
custom wire does, so the plainest branch is the one hardware actually exercises.

Scenarios are selected BY MODEL NAME in the request body (the adapter always
sends ``store::customModel()``, live-settable via ``POST /api/orch custModel=``):

  mock-echo         200, content "mock says hi"
  mock-toolloop     tools advertised -> tool_calls round then final; no tools
                    (today's wire) -> immediate final "toolloop final"
  mock-badjson      200 with a TRUNCATED/invalid JSON body
  mock-500-then-ok  HTTP 500 once (per reset()), then a good response
  mock-slow         sleeps past the adapter timeout, then answers
  mock-usage        good turn + usage {prompt_tokens:1234, completion_tokens:56}

Stdlib only (http.server + threading) - no external deps; importing this module
does nothing but define classes (collection-safe).

Self-test (no device needed):  python3 tests/hil/mock_llm.py
"""

from __future__ import annotations

import json
import os
import socket
import threading
import time
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List, Optional

import pytest

MOCK_REPLY = "mock says hi"
TOOLLOOP_FINAL = "toolloop final"
USAGE_PROMPT_TOKENS = 1234
USAGE_COMPLETION_TOKENS = 56
# Firmware constant (custom_adapter.cpp: CUST_TIMEOUT_MS = 30000) - mirrored here
# so tests size their waits off one number.
DEVICE_CUST_TIMEOUT_S = 30.0
BADJSON_BODY = b'{"choices":[{"message":{"content":"trunc'


@dataclass
class RecordedRequest:
    """One request as the device actually sent it (headers lowercased)."""

    method: str
    path: str
    headers: Dict[str, str]
    body: bytes
    ts: float
    json: Optional[dict] = None

    @property
    def model(self) -> str:
        return (self.json or {}).get("model", "")

    @property
    def messages(self) -> List[dict]:
        return (self.json or {}).get("messages", [])

    def to_jsonable(self) -> dict:
        return {
            "ts": self.ts,
            "method": self.method,
            "path": self.path,
            "headers": self.headers,
            "body": self.body.decode("utf-8", "replace"),
        }


class _Handler(BaseHTTPRequestHandler):
    # The device speaks HTTP/1.0 and reads to connection-close; answering 1.0
    # keeps the server on exactly that wire (close after each response).
    protocol_version = "HTTP/1.0"
    server_version = "MockLLM/1.0"

    # -- silence the default stderr access log (tests read the recording) -----
    def log_message(self, fmt, *args):  # noqa: A003 - BaseHTTPRequestHandler API
        pass

    # -- helpers ---------------------------------------------------------------
    def _record(self) -> RecordedRequest:
        length = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(length) if length else b""
        parsed: Optional[dict] = None
        try:
            parsed = json.loads(body.decode("utf-8")) if body else None
        except (ValueError, UnicodeDecodeError):
            parsed = None
        rec = RecordedRequest(
            method=self.command,
            path=self.path,
            headers={k.lower(): v for k, v in self.headers.items()},
            body=body,
            ts=time.time(),
            json=parsed,
        )
        self.server.owner._record(rec)  # type: ignore[attr-defined]
        return rec

    def _send(self, code: int, payload: bytes, content_type: str = "application/json") -> None:
        try:
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError, OSError):
            # The device gave up (timeout scenario) - expected, never fatal.
            pass

    def _send_json(self, code: int, doc: dict) -> None:
        self._send(code, json.dumps(doc).encode("utf-8"))

    # -- completion builders ---------------------------------------------------
    def _completion(
        self, model: str, content: str, usage: Optional[dict] = None, tool_calls: Optional[list] = None
    ) -> dict:
        msg: Dict[str, Any] = {"role": "assistant", "content": content}
        finish = "stop"
        if tool_calls:
            msg["tool_calls"] = tool_calls
            msg["content"] = None
            finish = "tool_calls"
        doc = {
            "id": f"chatcmpl-mock-{self.server.owner.next_id()}",  # type: ignore[attr-defined]
            "object": "chat.completion",
            "created": int(time.time()),
            "model": model,
            "choices": [{"index": 0, "message": msg, "finish_reason": finish}],
        }
        if usage is not None:
            doc["usage"] = usage
        return doc

    def _final_content(self, req: dict, reply: str) -> str:
        """Honour structured-output IF requested (a future head path): with a
        json_schema response_format the content itself must be the orch_turn
        JSON. Today's custom sub-session wire sends neither - plain text."""
        rf = req.get("response_format") or {}
        if rf.get("type") == "json_schema":
            return json.dumps({"reply": reply})
        return reply

    # -- routes ----------------------------------------------------------------
    def do_POST(self):  # noqa: N802 - BaseHTTPRequestHandler API
        rec = self._record()
        if not self.path.endswith("/chat/completions"):
            self._send_json(404, {"error": {"message": f"no route {self.path}"}})
            return
        req = rec.json or {}
        model = req.get("model", "")
        owner: "MockLLMServer" = self.server.owner  # type: ignore[attr-defined]

        if model == "mock-badjson":
            # 200 with a truncated body: the device's stream deserializeJson
            # fails -> empty content -> FabricErr::ParseFail (never delivered raw).
            self._send(200, BADJSON_BODY)
            return

        if model == "mock-500-then-ok":
            if owner.bump_counter(model) == 1:
                self._send_json(500, {"error": {"message": "mock transient 500"}})
                return
            self._send_json(200, self._completion(model, self._final_content(req, MOCK_REPLY)))
            return

        if model == "mock-slow":
            time.sleep(owner.slow_delay_s)
            self._send_json(200, self._completion(model, self._final_content(req, "slow reply (too late)")))
            return

        if model == "mock-usage":
            self._send_json(
                200,
                self._completion(
                    model,
                    self._final_content(req, MOCK_REPLY),
                    usage={
                        "prompt_tokens": USAGE_PROMPT_TOKENS,
                        "completion_tokens": USAGE_COMPLETION_TOKENS,
                        "total_tokens": USAGE_PROMPT_TOKENS + USAGE_COMPLETION_TOKENS,
                    },
                ),
            )
            return

        if model == "mock-toolloop":
            tools = req.get("tools") or []
            has_tool_result = any(m.get("role") == "tool" for m in req.get("messages", []))
            if tools and not has_tool_result:
                # Round 1 of a tool-capable caller: call the first advertised
                # tool (prefer a memory search if offered).
                names = [t.get("function", {}).get("name", "") for t in tools]
                pick = next((n for n in names if "memory" in n and "search" in n), names[0] if names else "")
                self._send_json(
                    200,
                    self._completion(
                        model,
                        "",
                        tool_calls=[
                            {
                                "id": "call_mock_1",
                                "type": "function",
                                "function": {"name": pick, "arguments": json.dumps({"query": "mock"})},
                            }
                        ],
                    ),
                )
                return
            # Round 2 (tool result echoed back) - or, on today's tool-less custom
            # wire, the one-and-only round: the final answer.
            self._send_json(200, self._completion(model, self._final_content(req, TOOLLOOP_FINAL)))
            return

        # mock-echo + any unknown model: the fixed deterministic reply.
        self._send_json(200, self._completion(model, self._final_content(req, MOCK_REPLY)))

    def do_GET(self):  # noqa: N802
        self._record()
        if self.path.endswith("/models"):
            self._send_json(
                200,
                {
                    "object": "list",
                    "data": [
                        {"id": m, "object": "model"}
                        for m in (
                            "mock-echo",
                            "mock-toolloop",
                            "mock-badjson",
                            "mock-500-then-ok",
                            "mock-slow",
                            "mock-usage",
                        )
                    ],
                },
            )
            return
        self._send_json(200, {"ok": True, "mock": "nimbus-mock-llm"})


class MockLLMServer:
    """Threaded mock server. Start on an ephemeral port, record every request,
    optionally mirror each request to a JSONL file for debugging."""

    def __init__(self, host: str = "0.0.0.0", port: int = 0, dump_path: Optional[str] = None):
        self._httpd = ThreadingHTTPServer((host, port), _Handler)
        self._httpd.daemon_threads = True  # a sleeping mock-slow handler can't
        self._httpd.owner = self  # block teardown
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        self._id = 0
        self._counters: Dict[str, int] = {}
        self.requests: List[RecordedRequest] = []
        self.slow_delay_s = float(os.environ.get("NIMBUS_MOCK_SLOW_S", str(DEVICE_CUST_TIMEOUT_S + 5.0)))
        self.dump_path = dump_path

    # -- lifecycle -------------------------------------------------------------
    @property
    def port(self) -> int:
        return self._httpd.server_address[1]

    def base_url(self, host_ip: str) -> str:
        """The value to provision as ``custBase``. http:// => the adapter's
        plain-HTTP, keyless path; the /v1 suffix is stripped by the adapter (it
        keeps only host:port) and is included for human readability."""
        return f"http://{host_ip}:{self.port}/v1"

    def start(self) -> "MockLLMServer":
        self._thread = threading.Thread(target=self._httpd.serve_forever, name="mock-llm", daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        self._httpd.shutdown()
        self._httpd.server_close()
        if self._thread:
            self._thread.join(timeout=5.0)

    # -- state -----------------------------------------------------------------
    def _record(self, rec: RecordedRequest) -> None:
        with self._lock:
            self.requests.append(rec)
        if self.dump_path:
            try:
                with open(self.dump_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps(rec.to_jsonable()) + "\n")
            except OSError:
                pass

    def next_id(self) -> int:
        with self._lock:
            self._id += 1
            return self._id

    def bump_counter(self, key: str) -> int:
        with self._lock:
            self._counters[key] = self._counters.get(key, 0) + 1
            return self._counters[key]

    def reset(self) -> None:
        """Per-test isolation: drop recorded requests + scenario counters."""
        with self._lock:
            self.requests = []
            self._counters = {}

    def requests_for(self, model: str) -> List[RecordedRequest]:
        with self._lock:
            return [r for r in self.requests if r.model == model]

    def completions(self) -> List[RecordedRequest]:
        with self._lock:
            return [r for r in self.requests if r.path.endswith("/chat/completions")]

    def wait_for_request(self, model: str, count: int = 1, timeout: float = 90.0) -> List[RecordedRequest]:
        """Block until >= ``count`` chat-completion requests for ``model`` have
        arrived (returns them), or return what there is after ``timeout``."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            got = self.requests_for(model)
            if len(got) >= count:
                return got
            time.sleep(0.5)
        return self.requests_for(model)


# ---- LAN IP discovery --------------------------------------------------------
def find_lan_ip(peer_ip: Optional[str] = None) -> str:
    """The host's outbound-interface IP toward ``peer_ip`` (the device), i.e. the
    address the DEVICE can reach the mock on. UDP-connect trick - no packet is
    actually sent. Returns "" when undeterminable (caller loud-skips)."""
    for target in ([peer_ip] if peer_ip else []) + ["8.8.8.8"]:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                s.connect((target, 9))
                ip = s.getsockname()[0]
            finally:
                s.close()
            if ip and not ip.startswith("127."):
                return ip
        except OSError:
            continue
    return ""


# ---- pytest fixture (imported by test_l12_mock_llm.py - conftest untouched) --
@pytest.fixture(scope="module")
def mock_llm(tmp_path_factory) -> "MockLLMServer":
    """Module-scoped mock server on an ephemeral port. Module-scoped so ONE
    device provisioning + reboot cycle (the adapter registration is boot-gated:
    adapter_factory.cpp registers backend "custom" only when store::hasCustom()
    at fabricInit) serves every scenario - tests switch scenarios live via
    ``custModel`` (read per-dispatch by the firmware) and call ``reset()``."""
    dump = str(tmp_path_factory.mktemp("mock_llm") / "requests.jsonl")
    server = MockLLMServer(dump_path=dump).start()
    yield server
    server.stop()


# ---- host-side self-test -----------------------------------------------------
def _selftest() -> int:  # pragma: no cover - developer smoke, run via __main__
    import urllib.request

    failures: List[str] = []

    def check(name: str, cond: bool, detail: str = "") -> None:
        tag = "ok " if cond else "FAIL"
        print(f"  [{tag}] {name}" + (f" - {detail}" if detail and not cond else ""))
        if not cond:
            failures.append(name)

    srv = MockLLMServer(dump_path="/tmp/nimbus_mock_llm_selftest.jsonl")
    srv.slow_delay_s = 0.8
    srv.start()
    base = f"http://127.0.0.1:{srv.port}"
    print(f"mock-llm self-test on {base}")

    def post(model: str, extra: Optional[dict] = None, messages: Optional[list] = None):
        body = {
            "model": model,
            "messages": messages
            or [
                {
                    "role": "system",
                    "content": "You are an autonomous ops agent. "
                    "Complete the task and reply with the final result only.",
                },
                {"role": "user", "content": "say hi"},
            ],
        }
        body.update(extra or {})
        req = urllib.request.Request(
            base + "/v1/chat/completions",
            data=json.dumps(body).encode(),
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    # 1. THE FIRMWARE WIRE: a raw HTTP/1.0 request byte-for-byte like
    #    custom_adapter.cpp custRequest() builds it (no auth header, HTTP/1.0,
    #    Connection: close) - proves the server serves that exact dialect.
    raw_body = json.dumps(
        {
            "model": "mock-echo",
            "messages": [
                {
                    "role": "system",
                    "content": "You are an autonomous ops agent. "
                    "Complete the task and reply with the final result only.",
                },
                {"role": "user", "content": "say hi"},
            ],
        }
    ).encode()
    raw = (
        b"POST /v1/chat/completions HTTP/1.0\r\n"
        b"Host: 127.0.0.1\r\n"
        b"Content-Type: application/json\r\n"
        b"Content-Length: " + str(len(raw_body)).encode() + b"\r\n"
        b"Connection: close\r\n\r\n" + raw_body
    )
    s = socket.create_connection(("127.0.0.1", srv.port), timeout=10)
    s.sendall(raw)
    resp = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        resp += chunk
    s.close()
    head, _, payload = resp.partition(b"\r\n\r\n")
    status = head.split(b"\r\n", 1)[0]
    doc = json.loads(payload)
    check("http/1.0 status 200", b" 200 " in status + b" ", status.decode())
    check("http/1.0 closes the connection (read-to-EOF worked)", True)
    check("echo content over raw wire", doc["choices"][0]["message"]["content"] == MOCK_REPLY, str(doc))

    # 2. echo via urllib
    code, body = post("mock-echo")
    doc = json.loads(body)
    check("echo 200", code == 200)
    check("echo content", doc["choices"][0]["message"]["content"] == MOCK_REPLY)

    # 3. toolloop - today's wire (no tools): immediate final, ONE round
    code, body = post("mock-toolloop")
    doc = json.loads(body)
    check(
        "toolloop tool-less final", code == 200 and doc["choices"][0]["message"]["content"] == TOOLLOOP_FINAL, str(doc)
    )
    # toolloop - a tool-capable caller: round 1 = tool_calls, round 2 = final
    tools = [{"type": "function", "function": {"name": "memory_search", "parameters": {"type": "object"}}}]
    code, body = post("mock-toolloop", extra={"tools": tools})
    doc = json.loads(body)
    msg = doc["choices"][0]["message"]
    check(
        "toolloop round1 tool_calls",
        code == 200 and msg.get("tool_calls") and msg["tool_calls"][0]["function"]["name"] == "memory_search",
        str(doc),
    )
    code, body = post(
        "mock-toolloop",
        extra={"tools": tools},
        messages=[
            {"role": "user", "content": "say hi"},
            {"role": "assistant", "content": None, "tool_calls": msg.get("tool_calls")},
            {"role": "tool", "tool_call_id": "call_mock_1", "content": "found: mock"},
        ],
    )
    doc = json.loads(body)
    check("toolloop round2 final", doc["choices"][0]["message"]["content"] == TOOLLOOP_FINAL, str(doc))

    # 4. badjson: 200 + invalid body
    code, body = post("mock-badjson")
    bad = False
    try:
        json.loads(body)
    except ValueError:
        bad = True
    check("badjson 200 + unparseable", code == 200 and bad, body.decode("utf-8", "replace"))

    # 5. 500-then-ok
    code1, _ = post("mock-500-then-ok")
    code2, body = post("mock-500-then-ok")
    doc = json.loads(body)
    check(
        "500 then ok",
        code1 == 500 and code2 == 200 and doc["choices"][0]["message"]["content"] == MOCK_REPLY,
        f"first={code1} second={code2}",
    )

    # 6. slow honours the configured delay
    t0 = time.time()
    code, _ = post("mock-slow")
    dt = time.time() - t0
    check(
        "slow delayed >= configured",
        code == 200 and dt >= srv.slow_delay_s - 0.1,
        f"dt={dt:.2f}s want>={srv.slow_delay_s}",
    )

    # 7. usage numbers
    code, body = post("mock-usage")
    doc = json.loads(body)
    u = doc.get("usage", {})
    check(
        "usage numbers",
        u.get("prompt_tokens") == USAGE_PROMPT_TOKENS and u.get("completion_tokens") == USAGE_COMPLETION_TOKENS,
        str(u),
    )

    # 8. structured-output honoured when requested (future head path)
    code, body = post(
        "mock-echo", extra={"response_format": {"type": "json_schema", "json_schema": {"name": "orch_turn"}}}
    )
    doc = json.loads(body)
    inner = json.loads(doc["choices"][0]["message"]["content"])
    check("json_schema => orch_turn JSON content", inner.get("reply") == MOCK_REPLY)

    # 9. recording + JSONL dump
    n = len(srv.completions())
    check("all requests recorded", n >= 10, f"n={n}")
    check("no auth header on the raw firmware-wire request", "authorization" not in srv.completions()[0].headers)
    check("jsonl dump written", os.path.exists(srv.dump_path) and os.path.getsize(srv.dump_path) > 0)

    srv.stop()
    if failures:
        print(f"SELF-TEST FAILED: {failures}")
        return 1
    print("SELF-TEST PASS")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(_selftest())
