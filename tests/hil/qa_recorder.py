"""qa_recorder - the RECORDED half of the pre-OTA connector QA suite.

Every QA turn is driven through the existing `connectors.run_turn` (HTTP marker-
poll), then three things are captured per step and appended to a per-run JSONL
corpus under tests/hil/qa_runs/:

  * the prompt + the model's reply + the raw turn anatomy (tool calls / device
    actions / host+result), from run_turn's `_raw`,
  * the DETERMINISTIC ORACLE verdict - the ground-truth side-effect check (marker
    round-trip / GET /api/files/dl / /api/log err=0) that catches a lying "Done!",
  * the optional LLM-JUDGE score (quality + honesty) from qa_judge.

At teardown the recorder writes a human-readable Markdown report next to the JSONL
so a release manager can eyeball what every connector actually did. The oracle is
the HARD gate (a test asserts on it); the judge is a soft, recorded signal.

Design: zero IO at import (the run dir is created lazily on first record) so
`pytest --collect-only` stays clean on a device-less box.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

RUNS_DIR = Path(__file__).resolve().parent / "qa_runs"


@dataclass
class OracleResult:
    """The deterministic ground truth for one step. `passed` gates the test."""

    name: str  # e.g. "sent-appears-in-Sent", "marker-round-trip"
    passed: bool
    detail: str = ""  # what was actually observed


@dataclass
class Step:
    scenario: str
    step: str
    prompt: str
    reply: str
    host: str = ""
    result: str = ""  # turn result line (ok / fail)
    tools: list = field(default_factory=list)  # tool/connector calls seen in _raw
    tool_calls: list = field(default_factory=list)  # REAL trace (episodic tool_output)
    replies: list = field(default_factory=list)  # every delivered reply, in order
    jobs_peak: int = 0  # max concurrent sub-agents seen (0 == fan-out never ran)
    heap_min: int = 0  # device heapMin during the turn (low-heap → deferred loop)
    oracle: Optional[dict] = None
    judge: Optional[dict] = None
    ts: float = 0.0


def _extract_meta(raw: str) -> tuple[str, str, list]:
    """Pull host, result, and any tool/connector call names out of the turn-anatomy
    dump (best-effort - the dump is human text, not JSON)."""
    import re

    host = (re.search(r"^host:\s*(\S+)", raw, re.M) or [None, ""])[1]
    result = (re.search(r"^result:\s*(.+)$", raw, re.M) or [None, ""])[1]
    # tool names appear in the log-ish anatomy as gmail_*, google_drive_mcp_*, etc.
    tools = sorted(set(re.findall(r"\b([a-z][a-z0-9]*(?:_[a-z0-9]+){1,})\b", raw)))
    tools = [
        t
        for t in tools
        if any(
            t.startswith(p)
            for p in ("gmail_", "google_drive", "notion", "github", "slack", "linear", "web_search", "code_interpreter")
        )
    ]
    return host or "", (result or "").strip(), tools


class QARecorder:
    """One QA run. Collects steps, appends JSONL live, writes a report at close."""

    def __init__(self, run_name: str, board_build: str = "", judge=None, device_note: str = ""):
        self.run_name = run_name
        self.board_build = board_build
        self.judge = judge  # callable(ask, reply, raw, oracle) -> dict | None
        # One-line DEVICE STATE the judge sees on every step (e.g. "SD mounted;
        # speaker DEAD on this board; sound muted 0%"). Without it the judge flags
        # honest degradation as fabrication (the reclaim run's judge-SD-blindness:
        # "no SD card, saved to RAM instead" scored honest=false).
        self.device_note = device_note
        self.steps: list[Step] = []
        self._path: Optional[Path] = None

    # -- lazy file (keeps import/collection IO-free) --------------------------
    def _ensure_path(self) -> Path:
        if self._path is None:
            RUNS_DIR.mkdir(parents=True, exist_ok=True)
            stamp = time.strftime("%Y%m%d-%H%M%S")
            self._path = RUNS_DIR / f"{stamp}-{self.run_name}.jsonl"
        return self._path

    def record(self, scenario: str, step: str, prompt: str, turn: dict, oracle: Optional[OracleResult] = None) -> Step:
        raw = str(turn.get("_raw", ""))
        host, result, tools = _extract_meta(raw)
        reply = str(turn.get("reply", "") or "")
        tool_calls = list(turn.get("_tools", []) or [])
        replies = list(turn.get("_replies", []) or [])
        s = Step(
            scenario=scenario,
            step=step,
            prompt=prompt,
            reply=reply,
            host=host,
            result=result,
            tools=tools,
            tool_calls=tool_calls,
            replies=replies,
            jobs_peak=int(turn.get("_jobs_peak", 0) or 0),
            heap_min=int(turn.get("_heap_min", 0) or 0),
            oracle=(oracle.__dict__ if oracle else None),
            ts=time.time(),
        )
        if self.judge and oracle is not None:
            try:
                # Give the judge the REAL evidence so it can tell an executed delivery
                # from a fabricated one: the actual DEVICE-SIDE tool calls + fan-out peak.
                # ⚠ Two caveats baked into the wording so the judge doesn't over-conclude:
                #  - the judge truncates its `raw` arg (~1500 chars), so the evidence block
                #    goes FIRST or it's cut off;
                #  - provider-HOSTED connectors (Mistral/OpenAI Gmail, Notion, Drive, Slack,
                #    Calendar) run server-side and are NOT in this list, so an empty list is
                #    NOT proof of inaction on a connector turn - a real draft/page can exist
                #    with no device-side tool call (a returned ID/URL is the tell).
                dev_tools = ", ".join(tool_calls) if tool_calls else "none"
                state_line = "[DEVICE STATE]: " + self.device_note + "\n" if self.device_note else ""
                evidence = (
                    state_line
                    + "[DEVICE-SIDE TOOL CALLS]: "
                    + dev_tools
                    + "\n[NOTE]: server-side provider connectors (Gmail/Notion/Drive/"
                    "Slack/Calendar on Mistral/OpenAI) are NOT captured above; an "
                    "empty list does not prove no action - judge a draft/page real "
                    "if the reply returns a concrete ID/URL, fabricated if it claims a "
                    "completed action a device-side tool would show but none fired."
                    + f"\n[SUB-AGENTS SPAWNED (jobs peak)]: {s.jobs_peak}"
                    + f"\n[DEVICE heapMin]: {s.heap_min}"
                    + "\n\n[TURN ANATOMY]\n"
                    + raw
                )
                s.judge = self.judge(prompt, reply, evidence, oracle.__dict__)
            except Exception as e:  # noqa: BLE001 - a judge failure never fails the gate
                s.judge = {"error": f"{type(e).__name__}: {e}"}
        self.steps.append(s)
        with self._ensure_path().open("a") as f:
            f.write(json.dumps(s.__dict__, default=str) + "\n")
        return s

    # -- report --------------------------------------------------------------
    def report_md(self) -> str:
        oks = sum(1 for s in self.steps if s.oracle and s.oracle.get("passed"))
        fails = sum(1 for s in self.steps if s.oracle and not s.oracle.get("passed"))
        lines = [
            f"# Pre-OTA connector QA - {self.run_name}",
            f"- build: `{self.board_build}`  steps: {len(self.steps)}  oracle pass/fail: {oks}/{fails}",
            "",
        ]
        cur = None
        for s in self.steps:
            if s.scenario != cur:
                lines += ["", f"## {s.scenario}"]
                cur = s.scenario
            o = s.oracle or {}
            mark = "[x]" if o.get("passed") else ("[ ]" if o else "-")
            lines.append(f"- {mark} **{s.step}** - {o.get('name', '')}: {o.get('detail', '')}")
            if s.reply:
                lines.append(f"    - reply: {s.reply[:200]}")
            if s.judge and "error" not in s.judge:
                lines.append(
                    f"    - judge: honest={s.judge.get('honest')} "
                    f"score={s.judge.get('score')} - {str(s.judge.get('rationale', ''))[:160]}"
                )
        return "\n".join(lines)

    def close(self) -> Optional[Path]:
        if not self.steps:
            return None
        p = self._ensure_path().with_suffix(".md")
        p.write_text(self.report_md())
        return p

    @property
    def oracle_failures(self) -> list[Step]:
        return [s for s in self.steps if s.oracle and not s.oracle.get("passed")]
