"""The shared pieces of the bench drivers (soak_24h, onboarding_gate, ota_rehearsal, bench_rows).

An HTTP client that sends the device token only in the X-Nimbus-Token header and never prints it, one long-lived
quiet console session that goes through the forbidden-device guard and logs with UTC stamps, a turn over the web
chat, and an evidence folder. Importing this module touches nothing; pyserial is imported only when a console opens.
The self tests run all of it against tests/hil/fake_bench_device.py.
"""

from __future__ import annotations

import json
import os
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Callable, Dict, List, NamedTuple, Optional, Sequence

import port_guard

GRADES = ("PASS", "OPEN", "BLOCKED", "FAIL")
CONSOLE_PACE_BYTES = 64  # the console's input line is bounded and read in bursts, so long lines are paced
CONSOLE_PACE_GAP_S = 0.01


def utc_iso(when: Optional[float] = None) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(when))


def utc_stamp(when: Optional[float] = None) -> str:
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(when))


def read_token(path: str) -> str:
    """A device token from a file (never from the command line, where `ps` shows it)."""
    with open(path, encoding="utf-8") as f:
        return f.read().strip()


def read_env_value(path: str, name: str) -> str:
    """One value by its exact name from a .env file; '' if absent. Nothing else in the file is read into memory."""
    prefix = name + "="
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if line.startswith("export "):
                line = line[len("export ") :]
            if line.startswith(prefix):
                return line[len(prefix) :].strip().strip('"').strip("'")
    return ""


class Reply(NamedTuple):
    code: int  # 0: the request never got an HTTP answer
    body: bytes
    seconds: float
    error: str = ""

    @property
    def text(self) -> str:
        return self.body.decode("utf-8", "replace")

    def json(self) -> Dict[str, Any]:
        """The body as a JSON object; {} for anything else, so a caller reads a missing field, not an exception."""
        try:
            value = json.loads(self.body or b"{}")
        except ValueError:
            return {}
        return value if isinstance(value, dict) else {}


class Api:
    """The device's web API on the LAN. The token travels only in the header and is not in any repr or log."""

    def __init__(self, ip: str, token: str, timeout: float = 20.0):
        self.ip, self.timeout = ip, timeout
        self._token = token
        # No proxy: a sandboxed shell's proxy cannot reach LAN devices.
        self._opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def __repr__(self) -> str:
        return f"Api({self.ip})"

    def scrub(self, text: str) -> str:
        return text.replace(self._token, "[token]") if self._token else text

    def set_token(self, token: str) -> None:
        self._token = token

    def request(
        self, path: str, data: Optional[bytes] = None, ctype: str = "", timeout: Optional[float] = None
    ) -> Reply:
        req = urllib.request.Request(f"http://{self.ip}{path}", data=data)
        req.add_header("X-Nimbus-Token", self._token)
        if ctype:
            req.add_header("Content-Type", ctype)
        started = time.time()
        try:
            with self._opener.open(req, timeout=timeout or self.timeout) as r:
                return Reply(r.status, r.read(), time.time() - started)
        except urllib.error.HTTPError as e:
            return Reply(e.code, e.read(), time.time() - started, f"HTTP {e.code}")
        except Exception as e:  # noqa: BLE001 - a dropped request is data on a soak, not a crash of the driver
            return Reply(0, b"", time.time() - started, self.scrub(f"{type(e).__name__}: {e}")[:200])

    def get(self, path: str, timeout: Optional[float] = None) -> Reply:
        return self.request(path, timeout=timeout)

    def get_json(self, path: str, timeout: Optional[float] = None) -> Dict[str, Any]:
        return self.get(path, timeout).json()

    def state(self, timeout: Optional[float] = None) -> Dict[str, Any]:
        return self.get_json("/api/state", timeout)

    def post_form(self, path: str, fields: Dict[str, Any], timeout: Optional[float] = None) -> Reply:
        body = urllib.parse.urlencode(fields).encode()
        return self.request(path, body, "application/x-www-form-urlencoded", timeout)

    def upload(self, project: str, name: str, data: bytes, timeout: float = 60.0) -> Reply:
        boundary = "----nimbusbench"
        head = (
            f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{name}"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n"
        ).encode()
        query = urllib.parse.urlencode({"project": project, "name": name})
        return self.request(
            f"/api/files/upload?{query}",
            head + data + f"\r\n--{boundary}--\r\n".encode(),
            f"multipart/form-data; boundary={boundary}",
            timeout,
        )

    def download(self, project: str, name: str, timeout: float = 60.0) -> Reply:
        return self.get("/api/files/dl?" + urllib.parse.urlencode({"project": project, "name": name}), timeout)

    def tool(self, name: str, arguments: Dict[str, Any], chat: str = "web", timeout: Optional[float] = None) -> Reply:
        """One tool call through the test image's /api/test/astool."""
        call = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": arguments}}
        return self.post_form("/api/test/astool", {"chat": chat, "body": json.dumps(call)}, timeout)


# The device's own words for a turn it could not answer (lib/harness engine.cpp deliverFailure: "That didn't finish -
# <cause>."; lib/core orch_head_loop.cpp stepErrCause: "<host> could not be reached"). The web chat marks such a turn
# done, because a reply was delivered, so a grader that read only `done` passed a build whose every turn failed: the
# K01 soak on c168797b counted 28 of 28 of these turns ok (lane SNI).
FAILURE_NOTICES = re.compile(r"\b(?:didn['\u2019]t|did not) finish\b|\bcould(?:n['\u2019]t| not) be reached\b", re.I)


def failure_notice(reply: str) -> str:
    """The failure phrase a reply carries, or "" when it is an answer."""
    m = FAILURE_NOTICES.search(reply or "")
    return m.group(0) if m else ""


class TurnResult(NamedTuple):
    ok: bool
    reply: str
    error: str
    seconds: float
    http: int


def chat_turn(api: Api, text: str, timeout: float = 90.0, poll_s: float = 2.0) -> TurnResult:
    """One message over the web chat, waited out: POST /api/chat, then poll that turn's reply. A busy device (503)
    is retried once after a pause, as the page does."""
    started = time.time()
    sent = api.post_form("/api/chat", {"text": text}, 30.0)
    if sent.code == 503:
        time.sleep(poll_s * 5)
        sent = api.post_form("/api/chat", {"text": text}, 30.0)
    if sent.code != 200:
        return TurnResult(False, "", sent.error or sent.text[:200], time.time() - started, sent.code)
    turn = str(sent.json().get("turn") or "")
    path = "/api/chat?" + urllib.parse.urlencode({"turn": turn}) if turn else "/api/chat"
    while time.time() - started < timeout:
        time.sleep(poll_s)
        view = api.get_json(path, 15.0)
        if view and not view.get("pending", True):
            reply = str(view.get("reply") or "")
            notice = failure_notice(reply)
            ok = bool(view.get("done")) and not notice
            if notice:
                error = f"the turn ended with a failure notice ({notice!r}): {reply[:120]}"
            else:
                error = "" if ok else f"the turn ended unanswered: {reply[:120] or 'no reply'}"
            return TurnResult(ok, reply, error, time.time() - started, 200)
    return TurnResult(False, "", f"no reply within {timeout:.0f} s", time.time() - started, 200)


class Evidence:
    """One row's folder of raw evidence. Everything written through it has the device token scrubbed."""

    def __init__(self, root: str, scrub: Callable[[str], str] = lambda s: s):
        self.root = root
        self.scrub = scrub
        os.makedirs(root, exist_ok=True)

    def path(self, name: str) -> str:
        full = os.path.join(self.root, name)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        return full

    def write(self, name: str, content: Any) -> str:
        """Bytes go to disk as they are; text and JSON values are scrubbed first."""
        full = self.path(name)
        if isinstance(content, (bytes, bytearray)):
            with open(full, "wb") as f:
                f.write(content)
            return full
        text = content if isinstance(content, str) else json.dumps(content, indent=1, sort_keys=True)
        with open(full, "w", encoding="utf-8") as f:
            f.write(self.scrub(text))
        return full

    def append(self, name: str, line: str) -> None:
        with open(self.path(name), "a", encoding="utf-8") as f:
            f.write(self.scrub(line) + "\n")

    def note(self, line: str, name: str = "driver.log") -> None:
        stamped = f"[{utc_iso()}] {self.scrub(line)}"
        print(stamped, flush=True)
        self.append(name, stamped)


class Console:
    """One long-lived quiet console session (DTR and RTS low). The guard runs before the open; a line sent with
    secret=True is not logged. A wider soak or rehearsal holds this one session for its whole run, because a RAM-only
    setting (OTAURL, FAULT sram_ballast, PROVFAULT) can die with a reopen."""

    def __init__(self, port: str, log_path: str, tool: str = "bench console"):
        port_guard.check_port(port, tool)
        import serial

        self.port = port
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.2
        self.ser.dtr = self.ser.rts = False
        self.ser.open()
        self._log = open(log_path, "a", buffering=1, encoding="utf-8")
        self._lock = threading.Lock()
        self.lines: List[tuple] = []
        self.alive, self.dead = True, False
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self) -> None:
        buf = b""
        while self.alive:
            try:
                chunk = self.ser.read(4096)
            except Exception as e:  # noqa: BLE001 - the board re-enumerated: stay closed, a reopen can reset it
                self._write_log(f"[bench] port error {type(e).__name__}; console closed")
                self.dead = True
                return
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self._line(raw.decode("utf-8", "replace").rstrip())

    def _write_log(self, text: str) -> None:
        self._log.write(f"{utc_iso()} {text}\n")

    def _line(self, text: str) -> None:
        if not text:
            return
        self._write_log(text)
        with self._lock:
            self.lines.append((time.time(), text))
            if len(self.lines) > 8000:
                del self.lines[:2000]

    def send(self, line: str, secret: bool = False) -> None:
        self._write_log(">> [secret line not logged]" if secret else f">> {line}")
        data = (line + "\n").encode()
        for i in range(0, len(data), CONSOLE_PACE_BYTES):
            self.ser.write(data[i : i + CONSOLE_PACE_BYTES])
            self.ser.flush()
            time.sleep(CONSOLE_PACE_GAP_S)

    def wait(self, needle: str, timeout: float = 30.0, since: Optional[float] = None) -> str:
        """The first line holding ``needle`` (at or after ``since``), or '' on timeout."""
        end = time.time() + timeout
        while time.time() < end:
            with self._lock:
                for t, text in self.lines:
                    if (since is None or t >= since) and needle in text:
                        return text
            time.sleep(0.05)
        return ""

    def cmd(self, line: str, expect: str, timeout: float = 8.0, secret: bool = False) -> str:
        """Send a line and return the first reply holding ``expect`` ('' on silence or a dead console)."""
        if self.dead:
            return ""
        started = time.time()
        self.send(line, secret)
        return self.wait(expect, timeout, since=started - 0.01)

    def since(self, started: float) -> List[str]:
        with self._lock:
            return [text for t, text in self.lines if t >= started]

    def close(self) -> None:
        self.alive = False
        self._thread.join(timeout=1.0)
        try:
            self.ser.close()
        finally:
            self._log.close()


# ---- the scanner -----------------------------------------------------------------------------

# The firmware's own record is "heap: alloc FAILED size=1626 caps=DMA|INTERNAL|8BIT task=async_t free=.. largest=..".
# The 28 Oct soak's scanner looked for the lower-case words and counted 24 of these lines as zero, so every pattern
# here ignores case, and a host test runs that day's lines through it.
_HEAP_RECORD = re.compile(r"alloc failed\s+size=(?P<size>\d+)\s+caps=(?P<caps>\S+)(?:\s+task=(?P<task>\S+))?", re.I)
_ALLOC_CLASSES = (
    ("allocFailed", re.compile(r"alloc\s+failed", re.I)),
    ("failedToAllocate", re.compile(r"failed to allocate", re.I)),
    ("notEnoughMem", re.compile(r"not enough mem", re.I)),
    ("turnFailedLowMemory", re.compile(r"turn failed \(low memory", re.I)),
)
_TURN_END = re.compile(r"turn end:\s*(ok|fail)", re.I)
# One provider exchange's record (src/agent/transport_tls.cpp, heapwatch ExchangeTrace::line): "transport: heaptrace
# <host><path> ... fail=<stage> ...". fail=none is an exchange that completed; any other stage did not. An exchange that
# failed at connect never reached the provider: on the build that dialed TLS by address it was every one of them.
_EXCHANGE = re.compile(r"\bheaptrace\s+(?P<host>[^\s/]+)\S*\s.*?\bfail=(?P<stage>\w+)", re.I)


def scan_lines(lines: Sequence[str]) -> Dict[str, Any]:
    """Count every memory-failure line, whatever its case, every turn end, and every provider exchange by the stage it
    stopped at. ``total`` is the number of lines that match a memory class; ``heapBySize`` breaks the firmware's own
    records down by size, capabilities and task; ``connectFailed`` counts the exchanges that failed at connect."""
    counts = {name: 0 for name, _ in _ALLOC_CLASSES}
    by_size: Dict[str, int] = {}
    turn_ends = {"ok": 0, "fail": 0}
    exchanges: Dict[str, int] = {}
    connect_by_host: Dict[str, int] = {}
    total = 0
    for line in lines:
        trace = _EXCHANGE.search(line)
        if trace:
            stage = trace["stage"].lower()
            exchanges[stage] = exchanges.get(stage, 0) + 1
            if stage == "connect":
                connect_by_host[trace["host"]] = connect_by_host.get(trace["host"], 0) + 1
        hit = False
        for name, pattern in _ALLOC_CLASSES:
            if pattern.search(line):
                counts[name] += 1
                hit = True
        total += int(hit)
        heap = _HEAP_RECORD.search(line)
        if heap:
            key = f"size={heap['size']} caps={heap['caps']} task={heap['task'] or '?'}"
            by_size[key] = by_size.get(key, 0) + 1
        end = _TURN_END.search(line)
        if end:
            turn_ends[end.group(1).lower()] += 1
    return {
        "total": total,
        **counts,
        "heapBySize": by_size,
        "turnEnds": turn_ends,
        "exchanges": exchanges,
        "connectFailed": exchanges.get("connect", 0),
        "connectFailedByHost": connect_by_host,
    }


def worst_grade(grades: Sequence[str]) -> str:
    """FAIL over BLOCKED over OPEN over PASS. No grades at all is BLOCKED: nothing was measured."""
    return max(grades, key=GRADES.index) if grades else "BLOCKED"
