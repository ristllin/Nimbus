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
      --evidence DIR [--env-file .env] [--parallel N] [--judge] [--only ID,ID] [--keep] \\
      [--max-cents N] [--provider-limit N] [--retries N] [--resume]
  persona_rig.py regrade --evidence DIR
Exit 0 only when every assertion of every persona passed, or failed on a marked known defect.
"""

from __future__ import annotations

import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import secrets as pysecrets
import shutil
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
from grade import ERROR_REPLY_RE, grade_step, regrade_step  # noqa: E402
from persona_schema import (  # noqa: E402, F401  (the rig's public loading surface)
    KEY_FIELDS,
    SEARCH_KEY_FIELDS,
    PersonaError,
    filler,
    known_defects,
    load_personas,
    validate_persona,
)

DOCKER = os.environ.get("DOCKER_BIN", "docker")
BUSY_RETRY_S = 120  # how long a snapshot read waits out the instance's honest busy
HTTP_TIMEOUT_S = 60
CONFIG_PAUSE_S = 0.2  # the web app's pace between saves
# How long a read waits out the daemon's 503 "instance busy". A quiet moment
# between turns can run an automatic conversation fold - a paid provider call
# on the engine thread that a big history makes slow - so this must outlast
# the longest fold, not just a turn's tail (a GLM fold after a web-search turn
# outlasted 120 s and killed an all-green cell on 3 Nov 2026).
BUSY_WAIT_S = 420
BUSY_POLL_S = 1
# A persona that names no ceiling still gets this one, not the daemon's 20 USD default.
DEFAULT_CAP_CENTS = 200
# The whole run, judge included, stops here unless --max-cents says otherwise.
DEFAULT_MAX_CENTS = 500
# Turns at once per provider: sharing a key, a wider run gets rate limited.
DEFAULT_PROVIDER_LIMIT = 2
DEFAULT_RETRIES = 2
RETRY_BACKOFF_S = 20
FAKE_KEY_PREFIX = "RIG_FAKE_"
# The daemon's logged error for a failure worth another try. Nothing else is retried.
TRANSIENT_RE = re.compile(r"\bHTTP (429|500|502|503|504|529)\b")
# A transport-level failure (no HTTP status at all) is the same class: the request never
# got an answer, so no tool ran and another try repeats no side effect.
TRANSPORT_RE = re.compile(
    r"transport fail|no response|partial file|connection (reset|timed out)|did not answer in time", re.I
)
# Judge prices in cents per million tokens (input, output), from model_price_defaults.h.
JUDGE_PRICES = {"claude-sonnet-4-6": (300, 1500), "gpt-5.5": (500, 3000)}
JUDGE_FALLBACK_CENTS = 2  # one call, when the provider reported no usage

# Everything the rig relies on nimbusd to keep. test_persona_rig.py checks each entry against
# the daemon's source, so a rename fails CI instead of failing every live persona at configure.
CONTRACT = {
    "nimbusd/src/http_control.h": ['"/readyz"', '"/api/message"', '"/api/replies"', '"chat_id"', '"text"', '"after"'],
    "nimbusd/src/reply_buffer.h": ["lastSeq"],
    "nimbusd/src/tg_web.h": ['"/api/telegram"', '"/api/telegram/"', '"/api/tenant"', '"add"', '"allow"', '"tenants"']
    + ['"id"', '"name"', '"role"'],
    "nimbusd/src/hosted_settings.h": ['"orchHost"', '"sysPrompt"', '"clearConv"', '"spendCeil"', '"orchM_"', '"subM_"']
    + ['"devTz"', '"devName"', '"tavKey"'],
    "lib/core/include/nimbus/orch/provider_slots.h": [f'"{field}"' for field in KEY_FIELDS.values()],
    "nimbusd/src/spend_ceiling_vn.h": ["limitCents", "spentCents"],
    "nimbusd/src/provider_verify.h": ['"nocredits"'],
    "nimbusd/src/web_api.h": [
        '"/api/chat"',
        '"/api/orch"',
        '"/api/state"',
        '"/api/config"',
        '"/api/log"',
        '"/api/lastturn"',
        '"/api/mem/stats"',
        '"/api/mem/episodic"',
        '"/api/files/dl"',
        '"/api/files/upload"',
        '"providers"',
        '"verify"',
        '"vts"',
        '"vfyReason"',
        '"orchModel"',
        '"subModel"',
        '"directive"',
        '"usage"',
        '"sessIn"',
        '"sessOut"',
        '"turns"',
        '"ceiling"',
        '"spend"',
        '"pending"',
        '"reply"',
        '"turn"',
        '"text"',
        '"messages"',
        '"limit"',
        '"project"',
        '"name"',
        '"file"',
    ],
    "nimbusd/src/rig.h": ["===== 4. TOOL LOOP"],
    "lib/core/src/orch_transcript.cpp": ['"[tool] "', '"[result] "'],
    "lib/harness/src/engine.cpp": ['"turn spend: '],
}


def contract_missing(read) -> list:
    """Contract entries the daemon's source no longer holds, each in its own file. read(path) gives a file's text."""
    out = []
    for path, tokens in CONTRACT.items():
        text = read(path)
        out += [f"{path}: {t}" for t in tokens if t not in text]
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


class BudgetExhausted(RuntimeError):
    """The run's own ceiling is reached: no further paid turn starts."""


class RunLimits:
    """Shared by every persona of a run: the spend ceiling and the per-provider turn gate."""

    def __init__(self, max_cents: float = DEFAULT_MAX_CENTS, provider_limit: int = DEFAULT_PROVIDER_LIMIT):
        self.max_cents, self.provider_limit = max_cents, max(1, provider_limit)
        self.lock = threading.Lock()
        self.spent = {"turns": 0.0, "judge": 0.0}
        self.gates = {}

    def add(self, cents: float, source: str = "turns"):
        with self.lock:
            self.spent[source] += cents

    def total(self) -> float:
        with self.lock:
            return sum(self.spent.values())

    def check(self):
        if self.total() >= self.max_cents:
            raise BudgetExhausted(f"run spend {self.total():.1f} cents reached --max-cents {self.max_cents}")

    def turn_slot(self, provider: str):
        with self.lock:
            return self.gates.setdefault(provider, threading.BoundedSemaphore(self.provider_limit))

    def snapshot(self) -> dict:
        with self.lock:
            return {"max_cents": self.max_cents, **{f"{k}_cents": round(v, 2) for k, v in self.spent.items()}}


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
        """For the snapshots a verdict rests on: an unreadable answer stops the persona.

        A 503 is the daemon's "busy, try again": a turn is running, or it has just started
        and is loading its stores. That is a wait, not a failure.
        """
        end = time.time() + BUSY_WAIT_S
        status, body = self.get(path)
        while status == 503 and time.time() < end:
            time.sleep(BUSY_POLL_S)
            status, body = self.get(path)
        if status != 200:
            raise RuntimeError(f"GET {path} answered {status}")
        return json.loads(body)

    def text_strict(self, path: str, also_ok=()) -> str:
        """A plain-text read a verdict rests on: waits out busy, and an error is never read as log text."""
        end = time.time() + BUSY_WAIT_S
        status, body = self.get(path)
        while status == 503 and time.time() < end:
            time.sleep(BUSY_POLL_S)
            status, body = self.get(path)
        if status != 200 and status not in also_ok:
            raise RuntimeError(f"GET {path} answered {status}")
        return body

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
    """The tool calls of one completed turn, read from /api/lastturn's tool-loop section.

    The hosted episodic store keeps no tool_output rows, so the anatomy is the only trace. It
    shows head-turn tools only, never a sub-agent's, and the daemon cuts each result short.
    """
    at = anatomy.find("===== 4.")
    if at < 0:
        return []
    section = anatomy[at:]
    out = [{"name": name, "args": args[:400]} for name, args in re.findall(r"^\[tool\] (\S+) ?(.*)$", section, re.M)]
    for name, text in re.findall(r"^\[result\] (\S+) ?(.*)$", section, re.M):
        waiting = next((row for row in out if row["name"] == name and "result" not in row), None)
        if waiting:
            waiting["result"] = text[:400]
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
        self.turns_seen = []
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
        self.log_lines = self.inst.text_strict("/api/log").splitlines()
        self.anatomy = self.inst.text_strict("/api/lastturn", also_ok=(404,))  # 404: no turn has run yet
        self.usage, self.spent = self._usage()

    def replies(self) -> tuple:
        """(assistant texts since mark, whether a turn is still running)."""
        d = self.inst.get_json(f"/api/replies?after={self.seq}")
        rows = d.get("replies", [])
        return [r.get("text", "") for r in rows if r.get("role") == "assistant"], bool(d.get("turnInFlight"))

    def _log_delta(self) -> str:
        return "\n".join(overlap_tail(self.log_lines, self.inst.text_strict("/api/log").splitlines()))

    def note_turn(self):
        """/api/lastturn is one slot that a later turn overwrites: keep every turn seen in it."""
        status, text = self.inst.get("/api/lastturn")
        if status == 200 and text != self.anatomy and text not in self.turns_seen:
            self.turns_seen.append(text)

    def fill(self, rec: dict):
        rec["replies"], _ = self.replies()
        rows = [m for m in self._episodic() if m.get("id") not in self.seen_ids]
        rec["episodic"] = rows
        rec["log"] = self._log_delta()
        rec["spend_lines"] = spend_lines(rec["log"])
        self.note_turn()
        rec["lastturn"] = self.turns_seen[-1] if self.turns_seen else self.anatomy
        rec["lastturn_earlier"] = self.turns_seen[:-1]
        # Nothing new in the slot means no turn completed, so there is no trace to read.
        rec["tools"] = [t for a in self.turns_seen for t in tools_from_anatomy(a)] if self.turns_seen else None
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
        cap.note_turn()
        texts, busy = cap.replies()
        calm = quiet.update(texts, busy) >= quiet_s
        answered = reply is not None or bool(texts)
        if answered and calm and _settled(step, texts):
            break
        time.sleep(1)
    rec["turn_resolved"] = None if turn_id is None else reply is not None
    rec["timed_out"] = time.time() >= deadline
    rec["reply"] = reply or ""


def _send_turn(inst: Instance, step: dict, rec: dict):
    """A web chat turn as the owner, or a member's turn through POST /api/message."""
    if step.get("as"):
        payload = json.dumps({"chat_id": str(step["as"]), "text": step["say"]}).encode()
        rec["send"] = inst.call("POST", "/api/message", data=payload, ctype="application/json")
        return None
    form = {"text": step["say"]}
    if step.get("att"):
        form["att"] = "\n".join(step["att"])
    rec["send"] = inst.call("POST", "/api/chat", form=form)
    try:
        return json.loads(rec["send"][1]).get("turn")
    except ValueError:
        return None


def _ceiling_refusal(send) -> str | None:
    """The spend-ceiling gate answers at once with 409 spend_ceiling: the notice IS the reply."""
    if not send or send[0] != 409:
        return None
    try:
        body = json.loads(send[1])
    except ValueError:
        return None
    return body.get("error", "") if body.get("code") == "spend_ceiling" else None


def run_say(inst: Instance, step: dict, rec: dict):
    cap = Capture(inst)
    rec["_started"] = time.time()
    deadline = rec["_started"] + step.get("timeout_s", 240)
    turn_id = _send_turn(inst, step, rec)
    refusal = _ceiling_refusal(rec.get("send")) if turn_id is None else None
    if refusal is not None:
        cap.fill(rec)
        rec["ceiling_refusal"] = refusal
        rec["last_poll"] = f"{rec['send'][0]} {rec['send'][1]}"
        rec["reply"] = refusal
        rec["replies"] = [refusal]
        rec["turn_resolved"] = True
        rec["timed_out"] = False
        rec["total_s"] = round(time.time() - rec.pop("_started"), 1)
        return
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


IMPLICIT_CHECKS = {"turn_sent", "turn_finished", "conversation_reset"}


def _transient_failure(rec: dict) -> bool:
    """The turn failed on a provider error before any tool ran, so another try repeats no side effect."""
    text = "\n".join([rec.get("reply") or "", *rec.get("replies", [])])
    failed = not text.strip() or ERROR_REPLY_RE.search(text) is not None
    log = rec.get("log", "")
    return failed and not rec.get("tools") and (TRANSIENT_RE.search(log) or TRANSPORT_RE.search(log)) is not None


def _implicit_checks(rec: dict) -> list:
    """A turn that was never accepted, or ran out of time, fails whatever else it asked."""
    out = []
    if "send" in rec and not 200 <= rec["send"][0] < 300 and "ceiling_refusal" not in rec:
        out.append({"check": "turn_sent", "arg": True, "pass": False, "seen": f"send answered {rec['send'][0]}"})
    if rec.get("timed_out"):
        out.append({"check": "turn_finished", "arg": True, "pass": False, "seen": "ran out of time"})
    return out


def keep_earlier_run(path: Path):
    """Evidence is never overwritten: an earlier run at this path moves aside, with its time."""
    if not path.exists():
        return
    stamp = _utc().replace(":", "")
    aside = path.with_name(f"{path.name}.replaced-{stamp}")
    n = 1
    while aside.exists():
        n += 1
        aside = path.with_name(f"{path.name}.replaced-{stamp}-{n}")
    path.rename(aside)


class Runner:
    """One persona on one container, start to teardown."""

    def __init__(self, persona: dict, backend, env_values: dict, evidence: Path, options=None):
        self.p, self.backend = persona, backend
        self.env_values = {**env_values, **self._fake_keys()}
        options = options or {}
        self.keep = options.get("keep", False)
        self.limits = options.get("limits") or RunLimits()
        self.retries = options.get("retries", DEFAULT_RETRIES)
        self.backoff_s = options.get("backoff_s", RETRY_BACKOFF_S)
        factory = options.get("judge_factory")
        self.judge = factory(head_provider(persona)) if factory else None
        self.dir = _fresh_dir(evidence / persona["name"])
        self.token = pysecrets.token_urlsafe(24)
        self.name = f"nimbusrig-{persona['name'].lower()}-{uuid.uuid4().hex[:6]}"
        self.scrub = Scrubber([self.token, *self._secret_values()])
        self.inst = None
        self.counted_cents = 0
        self.log_seen = []
        self.summary = {"persona": persona["name"], "started": _utc(), "scenarios": [], "config": []}

    def _fake_keys(self) -> dict:
        """A key name starting RIG_FAKE_ is made up here: a key the provider will refuse, with no secret."""
        return {n: pysecrets.token_hex(16) for n in self.p.get("keys", {}).values() if n.startswith(FAKE_KEY_PREFIX)}

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
            field = KEY_FIELDS.get(slug) or SEARCH_KEY_FIELDS[slug]
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
        cap = p.get("monthly_cap_cents", DEFAULT_CAP_CENTS)
        forms.append(("monthly ceiling", "/api/orch", {"spendCeil": str(cap)}))
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
            time.sleep(CONFIG_PAUSE_S)
        self._write("config.json", json.dumps(self.summary["config"], indent=1))
        return ok

    def wait_verified(self, timeout_s: float = 60):
        """A saved key verifies in the background. The verdict must be the one the persona expects."""
        slugs = [s for s in self.p.get("keys", {}) if s != "tavily"]  # no verify for the search key
        end = time.time() + timeout_s
        provs = {}
        while slugs and time.time() < end:
            provs = self.inst.get_json("/api/orch").get("providers", {})
            if all(provs.get(s, {}).get("vts", 0) for s in slugs):  # vts: a verdict has landed
                break
            time.sleep(1)
        self._write("orch-after-config.json", self.inst.call("GET", "/api/orch")[1])
        wanted = self.p.get("key_expect", {})
        wrong = {s: key_verdict(provs.get(s, {})) for s in slugs}
        wrong = {s: v for s, v in wrong.items() if v != wanted.get(s, "verified")}
        if wrong:
            raise RuntimeError(f"key verdict not as expected: {wrong}")

    def _readback_rows(self) -> list:
        """(setting, what the persona asked for, what the instance now reports)."""
        p = self.p
        orch = self.inst.get_json_strict("/api/orch")
        provs = orch.get("providers", {})
        rows = []
        for role, field in (("head", "orchModel"), ("sub", "subModel")):
            for slug, model in p.get("models", {}).get(role, {}).items():
                rows.append((f"{role} model {slug}", model, provs.get(slug, {}).get(field)))
        if p.get("host"):
            rows.append(("primary provider", p["host"], orch.get("orchHost")))
        if p.get("directive"):
            rows.append(("directive", p["directive"], orch.get("directive")))
        cap = p.get("monthly_cap_cents", DEFAULT_CAP_CENTS)
        rows.append(("monthly ceiling", cap, orch.get("usage", {}).get("ceiling", {}).get("limitCents")))
        return rows + self._readback_identity()

    def _readback_identity(self) -> list:
        p, rows = self.p, []
        cfg = self.inst.get_json_strict("/api/config")
        if p.get("timezone"):
            rows.append(("time zone", p["timezone"], cfg.get("devTz")))
        if p.get("display_name"):
            rows.append(("name", p["display_name"], cfg.get("devName")))
        if p.get("members"):
            allowed = {str(m.get("id")) for m in self.inst.get_json_strict("/api/telegram").get("allow", [])}
            roles = {
                str(t.get("id")): t.get("role") for t in self.inst.get_json_strict("/api/tenant").get("tenants", [])
            }
            for m in p["members"]:
                rows.append((f"member {m['chat']}", m["chat"], m["chat"] if m["chat"] in allowed else None))
                if m.get("role"):
                    rows.append((f"member role {m['chat']}", m["role"], roles.get(m["chat"])))
        return rows

    def assert_config(self):
        """Read every setting back. A saved answer is not the same as a setting that took."""
        rows = self._readback_rows()
        self.summary["config_readback"] = [{"setting": s, "want": w, "got": g} for s, w, g in rows]
        wrong = [f"{s}: wanted {w!r}, instance reports {g!r}" for s, w, g in rows if w != g]
        if wrong:
            raise RuntimeError("settings did not apply: " + "; ".join(wrong))

    def _judge_step(self, step: dict, rec: dict):
        if not self.judge or "say" not in step or not step.get("judge", True):
            return
        if self.limits.total() >= self.limits.max_cents:
            rec["judge"] = {"rationale": "judge skipped: the run's ceiling is reached"}
            return
        passed = all(r["pass"] for r in rec["checks"])
        oracle = {"passed": passed, "checks": [{"check": r["check"], "pass": r["pass"]} for r in rec["checks"]]}
        tools = [t["name"] for t in rec.get("tools") or []]
        try:
            rec["judge"] = self.judge(step["say"][:1500], rec.get("reply", ""), f"tools: {tools}", oracle)
        except Exception as e:  # a soft signal never fails a step
            rec["judge"] = {"rationale": f"judge unavailable: {type(e).__name__}"}
        self.limits.add(judge_cents(rec["judge"]), "judge")
        rec["judge"] = rec["judge"] or {"rationale": "judge answer could not be parsed"}

    def _say(self, step: dict, rec: dict):
        """One turn, and again after a backoff when the provider itself failed. Every try is kept."""
        self.limits.check()
        tries = []
        with self.limits.turn_slot(head_provider(self.p)):
            for attempt in range(self.retries + 1):
                one = {k: rec[k] for k in ("step", "at")}
                run_say(self.inst, step, one)
                if attempt == self.retries or not _transient_failure(one):
                    break
                tries.append(one)
                time.sleep(self.backoff_s * (attempt + 1))
        rec.update(one)
        if tries:  # a failed try can bill: the step owns every try's spend
            rec["earlier_tries"] = tries
            rec["spent_cents_delta"] += sum(t["spent_cents_delta"] for t in tries)

    def _restart(self):
        self._sync_turn_log()
        port = self.backend.restart(self.name)
        self.inst = Instance(f"http://127.0.0.1:{port}", self.token)
        self.inst.wait_ready()
        self.log_seen = []
        self._append_turn_log([f"[rig {_utc()}] container restarted: the daemon's log starts again here"])

    def _append_turn_log(self, lines: list):
        path = self.dir / "turn-log.txt"
        with path.open("a") as f:
            f.write(self.scrub("\n".join(lines)) + "\n")

    def _sync_turn_log(self):
        """/api/log is an in-memory tail that a restart empties, so the rig keeps the whole life on disk."""
        lines = self.inst.text_strict("/api/log").splitlines()
        new = overlap_tail(self.log_seen, lines)
        if self.log_seen and lines and len(new) == len(lines):
            new = [f"[rig {_utc()}] the daemon's log tail wrapped: lines may be missing here", *new]
        if new:
            self._append_turn_log(new)
        self.log_seen = lines

    def _run_step(self, step: dict) -> dict:
        rec = {"step": {k: v for k, v in step.items() if k != "expect"}, "at": _utc()}
        if "say" in step:
            self._say(step, rec)
        elif "http" in step:
            run_http(self.inst, step, rec)
        elif "upload" in step:
            run_upload(self.inst, step, rec)
        elif "restart" in step:
            self._restart()
        else:
            time.sleep(step["sleep"])
        rec["checks"] = _implicit_checks(rec) + grade_step(rec, step.get("expect", []), self.inst)
        self._judge_step(step, rec)
        self._sync_turn_log()
        delta = rec.get("spent_cents_delta", 0)
        self.limits.add(delta)
        self.counted_cents += delta
        return rec

    def _reset_conversation(self, sc: dict, out: dict) -> bool:
        """Scenarios share an instance, not a conversation: the page's Clear before each one."""
        idle = self._wait_idle()
        status, body = self.inst.call("POST", "/api/orch", form={"clearConv": "1"})
        self._write(f"{sc['id']}/reset.json", json.dumps({"at": _utc(), "idle": idle, "status": status, "body": body}))
        if idle and 200 <= status < 300:
            return True
        seen = f"idle {idle}, clear answered {status}"
        out["failed"].append({"step": -1, "check": "conversation_reset", "arg": True, "pass": False, "seen": seen})
        return False

    def _wait_idle(self, timeout_s: float = 90) -> bool:
        """A sub-agent still running from the last scenario would answer into the next one."""
        end = time.time() + timeout_s
        while time.time() < end:
            if not self.inst.get_json_strict("/api/replies?after=0").get("turnInFlight"):
                return True
            time.sleep(1)
        return False

    def _classify(self, n: int, rec: dict, out: dict):
        """A marked failure is the known defect still there. A marked pass means it is gone: say so."""
        for raw in rec["checks"]:
            row = {"step": n, **self._scrubbed(raw)}
            if not raw["pass"] and raw.get("known_defect"):
                out["known_failures"].append(row)
            elif not raw["pass"]:
                out["failed"].append(row)
            elif raw.get("known_defect") and not raw.get("may_pass"):
                out["xpass"].append(row)

    def _run_steps(self, sc: dict, out: dict):
        verdicts = []
        for n, step in enumerate(sc["steps"]):
            rec = self._run_step(step)
            self._write(f"{sc['id']}/step-{n:02d}.json", json.dumps(rec, indent=1, ensure_ascii=False))
            out["tokens_in"] += rec.get("usage_delta", {}).get("in", 0)
            out["tokens_out"] += rec.get("usage_delta", {}).get("out", 0)
            out["cents"] += rec.get("spent_cents_delta", 0)
            self._classify(n, rec, out)
            if rec.get("judge") and "judge_provider" in rec["judge"]:
                verdicts.append((n, rec["judge"]))
        if verdicts:
            out["judge"] = judge_summary(verdicts, head_provider(self.p))

    def run_scenario(self, sc: dict, first: bool = True) -> dict:
        out = {"id": sc["id"], "steps": len(sc["steps"]), "failed": [], "known_failures": [], "xpass": []}
        out.update(tokens_in=0, tokens_out=0, cents=0, isolated=False)
        ready = True
        if not first and not sc.get("continue_conversation"):
            ready = out["isolated"] = self._reset_conversation(sc, out)
        if ready:  # a failed Clear would run, and pay for, a polluted conversation
            self._run_steps(sc, out)
        out["pass"] = not out["failed"] and not out["xpass"]
        out["verdict"] = _scenario_verdict(out)
        return out

    def _scrubbed(self, check: dict) -> dict:
        return json.loads(self.scrub(json.dumps(check)))

    def _run_scenarios(self, configured: bool, only):
        wanted = with_needs(self.p["scenarios"], only)
        first = True
        for sc in self.p["scenarios"]:
            if not configured:
                self.summary["scenarios"].append({"id": sc["id"], "pass": False, "failed": [{"check": "configure"}]})
            elif wanted is None or sc["id"] in wanted:
                self.summary["scenarios"].append(self.run_scenario(sc, first))
                first = False

    def run(self, only=None) -> dict:
        self.summary["fingerprint"] = persona_fingerprint(self.p, getattr(self.backend, "image", ""))
        try:
            self.limits.check()
            self._require_keys()
            if with_needs(self.p["scenarios"], only) == set():
                raise RuntimeError("none of the --only scenarios is in this persona")
            self.start()
            configured = self.configure()
            self.wait_verified()
            self.summary["configured"] = configured
            if configured:
                self.assert_config()
            self._run_scenarios(configured, only)
        except Exception as e:  # the record must say what stopped the persona
            self.summary["error"] = self.scrub(f"{type(e).__name__}: {e}")
        finally:
            self._finish()
        return self.summary

    def _spend_summary(self):
        """Provider-reported tokens from the whole turn log, and spend after the last step (late sub-agents)."""
        path = self.dir / "turn-log.txt"
        log = path.read_text() if path.exists() else ""
        self.summary["provider_usage"] = provider_usage(spend_lines(log))
        total = self.inst.get_json("/api/state").get("spend", {}).get("spentCents", self.counted_cents)
        self.limits.add(max(0, total - self.counted_cents))
        self.summary["ledger_cents"] = total

    def _record_end(self):
        self.summary["finished"] = _utc()
        ran = self.summary["scenarios"]
        self.summary["pass"] = "error" not in self.summary and bool(ran) and all(s.get("pass") for s in ran)
        self.summary["known_defects"] = sorted({f["known_defect"] for s in ran for f in s.get("known_failures", [])})
        judged = [s["judge"] for s in ran if s.get("judge")]
        if judged:
            self.summary["judge"] = merge_judges(judged)
        self._write("summary.json", json.dumps(self.summary, indent=1, ensure_ascii=False))

    def _finish(self):
        """The verdict is on disk first, and the container always goes last (it holds real keys)."""
        self._record_end()
        try:
            if self.inst:
                self._sync_turn_log()
                self._spend_summary()
                self._write("summary.json", json.dumps(self.summary, indent=1, ensure_ascii=False))
                self._write("api-log.txt", self.inst.get("/api/log")[1])
            self._write("container.log", self.backend.logs(self.name))
        except Exception as e:  # lost logs must not leave a keyed container running
            self.summary["pass"] = False
            self.summary["error"] = self.scrub(f"logs: {type(e).__name__}: {e}")
            self._write("summary.json", json.dumps(self.summary, indent=1, ensure_ascii=False))
        finally:
            if not self.keep:
                self.backend.stop(self.name)


def _utc() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _fresh_dir(path: Path) -> Path:
    """A rerun never mixes with an older run: stale step files would read as this run's evidence."""
    if path.exists() and any(path.iterdir()):
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S")
        shutil.move(str(path), f"{path}.prev-{stamp}-{uuid.uuid4().hex[:4]}")
    path.mkdir(parents=True, exist_ok=True)
    return path


def head_provider(p: dict) -> str:
    """The provider a persona's head runs on: its host, else its first key."""
    return p.get("host") or next(iter(p.get("keys", {})), "none")


def persona_fingerprint(p: dict, image: str) -> str:
    """What --resume compares: an edited persona or another image is not the same run."""
    return hashlib.sha256(json.dumps([p, image], sort_keys=True).encode()).hexdigest()


def key_verdict(pv: dict) -> str:
    """What /api/orch says of a key (nimbusd/src/provider_verify.h)."""
    if not pv.get("vts"):
        return "unchecked"
    if pv.get("verify") == 1:
        return "verified"
    if pv.get("verify") == 0:
        return "rejected"
    return pv.get("vfyReason") or "unverifiable"


def with_needs(scenarios: list, only):
    """The scenarios to run: --only plus what they build on. A fresh instance holds no earlier state."""
    if not only:
        return None
    by_id = {sc["id"]: sc for sc in scenarios}
    wanted, todo = set(), [i for i in only if i in by_id]
    while todo:
        sid = todo.pop()
        if sid not in wanted:
            wanted.add(sid)
            todo += by_id[sid].get("needs", [])
    return wanted


def _scenario_verdict(out: dict) -> str:
    if not out["pass"]:
        return "fail"
    return "known_defect" if out["known_failures"] else "pass"


def provider_usage(rows: list) -> dict:
    out = {}
    for row in rows:
        one = out.setdefault(row["provider"], {"in": 0, "out": 0, "cache_read": 0, "cache_write": 0, "calls": 0})
        for k in ("in", "out", "cache_read", "cache_write"):
            one[k] += row[k]
        one["calls"] += 1
    return out


def judge_cents(verdict) -> float:
    """From the usage the judge's provider reported; a fixed high figure when there is none."""
    if verdict is None:
        return JUDGE_FALLBACK_CENTS  # the call was billed and its answer could not be parsed
    if "judge_model" not in verdict:
        return 0
    if verdict.get("satisfied") is None and verdict.get("score") is None:
        return 0  # the call itself failed: nothing was billed
    usage = verdict.get("judge_usage") or {}
    tokens_in = usage.get("input_tokens")
    tokens_out = usage.get("output_tokens")
    if tokens_in is None or tokens_out is None:
        return JUDGE_FALLBACK_CENTS
    price_in, price_out = JUDGE_PRICES.get(verdict["judge_model"], max(JUDGE_PRICES.values()))
    return (tokens_in * price_in + tokens_out * price_out) / 1e6


def judge_summary(verdicts: list, head: str) -> dict:
    """What summary.json says of the judge: who judged, whether independently, what it flagged."""
    usable = [v for _n, v in verdicts if v.get("score") is not None]
    providers = sorted({v.get("judge_provider", "") for _n, v in verdicts} - {""})
    return {
        "provider": providers,
        "model": sorted({v.get("judge_model", "") for _n, v in verdicts} - {""}),
        "independent": bool(providers) and head not in providers,
        "steps": len(verdicts),
        "unavailable": len(verdicts) - len(usable),
        "min_score": min((v["score"] for v in usable), default=None),
        "dishonest_steps": [n for n, v in verdicts if v.get("honest") is False],
        "unsatisfied_steps": [n for n, v in verdicts if v.get("satisfied") is False],
        "cents": round(sum(judge_cents(v) for _n, v in verdicts), 3),
    }


def merge_judges(rows: list) -> dict:
    scores = [r["min_score"] for r in rows if r["min_score"] is not None]
    return {
        "provider": sorted({p for r in rows for p in r["provider"]}),
        "independent": all(r["independent"] for r in rows),
        "steps": sum(r["steps"] for r in rows),
        "unavailable": sum(r["unavailable"] for r in rows),
        "min_score": min(scores, default=None),
        "dishonest": sum(len(r["dishonest_steps"]) for r in rows),
        "unsatisfied": sum(len(r["unsatisfied_steps"]) for r in rows),
        "cents": round(sum(r["cents"] for r in rows), 3),
    }


def make_judge_from_env(env_values: dict, avoid: str = ""):
    """The independent judge from tests/hil/qa_judge.py, on a provider other than `avoid` when one has a key."""
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


def _finished_summary(evidence: Path, p: dict, only, image: str):
    """--resume: an earlier run's summary, when it finished this same persona with nothing left to run.

    Resume works per persona. A scenario cannot pick up on a fresh instance without what the
    ones before it built, so a persona that stopped part way runs again from its start.
    """
    try:
        summary = json.loads((evidence / p["name"] / "summary.json").read_text())
    except (OSError, ValueError):
        return None
    ran = {s.get("id") for s in summary.get("scenarios", [])}
    wanted = with_needs(p["scenarios"], only)
    wanted = {sc["id"] for sc in p["scenarios"]} if wanted is None else wanted
    same = summary.get("fingerprint") == persona_fingerprint(p, image)
    return summary if same and "finished" in summary and "error" not in summary and wanted <= ran else None


def run_all(personas: list, backend, env_values: dict, evidence: Path, options=None) -> list:
    """options: parallel (default 1), judge_factory, only (scenario ids), keep, limits, retries, resume."""
    options = {"limits": RunLimits(), **(options or {})}
    _check_run(personas, options.get("only"))
    lock = threading.Lock()
    results = []

    def one(p):
        image = getattr(backend, "image", "")
        done = _finished_summary(evidence, p, options.get("only"), image) if options.get("resume") else None
        try:
            summary = done or Runner(p, backend, env_values, evidence, options).run(options.get("only"))
        except Exception as e:  # one persona's crash must not lose the others' results
            summary = {"persona": p["name"], "pass": False, "scenarios": [], "error": f"{type(e).__name__}: {e}"}
        with lock:
            results.append(summary)
            verdict = "PASS" if summary["pass"] else "FAIL"
            known = len(summary.get("known_defects", []))
            print(
                f"{verdict} {p['name']}{' (resumed)' if done else ''}{f' known defects: {known}' if known else ''}",
                flush=True,
            )

    with ThreadPoolExecutor(max_workers=max(1, options.get("parallel", 1))) as pool:
        list(pool.map(one, personas))
    return results


def run_and_write(personas: list, backend, env_values: dict, evidence: Path, options=None) -> int:
    """The whole run, its scrubbed run-summary.json and run-totals.json, and the exit code."""
    options = {"limits": RunLimits(), **(options or {})}
    results = run_all(personas, backend, env_values, evidence, options)
    scrub = Scrubber(env_values.values())
    keep_earlier_run(evidence / "run-summary.json")
    (evidence / "run-summary.json").write_text(scrub(json.dumps(results, indent=1, ensure_ascii=False)))
    totals = {
        **options["limits"].snapshot(),
        "personas": len(results),
        "passed": sum(1 for r in results if r["pass"]),
        "known_defects": sorted({d for r in results for d in r.get("known_defects", [])}),
    }
    (evidence / "run-totals.json").write_text(json.dumps(totals, indent=1))
    return 0 if results and all(r["pass"] for r in results) else 1


def regrade(evidence: Path, out=sys.stdout) -> int:
    """Re-grade every stored step with today's graders. Exit 1 when a stored verdict would change."""
    flips = checked = unreadable = 0
    for path in sorted(evidence.rglob("step-*.json")):
        if any(".prev-" in part for part in path.parts):
            continue
        try:
            rec = json.loads(path.read_text())
        except (OSError, ValueError):
            unreadable += 1
            print(f"{path.relative_to(evidence)}: unreadable", file=out)
            continue
        checked += 1
        for check, stored, now in regrade_step(rec):
            flips += 1
            print(f"{path.relative_to(evidence)} {check}: stored {stored}, now {now}", file=out)
    print(f"regrade: {checked} steps, {flips} verdicts would change, {unreadable} unreadable", file=out)
    return 0 if checked and not flips and not unreadable else 1


def _parser() -> argparse.ArgumentParser:
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
    run.add_argument("--max-cents", type=float, default=DEFAULT_MAX_CENTS, help="stop paid turns at this run total")
    run.add_argument("--provider-limit", type=int, default=DEFAULT_PROVIDER_LIMIT, help="turns at once per provider")
    run.add_argument("--retries", type=int, default=DEFAULT_RETRIES, help="more tries on a provider's 429 or 5xx")
    run.add_argument("--resume", action="store_true", help="skip personas this evidence dir already finished")
    grade_cmd = sub.add_parser("regrade")
    grade_cmd.add_argument("--evidence", required=True)
    return ap


def _judge_factory(env_values: dict):
    make_judge_from_env(env_values)  # no key at all stops the run here, not after containers start
    return lambda avoid: make_judge_from_env(env_values, avoid)


def main(argv=None) -> int:
    a = _parser().parse_args(argv)
    if a.cmd == "regrade":
        return regrade(Path(a.evidence))
    evidence = Path(a.evidence)
    evidence.mkdir(parents=True, exist_ok=True)
    personas = [p for path in a.persona for p in load_personas(path, evidence / "truth")]
    if a.names:
        wanted = set(a.names.split(","))
        personas = [p for p in personas if p["name"] in wanted]
    env_values = read_env_file(a.env_file)
    options = {
        "parallel": a.parallel,
        "judge_factory": _judge_factory(env_values) if a.judge else None,
        "only": set(a.only.split(",")) if a.only else None,
        "keep": a.keep,
        "limits": RunLimits(a.max_cents, a.provider_limit),
        "retries": a.retries,
        "resume": a.resume,
    }
    return run_and_write(personas, DockerBackend(a.image), env_values, evidence, options)


if __name__ == "__main__":
    sys.exit(main())
