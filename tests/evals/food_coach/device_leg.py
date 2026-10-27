"""The food coach eval on a bench unit, over its web API (the device leg).

The same scenarios, judge and records as the host lab, but the turns run on a real
unit with its real prompt, clock, SD card, skills store and provider. It is a
confirmation leg: small repetition counts, one unit, one repetition at a time.

What it touches on the unit, and puts back when it ends (even on Ctrl-C):
the head provider and model, the stored directive, the monthly spend ceiling, the
chat history of the web chat, the vector memory, the food log's file project, the
`meal-photo-science` skill and nothing else. It never touches Lumi: the unit comes
only from the bench secrets (unit A or L), and an address that is Lumi's is refused.
It runs only while holding the bench lock of the unit (an atomic mkdir), for at most
the time given.
"""

from __future__ import annotations

import contextlib
import datetime as dt
import json
import os
import re
import time
from dataclasses import dataclass, field
from pathlib import Path

import requests

import fcworld

UNITS = ("A", "L")
LOG_PROJECT = fcworld.LOG_PATH.split("/", 1)[0]  # the food log's project, removed with the leg's seeds
LUMI_IPS = {"192.168.50.76"}
CHAT = "web"
TURN_TIMEOUT_S = 300
POLL_S = 3.0


class UnitRefused(RuntimeError):
    """The unit cannot or must not be used for this leg."""


def bench_paths() -> tuple[Path, Path]:
    """(device secrets dir, bench locks dir): NIMBUS_DEVICE_SECRETS and NIMBUS_BENCH_LOCKS,
    else the program layout when this tree is a lane worktree (<program>/lanes/<lane>/nimbus)."""
    program = fcworld.HERE.parents[5]
    secrets = os.environ.get("NIMBUS_DEVICE_SECRETS") or str(program.parent / "secrets" / "devices")
    locks = os.environ.get("NIMBUS_BENCH_LOCKS") or str(program / "locks")
    return Path(secrets), Path(locks)


def read_unit(unit: str, secrets_dir: Path | None = None) -> tuple[str, str]:
    """(ip, token) of a bench unit. The token is never printed or logged."""
    if unit not in UNITS:
        raise UnitRefused(f"unit must be one of {UNITS}; Lumi and every other device are refused")
    root = secrets_dir or bench_paths()[0]
    ip = (root / f"unit{unit}_ip.txt").read_text().strip()
    token = (root / f"unit{unit}_webtok.txt").read_text().strip()
    if not ip or not token:
        raise UnitRefused(f"unit {unit}: missing address or token file")
    if ip in LUMI_IPS:
        raise UnitRefused(f"unit {unit}: address {ip} is Lumi's; refusing")
    return ip, token


class Unit:
    """HTTP view of one unit. The token rides a header and is never echoed."""

    def __init__(self, ip: str, token: str, session: requests.Session | None = None):
        if ip in LUMI_IPS:
            raise UnitRefused("refusing to contact Lumi")
        self.ip = ip
        self.s = session or requests.Session()
        self.s.headers["X-Nimbus-Token"] = token

    def url(self, path: str) -> str:
        return f"http://{self.ip}{path}"

    def get(self, path: str, timeout: float = 15):
        return self.s.get(self.url(path), timeout=timeout)

    def post(self, path: str, data=None, files=None, timeout: float = 20):
        return self.s.post(self.url(path), data=data or {}, files=files, timeout=timeout)

    def json(self, path: str) -> dict:
        r = self.get(path)
        if r.status_code != 200:
            raise UnitRefused(f"GET {path}: HTTP {r.status_code}")
        return r.json()

    def orch(self, fields: dict) -> None:
        r = self.post("/api/orch", fields)
        if r.status_code not in (200, 507):
            raise UnitRefused(f"POST /api/orch {sorted(fields)}: HTTP {r.status_code} {r.text[:120]}")


@contextlib.contextmanager
def bench_lock(unit: str, who: str, locks_dir: Path | None = None, held: bool = False):
    """The unit's bench lock: atomic mkdir, an owner note inside, removed in a finally.
    `held` means the caller already took it (to flash the unit under it) and releases it:
    the lock must exist with a note that starts with the lane id (the first word of `who`),
    and is left in place."""
    root = locks_dir or bench_paths()[1]
    path = root / f"device-unit{unit}"
    note_file = path / "owner"
    if held:
        if not note_file.exists() or not note_file.read_text().startswith(who.split()[0] + " "):
            raise UnitRefused(f"--lock-held, but {path.name} is not held by {who}")
        yield path
        return
    try:
        path.mkdir()
    except FileExistsError:
        note = note_file.read_text().strip() if note_file.exists() else "unknown"
        raise UnitRefused(f"bench lock {path.name} is held by {note}") from None
    note_file.write_text(f"{who} {dt.datetime.now(dt.timezone.utc):%Y-%m-%dT%H:%M:%SZ}\n")
    try:
        yield path
    finally:
        with contextlib.suppress(OSError):
            note_file.unlink()
        with contextlib.suppress(OSError):
            path.rmdir()


@dataclass
class Saved:
    """What a leg changed on the unit, so it can be put back."""

    orch: dict = field(default_factory=dict)
    skill: str = ""


def ceiling_of(orch: dict) -> dict:
    """The monthly ceiling state of a /api/orch read (usage.ceiling)."""
    return (orch.get("usage") or {}).get("ceiling") or orch.get("spend") or {}


def snapshot(unit: Unit) -> Saved:
    o = unit.json("/api/orch")
    anthropic = (o.get("providers") or {}).get("anthropic") or {}
    if not anthropic.get("hasKey"):
        raise UnitRefused("the unit holds no Anthropic key; push one from the bench dotenv first")
    if ((o.get("providers") or {}).get("cumulo") or {}).get("hasKey"):
        raise UnitRefused("the unit holds a cloud router key: a refused turn could fail over to it")
    spend = ceiling_of(o)
    return Saved(
        orch={
            "orchHost": o.get("orchHost") or "",
            "provPrio": o.get("provPrio") or "",
            "orchM_anthropic": anthropic.get("orchModel") or "",
            "sysPrompt": o.get("directive") or "",
            "spendCeil": str(spend.get("limitCents") or "") if spend.get("active", True) else "off",
        }
    )


def restore(unit: Unit, saved: Saved) -> None:
    fields = {k: v for k, v in saved.orch.items() if v != ""}
    if "sysPrompt" not in fields:
        fields["sysPrompt"] = ""
    unit.orch(fields | {"convReset": "1"})


def room_under_ceiling(unit: Unit, need_cents: int = 500) -> str:
    """Raise the monthly ceiling for the leg when less than `need_cents` is left."""
    spend = ceiling_of(unit.json("/api/orch"))
    if not spend.get("active", True):
        return ""
    limit, spent = int(spend.get("limitCents") or 0), int(spend.get("spentCents") or 0)
    if limit - spent >= need_cents and not spend.get("frozen"):
        return ""
    return str(spent + 5000)


def apply_leg_settings(unit: Unit, model: str, directive: str) -> None:
    fields = {
        "orchHost": "anthropic",
        "provPrio": "anthropic",  # one provider: a failure is attributable, nothing fails over
        "orchM_anthropic": model,
        "sysPrompt": directive,
        "convReset": "1",
    }
    ceil = room_under_ceiling(unit)
    if ceil:
        fields["spendCeil"] = ceil
    unit.orch(fields)


MEMORY_MARK = "The owner's daily food targets are"


def forget_seeded_memory(unit: Unit) -> int:
    """Delete the vector memories this leg wrote (found by their opening words)."""
    gone = 0
    for offset in range(0, 400, 20):
        rows = unit.json(f"/api/mem/vector?limit=20&offset={offset}").get("entries", [])
        if not rows:
            break
        for row in rows:
            if MEMORY_MARK in (row.get("content") or ""):
                unit.post("/api/mem/vector", {"op": "delete", "id": row["id"]})
                gone += 1
    return gone


def cleanup(unit: Unit) -> None:
    """Take the leg's seeds off the unit; every step runs even if an earlier one fails."""
    steps = (
        lambda: unit.post("/api/files/rmproject", {"project": LOG_PROJECT, "confirm": "DELETE"}),
        lambda: unit.post("/api/skills/delete", {"id": "meal-photo-science"}),
        lambda: forget_seeded_memory(unit),
        lambda: unit.orch({"clearConv": "1", "memClear": "1"}),
    )
    for step in steps:
        with contextlib.suppress(Exception):
            step()


@contextlib.contextmanager
def leg_session(unit_id: str, model: str, directive: str, who: str, dirs=(None, None), session=None, held=False):
    """Hold the unit's bench lock, set it up for the leg, and put everything back.
    `dirs` is (secrets dir, locks dir); None takes the program layout."""
    with bench_lock(unit_id, who, dirs[1], held):
        ip, token = read_unit(unit_id, dirs[0])
        unit = Unit(ip, token, session)
        info = unit.json("/api/state")
        if info.get("mode") != 1:
            raise UnitRefused("the unit is not in Orchestrator mode")
        saved = snapshot(unit)
        try:
            apply_leg_settings(unit, model, directive)
            yield unit, info
        finally:
            with contextlib.suppress(Exception):
                cleanup(unit)
            restore(unit, saved)


# ---- seeding ---------------------------------------------------------------------
def astool(unit: Unit, tool: str, args: dict) -> str:
    """Call a tool as the web chat does (test builds only: /api/test/astool)."""
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    r = unit.post("/api/test/astool", {"chat": CHAT, "body": body})
    if r.status_code != 200:
        raise UnitRefused(f"{tool}: HTTP {r.status_code}; flash a test build")
    return r.text


def reset_world(unit: Unit) -> None:
    """A clean web chat: a conversation boundary (every read of the history stops there), no
    running notes, none of this leg's earlier seeds. Long-term memory and files the leg did
    not write are left alone."""
    unit.orch({"clearConv": "1", "convReset": "1", "memClear": "1"})
    unit.post("/api/files/rmproject", {"project": LOG_PROJECT, "confirm": "DELETE"})
    unit.post("/api/skills/delete", {"id": "meal-photo-science"})
    forget_seeded_memory(unit)
    time.sleep(6)  # the turn task drains the staged clear between turns


def seed_world(unit: Unit, payload: dict) -> None:
    seed = payload["seed"]
    for f in seed["files"]:
        project, name = f["path"].split("/", 1)
        r = unit.post(f"/api/files/upload?project={project}&name={name}", files={"file": (name, f["text"].encode())})
        if r.status_code != 200:
            raise UnitRefused(f"food log upload: HTTP {r.status_code} {r.text[:120]}")
    for s in seed["skills"]:
        unit.post("/api/skills/save", {"id": s["id"], "md": s["md"]})
        unit.post("/api/skills/approve", {"id": s["id"]})
    for m in seed["memory"]:
        astool(unit, "memory.write", {"content": m, "importance": 0.9, "ttl": "days"})


# ---- a turn ----------------------------------------------------------------------
def send_turn(unit: Unit, text: str, timeout_s: float = TURN_TIMEOUT_S) -> dict:
    """One owner message on the web chat; returns {'reply', 'ok', 'seconds'}."""
    t0 = time.time()
    for _ in range(3):
        r = unit.post("/api/chat", {"text": text}, timeout=20)
        if r.status_code != 503:
            break
        time.sleep(8)
    if r.status_code != 200:
        return {
            "reply": "",
            "ok": False,
            "seconds": 0.0,
            "error": f"POST /api/chat: HTTP {r.status_code} {r.text[:120]}",
        }
    turn = r.json().get("turn", "")
    while time.time() - t0 < timeout_s:
        time.sleep(POLL_S)
        g = unit.get(f"/api/chat?turn={turn}", timeout=15).json()
        if g.get("done") or g.get("gone"):
            reply = str(g.get("reply") or "")
            return {"reply": reply, "ok": bool(reply), "seconds": round(time.time() - t0, 1)}
    return {"reply": "", "ok": False, "seconds": round(time.time() - t0, 1), "error": "turn timed out"}


def tool_rows(unit: Unit, since_id: int) -> list[dict]:
    rows = unit.json(f"/api/mem/episodic?session={CHAT}&kind=tool_output&limit=200").get("messages", [])
    return [r for r in rows if epi_id(r) > since_id]


def epi_id(row: dict) -> int:
    try:
        return int(str(row.get("id", "m0"))[1:], 16)
    except ValueError:
        return 0


TRACE_RE = re.compile(r"^(?P<call>.+?\))\s*(?:->|\u2192)\s*(?P<out>.*)$", re.S)


def split_trace(rows: list[dict]) -> tuple[list[str], list[str]]:
    """Episodic tool rows ('name(args) -> output') as calls and results, oldest first."""
    calls, results = [], []
    for r in sorted(rows, key=epi_id):
        text = r.get("text") or ""
        m = TRACE_RE.match(text)
        calls.append(m.group("call") if m else text)
        results.append(m.group("out") if m else "")
    return calls, results


def newest_row_id(unit: Unit) -> int:
    rows = unit.json(f"/api/mem/episodic?session={CHAT}&limit=5").get("messages", [])
    return max((epi_id(r) for r in rows), default=0)


def usage_snapshot(unit: Unit) -> dict:
    """Per model token counts the unit has recorded (the provider's own usage numbers)."""
    return unit.json("/api/usage/models")


def turn_error(unit: Unit, since_id: int) -> str:
    """Why the unit's turn ended without a reply: the `err` of its newest turn-end log row."""
    rows = unit.json(f"/api/mem/episodic?session={CHAT}&kind=log&limit=6").get("messages", [])
    for row in sorted(rows, key=epi_id, reverse=True):
        if epi_id(row) > since_id:
            try:
                return str(json.loads(row.get("text") or "{}").get("err") or "")
            except ValueError:
                return ""
    return ""


def checked_turn(unit: Unit, text: str) -> dict:
    """send_turn, plus the unit's own reason when there was no reply. A provider the unit has
    parked (it learned a billing or key refusal earlier, perhaps from another leg) ends the leg:
    every further turn would be refused without a request."""
    before = newest_row_id(unit)
    got = send_turn(unit, text)
    if not got["ok"]:
        got["error"] = got.get("error") or turn_error(unit, before) or "the unit answered with nothing"
        if "parked" in got["error"]:
            raise UnitRefused(f"{got['error']}. Clear it on the console (PARK clear) and run the leg again")
    return got


def run_scenario(unit: Unit, payload: dict) -> dict:
    """Replay a scenario on the unit; the earlier owner turns are replayed live, so the
    assistant's earlier replies are the unit's own."""
    reset_world(unit)
    seed_world(unit, payload)
    history = []
    for h in payload["seed"]["history"]:
        if h["role"] != "user":
            continue
        got = checked_turn(unit, h["text"])
        history += [{"role": "user", "text": h["text"]}, {"role": "assistant", "text": got["reply"]}]
    since = newest_row_id(unit)
    turns = []
    for text in payload["turns"]:
        got = checked_turn(unit, text)
        calls, results = split_trace(tool_rows(unit, since))
        turns.append(
            {
                "owner": text,
                "reply": got["reply"],
                "deliveries": [got["reply"]] if got["reply"] else [],
                "ok": got["ok"],
                "error": got.get("error", ""),
                "seconds": got["seconds"],
                "tool_calls": calls,
                "tool_results": results,
            }
        )
    return {"turns": turns, "history": history}


def priced_delta(before: dict, after: dict) -> dict:
    """Cost of the usage that grew between two /api/usage/models reads: the unit's own
    record of the tokens each provider reported (period counts, as priced)."""
    import fcrun

    ledger = fcrun.spend_ledger()
    old = {(m.get("prov"), m.get("m")): m for m in before.get("models", [])}
    by: dict = {}
    for m in after.get("models", []):
        was = old.get((m.get("prov"), m.get("m")), {})
        usage = {
            "input_tokens": int(m.get("pIn", 0)) - int(was.get("pIn", 0)),
            "output_tokens": int(m.get("pOut", 0)) - int(was.get("pOut", 0)),
            "cache_read_input_tokens": int(m.get("pCr", 0)) - int(was.get("pCr", 0)),
            "cache_creation_input_tokens": int(m.get("pCw", 0)) - int(was.get("pCw", 0)),
        }
        if not any(v > 0 for v in usage.values()):
            continue
        usd, _ = ledger.cost(m["prov"], m["m"], usage)
        by[f"{m['prov']} {m['m']}"] = round(usd, 6)
    return {"total": round(sum(by.values()), 6), "by": by, "source": "unit usage record"}


def run_task(t, files: dict) -> dict:
    """The device-leg backend for one scenario repetition (see fcrun.execute)."""
    unit: Unit = t.extra["unit"]
    payload = fcworld.run_payload(t.scn, t.cfg, dt.date.today())
    files["run"].write_text(json.dumps(payload, indent=1), encoding="utf-8")
    before = usage_snapshot(unit)
    try:
        got, err = run_scenario(unit, payload), ""
    except (UnitRefused, requests.RequestException, ValueError) as exc:
        got, err = {"turns": [], "history": []}, f"{type(exc).__name__}: {exc}"
    cost = priced_delta(before, usage_snapshot(unit))
    banner = f"device unit {t.extra.get('unit_id', '?')} fw {t.extra.get('fw', '?')} model {t.model}"
    return {"attempts": 1, "error": err, "cost": cost, "driver": {**got, "banner": banner}}
