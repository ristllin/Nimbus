#!/usr/bin/env python3
"""nimbusd_ctl - start, configure, drive and restart a local nimbusd for the rig.

One Nimbusd object is one daemon on one data dir. Arm A talks to its chat API; arm B
uses only its MCP endpoint (tools/list, tools/call). Both are started the same way,
with the same keys and settings, so the tools behave the same in both arms.

Keys are read from a dotenv file inside this process and reach the daemon only in
its environment. Nothing here prints a key or writes one to disk.

Every rig daemon runs with the test clock (NIMBUSD_TEST_CLOCK=1, its token, the loopback
bind): advance() moves it through POST /test/clock, and a restart puts the offset back,
because a new process starts at the host clock and a scenario's time never runs backwards.
"""

from __future__ import annotations

import json
import os
import secrets
import signal
import socket
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

from scenario import MAX_ADVANCE_S, RIG_TZ

REPO = Path(__file__).resolve().parents[2]
DAEMON = REPO / "nimbusd" / "build" / "nimbusd"
SHIM_SRC = Path(__file__).resolve().parent / "capture_shim.c"
SHIM_OUT = REPO / "nimbusd" / "build" / "capture_shim.dylib"
HTTP_TIMEOUT_S = 60
# An advance answers after one idle step, which may be a due routine or a compaction (a paid
# call): nimbusd gives it up to 600 s (RB-VCa), so the rig waits a little longer.
CLOCK_TIMEOUT_S = 630
# Idle steps run after an advance: at least this many, then more while a compaction is running
# or queued, up to the cap, so the compaction a gap makes due has finished before the next turn.
IDLE_TICKS_MIN = 2
IDLE_TICKS_MAX = 60
# The slug -> env name pairs the daemon reads (nimbusd/src/daemon_config.h).
PROVIDER_ENV = {"anthropic": "ANTHROPIC_API_KEY", "openai": "OPENAI_API_KEY", "mistral": "MISTRAL_API_KEY"}


def load_env(path: str | Path) -> dict:
    """KEY=VALUE lines of a dotenv file, or of several joined by commas (a later file wins a name). Values stay in
    memory: never printed, never written."""
    env = {}
    paths = [Path(p) for p in str(path).split(",") if p]
    for line in [ln for p in paths for ln in p.read_text().splitlines()]:
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.removeprefix("export ").split("=", 1)
        env[k.strip()] = v.strip().strip("'\"")
    return env


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def build_shim() -> Path:
    """Compile the libcurl capture shim next to the daemon (rebuilt when its source is newer)."""
    if SHIM_OUT.exists() and SHIM_OUT.stat().st_mtime >= SHIM_SRC.stat().st_mtime:
        return SHIM_OUT
    cflags = subprocess.run(["curl-config", "--cflags"], capture_output=True, text=True).stdout.split()
    libs = subprocess.run(["curl-config", "--libs"], capture_output=True, text=True).stdout.split() or ["-lcurl"]
    prefix = subprocess.run(["curl-config", "--prefix"], capture_output=True, text=True).stdout.strip()
    rpath = [f"-Wl,-rpath,{prefix}/lib"] if prefix else []
    SHIM_OUT.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["cc", "-O1", "-Wall", "-dynamiclib", *cflags, str(SHIM_SRC), "-o", str(SHIM_OUT), *libs, *rpath]
    subprocess.run(cmd, check=True)
    return SHIM_OUT


class Nimbusd:
    """One daemon process on one data dir, over the routes the web app uses."""

    def __init__(self, data_dir: Path, keys: dict, log_path: Path, capture_url: str = ""):
        self.data_dir, self.log_path, self.capture_url = Path(data_dir), Path(log_path), capture_url
        self.keys = {k: v for k, v in keys.items() if v}
        self.token = secrets.token_urlsafe(24)
        self.port = 0
        self.proc = None
        self.head = ""
        self.offset_s = 0  # the test clock's offset as last read; put back after a restart

    # ---------------------------------------------------------------- process
    def _environment(self, head: str) -> dict:
        env = {
            "PATH": "/usr/bin:/bin",
            "HOME": str(self.data_dir),
            "TZ": RIG_TZ,
            "NIMBUSD_DATA_DIR": str(self.data_dir),
            "NIMBUSD_WEB_TOKEN": self.token,
            "NIMBUSD_CONTROL_PORT": str(self.port),
            # The test clock: /test/clock exists (token-gated, loopback bind only).
            "NIMBUSD_TEST_CLOCK": "1",
            "NIMBUSD_DEVICE_NAME": "Nimbus",
            # One head provider and no other in the failover order: a turn that fails
            # must fail, not be answered by another model.
            "NIMBUSD_PRIORITY": head,
            "NIMBUSD_ORCH_HOST": head,
            # Memory embeddings on the one embedding provider the rig has a working key for.
            "NIMBUSD_EMBED_HOST": "openai",
            "NIMBUSD_EMBED_MODEL": "text-embedding-3-small",
            "NIMBUSD_EMBED_DIMS": "256",
            **self.keys,
        }
        if self.capture_url:
            env["REFLOOP_CAPTURE"] = self.capture_url
            env["DYLD_INSERT_LIBRARIES"] = str(build_shim())
        return env

    def start(self, head: str):
        if not DAEMON.exists():
            raise RuntimeError(f"{DAEMON} is not built: run make -C nimbusd daemon (see tools/refloop/README.md)")
        self.head = head
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.port = free_port()
        log = open(self.log_path, "ab")
        self.proc = subprocess.Popen(
            [str(DAEMON)], env=self._environment(head), stdout=log, stderr=log, cwd=str(self.data_dir)
        )
        log.close()
        self.wait_ready()

    def stop(self):
        if not self.proc:
            return
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=10)
        self.proc = None

    def restart(self, while_stopped=None):
        """Stop cleanly and start again on the same data dir, as a container restart does.

        The new process starts at the host clock, so the offset the scenario had reached is put
        back before anything else runs on it (one idle step, like any advance). `while_stopped`,
        when given, runs between the stop and the start (the rig's engine-2 new conversation)."""
        offset = self.offset_s
        self.stop()
        if while_stopped:
            while_stopped()
        self.start(self.head)
        self.offset_s = 0
        while self.offset_s < offset:  # one POST moves at most MAX_ADVANCE_S
            self.advance(min(offset - self.offset_s, MAX_ADVANCE_S))

    def wait_ready(self, timeout_s: float = 30):
        end = time.time() + timeout_s
        while time.time() < end:
            if self.proc.poll() is not None:
                raise RuntimeError(f"nimbusd exited with {self.proc.returncode}: see {self.log_path}")
            if self.call("GET", "/readyz")[0] == 200:
                return
            time.sleep(0.2)
        raise RuntimeError(f"nimbusd not ready after {timeout_s} s: see {self.log_path}")

    # ------------------------------------------------------------------- http
    def call(self, method: str, path: str, form=None, data=None, ctype=None):
        """(status, text). A transport failure reads as status 0, never as a raise."""
        if form is not None:
            data, ctype = urllib.parse.urlencode(form).encode(), "application/x-www-form-urlencoded"
        return self._send(self._request(method, path, data, ctype))

    def _request(self, method: str, path: str, data=None, ctype=None) -> urllib.request.Request:
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", data=data, method=method)
        req.add_header("X-Nimbus-Token", self.token)
        if ctype:
            req.add_header("Content-Type", ctype)
        return req

    def _send(self, req: urllib.request.Request, timeout: float = HTTP_TIMEOUT_S) -> tuple:
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
            return 0, f"{type(e).__name__}: {e}"

    def get_json(self, path: str, wait_s: float = 120) -> dict:
        """A JSON read that waits out the daemon's 503 "busy" (a turn or a fold is running)."""
        end = time.time() + wait_s
        status, body = self.call("GET", path)
        while status == 503 and time.time() < end:
            time.sleep(0.5)
            status, body = self.call("GET", path)
        if status != 200:
            raise RuntimeError(f"GET {path} answered {status}: {body[:200]}")
        return json.loads(body)

    def get_text(self, path: str, wait_s: float = 120) -> str:
        end = time.time() + wait_s
        status, body = self.call("GET", path)
        while status == 503 and time.time() < end:
            time.sleep(0.5)
            status, body = self.call("GET", path)
        return body if status == 200 else ""

    def post_form(self, path: str, form: dict):
        """A settings post that must apply. Waits out busy; anything but 2xx is an error."""
        end = time.time() + 60
        status, body = self.call("POST", path, form=form)
        while status == 503 and time.time() < end:
            time.sleep(0.5)
            status, body = self.call("POST", path, form=form)
        if not 200 <= status < 300:
            raise RuntimeError(f"POST {path} {sorted(form)} answered {status}: {body[:200]}")
        return body

    def upload(self, project: str, name: str, content: bytes):
        boundary = uuid.uuid4().hex
        head = (
            f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{name}"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n"
        ).encode()
        body = head + content + f"\r\n--{boundary}--\r\n".encode()
        q = urllib.parse.urlencode({"project": project, "name": name})
        status, text = self.call(
            "POST", f"/api/files/upload?{q}", data=body, ctype=f"multipart/form-data; boundary={boundary}"
        )
        if not 200 <= status < 300:
            raise RuntimeError(f"upload {project}/{name} answered {status}: {text[:200]}")

    # ------------------------------------------------------------------ clock
    def _clock_answer(self, status: int, body: str, what: str) -> dict:
        if status != 200:
            raise RuntimeError(f"{what} answered {status}: {body[:200]} (is nimbusd built from rb-main?)")
        answer = json.loads(body)
        self.offset_s = int(answer["offset_s"])
        return answer

    def clock(self) -> dict:
        """{offset_s, epoch, idle_steps}: the daemon's virtual clock."""
        return self._clock_answer(*self.call("GET", "/test/clock"), "GET /test/clock")

    def advance(self, seconds: int) -> dict:
        """Move the clock forward by whole seconds and run one idle step; answers after that step."""
        return self._move({"advance_s": int(seconds)})

    def advance_to(self, epoch: int) -> dict:
        """Move the clock to a unix time (not in the past) and run one idle step: exact for a wall time."""
        return self._move({"to_epoch": int(epoch)})

    def _move(self, body: dict) -> dict:
        payload = json.dumps(body).encode()
        reply = self._send(self._request("POST", "/test/clock", payload, "application/json"), CLOCK_TIMEOUT_S)
        return self._clock_answer(*reply, f"POST /test/clock {body}")

    def fold_busy(self) -> bool:
        """A compaction is running or queued. No fold chat yet (404) is not busy; engine busy (503) is."""
        status, body = self.call("GET", "/api/fold/status")
        if status == 503:
            return True
        if status != 200:
            return False
        d = json.loads(body)
        return bool(d.get("running") or d.get("queued"))

    def tick_idle(self) -> int:
        """Idle steps after an advance, until no compaction is running or queued. Returns how many ran."""
        for n in range(1, IDLE_TICKS_MAX + 1):
            self.advance(0)
            if n >= IDLE_TICKS_MIN and not self.fold_busy():
                return n
        return IDLE_TICKS_MAX

    # -------------------------------------------------------------------- mcp
    def mcp(self, method: str, params: dict | None = None, wait_s: float = 120) -> dict:
        """One JSON-RPC call to the tool registry. Waits out "engine busy" (503)."""
        payload = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}).encode()
        end = time.time() + wait_s
        status, body = self.call("POST", "/mcp", data=payload, ctype="application/json")
        while status == 503 and time.time() < end:
            time.sleep(0.5)
            status, body = self.call("POST", "/mcp", data=payload, ctype="application/json")
        if status != 200:
            raise RuntimeError(f"mcp {method} answered {status}: {body[:200]}")
        reply = json.loads(body)
        if "error" in reply:
            raise RuntimeError(f"mcp {method}: {reply['error']}")
        return reply["result"]

    def tools(self) -> list:
        return self.mcp("tools/list")["tools"]

    def call_tool(self, name: str, arguments: dict) -> tuple:
        """(text, is_error) of one tool call, exactly as the registry answers it."""
        result = self.mcp("tools/call", {"name": name, "arguments": arguments})
        text = "\n".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")
        return text, bool(result.get("isError"))

    # ---------------------------------------------------------------- settings
    def configure(self, head: str, model: str, sub_model: str = "", ceiling_cents: int = 300):
        """The web app's own posts, then a readback: a setting that did not take stops the run."""
        self.post_form("/api/orch", {f"orchM_{head}": model})
        if sub_model:
            self.post_form("/api/orch", {f"subM_{head}": sub_model})
        self.post_form("/api/orch", {"spendCeil": str(ceiling_cents)})
        orch = self.get_json("/api/orch")
        got = orch.get("providers", {}).get(head, {}).get("orchModel")
        if got != model or orch.get("orchHost") != head:
            raise RuntimeError(f"head not as configured: host {orch.get('orchHost')} model {got}")

    def wait_verified(self, head: str, timeout_s: float = 60):
        """The key's background verify must land as verified before a paid turn."""
        end = time.time() + timeout_s
        prov = {}
        while time.time() < end:
            prov = self.get_json("/api/orch").get("providers", {}).get(head, {})
            if prov.get("vts"):
                break
            time.sleep(0.5)
        if prov.get("verify") != 1:
            raise RuntimeError(f"{head} key did not verify: verify={prov.get('verify')} {prov.get('vfyReason', '')}")


def keys_for(env: dict) -> dict:
    """The provider keys the rig hands a daemon, by the env names it reads."""
    return {name: env.get(name, "") for name in PROVIDER_ENV.values()}


if __name__ == "__main__":
    print("built", build_shim())
    print("daemon", DAEMON, "present" if DAEMON.exists() else "missing")
    print("keys file readable:", os.path.exists(os.environ.get("NIMBUS_ENV_FILE", "")))
