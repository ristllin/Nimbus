#!/usr/bin/env python3
"""arms - the two arms of the comparison, behind one small interface.

  ArmA  nimbusd's own harness, driven over its chat API the way the web page drives it.
  ArmB  the reference loop (refloop.py) with the same model, calling the tools of its own
        nimbusd over MCP, on a separate data dir seeded the same way.

Both arms send every provider request through a capture proxy, so each step's record
holds the same things, measured the same way: the replies, the latency, the provider
requests, the tokens and cost, and every tool call with its result.
"""

from __future__ import annotations

import base64
import json
import queue
import shutil
import threading
import time
import urllib.parse
from dataclasses import dataclass, field
from pathlib import Path

import fixtures
import refloop
from capture_proxy import CaptureProxy
from costs import cold_cost_usd, cost_usd
from nimbusd_ctl import PROVIDER_ENV, Nimbusd
from scenario import needs_state

HEAD_PATHS = ("/v1/messages", "/v1/responses")
PROVIDER_HOST = {"anthropic": "api.anthropic.com", "openai": "api.openai.com/v1"}
TURN_TIMEOUT_S = 300
SETTLE_MAX_S = 90
RESULT_KEEP = 4000          # characters of a tool result kept in a step record (the capture has it whole)


@dataclass
class Job:
    """One scenario repetition on one model: everything an arm needs."""
    model: str
    head: str                    # provider slug: anthropic or openai
    effort: str | None           # what the harness sends for this model, mirrored by the reference loop
    sub_model: str
    scenario: dict
    rep: int
    dir: Path
    env: dict = field(repr=False, default_factory=dict)
    loop_note: str = ""          # an experiment: one extra line on the reference loop's system prompt

    @property
    def keys(self) -> dict:
        return {name: self.env.get(name, "") for name in PROVIDER_ENV.values()}


# ------------------------------------------------------------------ wire parsing
def _tool_uses(response) -> list:
    """(call id, name, arguments) of every tool call in one provider response, either wire."""
    if not isinstance(response, dict):
        return []
    out = [(b.get("id"), b.get("name"), b.get("input") or {})
           for b in response.get("content") or [] if isinstance(b, dict) and b.get("type") == "tool_use"]
    for item in response.get("output") or []:
        if isinstance(item, dict) and item.get("type") == "function_call":
            try:
                args = json.loads(item.get("arguments") or "{}")
            except ValueError:
                args = {"_raw": item.get("arguments")}
            out.append((item.get("call_id"), item.get("name"), args))
    return out


def _result_text(content) -> str:
    if isinstance(content, str):
        return content
    return "\n".join(b.get("text", "") for b in content or [] if isinstance(b, dict) and b.get("type") == "text")


def _tool_results(request) -> list:
    """(call id, text) of every tool result a request carries, either wire."""
    if not isinstance(request, dict):
        return []
    out = []
    for m in request.get("messages") or []:
        for b in (m.get("content") if isinstance(m.get("content"), list) else []):
            if isinstance(b, dict) and b.get("type") == "tool_result":
                out.append((b.get("tool_use_id"), _result_text(b.get("content"))))
    for item in request.get("input") if isinstance(request.get("input"), list) else []:
        if isinstance(item, dict) and item.get("type") == "function_call_output":
            out.append((item.get("call_id"), _result_text(item.get("output"))))
    return out


# orch_turn reply fields that act like a tool call, and the tool each stands for.
FIELD_TOOLS = {"mem_write": "memory_write", "mem_query": "memory_search", "session_ops": "session_spawn"}


def harness_calls(exchanges: list) -> tuple:
    """One harness turn's tool calls and reply fields, read from its captured head exchanges.

    Returns (tools, harness): tools is the registry calls in order with their results, plus one
    entry per acting reply field (a mem_write item is a memory write the harness performs);
    harness holds the final orch_turn fields and any orch_turn the harness sent back.
    """
    calls, by_id = [], {}
    for ex in exchanges:
        for cid, text in _tool_results(ex.get("request")):
            if cid in by_id and "result" not in by_id[cid]:
                by_id[cid]["result"] = text
        for cid, name, args in _tool_uses(ex.get("response")):
            by_id[cid] = {"name": name, "args": args}
            calls.append(by_id[cid])
    finals = [c for c in calls if c["name"] == "orch_turn"]
    tools = [c for c in calls if c["name"] != "orch_turn"]
    fields = {k: v for k, v in (finals[-1]["args"] if finals else {}).items() if k != "reply" and v}
    for name, tool in FIELD_TOOLS.items():
        for item in fields.get(name) or []:
            tools.append({"name": tool, "args": item if isinstance(item, dict) else {"query": item},
                          "result": f"(reply field {name}, applied by the harness after the turn)", "via": name})
    bounced = [c["result"][:600] for c in finals if c.get("result")]
    return _clip(tools), {"fields": fields, "bounced": bounced, "orch_turn_calls": len(finals)}


def _clip(tools: list) -> list:
    for t in tools:
        t["result"] = (t.get("result") or "")[:RESULT_KEEP]
    return tools


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
    return {"files": files, "loops": d.get_json("/api/loops"), "memories": memories, "rules": rules,
            "scratchpad": scratch if status == 200 else ""}


# ------------------------------------------------------------------------- arms
class Arm:
    name = "?"

    def __init__(self, job: Job):
        self.job, self.dir = job, job.dir / self.name
        self.proxy = CaptureProxy(self.dir / "requests.jsonl.gz", self.name, keep_bodies=self.name == "A")
        self.d = None
        self.turn = 0

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

    def close(self):
        if self.d:
            self.d.stop()
        self.proxy.stop()
        shutil.rmtree(self.dir / "data", ignore_errors=True)   # the instance volume is not evidence

    def step(self, step: dict) -> dict:
        if "restart" in step:
            self.restart()
            return {"kind": "restart"}
        if "new_conversation" in step:
            self.new_conversation()
            return {"kind": "new_conversation"}
        self.turn += 1
        self.proxy.mark(turn=self.turn)
        texts = step["burst"] if "burst" in step else [step["say"]]
        rec = {"kind": "burst" if "burst" in step else "say", "turn": self.turn, "user": texts,
               "photos": step.get("photos", [])}
        started = time.time()
        try:
            self.speak(texts, step, rec)
        except Exception as e:      # a failed turn is a result, not the end of the run
            rec.setdefault("replies", [])
            rec["error"] = f"{type(e).__name__}: {e}"[:600]
        rec.setdefault("latency_s", round(time.time() - started, 2))
        rec["reply"] = "\n".join(rec["replies"])
        rec.update(self.metrics(self.turn))
        if needs_state(step):
            rec["state"] = snapshot_state(self.d)
        return rec

    def metrics(self, turn: int) -> dict:
        """Provider requests, tokens and cost of one turn, from the capture."""
        rows = self.proxy.rows(turn=turn)
        head = [r for r in rows if r["path"] in HEAD_PATHS and r["model"] == self.job.model]
        head_seqs = {r["seq"] for r in head}
        other = [r for r in rows if r["seq"] not in head_seqs and r["method"] == "POST"]
        tokens = {k: sum((r["usage"] or {}).get(k, 0) for r in head) for k in ("input", "cached", "cache_write", "output")}
        return {
            "head_requests": len(head), "other_requests": len(other),
            "other_models": sorted({r["model"] or r["path"] for r in other}),
            "tokens": tokens, "provider_ms": sum(r["ms"] for r in head),
            "failed_requests": sum(1 for r in rows if r["status"] >= 400),
            "cost_usd": round(sum(cost_usd(r["model"], r["usage"]) for r in rows), 6),
            "cold_cost_usd": round(sum(cold_cost_usd(r["model"], r["usage"]) for r in rows), 6),
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
        rows = self.d.get_json(f"/api/replies?after={seq0}").get("replies", [])
        rec["replies"] = [r.get("text", "") for r in rows if r.get("role") == "assistant" and r.get("text")]
        rec["tools"], rec["harness"] = harness_calls(
            [b for b in self.proxy.take_bodies(turn=self.turn) if b["path"] in HEAD_PATHS])
        (self.dir / f"lastturn-{self.turn:02d}.txt").write_text(self.d.get_text("/api/lastturn"))

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

    def _settle(self):
        """Wait until the engine is idle, so work a turn leaves behind is counted on that turn."""
        end, quiet = time.time() + SETTLE_MAX_S, 0
        while time.time() < end and quiet < 4:
            status, body = self.d.call("GET", "/api/chat?watch=1")
            busy = json.loads(body).get("busy", 1) if status == 200 else 1
            quiet = quiet + 1 if not busy else 0
            time.sleep(0.25)

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
        self.conversations = 0
        self._open(load=False)

    def _open(self, load: bool):
        key = self.job.env.get(PROVIDER_ENV[self.job.head], "")
        kw = {"effort": self.job.effort} if self.job.effort else {}
        self.provider = refloop.provider_for(self.job.model, api_key=key,
                                             base_url=self.proxy.url(PROVIDER_HOST[self.job.head]), **kw)
        self.tools = refloop.McpTools(f"http://127.0.0.1:{self.d.port}", self.d.token)
        path = self.dir / f"transcript-{self.conversations}.json"
        self.conv = refloop.Conversation.load(path) if load else refloop.Conversation(
            self.tools.specs(), path, note=self.job.loop_note)

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
        worker = threading.Thread(target=refloop.serve, daemon=True,
                                  args=(self.conv, self.provider, self.tools, inbox, replies.append, on_tool))
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
        self._open(load=True)

    def new_conversation(self):
        self.conversations += 1
        self._open(load=False)


ARMS = {"A": ArmA, "B": ArmB}
