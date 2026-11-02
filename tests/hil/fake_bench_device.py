"""A fake Nimbus for the bench drivers' self tests: a real HTTP server on localhost and a console stand-in.

It answers the routes and console verbs the drivers use with the firmware's own reply shapes, and each behaviour a
driver must catch can be turned wrong from a test (a restart, a dead provider, a screen stuck on a wrong page), so a
driver's green result can also be shown to go red. No port, no board, no network beyond 127.0.0.1.
"""

from __future__ import annotations

import http.server
import json
import re
import threading
import time
import urllib.parse
from typing import Any, Callable, Dict, List, Optional

TOKEN = "f" * 24  # the fake's device token: never a real one


def fresh_state() -> Dict[str, Any]:
    return {
        "fw": "v5.0.0",
        "build": "v5.0.0-1-gabc1234",
        "uptimeMs": 1000,
        "otaSlot": "app0",
        "lastOta": "",
        "needsOnboarding": True,
        "provVerified": False,
        "sta": False,
        "heap": 40000,
        "heapMin": 20000,
        "mem": {"intFree": 40000, "intLargest": 16000, "lowWater": 15000, "allocFails": 0, "pollStackMin": 9000},
        "boot": {"n": 7, "reason": "usb", "crash": None},
        "nvs": {"free": 7712, "fails": 0},
        "dream": {"known": True, "last": "ran", "stateSaved": True},
        "spend": {"limitCents": 2000, "frozen": False},
        "fsMounted": True,
    }


class FakeDevice:
    """The web half. Mutate ``state``, ``errlog`` and the hooks freely from a test."""

    def __init__(self):
        self.state = fresh_state()
        self.errlog: List[str] = []
        self.orch: Dict[str, Any] = {"hasTg": False, "tgAllow": "", "providers": {}, "tlsVerify": True}
        self.files: Dict[tuple, bytes] = {}
        self.requests: List[tuple] = []
        self.auth_headers: List[Dict[str, str]] = []
        self.turns: Dict[str, str] = {}
        self.chat_reply: Callable[[str], str] = lambda text: "ok"
        self.chat_code = 200
        self.verify_ok = True
        self.fail_state = False
        self.state_delay = 0.0
        self.astool: Callable[[str, Dict[str, Any]], Any] = lambda name, arguments: {"result": "done"}
        self.lock = threading.Lock()
        self._next_turn = 100
        self.started = time.time()
        self.httpd: Optional[http.server.ThreadingHTTPServer] = None

    # -- life cycle -----------------------------------------------------------------------------
    def start(self) -> "FakeDevice":
        device = self

        class Handler(_Handler):
            fake = device

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        return self

    def stop(self) -> None:
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()

    @property
    def ip(self) -> str:
        return f"127.0.0.1:{self.httpd.server_address[1]}"

    def __enter__(self) -> "FakeDevice":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    # -- behaviour --------------------------------------------------------------------------------
    def log(self, line: str) -> None:
        self.errlog.append(line)

    def restart(self) -> None:
        with self.lock:
            self.state["boot"]["n"] += 1
            self.state["uptimeMs"] = 1000

    def public_state(self) -> Dict[str, Any]:
        with self.lock:
            self.state["uptimeMs"] += 1000
            return json.loads(json.dumps(self.state))

    def run_turn(self, text: str) -> str:
        self._next_turn += 1
        turn = str(self._next_turn)
        reply = self.chat_reply(text)
        self.turns[turn] = reply
        self.log(f"turn end: {'ok' if reply else 'FAIL'} host=fake tokens in=1 out=1 tools=0 reply={len(reply)}B src=web")
        return turn


class _Handler(http.server.BaseHTTPRequestHandler):
    fake: FakeDevice
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code: int, body: Any, ctype: str = "application/json") -> None:
        data = body if isinstance(body, bytes) else (body if isinstance(body, str) else json.dumps(body)).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _authed(self) -> bool:
        self.fake.auth_headers.append(dict(self.headers.items()))
        if self.headers.get("X-Nimbus-Token") != TOKEN:
            self._send(401, {"error": "unauthorized"})
            return False
        return True

    def _query(self) -> Dict[str, str]:
        return {k: v[0] for k, v in urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).items()}

    def _form(self, body: bytes) -> Dict[str, str]:
        return {k: v[0] for k, v in urllib.parse.parse_qs(body.decode("utf-8", "replace")).items()}

    # -- GET ----------------------------------------------------------------------------------------
    def do_GET(self):  # noqa: N802 - http.server API
        route = urllib.parse.urlsplit(self.path).path
        self.fake.requests.append(("GET", route, {}))
        if not self._authed():
            return
        handler = {
            "/api/state": self._get_state,
            "/api/errlog": lambda: self._send(200, "\n".join(self.fake.errlog) + "\n", "text/plain"),
            "/api/orch": lambda: self._send(200, self.fake.orch),
            "/api/chat": self._get_chat,
            "/api/files/dl": self._get_file,
            "/api/files/list": self._list_files,
            "/api/lastturn": lambda: self._send(200, "model: fake\n", "text/plain"),
            "/api/models": lambda: self._send(200, {"providers": {}}),
        }.get(route)
        if handler:
            handler()
        else:
            self._send(404, {"error": f"no route {route}"})

    def _get_state(self) -> None:
        if self.fake.state_delay:
            time.sleep(self.fake.state_delay)
        if self.fake.fail_state:
            self._send(500, {"error": "down"})
        else:
            self._send(200, self.fake.public_state())

    def _get_chat(self) -> None:
        reply = self.fake.turns.get(self._query().get("turn", ""), "")
        self._send(200, {"turn": self._query().get("turn"), "reply": reply, "pending": False, "done": bool(reply)})

    def _get_file(self) -> None:
        q = self._query()
        data = self.fake.files.get((q.get("project"), q.get("name")))
        if data is None:
            self._send(404, {"error": "no file"})
        else:
            self._send(200, data, "application/octet-stream")

    def _list_files(self) -> None:
        project = self._query().get("project")
        names = [{"name": n, "size": len(d)} for (p, n), d in self.fake.files.items() if p == project]
        self._send(200, {"project": project, "files": names})

    # -- POST -------------------------------------------------------------------------------------
    def do_POST(self):  # noqa: N802 - http.server API
        route = urllib.parse.urlsplit(self.path).path
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        form = {} if route == "/api/files/upload" else self._form(body)
        self.fake.requests.append(("POST", route, form))
        if not self._authed():
            return
        handler = {
            "/api/orch": self._post_orch,
            "/api/verify": self._post_verify,
            "/api/onboard/complete": self._post_complete,
            "/api/chat": self._post_chat,
            "/api/files/rm": self._post_rm,
            "/api/test/astool": self._post_astool,
        }.get(route)
        if route == "/api/files/upload":
            self._post_upload(body)
        elif handler:
            handler(form)
        else:
            self._send(404, {"error": f"no route {route}"})

    def _post_orch(self, form: Dict[str, str]) -> None:
        self.fake.orch.update(form)
        self._send(200, {"ok": True})

    def _post_verify(self, form: Dict[str, str]) -> None:
        if self.fake.verify_ok:
            self.fake.state["provVerified"] = True
        self._send(200, {"ok": True, "provider": form.get("provider")})

    def _post_complete(self, form: Dict[str, str]) -> None:
        if not self.fake.state.get("sta"):
            self._send(409, {"error": "wifi not connected"})
        elif not self.fake.state.get("provVerified"):
            self._send(409, {"error": "no verified provider"})
        else:
            self.fake.state["needsOnboarding"] = False
            self._send(200, {"ok": True, "restart": False})

    def _post_chat(self, form: Dict[str, str]) -> None:
        if self.fake.chat_code != 200:
            self._send(self.fake.chat_code, {"error": "refused"})
            return
        self._send(200, {"ok": True, "turn": self.fake.run_turn(form.get("text", ""))})

    def _post_rm(self, form: Dict[str, str]) -> None:
        self.fake.files.pop((form.get("project"), form.get("name")), None)
        self._send(200, {"ok": True})

    def _post_astool(self, form: Dict[str, str]) -> None:
        call = json.loads(form.get("body", "{}"))
        params = call.get("params", {})
        self._send(200, self.fake.astool(params.get("name", ""), params.get("arguments", {})))

    def _post_upload(self, body: bytes) -> None:
        q = self._query()
        match = re.search(rb"\r\n\r\n(.*)\r\n--", body, re.S)
        self.fake.files[(q.get("project"), q.get("name"))] = match.group(1) if match else b""
        self._send(200, {"ok": True})


class FakeConsole:
    """The driver side of bench_driver.Console. ``verbs`` maps a first word to a handler that takes the whole line
    and returns reply lines; the common verbs are built in and read the fake's state."""

    def __init__(self, device: FakeDevice, ip_on_join: str = ""):
        self.device = device
        self.ip_on_join = ip_on_join
        self.lines: List[tuple] = []
        self.sent: List[str] = []
        self.dead = False
        self.closed = False
        self.screen = 7  # SetupInfo
        self.menu_open = 0
        self.onboarded_override: Optional[int] = None
        self.calgate = "CALGATE active=0 kind=cap stored=0"
        self.ota_state = "idle"
        self.ota_latest = ""
        self.ota_flip: Callable[[], None] = lambda: None
        self.verbs: Dict[str, Callable[[str], List[str]]] = {}

    def _emit(self, text: str) -> None:
        self.lines.append((time.time(), text))

    def inject(self, text: str) -> None:
        self._emit(text)

    def send(self, line: str, secret: bool = False) -> None:
        self.sent.append("[secret]" if secret else line)
        for reply in self._answer(line):
            self._emit(reply)

    def cmd(self, line: str, expect: str, timeout: float = 8.0, secret: bool = False) -> str:
        if self.dead:
            return ""
        started = time.time() - 0.001
        self.send(line, secret)
        return self.wait(expect, min(timeout, 0.05), since=started)

    def wait(self, needle: str, timeout: float = 30.0, since: Optional[float] = None) -> str:
        end = time.time() + min(timeout, 1.0)
        while True:
            for t, text in self.lines:
                if (since is None or t >= since) and needle in text:
                    return text
            if time.time() >= end:
                return ""
            time.sleep(0.005)

    def since(self, started: float) -> List[str]:
        return [text for t, text in self.lines if t >= started]

    def close(self) -> None:
        self.closed = True

    # -- verbs ------------------------------------------------------------------------------------
    def _answer(self, line: str) -> List[str]:
        word = line.split(" ", 1)[0]
        if word in self.verbs:
            return self.verbs[word](line)
        builtin = getattr(self, "_v_" + re.sub(r"\W", "", word).lower(), None)
        return builtin(line) if builtin else [f"{word} (fake: no such verb)"]

    def _v_ping(self, line: str) -> List[str]:
        return ["PONG"]

    def _v_status(self, line: str) -> List[str]:
        s = self.device.state
        ip = self.ip_on_join if s.get("sta") else "0.0.0.0"
        return [
            f"STATUS fw={s['fw']} build={s['build']} mode=1 wifi={int(bool(s.get('sta')))} ip={ip} rssi=-40 "
            f"heap={s['heap']} scr=tft scrok=ok uptime={s['uptimeMs'] // 1000} board=freenove_s3"
        ]

    def _v_wifiap(self, line: str) -> List[str]:
        s = self.device.state
        onboarded = int(not s.get("needsOnboarding")) if self.onboarded_override is None else self.onboarded_override
        return [f"WIFIAP? ssid=Nimbus-1234-setup ip=192.168.4.1 up=1 sta={int(bool(s.get('sta')))} onboarded={onboarded} uptime=9 radio=1"]

    def _v_render(self, line: str) -> List[str]:
        return [f"RENDER screen={self.screen} posture=0 seg=0 single=0 dark=0 bright=80"]

    def _v_calgate(self, line: str) -> List[str]:
        return [self.calgate]

    def _v_menu(self, line: str) -> List[str]:
        return [f"MENU open={self.menu_open} sel=0 n=3 title=Settings items=Display|Connectivity|Reset"]

    def _v_tap(self, line: str) -> List[str]:
        _, x, y = line.split()[:3]
        if int(x) > 200 and int(y) < 44:
            self.menu_open, self.screen = 1, 3
        elif int(x) < 96 and int(y) < 44:
            self.menu_open, self.screen = 0, 0
        return [f"TAP {x} {y}"]

    def _v_wifi(self, line: str) -> List[str]:
        self.device.state["sta"] = True
        return ["WIFI provisioning ssid=hidden", f"WIFI_GOT_IP {self.ip_on_join}"]

    def _v_webtok(self, line: str) -> List[str]:
        from fake_bench_device import TOKEN

        return [f"WEBTOK {TOKEN}"]

    def _v_factreset(self, line: str) -> List[str]:
        self.device.state.update(needsOnboarding=True, provVerified=False, sta=False)
        return ["FACTRESET ok", "READY mode=1 ip=0.0.0.0"]

    def _v_reboot(self, line: str) -> List[str]:
        self.device.restart()
        return ["REBOOTING", "READY mode=1 ip=" + self.ip_on_join]

    def _v_nvsfill(self, line: str) -> List[str]:
        n = int(line.split()[1])
        self.device.state["nvs"]["free"] = 7712 - n * 32
        return [f"NVSFILL {n} free={self.device.state['nvs']['free']}"]

    def _v_otaurl(self, line: str) -> List[str]:
        return ["OTAURL " + (line.split(" ", 1)[1] if " " in line else "clear")]

    def _v_otacheck(self, line: str) -> List[str]:
        self.ota_state, self.ota_latest = "available", "v5.0.0"
        return ["OTACHECK started"]

    def _v_ota(self, line: str) -> List[str]:
        s = self.device.state
        return [
            f"OTA state={self.ota_state} err=- pct=0 latest={self.ota_latest} notes=n lastOta={s['lastOta']} "
            f"pend=0 boots=0 prev= slot={s['otaSlot']}"
        ]

    def _v_otaapply(self, line: str) -> List[str]:
        self.ota_flip()
        return ["OTAAPPLY started"]

    def _v_sfxvol(self, line: str) -> List[str]:
        return [f"SFXVOL -> {line.split()[1]}%"]

    def _v_turn(self, line: str) -> List[str]:
        text = line.split(" ", 1)[1]
        self.device.run_turn(text)
        return [f"TURN <- {text}", "turn end: ok host=fake"]

    def _v_histadd(self, line: str) -> List[str]:
        return ["HISTADD chat=web n=1"]

    def _v_models(self, line: str) -> List[str]:
        return ["MODELS passive queued=1"] if "passive" in line else ["MODELS? fw=v5.0.0 build=x"]

    def _v_provfault(self, line: str) -> List[str]:
        return ["PROVFAULT off"] if line.endswith(" off") else ["PROVFAULT host 402 armed (10 B body)"]

    def _v_park(self, line: str) -> List[str]:
        return ["PARK? providers=0"]

    def _v_snapshot(self, line: str) -> List[str]:
        return []
