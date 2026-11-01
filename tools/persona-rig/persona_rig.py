#!/usr/bin/env python3
"""persona_rig - run scripted personas against isolated nimbusd containers.

A persona is a small JSON file (format in README.md): the keys, models, caps, time
zone, name and tenant members of one instance, plus scenarios of scripted steps with
hard assertions. For each persona the rig starts one container on a throwaway volume,
configures it over /api/* the way the web app does, runs the steps, and stores the raw
evidence of every step: reply, tool calls, spend, log delta, assertion results.

Secrets: keys are read by name from a dotenv file, reach the instance only in a POST
body or a child-process environment (never argv), and every artifact is scrubbed.

Usage:
  persona_rig.py run --persona FILE [--persona FILE ...] --image nimbusd:dev \\
      --evidence DIR [--env-file .env] [--parallel N] [--judge] [--only ID,ID] [--keep]
Exit 0 only when every assertion of every persona passed.
"""

from __future__ import annotations

import argparse
import base64
import datetime
import json
import os
import re
import secrets as pysecrets
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from grade import ASSERTIONS, grade_step  # noqa: E402

DOCKER = os.environ.get("DOCKER_BIN", "docker")
HTTP_TIMEOUT_S = 60
STEP_KINDS = ("say", "http", "upload", "restart", "sleep")
FILLER_WORDS = "harbor lantern quartz meadow copper violet ember thistle marble willow".split()


# --------------------------------------------------------------------- loading
class PersonaError(ValueError):
    """A persona file that cannot run. Refused at load, before anything is spent."""


PLACEHOLDER = re.compile(r"\$\{([a-z_][a-z0-9_]*)\}")
SAFE_NAME = r"[A-Za-z0-9][A-Za-z0-9_.-]{0,62}"


def _fill(text: str, values: dict, where: str) -> str:
    def one(m):
        if m.group(1) not in values:
            raise PersonaError(f"{where}: no value for ${{{m.group(1)}}}")
        return str(values[m.group(1)])

    return PLACEHOLDER.sub(one, text)


def _substitute(node, values: dict, where: str):
    """${x} from the matrix row, in values and object keys. A bare $ is plain text."""
    if isinstance(node, str):
        return _fill(node, values, where)
    if isinstance(node, list):
        return [_substitute(n, values, where) for n in node]
    if isinstance(node, dict):
        return {_fill(k, values, where): _substitute(v, values, where) for k, v in node.items()}
    return node


def filler(n_bytes: int, seed: int) -> str:
    """Readable, distinct padding (a fold counts distinct content, not repeats)."""
    words, i = [], seed
    size = 0
    while size < n_bytes:
        w = f"{FILLER_WORDS[i % len(FILLER_WORDS)]}{i % 977}"
        words.append(w)
        size += len(w) + 1
        i = i * 31 + 7
    return " ".join(words)


def _expand_repeats(steps: list) -> list:
    """A step with "repeat": N becomes N steps; "$i" counts from 1 and "pad" adds filler."""
    out = []
    for step in steps:
        count = step.get("repeat")
        if not count:
            out.append(step)
            continue
        body = {k: v for k, v in step.items() if k not in ("repeat", "pad")}
        for i in range(1, count + 1):
            one = {**body, "say": body["say"].replace("$i", str(i))}
            if step.get("pad"):
                one["say"] = f"{one['say']}\n\n{filler(step['pad'], i * 101)}"
            out.append(one)
    return out


def _validate_step(step: dict, where: str):
    kinds = [k for k in STEP_KINDS if k in step]
    if len(kinds) != 1:
        raise PersonaError(f"{where}: a step needs exactly one of {STEP_KINDS}, got {kinds}")
    if "say" in step and not step.get("expect"):
        raise PersonaError(f"{where}: a turn with no expect proves nothing")
    for item in step.get("expect", []):
        if not isinstance(item, dict) or len(item) != 1:
            raise PersonaError(f"{where}: an expect item is one name and one value: {item!r}")
        name = next(iter(item))
        if name not in ASSERTIONS:
            raise PersonaError(f"{where}: unknown assertion {name!r}")


def validate_persona(p: dict) -> dict:
    """Refuse what would silently test nothing: no name, no scenario, no step, a bad check."""
    name = p.get("name", "")
    if not re.fullmatch(SAFE_NAME, name):
        raise PersonaError(f"bad persona name {name!r}")
    if not p.get("scenarios"):
        raise PersonaError(f"{name}: no scenarios")
    seen = set()
    for sc in p["scenarios"]:
        sid = sc.get("id", "")
        if not re.fullmatch(SAFE_NAME, sid) or sid in seen:
            raise PersonaError(f"{name}: scenario id bad or repeated: {sid!r}")
        seen.add(sid)
        if not sc.get("steps"):
            raise PersonaError(f"{name}/{sid}: no steps")
        sc["steps"] = _expand_repeats(sc["steps"])
        for n, step in enumerate(sc["steps"]):
            _validate_step(step, f"{name}/{sid}#{n}")
    return p


def load_personas(path: str) -> list:
    """One persona, or {"vars": {...}, "matrix": [{...}], "persona": {...}} expanding to many."""
    raw = json.loads(Path(path).read_text())
    if "matrix" not in raw:
        return [validate_persona(raw)]
    if not raw["matrix"]:
        raise PersonaError(f"{path}: empty matrix")
    out = []
    for row in raw["matrix"]:
        values = {**raw.get("vars", {}), **row}
        out.append(validate_persona(_substitute(raw["persona"], values, path)))
    return out


def read_env_file(path: str) -> dict:
    """KEY=VALUE lines. Values are held in memory only, never printed or written."""
    env = {}
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.removeprefix("export ").split("=", 1)
        env[k.strip()] = v.strip().strip("'\"")
    return env


class Scrubber:
    """Replaces every known secret value, then anything key-shaped that slipped through."""

    KEY_SHAPED = re.compile(r"\b(?:sk|tvly|cumulo_sk)[-_][A-Za-z0-9_\-]{12,}\b")

    def __init__(self, values=()):
        self.values = sorted({v for v in values if v and len(v) >= 6}, key=len, reverse=True)

    def add(self, value: str):
        if value and len(value) >= 6 and value not in self.values:
            self.values.append(value)
            self.values.sort(key=len, reverse=True)

    def __call__(self, text: str) -> str:
        for v in self.values:
            text = text.replace(v, "[REDACTED]")
        return self.KEY_SHAPED.sub("[REDACTED]", text)


# ---------------------------------------------------------------------- docker
class DockerBackend:
    """Starts one container per persona, each on its own throwaway named volume.

    A secret rides the child-process environment with a bare `-e NAME`, so it is never
    in argv. The control surface is published on host loopback only.
    """

    def __init__(self, image: str, docker: str = DOCKER, runner=subprocess.run):
        self.image, self.docker, self.runner = image, docker, runner

    def _run(self, args: list, env=None, check=True) -> str:
        full_env = {**os.environ, **(env or {})}
        r = self.runner([self.docker, *args], env=full_env, capture_output=True, text=True, timeout=120)
        if check and r.returncode != 0:
            raise RuntimeError(f"docker {args[0]} failed: {r.stderr.strip()[:400]}")
        return r.stdout

    def start(self, name: str, env: dict) -> int:
        """Run the container; returns the host port of its control surface."""
        env = {**env, "NIMBUSD_CONTROL_ADDR": "0.0.0.0"}  # the published port is host-loopback
        args = ["run", "-d", "--name", name, "-v", f"{name}:/data", "-p", "127.0.0.1:0:8787"]
        for k in sorted(env):
            args += ["-e", k]
        self._run([*args, self.image], env=env)
        out = self._run(["port", name, "8787/tcp"])
        return int(out.strip().splitlines()[0].rsplit(":", 1)[1])

    def restart(self, name: str) -> int:
        self._run(["restart", "-t", "20", name])
        out = self._run(["port", name, "8787/tcp"])
        return int(out.strip().splitlines()[0].rsplit(":", 1)[1])

    def logs(self, name: str) -> str:
        r = self.runner([self.docker, "logs", name], capture_output=True, text=True, timeout=120)
        return (r.stdout or "") + (r.stderr or "")

    def stop(self, name: str):
        self._run(["rm", "-f", name], check=False)
        self._run(["volume", "rm", "-f", name], check=False)


# -------------------------------------------------------------------- instance
class Instance:
    """One running nimbusd, over the same HTTP routes the web app uses."""

    def __init__(self, base: str, token: str):
        self.base, self.token = base.rstrip("/"), token

    def call(self, method: str, path: str, form=None, data=None, ctype=None):
        """(status, text). A transport failure reads as status 0, never as a raise."""
        if form is not None:
            data = urllib.parse.urlencode(form).encode()
            ctype = "application/x-www-form-urlencoded"
        req = urllib.request.Request(self.base + path, data=data, method=method)
        req.add_header("X-Nimbus-Token", self.token)
        if ctype:
            req.add_header("Content-Type", ctype)
        try:
            with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_S) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
            return 0, f"{type(e).__name__}: {e}"

    def get(self, path: str):
        return self.call("GET", path)

    def get_json(self, path: str) -> dict:
        status, body = self.get(path)
        try:
            return json.loads(body) if status == 200 else {}
        except ValueError:
            return {}

    def get_json_strict(self, path: str) -> dict:
        """For the snapshots a verdict rests on: an unreadable answer stops the persona."""
        status, body = self.get(path)
        if status != 200:
            raise RuntimeError(f"GET {path} answered {status}")
        return json.loads(body)

    def file_bytes(self, project: str, name: str):
        q = urllib.parse.urlencode({"project": project, "name": name})
        return self.get(f"/api/files/dl?{q}")

    def wait_ready(self, timeout_s: float = 60):
        end = time.time() + timeout_s
        while time.time() < end:
            if self.get("/readyz")[0] == 200:
                return
            time.sleep(0.5)
        raise RuntimeError(f"instance not ready after {timeout_s} s")

    def upload(self, project: str, name: str, content: bytes):
        boundary = uuid.uuid4().hex
        body = (
            (
                f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{name}"\r\n'
                "Content-Type: application/octet-stream\r\n\r\n"
            ).encode()
            + content
            + f"\r\n--{boundary}--\r\n".encode()
        )
        q = urllib.parse.urlencode({"project": project, "name": name})
        return self.call("POST", f"/api/files/upload?{q}", data=body, ctype=f"multipart/form-data; boundary={boundary}")


# --------------------------------------------------------------------- capture
def solid_png(rgb: tuple, size: int = 64) -> bytes:
    """A solid-color PNG from the standard library: a known answer for an image step."""

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    row = b"\x00" + bytes(rgb) * size
    head = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(row * size)) + chunk(b"IEND", b"")


def tools_from_anatomy(anatomy: str) -> list:
    """The tool calls of the last completed turn, read from /api/lastturn's tool-loop section.

    The hosted episodic store keeps no tool_output rows, so the anatomy is the only trace. It
    is one global slot: a synthesis turn overwrites it, so check a spawn by its side effects.
    """
    at = anatomy.find("===== 4.")
    if at < 0:
        return []
    calls = re.findall(r"^\[tool\] (\S+) ?(.*)$", anatomy[at:], re.M)
    results = re.findall(r"^\[result\] (\S+) ?(.*)$", anatomy[at:], re.M)
    out = [{"name": name, "args": args[:400]} for name, args in calls]
    for row, (_name, text) in zip(out, results):
        row["result"] = text[:400]
    return out


def overlap_tail(old: list, new: list) -> list:
    """The lines of `new` after its longest prefix that is also a suffix of `old`.

    The log is a bounded tail and repeats identical lines, so a set difference would
    hide a repeated line; this keeps every line the step really added.
    """
    for k in range(min(len(old), len(new)), 0, -1):
        if old[-k:] == new[:k]:
            return new[k:]
    return new


SPEND_RE = re.compile(r"turn spend: \S+ \S+ \| (\S+?)/\S+ in=(\d+) out=(\d+) cR=(\d+) cW=(\d+)")


def spend_lines(log: str) -> list:
    """Provider-reported usage per engine call, from the daemon's own spend lines."""
    keys = ("provider", "in", "out", "cache_read", "cache_write")
    rows = []
    for m in SPEND_RE.finditer(log):
        rows.append(dict(zip(keys, [m.group(1), *map(int, m.groups()[1:])])))
    return rows


class Capture:
    """Snapshots an instance around one step, so a record holds only that step's effects."""

    def __init__(self, inst: Instance):
        self.inst = inst
        self.seq = self.seen_ids = self.log_lines = self.anatomy = self.usage = self.spent = None
        self.mark()

    def _episodic(self) -> list:
        return self.inst.get_json_strict("/api/mem/episodic?limit=400").get("messages", [])

    def _usage(self) -> tuple:
        u = self.inst.get_json_strict("/api/orch")["usage"]
        cents = self.inst.get_json_strict("/api/state")["spend"]["spentCents"]
        return {"in": u["sessIn"], "out": u["sessOut"], "turns": u["turns"]}, cents

    def mark(self):
        self.seq = self.inst.get_json_strict("/api/replies?after=0")["lastSeq"]
        self.seen_ids = {m.get("id") for m in self._episodic()}
        self.log_lines = self.inst.get("/api/log")[1].splitlines()
        self.anatomy = self.inst.get("/api/lastturn")[1]
        self.usage, self.spent = self._usage()

    def replies(self) -> tuple:
        """(assistant texts since mark, whether a turn is still running)."""
        d = self.inst.get_json(f"/api/replies?after={self.seq}")
        rows = d.get("replies", [])
        return [r.get("text", "") for r in rows if r.get("role") == "assistant"], bool(d.get("turnInFlight"))

    def _log_delta(self) -> str:
        return "\n".join(overlap_tail(self.log_lines, self.inst.get("/api/log")[1].splitlines()))

    def fill(self, rec: dict):
        rec["replies"], _ = self.replies()
        rows = [m for m in self._episodic() if m.get("id") not in self.seen_ids]
        rec["episodic"] = rows
        rec["log"] = self._log_delta()
        rec["spend_lines"] = spend_lines(rec["log"])
        rec["lastturn"] = self.inst.get("/api/lastturn")[1]
        # One global slot: unchanged means no turn completed, so there is no trace to read.
        rec["tools"] = None if rec["lastturn"] == self.anatomy else tools_from_anatomy(rec["lastturn"])
        usage, spent = self._usage()
        rec["usage_delta"] = {k: usage[k] - self.usage.get(k, 0) for k in usage}
        rec["spent_cents_delta"] = spent - self.spent


def _poll_turn(inst: Instance, turn_id) -> tuple:
    """One poll of this turn's own reply, as the page does. (reply or None, raw poll)."""
    status, body = inst.get(f"/api/chat?turn={turn_id}")
    try:
        d = json.loads(body)
    except ValueError:
        d = {}
    done = bool(d.get("reply")) or (status == 200 and d.get("pending") is False)
    return (d.get("reply", "") if done else None), f"{status} {body}"


def _settled(step: dict, texts: list) -> bool:
    """Has the step's wait condition been met? (extra replies from sub-agents, usually)."""
    if len(texts) < step.get("await_replies", 0):
        return False
    rx = step.get("await_reply_regex")
    return not rx or re.search(rx, "\n".join(texts), re.I | re.S) is not None


class _Quiet:
    """Tracks when the reply ring last changed or a turn last ran."""

    def __init__(self):
        self.at, self.count = time.time(), -1

    def update(self, texts: list, busy: bool) -> float:
        if busy or len(texts) != self.count:
            self.at, self.count = time.time(), len(texts)
        return time.time() - self.at


def _await_step(inst: Instance, cap: Capture, step: dict, turn_id, rec: dict, deadline: float):
    """Poll until the turn is answered, the step's wait condition holds and things went quiet.

    A turn that never resolves on its own poll (the page would keep spinning) is recorded
    as turn_resolved false; the wait then ends on the reply ring, so the step still grades.
    """
    quiet_s, quiet = step.get("settle_s", 3), _Quiet()
    reply = None
    rec["last_poll"] = ""
    while time.time() < deadline:
        if turn_id is not None and reply is None:
            reply, rec["last_poll"] = _poll_turn(inst, turn_id)
            if reply is not None:
                rec["reply_s"] = round(time.time() - rec["_started"], 1)
        texts, busy = cap.replies()
        calm = quiet.update(texts, busy) >= quiet_s
        answered = reply is not None or bool(texts)
        if answered and calm and _settled(step, texts):
            break
        time.sleep(1)
    rec["turn_resolved"] = reply is not None
    rec["timed_out"] = time.time() >= deadline
    rec["reply"] = reply or ""


def _send_turn(inst: Instance, step: dict, rec: dict):
    """A web chat turn as the owner, or a member's turn through POST /api/message."""
    if step.get("as"):
        payload = json.dumps({"chat_id": str(step["as"]), "text": step["say"]}).encode()
        rec["send"] = inst.call("POST", "/api/message", data=payload, ctype="application/json")
        return None
    rec["send"] = inst.call("POST", "/api/chat", form={"text": step["say"]})
    try:
        return json.loads(rec["send"][1]).get("turn")
    except ValueError:
        return None


def run_say(inst: Instance, step: dict, rec: dict):
    cap = Capture(inst)
    rec["_started"] = time.time()
    deadline = rec["_started"] + step.get("timeout_s", 240)
    turn_id = _send_turn(inst, step, rec)
    _await_step(inst, cap, step, turn_id, rec, deadline)
    cap.fill(rec)
    if not rec["reply"] and rec["replies"]:
        rec["reply"] = rec["replies"][0]
    rec["total_s"] = round(time.time() - rec.pop("_started"), 1)


def run_http(inst: Instance, step: dict, rec: dict):
    h = step["http"]
    data, ctype = None, None
    if "json" in h:
        data, ctype = json.dumps(h["json"]).encode(), "application/json"
    status, body = inst.call(h.get("method", "GET"), h["path"], form=h.get("form"), data=data, ctype=ctype)
    rec["http"] = {"status": status, "body": body}


def run_upload(inst: Instance, step: dict, rec: dict):
    u = step["upload"]
    content = (
        solid_png(tuple(u["png_rgb"]))
        if "png_rgb" in u
        else base64.b64decode(u.get("b64", "")) or u.get("text", "").encode()
    )
    status, body = inst.upload(u.get("project", ""), u["name"], content)
    rec["http"] = {"status": status, "body": body}


def _implicit_checks(rec: dict) -> list:
    """A turn that was never accepted, or ran out of time, fails whatever else it asked."""
    out = []
    if "send" in rec and not 200 <= rec["send"][0] < 300:
        out.append({"check": "turn_sent", "arg": True, "pass": False, "seen": f"send answered {rec['send'][0]}"})
    if rec.get("timed_out"):
        out.append({"check": "turn_finished", "arg": True, "pass": False, "seen": "ran out of time"})
    return out


class Runner:
    """One persona on one container, start to teardown."""

    def __init__(self, persona: dict, backend, env_values: dict, evidence: Path, options=None):
        self.p, self.backend, self.env_values = persona, backend, env_values
        options = options or {}
        self.judge, self.keep = options.get("judge"), options.get("keep", False)
        self.dir = evidence / persona["name"]
        self.dir.mkdir(parents=True, exist_ok=True)
        self.token = pysecrets.token_urlsafe(24)
        self.name = f"nimbusrig-{persona['name'].lower()}-{uuid.uuid4().hex[:6]}"
        self.scrub = Scrubber([self.token, *self._secret_values()])
        self.inst = None
        self.summary = {"persona": persona["name"], "started": _utc(), "scenarios": [], "config": []}

    def _secret_values(self) -> list:
        names = [*self.p.get("keys", {}).values(), *self.p.get("instance_env_keys", [])]
        return [self.env_values.get(n, "") for n in names]

    def _write(self, rel: str, text: str):
        path = self.dir / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(self.scrub(text))

    def _require_keys(self):
        """A key name missing from the env file would post a blank key and waste the run."""
        names = [*self.p.get("keys", {}).values(), *self.p.get("instance_env_keys", [])]
        missing = [n for n in names if not self.env_values.get(n)]
        if missing:
            raise RuntimeError(f"no value in the env file for: {', '.join(missing)}")

    def _container_env(self) -> dict:
        env = {"NIMBUSD_WEB_TOKEN": self.token, **self.p.get("env", {})}
        for name in self.p.get("instance_env_keys", []):
            if self.env_values.get(name):
                env[name] = self.env_values[name]
        return env

    def start(self):
        port = self.backend.start(self.name, self._container_env())
        self.inst = Instance(f"http://127.0.0.1:{port}", self.token)
        self.inst.wait_ready()

    def _config_call(self, label: str, path: str, form: dict):
        status, body = self.inst.call("POST", path, form=form)
        self.summary["config"].append(
            {"step": label, "status": status, "body": self.scrub(body)[:400], "ok": 200 <= status < 300}
        )
        return status

    def _config_forms(self) -> list:
        """The web app's own posts, one at a time so a refusal names one setting."""
        p, forms = self.p, []
        for slug, key_env in p.get("keys", {}).items():
            field = {
                "openai": "oaiKey",
                "anthropic": "antKey",
                "mistral": "mistKey",
                "zai": "zaiKey",
                "cumulo": "cumuloKey",
            }[slug]
            forms.append((f"key {slug}", "/api/orch", {field: self.env_values.get(key_env, "")}))
        models = p.get("models", {})
        for slug, model in models.get("head", {}).items():
            forms.append((f"head model {slug}", "/api/orch", {f"orchM_{slug}": model}))
        for slug, model in models.get("sub", {}).items():
            forms.append((f"sub model {slug}", "/api/orch", {f"subM_{slug}": model}))
        if p.get("host"):
            forms.append(("primary provider", "/api/orch", {"orchHost": p["host"]}))
        if p.get("directive"):
            forms.append(("directive", "/api/orch", {"sysPrompt": p["directive"]}))
        if "monthly_cap_cents" in p:
            forms.append(("monthly ceiling", "/api/orch", {"spendCeil": str(p["monthly_cap_cents"])}))
        if p.get("timezone"):
            forms.append(("time zone", "/api/config", {"devTz": p["timezone"]}))
        if p.get("display_name"):
            forms.append(("name", "/api/config", {"devName": p["display_name"]}))
        for m in p.get("members", []):
            forms.append((f"member {m['chat']}", "/api/telegram/add", {"id": m["chat"], "name": m.get("name", "")}))
            if m.get("role"):
                forms.append((f"member role {m['chat']}", "/api/tenant", {"id": m["chat"], "role": m["role"]}))
        return forms

    def configure(self) -> bool:
        """Every setting must apply. A refused setting stops the persona: nothing would be proven."""
        ok = True
        for label, path, form in self._config_forms():
            status = self._config_call(label, path, form)
            ok = ok and 200 <= status < 300
            time.sleep(0.2)
        self._write("config.json", json.dumps(self.summary["config"], indent=1))
        return ok

    def wait_verified(self, timeout_s: float = 60):
        """A saved key verifies in the background. A rejected or unchecked key stops the persona."""
        slugs = list(self.p.get("keys", {}))
        end = time.time() + timeout_s
        provs = {}
        while slugs and time.time() < end:
            provs = self.inst.get_json("/api/orch").get("providers", {})
            if all(provs.get(s, {}).get("vts", 0) for s in slugs):  # vts: a verdict has landed
                break
            time.sleep(1)
        self._write("orch-after-config.json", self.inst.call("GET", "/api/orch")[1])
        unverified = [s for s in slugs if provs.get(s, {}).get("verify") != 1]
        if unverified:
            raise RuntimeError(f"key not verified for: {', '.join(unverified)}")

    def _judge_step(self, step: dict, rec: dict):
        if not self.judge or "say" not in step or not step.get("judge", True):
            return
        passed = all(r["pass"] for r in rec["checks"])
        oracle = {"passed": passed, "checks": [{"check": r["check"], "pass": r["pass"]} for r in rec["checks"]]}
        tools = [t["name"] for t in rec.get("tools") or []]
        try:
            rec["judge"] = self.judge(step["say"][:1500], rec.get("reply", ""), f"tools: {tools}", oracle)
        except Exception as e:  # a soft signal never fails a step
            rec["judge"] = {"rationale": f"judge unavailable: {type(e).__name__}"}

    def _run_step(self, step: dict) -> dict:
        rec = {"step": {k: v for k, v in step.items() if k != "expect"}, "at": _utc()}
        if "say" in step:
            run_say(self.inst, step, rec)
        elif "http" in step:
            run_http(self.inst, step, rec)
        elif "upload" in step:
            run_upload(self.inst, step, rec)
        elif "restart" in step:
            port = self.backend.restart(self.name)
            self.inst = Instance(f"http://127.0.0.1:{port}", self.token)
            self.inst.wait_ready()
        else:
            time.sleep(step["sleep"])
        rec["checks"] = _implicit_checks(rec) + grade_step(rec, step.get("expect", []), self.inst)
        self._judge_step(step, rec)
        return rec

    def run_scenario(self, sc: dict) -> dict:
        out = {"id": sc["id"], "steps": len(sc["steps"]), "failed": [], "tokens_in": 0, "tokens_out": 0, "cents": 0}
        for n, step in enumerate(sc["steps"]):
            rec = self._run_step(step)
            self._write(f"{sc['id']}/step-{n:02d}.json", json.dumps(rec, indent=1, ensure_ascii=False))
            out["tokens_in"] += rec.get("usage_delta", {}).get("in", 0)
            out["tokens_out"] += rec.get("usage_delta", {}).get("out", 0)
            out["cents"] += rec.get("spent_cents_delta", 0)
            out["failed"] += [{"step": n, **self._scrubbed(c)} for c in rec["checks"] if not c["pass"]]
        out["pass"] = not out["failed"]
        return out

    def _scrubbed(self, check: dict) -> dict:
        return json.loads(self.scrub(json.dumps(check)))

    def run(self, only=None) -> dict:
        try:
            self._require_keys()
            self.start()
            configured = self.configure()
            self.wait_verified()
            self.summary["configured"] = configured
            for sc in self.p["scenarios"]:
                if not configured:
                    self.summary["scenarios"].append(
                        {"id": sc["id"], "pass": False, "failed": [{"check": "configure"}]}
                    )
                elif not only or sc["id"] in only:
                    self.summary["scenarios"].append(self.run_scenario(sc))
        except Exception as e:  # the record must say what stopped the persona
            self.summary["error"] = self.scrub(f"{type(e).__name__}: {e}")
        finally:
            self._finish()
        return self.summary

    def _record_end(self):
        self.summary["finished"] = _utc()
        ran = self.summary["scenarios"]
        self.summary["pass"] = "error" not in self.summary and bool(ran) and all(s.get("pass") for s in ran)
        self._write("summary.json", json.dumps(self.summary, indent=1, ensure_ascii=False))

    def _finish(self):
        """The verdict is on disk first, and the container always goes last (it holds real keys)."""
        self._record_end()
        try:
            self._write("container.log", self.backend.logs(self.name))
            if self.inst:
                self._write("api-log.txt", self.inst.get("/api/log")[1])
        except Exception as e:  # lost logs must not leave a keyed container running
            self.summary["pass"] = False
            self.summary["error"] = self.scrub(f"logs: {type(e).__name__}: {e}")
            self._write("summary.json", json.dumps(self.summary, indent=1, ensure_ascii=False))
        finally:
            if not self.keep:
                self.backend.stop(self.name)


def _utc() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def make_judge_from_env(env_values: dict, avoid: str = ""):
    """The independent judge from tests/hil/qa_judge.py. Needs requests and a key; raises otherwise."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "hil"))
    from qa_judge import make_judge

    judge = make_judge(env_values, avoid_provider=avoid)
    if judge is None:
        raise RuntimeError("--judge needs ANTHROPIC_API_KEY or OPENAI_API_KEY in the env file")
    return judge


def _check_run(personas: list, only) -> None:
    """Refuse a run that would prove less than it says."""
    names = [p["name"] for p in personas]
    if not names:
        raise PersonaError("no persona to run")
    if len(set(names)) != len(names):
        raise PersonaError(f"persona names repeat: {sorted(names)}")
    known = {sc["id"] for p in personas for sc in p["scenarios"]}
    unknown = set(only or ()) - known
    if unknown:
        raise PersonaError(f"--only names no scenario: {sorted(unknown)}")


def run_all(personas: list, backend, env_values: dict, evidence: Path, options=None) -> list:
    """options: parallel (default 1), judge, only (scenario ids), keep."""
    options = options or {}
    _check_run(personas, options.get("only"))
    lock = threading.Lock()
    results = []

    def one(p):
        try:
            summary = Runner(p, backend, env_values, evidence, options).run(options.get("only"))
        except Exception as e:  # one persona's crash must not lose the others' results
            summary = {"persona": p["name"], "pass": False, "scenarios": [], "error": f"{type(e).__name__}: {e}"}
        with lock:
            results.append(summary)
            verdict = "PASS" if summary["pass"] else "FAIL"
            print(f"{verdict} {p['name']}", flush=True)

    with ThreadPoolExecutor(max_workers=max(1, options.get("parallel", 1))) as pool:
        list(pool.map(one, personas))
    return results


def run_and_write(personas: list, backend, env_values: dict, evidence: Path, options=None) -> int:
    """The whole run, its scrubbed run-summary.json, and the exit code."""
    results = run_all(personas, backend, env_values, evidence, options)
    scrub = Scrubber(env_values.values())
    (evidence / "run-summary.json").write_text(scrub(json.dumps(results, indent=1, ensure_ascii=False)))
    return 0 if results and all(r["pass"] for r in results) else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    run = sub.add_parser("run")
    run.add_argument("--persona", action="append", required=True)
    run.add_argument("--image", required=True)
    run.add_argument("--evidence", required=True)
    run.add_argument("--env-file", default=".env")
    run.add_argument("--parallel", type=int, default=1)
    run.add_argument("--judge", action="store_true")
    run.add_argument("--only", default="")
    run.add_argument("--names", default="", help="comma list: run only these persona names")
    run.add_argument("--keep", action="store_true", help="leave containers and volumes for inspection")
    a = ap.parse_args(argv)
    personas = [p for path in a.persona for p in load_personas(path)]
    if a.names:
        wanted = set(a.names.split(","))
        personas = [p for p in personas if p["name"] in wanted]
    env_values = read_env_file(a.env_file)
    judge = make_judge_from_env(env_values) if a.judge else None
    evidence = Path(a.evidence)
    evidence.mkdir(parents=True, exist_ok=True)
    only = set(a.only.split(",")) if a.only else None
    options = {"parallel": a.parallel, "judge": judge, "only": only, "keep": a.keep}
    return run_and_write(personas, DockerBackend(a.image), env_values, evidence, options)


if __name__ == "__main__":
    sys.exit(main())
