#!/usr/bin/env python3
"""arms - the two arms of the comparison, behind one small interface.

  ArmA  nimbusd's own harness, driven over its chat API the way the web page drives it.
  ArmB  the reference loop (refloop.py) with the same model, calling the tools of its own
        nimbusd over MCP, on a separate data dir seeded the same way.

Both arms send every provider request through a capture proxy, so each step's record
holds the same things, measured the same way: the replies, the latency, the provider
requests, the tokens and cost, and every tool call with its result.

Time: an advance step moves the arm's daemon clock (and arm B's own stamps) and runs its
idle work; each turn's record carries its chat key, its virtual gap and whether it is a
cold transition, and its cost on the virtual clock (costs.virtual_costs).
"""

from __future__ import annotations

import base64
import json
import queue
import re
import shutil
import threading
import time
import urllib.parse
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

import fixtures
import refloop
from capture_proxy import CaptureProxy
from costs import cold_cost_usd, cost_usd, virtual_costs
from nimbusd_ctl import PROVIDER_ENV, Nimbusd
from scenario import RIG_TZ, advance_target, needs_state, parse_advance

HEAD_PATHS = ("/v1/messages", "/v1/responses", "/v1/chat/completions")
PROVIDER_HOST = {"anthropic": "api.anthropic.com", "openai": "api.openai.com/v1", "mistral": "api.mistral.ai/v1"}
TURN_TIMEOUT_S = 300
SETTLE_MAX_S = 90
SETTLE_POLL_S = 0.25
# A turn opens cold (PRD section 2) when it is the first on its chat key, follows a restart,
# or comes more than this many virtual seconds after the previous turn on that chat ended.
COLD_TURN_GAP_S = 270
ASYNC_MAX_S = 300  # how long a spawned sub-agent's result is waited for
RESULT_KEEP = 4000  # characters of a tool result kept in a step record (the capture has it whole)
# Chat rows the daemon itself posts beside the turn's reply (fixed product copy, not the model's words):
# the approval notice for a routine the assistant created, and the standing-rule notice.
NOTICE_RE = re.compile(r"^(The assistant wants to schedule |Standing rule saved \()")


@dataclass
class Job:
    """One scenario repetition on one model: everything an arm needs."""

    model: str
    head: str  # provider slug: anthropic or openai
    effort: str | None  # what the harness sends for this model, mirrored by the reference loop
    sub_model: str
    scenario: dict
    rep: int
    dir: Path
    env: dict = field(repr=False, default_factory=dict)
    loop_note: str = ""  # an experiment: one extra line on the reference loop's system prompt

    @property
    def keys(self) -> dict:
        return {name: self.env.get(name, "") for name in PROVIDER_ENV.values()}


# ------------------------------------------------------------------ wire parsing
def _tool_uses(response) -> list:
    """(call id, name, arguments) of every tool call in one provider response, either wire."""
    if not isinstance(response, dict):
        return []
    out = [
        (b.get("id"), b.get("name"), b.get("input") or {})
        for b in response.get("content") or []
        if isinstance(b, dict) and b.get("type") == "tool_use"
    ]
    for item in response.get("output") or []:
        if isinstance(item, dict) and item.get("type") == "function_call":
            try:
                args = json.loads(item.get("arguments") or "{}")
            except ValueError:
                args = {"_raw": item.get("arguments")}
            out.append((item.get("call_id"), item.get("name"), args))
    return out + _chat_tool_uses(response)


def _chat_args(raw):
    if not isinstance(raw, str):
        return raw or {}
    try:
        return json.loads(raw or "{}")
    except ValueError:
        return {"_raw": raw}


def _chat_tool_uses(response: dict) -> list:
    """(call id, name, arguments) of the tool calls in a Chat Completions response (the Mistral wire)."""
    out = []
    for choice in response.get("choices") or []:
        for c in ((choice or {}).get("message") or {}).get("tool_calls") or []:
            fn = c.get("function") or {}
            out.append((c.get("id"), fn.get("name"), _chat_args(fn.get("arguments"))))
    return out


def _result_text(content) -> str:
    if isinstance(content, str):
        return content
    return "\n".join(b.get("text", "") for b in content or [] if isinstance(b, dict) and b.get("type") == "text")


def _chat_tool_results(request: dict) -> list:
    """(call id, text) of the tool messages a Chat Completions request carries (the Mistral wire)."""
    return [
        (m.get("tool_call_id"), _result_text(m.get("content")))
        for m in request.get("messages") or []
        if isinstance(m, dict) and m.get("role") == "tool"
    ]


def _tool_results(request) -> list:
    """(call id, text) of every tool result a request carries, either wire."""
    if not isinstance(request, dict):
        return []
    out = []
    for m in request.get("messages") or []:
        for b in m.get("content") if isinstance(m.get("content"), list) else []:
            if isinstance(b, dict) and b.get("type") == "tool_result":
                out.append((b.get("tool_use_id"), _result_text(b.get("content"))))
    for item in request.get("input") if isinstance(request.get("input"), list) else []:
        if isinstance(item, dict) and item.get("type") == "function_call_output":
            out.append((item.get("call_id"), _result_text(item.get("output"))))
    out += _chat_tool_results(request)
    return out


# orch_turn reply fields that act like a tool call, and the tool each stands for.
FIELD_TOOLS = {"mem_write": "memory_write", "mem_query": "memory_search", "session_ops": "session_spawn"}


def _paired_calls(exchanges: list) -> list:
    """Every tool call of one turn in order, each with the result a later request carried for it."""
    calls, by_id = [], {}
    for ex in exchanges:
        for cid, text in _tool_results(ex.get("request")):
            if cid in by_id and "result" not in by_id[cid]:
                by_id[cid]["result"] = text
        for cid, name, args in _tool_uses(ex.get("response")):
            by_id[cid] = {"name": name, "args": args}
            calls.append(by_id[cid])
    return calls


def _field_calls(fields: dict) -> list:
    """The acting reply fields of an orch_turn as tool calls (a mem_write item is a memory write)."""
    out = []
    for name, tool in FIELD_TOOLS.items():
        for item in fields.get(name) or []:
            out.append(
                {
                    "name": tool,
                    "args": item if isinstance(item, dict) else {"query": item},
                    "result": f"(reply field {name}, applied by the harness after the turn)",
                    "via": name,
                }
            )
    return out


def harness_calls(exchanges: list) -> tuple:
    """One harness turn's tool calls and reply fields, read from its captured head exchanges.

    Returns (tools, harness): tools is the registry calls in order with their results, plus one
    entry per acting reply field of every accepted orch_turn (the owner's turn, and the follow-up
    turn a finished sub-agent triggers); harness holds those fields and any orch_turn sent back.
    """
    calls = _paired_calls(exchanges)
    finals = [c for c in calls if c["name"] == "orch_turn"]
    accepted = [{k: v for k, v in c["args"].items() if k != "reply" and v} for c in finals if not c.get("result")]
    tools = [c for c in calls if c["name"] != "orch_turn"]
    for fields in accepted:
        tools += _field_calls(fields)
    bounced = [c["result"][:600] for c in finals if c.get("result")]
    merged = {k: v for fields in accepted for k, v in fields.items()}
    return _clip(tools), {"fields": merged, "bounced": bounced, "orch_turn_calls": len(finals)}


RECALL_HEAD = "## RELEVANT MEMORIES"


def recalled_memories(exchanges: list) -> list:
    """The long-term memories the harness itself put into the turn's context (its automatic recall).

    They reach the model with no tool call, so without this the record (and the judge) could not
    tell an answer from memory apart from a guess.
    """
    request = exchanges[0].get("request") if exchanges else None
    for m in reversed((request or {}).get("messages") or (request or {}).get("input") or []):
        content = m.get("content") if isinstance(m, dict) else None
        text = content if isinstance(content, str) else _result_text(content)
        if RECALL_HEAD in (text or ""):
            block = text.split(RECALL_HEAD, 1)[1].split("\n\n", 1)[0]
            return [line[2:].strip() for line in block.splitlines() if line.startswith("- ")]
    return []


def spawned(exchanges: list) -> bool:
    """True when the turn started a sub-agent: its result arrives later, as another chat message."""
    return any(t.get("via") == "session_ops" for t in harness_calls(exchanges)[0])


def _clip(tools: list) -> list:
    for t in tools:
        t["result"] = (t.get("result") or "")[:RESULT_KEEP]
    return tools


def split_notices(texts: list) -> tuple:
    """(the assistant's replies, the daemon's own notices) among the chat rows a turn produced."""
    return [t for t in texts if not NOTICE_RE.match(t)], [t for t in texts if NOTICE_RE.match(t)]


# ------------------------------------------------------------------------ state
def snapshot_state(d: Nimbusd) -> dict:
    """What the instance holds right now: files with their contents, routines, and everything memory-like."""
    files = {}
    for f in d.get_json("/api/files/list").get("files", []):
        q = urllib.parse.urlencode({"project": f["project"], "name": f["name"]})
        status, body = d.call("GET", f"/api/files/dl?{q}")
        files[f"{f['project']}/{f['name']}"] = body[:60000] if status == 200 else f"(unreadable: {status})"
    memories = [e.get("content", "") for e in d.get_json("/api/mem/vector?limit=200").get("entries", [])]
    rules, _ = d.call_tool("prefs.edit", {"action": "list"})
    status, scratch = d.call("GET", "/api/mem/scratchpad")
    return {
        "files": files,
        "loops": d.get_json("/api/loops"),
        "memories": memories,
        "rules": rules,
        "scratchpad": scratch if status == 200 else "",
    }


def _token_sums(rows: list) -> dict:
    return {k: sum((r["usage"] or {}).get(k, 0) for r in rows) for k in ("input", "cached", "cache_write", "output")}


# ------------------------------------------------------------------------- arms
class Arm:
    name = "?"

    def __init__(self, job: Job):
        self.job, self.dir = job, job.dir / self.name
        self.proxy = CaptureProxy(self.dir / "requests.jsonl.gz", self.name, keep_bodies=self.name == "A")
        self.d = None
        self.turn = 0
        self.advances = 0
        self.restarts = 0
        self.clock_marks = []  # (real time, clock offset from then on): one per advance
        self.conversations = 0
        self.chat_key = self._chat_key()
        self.cold_next = True  # the next turn opens cold (first on its chat key, or after a restart)
        self.last_end_vt = None  # virtual time the previous turn on this chat key ended

    def _chat_key(self) -> str:
        return f"{self.job.scenario['id']}-rep{self.job.rep}-c{self.conversations}"

    @property
    def offset_s(self) -> int:
        return self.clock_marks[-1][1] if self.clock_marks else 0

    def vnow(self) -> float:
        return time.time() + self.offset_s

    def start(self):
        if self.dir.exists():
            shutil.rmtree(self.dir)
        self.proxy.start()
        self.proxy.mark(turn=0, phase="setup")
        self.d = Nimbusd(self.dir / "data", self.job.keys, self.dir / "nimbusd.log", capture_url=self.proxy.url())
        self.d.start(self.job.head)
        self.d.configure(self.job.head, self.job.model, self.job.sub_model)
        self.d.wait_verified(self.job.head)

    def seed(self):
        seed = self.job.scenario.get("seed") or {}
        for f in seed.get("files", []):
            text = fixtures.text(f["fixture"]) if "fixture" in f else f["text"]
            out, err = self.d.call_tool("artifact.save", {"project": f["project"], "name": f["name"], "text": text})
            if err:
                raise RuntimeError(f"seed file {f['project']}/{f['name']}: {out}")
        for fact in seed.get("memories", []):
            out, err = self.d.call_tool("memory.write", {"content": fact})
            if err:
                raise RuntimeError(f"seed memory: {out}")
        for skill in seed.get("skills", []):  # the owner's own save: live at once, read through the index
            body = self.d.post_form("/api/skills/save", {"id": skill["id"], "md": skill["md"]})
            if '"ok":false' in body.replace(" ", ""):
                raise RuntimeError(f"seed skill {skill['id']}: {body[:200]}")

    def close(self):
        if self.d:
            self.d.stop()
        self.proxy.stop()
        shutil.rmtree(self.dir / "data", ignore_errors=True)  # the instance volume is not evidence

    def step(self, step: dict) -> dict:
        if "restart" in step:
            return self._restart_step()
        if "new_conversation" in step:
            self.conversations += 1
            self.chat_key, self.cold_next, self.last_end_vt = self._chat_key(), True, None
            self.new_conversation()
            return {"kind": "new_conversation", "chat_key": self.chat_key}
        if "advance" in step:
            return self.advance(step["advance"])
        self.turn += 1
        self.proxy.mark(turn=self.turn)
        texts = step["burst"] if "burst" in step else [step["say"]]
        gap = None if self.last_end_vt is None else round(self.vnow() - self.last_end_vt)
        rec = {
            "kind": "burst" if "burst" in step else "say",
            "turn": self.turn,
            "user": texts,
            "photos": step.get("photos", []),
            "chat_key": self.chat_key,
            "gap_s": gap,
            "cold_transition": self.cold_next or (gap is not None and gap > COLD_TURN_GAP_S),
            "vt": round(self.vnow()),  # the arm's virtual time as the turn starts ({RD} in a check)
        }
        started = time.time()
        try:
            self.speak(texts, step, rec)
        except Exception as e:  # a failed turn is a result, not the end of the run
            rec.setdefault("replies", [])
            rec["error"] = f"{type(e).__name__}: {e}"[:600]
        rec.setdefault("latency_s", round(time.time() - started, 2))
        rec["reply"] = "\n".join(rec["replies"])
        self.cold_next, self.last_end_vt = False, self.vnow()
        rec.update(self.metrics(self.turn))
        if needs_state(step):
            rec["state"] = snapshot_state(self.d)
        return rec

    def _restart_step(self) -> dict:
        """A restart: the daemon comes back on the same clock (its own idle step runs then), and the
        next turn opens cold. Requests made meanwhile are this step's, not the previous turn's."""
        self.restarts += 1
        self.proxy.mark(restart=self.restarts)
        self.restart()
        self.cold_next = True
        self._settle()
        return {"kind": "restart", **self._metrics(self.proxy.rows(restart=self.restarts))}

    def advance(self, value) -> dict:
        """Move this arm's clock (its daemon's, and arm B's stamps), then run the daemon's idle work."""
        self.advances += 1
        self.proxy.mark(advance=self.advances)
        before = self.d.clock()
        target = advance_target(value, before["epoch"], RIG_TZ)
        seconds = max(target - before["epoch"], 0)
        started = time.time()
        # Marked before the move: a request the idle step makes is already on the new clock.
        self.clock_marks.append((started, before["offset_s"] + seconds))
        try:
            # A wall time lands exactly (to_epoch); a duration moves by its length.
            after = self.d.advance_to(target) if parse_advance(value)[0] == "wall" else self.d.advance(seconds)
        except Exception:
            self.clock_marks.pop()  # the clock did not move (or not knowably): price as if it had not
            raise
        self.clock_marks[-1] = (started, after["offset_s"])
        seconds = after["offset_s"] - before["offset_s"]
        self.clock_moved()
        ticks = self.d.tick_idle()
        self._settle()
        now = self.d.clock()
        rec = {
            "kind": "advance",
            "advance": value,
            "advanced_s": seconds,
            "epoch": now["epoch"],
            "local": datetime.fromtimestamp(now["epoch"], ZoneInfo(RIG_TZ)).strftime("%a %d %b %Y %H:%M"),
            "idle_steps": 1 + ticks,
            "latency_s": round(time.time() - started, 2),
        }
        rec.update(self._metrics(self.proxy.rows(advance=self.advances)))
        return rec

    def clock_moved(self):
        """The arm's own side of an advance (arm B's stamps). The daemon has already moved."""

    def _settle(self):
        """Wait until the engine is idle, so work a step leaves behind is counted on that step."""
        end, quiet = time.time() + SETTLE_MAX_S, 0
        while time.time() < end and quiet < 4:
            status, body = self.d.call("GET", "/api/chat?watch=1")
            busy = json.loads(body).get("busy", 1) if status == 200 else 1
            quiet = quiet + 1 if not busy else 0
            time.sleep(SETTLE_POLL_S)

    def metrics(self, turn: int) -> dict:
        """Provider requests, tokens and cost of one turn, from the capture."""
        return self._metrics(self.proxy.rows(turn=turn))

    def _metrics(self, rows: list) -> dict:
        """Provider requests, tokens and cost of a step's rows. virtual_cost_usd prices each request on
        the virtual clock (costs.virtual_costs: cold after a gap over 300 s); cost_usd is what was billed."""
        head = [r for r in rows if r["path"] in HEAD_PATHS and r["model"] == self.job.model]
        head_seqs = {r["seq"] for r in head}
        other = [r for r in rows if r["seq"] not in head_seqs and r["method"] == "POST"]
        return {
            "head_requests": len(head),
            "other_requests": len(other),
            "other_models": sorted({r["model"] or r["path"] for r in other}),
            "tokens": _token_sums(head),
            "provider_ms": sum(r["ms"] for r in head),
            "failed_requests": sum(1 for r in rows if r["status"] >= 400),
            "cost_usd": round(sum(cost_usd(r["model"], r["usage"]) for r in rows), 6),
            **self._virtual(rows),
            "cold_cost_usd": round(sum(cold_cost_usd(r["model"], r["usage"]) for r in rows), 6),
        }

    def _virtual(self, rows: list) -> dict:
        """The step's cost on the virtual clock, and how many of its requests were priced cold."""
        priced = virtual_costs(self.proxy.rows(), self.clock_marks)
        return {
            "virtual_cost_usd": round(sum(priced[r["seq"]][0] for r in rows), 6),
            "cold_requests": sum(1 for r in rows if priced[r["seq"]][1]),
        }

    def final_state(self) -> dict:
        return snapshot_state(self.d)


class ArmA(Arm):
    """nimbusd's own harness over POST /api/chat, polled the way the page polls."""

    name = "A"

    def _replies_seq(self) -> int:
        return self.d.get_json("/api/replies?after=0").get("lastSeq", 0)

    def speak(self, texts: list, step: dict, rec: dict):
        att = []
        for n, name in enumerate(step.get("photos", [])):
            fname = f"att-{self.turn}-{n}.png"
            self.d.upload("chat", fname, fixtures.photo(name))
            att.append(f"chat/{fname}")
        seq0 = self._replies_seq()
        started = time.time()
        ids = []
        for n, text in enumerate(texts):
            form = {"text": text, **({"att": "\n".join(att)} if att and n == len(texts) - 1 else {})}
            status, body = self.d.call("POST", "/api/chat", form=form)
            if status != 200:
                raise RuntimeError(f"POST /api/chat answered {status}: {body[:200]}")
            ids.append(json.loads(body).get("turn"))
            if n < len(texts) - 1:
                time.sleep(step.get("gap_s", 0.4))
        rec["timed_out"] = not self._wait_turns(ids, started + step.get("timeout_s", TURN_TIMEOUT_S))
        rec["latency_s"] = round(time.time() - started, 2)
        self._settle()
        if spawned(self._head_bodies(keep=True)):
            # A sub-agent was started: its result is delivered later as one more message, and the
            # owner waits for it. Wait too, so the record holds what the owner finally got.
            rec["awaited_s"] = self._await_more(seq0, len(self._chat_rows(seq0)))
        rec["replies"], rec["notices"] = split_notices(self._chat_rows(seq0))
        bodies = self._head_bodies()
        rec["tools"], rec["harness"] = harness_calls(bodies)
        rec["recalled"] = recalled_memories(bodies)
        (self.dir / f"lastturn-{self.turn:02d}.txt").write_text(self.d.get_text("/api/lastturn"))

    def _head_bodies(self, keep: bool = False) -> list:
        return [b for b in self.proxy.take_bodies(keep=keep, turn=self.turn) if b["path"] in HEAD_PATHS]

    def _chat_rows(self, seq0: int) -> list:
        rows = self.d.get_json(f"/api/replies?after={seq0}").get("replies", [])
        return [r.get("text", "") for r in rows if r.get("role") == "assistant" and r.get("text")]

    def _await_more(self, seq0: int, have: int) -> float:
        """Wait for one more chat row than `have` (the sub-agent's result), then for the engine to go idle."""
        started = time.time()
        while time.time() - started < ASYNC_MAX_S and len(self._chat_rows(seq0)) <= have:
            time.sleep(1)
        self._settle()
        return round(time.time() - started, 1)

    def _wait_turns(self, ids: list, deadline: float) -> bool:
        """Poll each web turn's own reply until every one is over. False when time runs out."""
        pending = list(ids)
        while pending and time.time() < deadline:
            status, body = self.d.call("GET", f"/api/chat?turn={pending[0]}")
            answer = json.loads(body) if status == 200 else {}
            if answer.get("done") or answer.get("gone"):
                pending.pop(0)
                continue
            time.sleep(0.1)
        return not pending

    def restart(self):
        (self.dir / f"api-log-before-restart-{self.turn:02d}.txt").write_text(self.d.get_text("/api/log"))
        self.d.restart()

    def new_conversation(self):
        self.d.post_form("/api/orch", {"clearConv": "1"})

    def close(self):
        if self.d and self.d.proc:
            (self.dir / "api-log.txt").write_text(self.d.get_text("/api/log"))
        super().close()


class ArmB(Arm):
    """The reference loop, with the same model, on the tools of its own nimbusd."""

    name = "B"

    def start(self):
        super().start()
        self._connect()
        self.conv = self._conversation(load=False)

    def _connect(self):
        """The provider client and the daemon's tools (its port changes on a restart)."""
        key = self.job.env.get(PROVIDER_ENV[self.job.head], "")
        kw = {"effort": self.job.effort} if self.job.effort else {}
        self.provider = refloop.provider_for(
            self.job.model, api_key=key, base_url=self.proxy.url(PROVIDER_HOST[self.job.head]), **kw
        )
        self.tools = refloop.McpTools(f"http://127.0.0.1:{self.d.port}", self.d.token)

    def _conversation(self, load: bool):
        """This chat key's conversation: reloaded from its saved transcript, or a new one on the arm's clock."""
        path = self.dir / f"transcript-{self.conversations}.json"
        if load:
            return refloop.Conversation.load(path)
        return refloop.Conversation(
            self.tools.specs(), path, note=self.job.loop_note, chat_key=self.chat_key, clock_offset_s=self.offset_s
        )

    def clock_moved(self):
        self.conv.advance(self.offset_s - self.conv.clock_offset_s)

    def speak(self, texts: list, step: dict, rec: dict):
        rec["tools"] = []

        def on_tool(name, args, text, is_error):
            rec["tools"].append({"name": name, "args": args, "result": text[:RESULT_KEEP], "is_error": is_error})

        images = [("image/png", base64.standard_b64encode(fixtures.photo(n)).decode()) for n in step.get("photos", [])]
        started = time.time()
        if len(texts) == 1:
            rec["replies"] = [refloop.run_turn(self.conv, self.provider, self.tools, texts, images, on_tool)]
        else:
            rec["replies"] = self._burst(texts, step.get("gap_s", 0.4), on_tool)
        rec["latency_s"] = round(time.time() - started, 2)

    def _burst(self, texts: list, gap_s: float, on_tool) -> list:
        """Messages arrive gap_s apart while the loop's channel side (refloop.serve) is running."""
        inbox, replies = queue.Queue(), []
        worker = threading.Thread(
            target=refloop.serve,
            daemon=True,
            args=(self.conv, self.provider, self.tools, inbox, replies.append, on_tool),
        )
        worker.start()
        for n, text in enumerate(texts):
            inbox.put(text)
            if n < len(texts) - 1:
                time.sleep(gap_s)
        inbox.put(None)
        worker.join(timeout=TURN_TIMEOUT_S)
        return replies

    def restart(self):
        """nimbusd restarts and the loop's process state is thrown away: only the saved transcript survives."""
        self.d.restart()
        self._connect()
        self.conv = self._conversation(load=True)

    def new_conversation(self):
        self.conv = self._conversation(load=False)


class DoubledAppend:
    """The tools of arm B with one fault: every append is written twice (the model sees the first
    result only). The counter-test arm: a suite that cannot see a doubled write is no gate."""

    APPENDS = {"files_append", "append_file"}

    def __init__(self, tools):
        self.inner = tools

    def specs(self) -> list:
        return self.inner.specs()

    def call(self, name: str, args: dict) -> tuple:
        first = self.inner.call(name, args)
        if name in self.APPENDS and not first[1]:
            self.inner.call(name, args)
        return first


class ArmX(ArmB):
    """Counter-test arm (RB-REC7): the reference loop whose appends write twice. Never a contender."""

    name = "X"

    def _connect(self):
        super()._connect()
        self.tools = DoubledAppend(self.tools)


ARMS = {"A": ArmA, "B": ArmB, "X": ArmX}
