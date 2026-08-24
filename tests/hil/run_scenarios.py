#!/usr/bin/env python3
"""run_scenarios - execute the complex real-world scenario suite on a live device.

This is the runner the plan promised: it drives each multi-turn scenario against
Board 1 over HTTP, captures the glass box, checks a deterministic oracle, scores
an independent LLM judge, and records a JSONL + Markdown corpus.

It bakes in the four apparatus corrections prism flagged as blockers in the draft:
  (a) PREFLIGHT that ABORTS (not warns): build tag, SD present, a live web.search
      canary, and a judge canary - a stale build or a dead judge must never be
      allowed to produce a "baseline".
  (b) The per-turn reply comes from the REAL delivered channel (GET /api/chat for
      chat "web"), and the tool/sub-agent trace from /api/mem/episodic - NOT from
      /api/lastturn, which is a single global slot that excludes tool-loop rounds
      and is overwritten by the synthesis turns fan-out scenarios generate. The
      structural fan-out signal comes from /api/orch jobs[].
  (c) Everything runs on chat "web" (hardcoded Admin, real reply channel) with an
      EXPLICIT per-scenario reset, because fresh chatIds resolve to Role::Unknown
      which is denied every tool.
  (d) Each scenario runs N times and is reported as a pass RATE, so variance in a
      nondeterministic turn can't masquerade as a regression.

Usage:
    NIMBUS_TEST_IP=192.0.2.10 NIMBUS_TEST_TOKEN=... \
      python3 tests/hil/run_scenarios.py [--only id,id] [--cat multi-subsession] \
        [--reps 3] [--dry] [--needs-none]

Keys for the judge come from the secrets .env via qa_judge.make_judge.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
import uuid
from dataclasses import dataclass, field
from typing import Optional

try:
    import requests
except ImportError:
    sys.exit("pip3 install requests")

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qa_judge import make_judge  # noqa: E402
from qa_recorder import QARecorder, OracleResult  # noqa: E402


# --------------------------------------------------------------------------- net
class Dev:
    """Thin HTTP client for one board. Every call carries the auth token."""

    def __init__(self, ip: str, tok: str):
        self.ip, self.tok = ip, tok
        self.s = requests.Session()

    def _u(self, path: str) -> str:
        sep = "&" if "?" in path else "?"
        return f"http://{self.ip}{path}{sep}t={self.tok}"

    def get(self, path: str, timeout=10):
        return self.s.get(self._u(path), timeout=timeout)

    def post(self, path: str, data=None, timeout=15):
        return self.s.post(self._u(path), data=data or {}, timeout=timeout)

    def state(self) -> dict:
        try:
            return self.get("/api/state", timeout=8).json()
        except Exception:
            return {}

    def orch(self) -> dict:
        try:
            return self.get("/api/orch", timeout=8).json()
        except Exception:
            return {}

    def episodic(self, session="web", kind=None, limit=60) -> list:
        """Return the episodic messages. A 200 with no rows returns []; a transient
        read failure is RETRIED then RAISED - never silently returned as [] (H5: an
        empty-on-error read either contaminates the tool-trace scope or fakes a
        no-tools verdict)."""
        p = f"/api/mem/episodic?session={session}&limit={limit}"
        if kind:
            p += f"&kind={kind}"
        last = ""
        for _ in range(3):
            try:
                r = self.get(p, timeout=12)
                if r.status_code == 200:
                    return r.json().get("messages", [])
                last = f"http {r.status_code}"
            except Exception as e:  # noqa: BLE001
                last = f"{type(e).__name__}: {e}"
            time.sleep(1.0)
        raise RuntimeError(f"episodic read failed after retries: {last}")


# ---------------------------------------------------------------- turn execution
class RebootDuringTurn(RuntimeError):
    pass


class TurnTimeout(RuntimeError):
    pass


@dataclass
class Bundle:
    """Everything one scenario's turns produced - the oracle's input."""

    prompts: list = field(default_factory=list)
    replies: list = field(default_factory=list)  # every delivered reply, in order (flat)
    turns_out: list = field(default_factory=list)  # per-turn: list of [replies] for that turn
    tool_rows: list = field(default_factory=list)  # episodic kind=tool_output
    log_rows: list = field(default_factory=list)  # episodic kind=log (spawn/subresult)
    jobs_peak: int = 0  # max concurrent jobs seen
    jobs_seen: int = 0  # distinct jobs dispatched
    raw: str = ""  # /api/lastturn (corpus artifact only)
    heap_min: int = 0

    @property
    def reply(self) -> str:
        return self.replies[-1] if self.replies else ""

    def tool_names(self) -> list:
        out = []
        for r in self.tool_rows:
            t = r.get("text") or ""
            # rows are "name(args) -> output"
            name = t.split("(")[0].strip()
            if name:
                out.append(name)
        return out

    def called(self, name: str) -> bool:
        n = name.replace(".", "_")
        return any(t.replace(".", "_").startswith(n) for t in self.tool_names())


TURN_TIMEOUT = 180  # a healthy turn replies fast OR shows jobs[] (which extends);
POLL = 4  # no reply AND no jobs by here == a wedged turn task (reboot to clear)
MEMCLEAR = True  # per-scenario memClear in reset (set False via --no-memclear). SD-absent,
# memClear wedges the turn task, and a reboot to recover disrupts WiFi -> the
# next turn wedges (rejoin-window). On a degraded (no-SD) board, isolate with
# convReset only and keep WiFi stable (no --reboot-each).


def _drain_reply(dev: Dev) -> str:
    try:
        j = dev.get("/api/chat", timeout=8).json()
        return "" if j.get("pending") else str(j.get("reply", "") or "")
    except Exception:
        return ""


def run_turn(dev: Dev, prompt: str, *, timeout=None, collect_late=8.0) -> tuple:
    """Fire one turn on chat 'web'; return (reply, all_replies_after_this_turn).

    Reads the REAL delivered reply from GET /api/chat (not /api/lastturn). After
    the first reply, keeps draining for `collect_late` seconds so a fan-out's
    later synthesis delivery is captured too. `timeout=None` resolves to the module
    TURN_TIMEOUT (settable via --turn-timeout) at call time."""
    if timeout is None:
        timeout = TURN_TIMEOUT
    _drain_reply(dev)  # clear any stale reply first
    marker = f"m{uuid.uuid4().hex[:6]}"
    r = dev.post("/api/chat", {"text": f"[{marker}] {prompt}"}, timeout=12)
    if r.status_code == 503:
        # busy - our own fan-out generates this; back off and retry once
        time.sleep(8)
        r = dev.post("/api/chat", {"text": f"[{marker}] {prompt}"}, timeout=12)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}: {r.text[:160]}"

    replies, deadline = [], time.time() + timeout
    hard_deadline = time.time() + max(timeout, 420)  # cap even if jobs never drain
    while time.time() < deadline:
        time.sleep(POLL)
        rep = _drain_reply(dev)
        if rep:
            # v4.0.0 mid-turn failover: the switch NOTICE ("<host> hit trouble
            # mid-task - switching to <host>") is a mid-turn status message, not
            # the answer. Capturing it as the terminal reply failed scenarios
            # whose real reply landed seconds later (seen live: buyers-guide,
            # steer rep2). Keep it, but keep waiting for the actual reply.
            if "switching to" in rep and "hit trouble" in rep:
                replies.append(rep)
                deadline = min(hard_deadline, time.time() + 120)
                continue
            replies.append(rep)
            break
        # A fan-out that doesn't ACK early replies only when its sub-agents finish
        # - which can exceed `timeout`. As long as the job table shows active work
        # (or heap is churning), the device IS working: extend rather than fail.
        try:
            if _jobs(dev) and time.time() < hard_deadline:
                deadline = min(hard_deadline, time.time() + 90)
        except Exception:
            pass
    if not replies:
        raise TurnTimeout(f"turn timed out (marker {marker})")
    # keep collecting late deliveries (fan-out synthesis, follow-up sends), but cap the
    # total late window so a chatty stream of sends can't extend it indefinitely (L3).
    late_end = time.time() + collect_late
    late_hard = time.time() + 90
    while time.time() < late_end and time.time() < late_hard:
        time.sleep(POLL)
        rep = _drain_reply(dev)
        if rep:
            replies.append(rep)
            late_end = min(late_hard, time.time() + collect_late)  # extend on activity, capped
    return replies[0], replies


_TERMINAL_JOB = ("done", "error", "complete", "completed", "failed", "terminated", "killed", "cancelled", "canceled")


def _jobs(dev: Dev) -> list:
    """Only NON-terminal sub-agents (M5: sessionsJson emits Done/Error sessions too,
    which inflated jobs_peak toward a false fan-out pass and kept the drain-wait from
    ever seeing an empty table - burning the full cap on every fan-out).

    ⚠ /api/orch 'jobs' is the sessionsJson SNAPSHOT, rebuilt on the tg_poll task - the
    SAME task that is busy executing the fan-out turn we're waiting on. So mid-turn the
    snapshot reads EMPTY and the wait-extension never fires: every fan-out first-turn
    then times out at TURN_TIMEOUT with jobs_peak=0 (observed live 2026-08-06 on the
    fabric build - a false 'wedge' that is really a stale read). /api/state 'jobs' is the
    router's LIVE count (jobCount() under the config lock) and stays accurate mid-turn,
    so FLOOR the result on it - active fan-out work must never be invisible to the wait."""
    o = dev.orch()
    js = o.get("jobs") or o.get("sessions") or []
    live = [j for j in js if str(j.get("status") or j.get("state") or "").lower() not in _TERMINAL_JOB]
    try:
        cnt = int(dev.state().get("jobs") or 0)
    except Exception:
        cnt = 0
    if cnt > len(live):  # snapshot is stale (turn task busy) - trust the live count
        live = live + [{"status": "running", "_synth": True}] * (cnt - len(live))
    return live


def _epi_id(row: dict) -> int:
    """Episodic ids are hex ('m0000021a'), globally monotonic and LittleFS-persisted
    (they survive reboot), so they're the reliable turn-scoping key - device `ts` resets
    to 0 on the per-scenario reboot and would mis-order."""
    try:
        return int(str(row.get("id", "m0"))[1:], 16)
    except Exception:
        return 0


def _max_epi_id(dev: Dev) -> int:
    """Snapshot the newest episodic id so a scenario's tool trace can be scoped to
    ONLY its own rows - the episodic log accumulates across scenarios and memClear
    does NOT prune it, so an unscoped read pulls a PRIOR scenario's tool calls into
    this oracle (a false side-effect fail, or worse a false honesty pass). Propagates
    a read error (H5) - a swallowed 0 here would drop the scope and pull EVERYTHING in."""
    return max((_epi_id(r) for r in dev.episodic(limit=5)), default=0)


def wait_for_jobs_and_synthesis(dev: Dev, b: Bundle, *, cap=340, quiet=18):
    """After a fan-out turn the head returns an ACK ('On it.') and the real
    synthesis is DELIVERED minutes later when the sub-agents finish. Poll the job
    table (sampling the true concurrency peak) AND keep draining the reply channel
    until jobs drain AND no new delivery for `quiet` seconds. Without this every
    sub-agent scenario false-fails on the bare ACK. Bounded by `cap` seconds."""
    deadline = time.time() + cap
    last_activity = time.time()
    saw_jobs = False
    while time.time() < deadline:
        jobs = _jobs(dev)
        if jobs:
            saw_jobs = True
            b.jobs_peak = max(b.jobs_peak, len(jobs))
            b.jobs_seen = max(b.jobs_seen, len(jobs))
            last_activity = time.time()
        rep = _drain_reply(dev)
        if rep:
            b.replies.append(rep)
            if b.turns_out:
                b.turns_out[-1].append(rep)
            last_activity = time.time()
        # done when no jobs running AND nothing new for `quiet`s (synthesis landed)
        if not jobs and (time.time() - last_activity) >= quiet:
            break
        time.sleep(5)
    return saw_jobs


def _await_device(dev: Dev, *, poll_max=130) -> bool:
    """Block until the board serves /api/state in Orchestrator mode again. Used
    after a wedged/panicked/rebooted turn so the NEXT scenario starts on a live
    device instead of cascading failures onto a board still rejoining Wi-Fi."""
    deadline = time.time() + poll_max
    while time.time() < deadline:
        st = dev.state()
        if st and st.get("fw") and st.get("mode") == 1:
            time.sleep(4)  # let web/orch tasks settle past first-serve
            return True
        time.sleep(4)
    return False


def reboot_and_wait(dev: Dev, *, poll_max=130) -> bool:
    """POST /api/test/reboot and wait until the device serves /api/state again.
    Returns True once the board is back and in Orchestrator mode."""
    try:
        dev.post("/api/test/reboot", {})
    except Exception:
        pass
    time.sleep(14)  # EN resets, ROM boots, WiFi rejoin begins (~reason 8 races)
    return _await_device(dev, poll_max=poll_max)


def recover_after_timeout(dev: Dev) -> bool:
    """A turn timed out on a degraded (no-SD) board. The device may have ALREADY
    self-rebooted - a heavy fan-out saturates core 0 past the 8 s task-watchdog, which
    RTC-resets to recover - or tg_poll may be wedged without a reboot. Rebooting a board
    that already rebooted just restarts the Wi-Fi-rejoin clock, so the NEXT turn fires
    mid-rejoin, can't reach the provider over TLS, and wedges -> reboot -> cascade (this
    is what corrupted the first run). So: only reboot when the board is reachable AND its
    heapMin shows NO reboot (a dipped low-water == a live wedge). Then wait for the network
    AND add a generous settle so the provider-TLS path - not just the web server - is ready
    before the next scenario. Breaks the cascade at the cost of ~30 s per recovery."""
    st = dev.state()
    reachable = bool(st.get("fw"))
    hm = st.get("heapMin") or 0
    # ⚠ NIMBUS_NO_TIMEOUT_REBOOT=1: on a board with a marginal SD contact, a reboot
    # dislodges the card mid-run. SD-MOUNTED turns rarely wedge (the crash is fixed), and
    # a normal slow turn also dips heapMin below 60000 - so the wedge heuristic would
    # misfire and reboot. In that mode NEVER reboot: just settle + continue. A genuine
    # wedge is far rarer than a card-drop, so this is the safer trade for the clean run.
    reboot_ok = os.environ.get("NIMBUS_NO_TIMEOUT_REBOOT") != "1"
    if reboot_ok and reachable and 0 < hm < 60000:
        # reachable, heapMin dipped -> tg_poll wedged, no reboot happened -> clear it once
        try:
            dev.post("/api/test/reboot", {})
        except Exception:
            pass
        time.sleep(14)
    ok = _await_device(dev, poll_max=150)
    time.sleep(30)  # provider-TLS settle cushion (the cascade root was firing too early)
    try:
        dev.post("/api/orch", {"sfxLvlN": "0", "sfxLvlO": "0", "sfxVol": "0"})
    except Exception:
        pass
    return ok


def run_scenario_once(dev: Dev, sc: dict) -> Bundle:
    """Run all of a scenario's turns on chat 'web', collecting the glass box.

    `auto` hooks (resilience scenarios only): reboot_after_turn=<idx> restarts the
    board after that turn and waits for recovery (multiday-continuity / mid-reboot);
    append_turn=<str> tacks on a synthetic follow-up (post-reboot 'what state are
    you in?')."""
    b = Bundle()
    auto = sc.get("auto") or {}
    since_id = _max_epi_id(dev)  # scope the tool trace to THIS scenario's rows only
    turns = list(sc["turns"])
    if auto.get("append_turn"):
        turns.append(auto["append_turn"])
    for ti, prompt in enumerate(turns):
        b.prompts.append(prompt)
        _first, reps = run_turn(dev, prompt)
        b.replies.extend(reps)
        b.turns_out.append(reps)
        b.jobs_peak = max(b.jobs_peak, len(_jobs(dev)))
        b.jobs_seen = max(b.jobs_seen, b.jobs_peak)
        # If this turn spawned sub-agents (job table non-empty, or an ACK-shaped
        # reply), wait for them to finish and collect the delayed synthesis.
        ackish = any(
            (
                "on it" in r.lower()
                or "stand by" in r.lower()
                or "job" in r.lower()
                or "sub-agent" in r.lower()
                or "launching" in r.lower()
            )
            for r in reps
        )
        if _jobs(dev) or ackish:
            wait_for_jobs_and_synthesis(dev, b)
        if auto.get("reboot_after_turn") == ti:
            ok = reboot_and_wait(dev)
            b.replies.append(f"[qa: device reboot {'recovered' if ok else 'FAILED to recover'}]")
    # scope to rows created AFTER this scenario began (see _max_epi_id) - an unscoped
    # read leaks a prior scenario's tool calls into this oracle.
    b.tool_rows = [r for r in dev.episodic(kind="tool_output", limit=200) if _epi_id(r) > since_id]
    b.log_rows = [r for r in dev.episodic(kind="log", limit=200) if _epi_id(r) > since_id]
    st = dev.state()
    b.heap_min = st.get("heapMin", 0)
    try:
        b.raw = dev.get("/api/lastturn", timeout=10).text[:8000]
    except Exception:
        pass
    return b


# ------------------------------------------------------------- scenario reset
def reset_scenario(dev: Dev):
    """Explicit per-scenario isolation on chat 'web' (fresh chatIds would be
    Role::Unknown). Clears the provider chain + running memory + drains replies."""
    dev.post("/api/orch", {"convReset": "1"})
    if MEMCLEAR:
        dev.post("/api/orch", {"memClear": "1"})
    _drain_reply(dev)
    time.sleep(2)


def set_host(dev: Dev, host: Optional[str]):
    if host:
        dev.post("/api/orch", {"orchHost": host})
        time.sleep(2)


def set_loop(dev: Dev, on):
    """Set the tool loop per scenario. ⚠ Mistral connectors + built-ins are
    DROPPED on tool-loop turns (AGENTS.md), so Mistral-connector scenarios must
    run single-shot (loop=0) or the connector silently isn't attached and the
    model fabricates 'starting now'. Default (None) leaves it on."""
    if on is None:
        dev.post("/api/orch", {"orchLoop": "1"})
    else:
        dev.post("/api/orch", {"orchLoop": "1" if on else "0"})
    time.sleep(1)


# ----------------------------------------------------------------- preflight
def credit_preflight() -> list:
    """One MINIMAL paid generation per keyed provider (~fractions of a cent) so a
    scored run can't burn scenarios on 'credit balance too low' mid-flight (the
    2026-08-06 fabric run lost ~4 scenarios to exactly that). Keys come from
    the secrets .env (the judge's source). A provider with no key is
    skipped; a billing/auth failure is FATAL and names the provider."""
    try:
        import requests
    except ImportError:
        return ["requests not installed (credit preflight impossible)"]
    env = _dotenv()
    problems = []
    checks = []
    if env.get("ANTHROPIC_API_KEY"):
        checks.append(
            (
                "anthropic",
                lambda k=env["ANTHROPIC_API_KEY"]: requests.post(
                    "https://api.anthropic.com/v1/messages",
                    timeout=25,
                    headers={"x-api-key": k, "anthropic-version": "2023-06-01", "content-type": "application/json"},
                    json={
                        "model": "claude-haiku-4-5-20251001",
                        "max_tokens": 1,
                        "messages": [{"role": "user", "content": "hi"}],
                    },
                ),
            )
        )
    if env.get("OPENAI_API_KEY"):
        checks.append(
            (
                "openai",
                lambda k=env["OPENAI_API_KEY"]: requests.post(
                    "https://api.openai.com/v1/responses",
                    timeout=25,
                    headers={"Authorization": f"Bearer {k}", "Content-Type": "application/json"},
                    json={"model": "gpt-5.4-nano", "input": "hi", "max_output_tokens": 16},
                ),
            )
        )
    if env.get("MISTRAL_API_KEY"):
        checks.append(
            (
                "mistral",
                lambda k=env["MISTRAL_API_KEY"]: requests.post(
                    "https://api.mistral.ai/v1/chat/completions",
                    timeout=25,
                    headers={"Authorization": f"Bearer {k}", "Content-Type": "application/json"},
                    json={
                        "model": "mistral-small-latest",
                        "max_tokens": 1,
                        "messages": [{"role": "user", "content": "hi"}],
                    },
                ),
            )
        )
    for prov, call in checks:
        try:
            r = call()
            if r.status_code in (401, 402, 403) or (r.status_code == 400 and "credit" in r.text.lower()):
                problems.append(f"{prov}: credit/auth preflight FAILED ({r.status_code}: {r.text[:120]})")
            elif r.status_code >= 500:
                print(f"   (credit preflight: {prov} 5xx - transient, not fatal)")
        except Exception as e:  # network blip ≠ billing failure; warn only
            print(f"   (credit preflight: {prov} unreachable: {e} - not fatal)")
    return problems


def preflight(dev: Dev, judge, want_build: str = "", need_sd: bool = True, allow_trace_blind: bool = False) -> list:
    """Abort the run unless the apparatus is trustworthy. Returns a list of
    fatal problems (empty == go)."""
    problems = []
    st = dev.state()
    if not st:
        return ["device unreachable"]
    build = st.get("build", "")
    if want_build and want_build not in build:
        problems.append(f"build {build} != wanted {want_build}")
    if st.get("mode") != 1:
        problems.append("not in Orchestrator mode (need MODE 1)")
    if st.get("memSd") is False:
        # Two distinct reasons a run needs SD: (1) a SELECTED scenario functionally
        # uses files/artifacts (need_sd, from the explicit sd=True marking); (2) the
        # tool-call TRACE that grounds every tool-based oracle is SD-gated on the
        # device, so a no-SD run is TRACE-BLIND for honesty (fabric-n5, 2026-08-06:
        # every corpus row had empty tool_calls and honest tool use judged as
        # fabrication). (2) applies to nearly all scenarios, so no-SD runs need an
        # explicit opt-in that stamps the run as trace-blind.
        if need_sd:
            problems.append(
                "SD not mounted and a selected scenario is SD-dependent (sd=True) - deselect those or fix the card"
            )
        elif not allow_trace_blind:
            problems.append(
                "SD not mounted - the tool trace is SD-gated, so tool-"
                "grounded oracles and the judge would be TRACE-BLIND. "
                "Pass --allow-trace-blind to run anyway (honesty metrics "
                "will NOT be trustworthy)"
            )
        else:
            print(
                "   ⚠ TRACE-BLIND RUN: no SD → empty tool traces → tool-grounded "
                "oracles and judge honesty verdicts are NOT trustworthy"
            )
    problems += credit_preflight()
    # H1: the tool trace MUST be on or every tool-based oracle is ungrounded - a
    # side-effect exfil reads as "no tools" (false PASS) and every FAB_DONE claim
    # false-fails. Firmware captures the trace only when orchTrace is true.
    o = dev.orch()
    # fail-closed: OFF or MISSING both abort (a missing field means /api/orch errored or
    # an older build that doesn't ground the trace - either way don't trust the oracles).
    if o.get("orchTrace") is not True:
        problems.append(
            "orchTrace not confirmed ON (got "
            + repr(o.get("orchTrace"))
            + ") - tool-trace oracles would be ungrounded; enable Activity "
            "recording (Assistant → Tools → Tool use) and retry"
        )
    # web.search canary - the whole point of Phase 0
    reset_scenario(dev)
    try:
        _r, reps = run_turn(dev, "Use web search for 'current UTC time' and quote one result URL.", timeout=220)
        joined = " ".join(reps).lower()
        # M4: match every known search-failure string, not just one, AND require a real
        # URL - a budget-exhausted or skipped search must not pass the canary.
        if any(x in joined for x in ("web search failed", "budget", "skipped", "couldn't search", "could not search")):
            problems.append("web.search canary FAILED (search error/budget/skipped)")
        elif not any(u in joined for u in ("http://", "https://", "www.")):
            problems.append("web.search canary returned NO url - search likely down")
    except Exception as e:
        problems.append(f"web.search canary errored: {e}")
    # judge canary
    if judge:
        v = judge("say hi", "Hi there!", "", {"name": "greeted", "passed": True})
        if not v or "satisfied" not in v:
            problems.append("judge canary did not return a parseable verdict")
    else:
        problems.append("no judge available (need a provider key in the secrets .env)")
    return problems


# ------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="comma-separated scenario ids")
    ap.add_argument(
        "--exclude",
        default="",
        help="comma-separated scenario ids to drop (e.g. sound/speaker tests the owner runs separately)",
    )
    ap.add_argument(
        "--turn-timeout",
        type=int,
        default=0,
        help="per-turn first-reply timeout (s); 0 keeps the default. A fan-out "
        "still extends while jobs[] are active, so a lower value only speeds up "
        "detecting a genuine hang.",
    )
    ap.add_argument("--cat", default="", help="only this category")
    ap.add_argument(
        "--exclude-cat",
        default="",
        help="comma-separated categories to drop (e.g. research-artifacts on a no-SD board)",
    )
    ap.add_argument(
        "--allow-trace-blind",
        action="store_true",
        help="permit a no-SD run of non-SD scenarios (tool traces will be "
        "EMPTY: tool-grounded oracles + judge honesty are untrustworthy)",
    )
    ap.add_argument("--needs-none", action="store_true", help="only scenarios needing no setup")
    ap.add_argument(
        "--have",
        default="",
        help="comma-list of satisfied prereqs; scenarios whose needs are all in this set (plus needs-none) run",
    )
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--build", default="", help="required build substring (preflight gate)")
    ap.add_argument("--skip-preflight", action="store_true")
    ap.add_argument(
        "--reboot-each",
        action="store_true",
        help="restart the board before every scenario - on the pre-fix "
        "firmware a tool-loop turn wedges once internal heap fragments, so "
        "a fresh boot per scenario maximizes real data (slow but honest)",
    )
    ap.add_argument("--dry", action="store_true", help="list what would run")
    ap.add_argument(
        "--no-memclear",
        action="store_true",
        help="isolate scenarios with convReset only (skip memClear). Required on a "
        "degraded no-SD board where memClear wedges the turn task.",
    )
    ap.add_argument(
        "--name",
        default="",
        help="corpus run-name suffix (distinguishes parallel runs on different boards; default 'complex-<timestamp>')",
    )
    ap.add_argument(
        "--device-note",
        default="",
        help="board-specific facts for the judge's "
        "[DEVICE STATE] line (e.g. 'speaker DEAD on this board') - appended to "
        "the auto-derived SD/sound state",
    )
    args = ap.parse_args()

    if args.turn_timeout:
        global TURN_TIMEOUT
        TURN_TIMEOUT = args.turn_timeout
    if args.no_memclear:
        global MEMCLEAR
        MEMCLEAR = False

    ip = os.environ.get("NIMBUS_TEST_IP")
    tok = os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        sys.exit("set NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN")

    from scenarios_complex import SCENARIOS, ORACLES  # noqa: E402

    sel = SCENARIOS
    if args.only:
        ids = set(args.only.split(","))
        sel = [s for s in sel if s["id"] in ids]
    if args.exclude:
        drop = set(x.strip() for x in args.exclude.split(",") if x.strip())
        sel = [s for s in sel if s["id"] not in drop]
    if args.cat:
        sel = [s for s in sel if s["cat"] == args.cat]
    if args.exclude_cat:
        dropc = set(x.strip() for x in args.exclude_cat.split(",") if x.strip())
        sel = [s for s in sel if s["cat"] not in dropc]
    if args.needs_none:
        sel = [s for s in sel if not s.get("needs")]
    if args.have:
        have = set(x.strip() for x in args.have.split(",") if x.strip())
        sel = [s for s in sel if all(n in have for n in s.get("needs", []))]

    print(f"selected {len(sel)}/{len(SCENARIOS)} scenarios")
    if args.dry:
        for s in sel:
            needs = ",".join(s.get("needs", [])) or "-"
            print(f"  {s['id']:44} cat={s['cat']:16} needs={needs} host={s.get('host') or '-'}")
        return

    dev = Dev(ip, tok)

    class _Sec:
        def get(self, k, d=""):
            return _dotenv().get(k, d)

    # M6: the judge must be independent of the provider UNDER TEST. Most scenarios run
    # host="anthropic", so avoid anthropic for the judge (it was avoiding openai, which
    # left the anthropic majority judged by their own provider).
    hosts = [s.get("host") for s in sel if s.get("host")]
    dominant = max(set(hosts), key=hosts.count) if hosts else "anthropic"
    judge = make_judge(_Sec(), avoid_provider=dominant)
    print(f"judge avoids '{dominant}' (dominant host under test)")

    if not args.skip_preflight:
        need_sd = any(s.get("sd") for s in sel)
        probs = preflight(dev, judge, args.build, need_sd=need_sd, allow_trace_blind=args.allow_trace_blind)
        if probs:
            print("PREFLIGHT FAILED - refusing to record a baseline:")
            for p in probs:
                print("   ✗", p)
            sys.exit(2)
        print("preflight OK\n")

    st0 = dev.state()
    build = st0.get("build", "?")
    # DEVICE STATE for the judge: auto-derived SD/sound reality + operator-supplied
    # board facts (e.g. a physically dead speaker the device cannot self-report).
    o0 = dev.orch()
    note_bits = [
        "SD " + ("mounted" if st0.get("memSd") else "ABSENT (degraded storage; tool trace NOT captured)"),
        "sound muted (volume 0)" if not (o0.get("sfxVol") or 0) else f"sound volume {o0.get('sfxVol')}",
    ]
    if args.device_note:
        note_bits.append(args.device_note)
    rec = QARecorder(
        run_name=(args.name or f"complex-{time.strftime('%Y%m%d-%H%M')}"),
        board_build=build,
        judge=judge,
        device_note="; ".join(note_bits),
    )

    for s in sel:
        print(f"\n=== {s['id']}  ({s['cat']}, needs={s.get('needs') or '-'}) ===")
        if args.reboot_each:
            # fresh heap per scenario - the pre-fix firmware wedges tool-loop turns
            # under fragmentation, so this lifts the real per-scenario success rate.
            print("   (reboot for fresh heap...)", flush=True)
            reboot_and_wait(dev)
        set_host(dev, s.get("host"))
        set_loop(dev, s.get("loop"))
        oracle_fn = ORACLES[s["oracle"]]
        auto = s.get("auto") or {}
        for rep in range(args.reps):
            reset_scenario(dev)
            if s.get("seed"):
                s["seed"](dev)  # optional pre-state
            # resilience fault injection (cleared in finally so a failed run
            # never leaves the device degraded for downstream scenarios)
            for cap in auto.get("fault_pre", []):
                dev.post("/api/fault", {"cap": cap, "on": "1"})
                time.sleep(1)
            try:
                b = run_scenario_once(dev, s)
            except TurnTimeout as e:
                # On this build a wedged turn task blocks EVERY later turn until a
                # restart, so a timeout must reboot to clear the wedge - not just
                # wait - or the whole rest of the run cascades into hangs.
                rec.record(
                    s["id"],
                    f"rep{rep}",
                    " | ".join(s["turns"]),
                    {"reply": "", "_raw": str(e)},
                    OracleResult(s["oracle"], False, f"turn timed out (wedge): {e}"),
                )
                # --reboot-each already restarts before the NEXT scenario, so don't
                # double-reboot here (that ~doubled the dead time per hang). Without
                # reboot-each, a hang must be cleared now.
                if args.reboot_each:
                    print(f"   rep{rep}: TURN TIMED OUT - next scenario reboots")
                else:
                    # Robust recovery: the board may have self-rebooted (fan-out tripped
                    # the 8 s watchdog) - do NOT blindly reboot again (that restarts the
                    # Wi-Fi-rejoin clock and cascades). recover_after_timeout reboots only a
                    # live wedge, then settles for the provider-TLS path.
                    print(f"   rep{rep}: TURN TIMED OUT - recovering (no double-reboot)")
                    recover_after_timeout(dev)
                continue
            except RebootDuringTurn as e:
                rec.record(
                    s["id"],
                    f"rep{rep}",
                    " | ".join(s["turns"]),
                    {"reply": "", "_raw": str(e)},
                    OracleResult(s["oracle"], False, f"turn failed: {e}"),
                )
                print(f"   rep{rep}: DEVICE REBOOT - {e}")
                recover_after_timeout(dev)
                continue
            except Exception as e:  # network blip, JSON error, anything
                rec.record(
                    s["id"],
                    f"rep{rep}",
                    " | ".join(s["turns"]),
                    {"reply": "", "_raw": str(e)},
                    OracleResult(s["oracle"], False, f"runner error: {e}"),
                )
                print(f"   rep{rep}: RUNNER ERROR - {e}")
                _await_device(dev)
                continue
            finally:
                # always restore a clean device - a faulted cap left set would
                # silently degrade every later scenario (learned live).
                if auto.get("fault_pre"):
                    try:
                        dev.post("/api/fault", {"cap": "all"})
                    except Exception:
                        pass
                    time.sleep(1)
            ok, detail = oracle_fn(b, s)
            turn = {
                "reply": b.reply,
                "_raw": b.raw,
                "_replies": b.replies,
                "_tools": b.tool_names(),
                "_jobs_peak": b.jobs_peak,
                "_heap_min": b.heap_min,
            }
            step = rec.record(s["id"], f"rep{rep}", " | ".join(s["turns"]), turn, OracleResult(s["oracle"], ok, detail))
            jv = (step.judge or {}).get("score")
            print(f"   rep{rep}: oracle={'PASS' if ok else 'FAIL'} judge={jv} - {detail[:90]}")

    path = rec.close()
    print(f"\ncorpus: {path}")
    print(rec.report_md()[:2000])


def _dotenv() -> dict:
    p = os.environ.get("NIMBUS_ENV_FILE") or os.path.join(os.path.dirname(__file__), "..", "..", ".env")
    out = {}
    if os.path.exists(p):
        for line in open(p):
            line = line.strip()
            if line.startswith("export "):
                line = line[7:]
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip().strip('"').strip("'")
    return {**out, **os.environ}


if __name__ == "__main__":
    main()
