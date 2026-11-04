#!/usr/bin/env python3
"""The bench rows that had no command (coverage/BENCH-5.0.0.md section 5.1), one command each.

    bench_rows.py list
    bench_rows.py A07 --ip IP --token-file F --port P --mac MAC --out $EV [--env-file F]

Each row writes its raw evidence under <out>/<row>/ and ends with verdict.json graded PASS, FAIL, OPEN or BLOCKED. A
missing measurement is BLOCKED, never PASS. What needs a human hand, such as a cable pull, gets a precise prompt and
an automatic check before and after. Three other rows have their own drivers: K01 soak_24h.py, H27 onboarding_gate.py,
B01 ota_rehearsal.py. V05 stays BLOCKED: the code has no seam for a short write on a stalled socket.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import random
import re
import sys
import threading
import time
from typing import Any, Callable, Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench_driver as bd
import bench_lock
import port_guard

MIN_LOW_WATER = 8000
POLL_STACK_ALLOWANCE = 512
LOW_WATER_ALLOWANCE = 1000
RECHECK_WAIT_S = 2100.0  # the 30 min re-check interval plus a margin; there is no console verb to force one
RECHECK_POLL_S = 300.0
CUMULO_HOST = "app.cumulo-nimbus.ai"
# Byte for byte from the router contract, as test_router_refusal_contract has them.
CLAMP_402 = (
    '{"error":"insufficient_credits_for_call","reason":"insufficient_credits_for_call","message":"This request could '
    "cost up to 525 credits and 309 are available. Nothing was charged. Limit the output to 74904 tokens or fewer, or "
    'top up.","charged":false,"retryable":false,"balanceCredits":309,"availableCredits":309,"heldCredits":0,'
    '"estimatedCredits":525,"maxAffordableOutputTokens":74904,"retryHint":"lower_max_output"}'
)
EMPTY_402 = (
    '{"error":"insufficient_credits","reason":"insufficient_credits","message":"Your credit balance is empty. Top up '
    'to continue.","balanceCredits":0,"charged":false,"retryable":false}'
)
HISTORY_LINE = "bench history line " + "x" * 880
LIGHTS_WORDINGS = (
    "Turn the ring lights off.",
    "Please switch the lights off now.",
    "Use your device lights action and set them to off.",
)
NO_CUT_S = 90.0


class Blocked(Exception):
    """The row could not run or could not measure: nothing was proven."""


@dataclasses.dataclass
class Bench:
    """What every row works with. ``ask`` puts a question to the person at the bench (None: nobody there)."""

    args: argparse.Namespace
    api: bd.Api
    ev: bd.Evidence
    console: Any = None
    ask: Callable[[str], Optional[bool]] = lambda question: None
    say: Callable[[str], None] = print
    sleep: Callable[[float], None] = time.sleep
    poll_s: float = 3.0

    def con(self, line: str, expect: str, timeout: float = 8.0) -> str:
        if self.console is None:
            raise Blocked("this row needs the test console: pass --port and --mac")
        return self.console.cmd(line, expect, timeout)

    def state(self) -> Dict[str, Any]:
        value = self.api.state(10.0)
        if not value:
            raise Blocked(f"no answer from {self.api.ip}")
        return value

    def snap(self, name: str) -> Dict[str, Any]:
        value = self.state()
        self.ev.write(f"{name}.json", value)
        return value

    def errlog(self) -> str:
        return self.api.get("/api/errlog", 60.0).text

    def mark(self) -> set:
        """The durable log's lines now, so a later read can show what is new."""
        return set(self.errlog().splitlines())

    def new_lines(self, seen: set) -> List[str]:
        return [line for line in self.errlog().splitlines() if line not in seen]

    def need_human(self, question: str) -> None:
        answer = self.ask(question)
        if answer is None:
            raise Blocked(f"needs a person at the bench: {question}")
        if not answer:
            raise Blocked(f"the person answered no: {question}")

    def wait_for_unit(self, timeout: float = 120.0) -> Dict[str, Any]:
        end = time.time() + timeout
        while time.time() < end:
            value = self.api.state(6.0)
            if value:
                return value
            self.sleep(self.poll_s)
        raise Blocked(f"the unit did not come back within {timeout:.0f} s")

    def turn(self, text: str, timeout: float = 180.0) -> bd.TurnResult:
        return bd.chat_turn(self.api, text, timeout, self.poll_s)

    def tool(self, name: str, arguments: Dict[str, Any], chat: str = "web") -> str:
        return self.api.tool(name, arguments, chat, 60.0).text

    def need_key(self, provider: str) -> None:
        if not ((self.api.get_json("/api/orch").get("providers") or {}).get(provider) or {}).get("hasKey"):
            raise Blocked(f"the unit has no {provider} key; the owner saves one first (plan section 1.6)")


Grade = Tuple[str, str]


def boot_n(state: Dict[str, Any]) -> Any:
    return (state.get("boot") or {}).get("n")


def crash_mark(state: Dict[str, Any]) -> Any:
    return ((state.get("boot") or {}).get("crash") or {}).get("fp")


def mem(state: Dict[str, Any], name: str) -> Any:
    return (state.get("mem") or {}).get(name)


def _no_restart(before: Dict[str, Any], after: Dict[str, Any]) -> List[str]:
    out = []
    if boot_n(before) != boot_n(after):
        out.append(f"the unit restarted: boot {boot_n(before)} to {boot_n(after)}")
    if crash_mark(before) != crash_mark(after):
        out.append(f"a new crash record: {crash_mark(after)}")
    return out


def _verdict(fails: List[str], good: str) -> Grade:
    return ("FAIL", "; ".join(fails)) if fails else ("PASS", good)


# ---- A07, A08: a turn that overflows, trimmed ----------------------------------------------------------


def row_a07(b: Bench) -> Grade:
    """A long history and a helper result waiting: the turn must answer on the same turn, trimmed and honest."""
    before = b.snap("state-before")
    seen = b.mark()
    for _ in range(20):
        if not b.con(f"HISTADD web 50 {HISTORY_LINE}", "HISTADD chat=", 10.0):
            raise Blocked("HISTADD did not answer")
    b.con("TURN Ask a helper for three facts about Lisbon, then answer me", "TURN <-", 10.0)
    result = b.turn("Now tell me the three facts about Lisbon you found.")
    b.ev.write("reply.txt", result.reply)
    lines = b.new_lines(seen)
    b.ev.write("errlog.txt", "\n".join(lines))
    after = b.snap("state-after")
    fails = _no_restart(before, after)
    if not result.ok:
        fails.append(f"the turn was not answered: {result.error}")
    if "didn't finish" in result.reply or "That didn't finish" in "\n".join(lines):
        fails.append("the owner got 'That didn't finish'")
    if not any("turn fit: left out" in line for line in lines):
        return (
            ("BLOCKED", "the turn answered but nothing was trimmed, so this did not reach the overflow")
            if not fails
            else _verdict(fails, "")
        )
    return _verdict(fails, "answered on the same turn with the body trimmed")


def row_a08(b: Bench) -> Grade:
    """The memory cost of the trim: reads A07's snapshots against A02's low water (--a02-state).

    Measurement validity (bench v5-28813029, lane DISPO): the row measures the
    TRIM's cost, so it is BLOCKED when A07's turn never trimmed (A07's own
    BLOCKED case: the HISTADD approximation cannot reach the overflow);
    pollStackMin's before-snap can catch the poll task's unset sentinel
    (0xFFFFFFFF), which makes the drop arithmetic meaningless instead of a
    real drop; and mem.lowWater is a since-boot minimum (heap_watch), so A02's
    state file is only a valid baseline when it was captured on the SAME boot
    as A07's run (a crash between A02 and A07 - e.g. the A03 row - resets the
    watermark, and every row in between dips it). Each guard reports the
    clause as unmeasurable; none of them loosens an allowance."""
    folder = os.path.join(b.args.out, "A07")
    if not os.path.exists(os.path.join(folder, "state-after.json")):
        raise Blocked("run A07 first: this row reads its snapshots")
    if not b.args.a02_state:
        raise Blocked("pass --a02-state with A02's state file")
    before, after = _load(os.path.join(folder, "state-before.json")), _load(os.path.join(folder, "state-after.json"))
    a02 = _load(b.args.a02_state)
    errlog_path = os.path.join(folder, "errlog.txt")
    trimmed = False
    if os.path.exists(errlog_path):
        with open(errlog_path, encoding="utf-8") as f:
            trimmed = "turn fit: left out" in f.read()
    if not trimmed:
        raise Blocked("A07's turn did not trim, so the trim's cost was never exercised")
    fails: List[str] = []
    unmeasured: List[str] = []
    stack_before, stack_after = mem(before, "pollStackMin") or 0, mem(after, "pollStackMin") or 0
    if stack_before >= 0xFFFF0000:   # the poll task's unset sentinel: no sample yet
        unmeasured.append(f"pollStackMin unmeasurable (before-snap held the unset sentinel {stack_before})")
    else:
        drop = stack_before - stack_after
        if drop > POLL_STACK_ALLOWANCE:
            fails.append(f"pollStackMin fell {drop} B (allowance {POLL_STACK_ALLOWANCE} B)")
    if boot_n(a02) != boot_n(after):
        unmeasured.append(
            f"lowWater unmeasurable (A02's state is boot {boot_n(a02)}, the run is boot {boot_n(after)}: "
            "the since-boot watermark reset in between)"
        )
    else:
        gap = (mem(a02, "lowWater") or 0) - (mem(after, "lowWater") or 0)
        if gap > LOW_WATER_ALLOWANCE:
            fails.append(f"low water {gap} B under A02's (allowance {LOW_WATER_ALLOWANCE} B)")
    if fails:
        return _verdict(fails, "")
    if unmeasured:
        return ("BLOCKED", "; ".join(unmeasured))
    return _verdict([], f"pollStackMin fell {stack_before - stack_after} B; low water within "
                        f"{max((mem(a02, 'lowWater') or 0) - (mem(after, 'lowWater') or 0), 0)} B of A02")


def _load(path: str) -> Dict[str, Any]:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# ---- C05, C06, V04: Telegram rows --------------------------------------------------------------------


def _with_bot(b: Bench, body: Callable[[], Grade]) -> Grade:
    """Bot mode: the test bot on the unit under locks/tg-bot, cleared in a finally."""
    env_file = b.args.env_file
    if not env_file:
        raise Blocked("pass --env-file: bot mode reads NIMBUS_TEST_TG_TOKEN and NIMBUS_TEST_TG_CHAT from it")
    token = bd.read_env_value(env_file, "NIMBUS_TEST_TG_TOKEN")
    chat = bd.read_env_value(env_file, "NIMBUS_TEST_TG_CHAT")
    if not (token and chat):
        raise Blocked("NIMBUS_TEST_TG_TOKEN and NIMBUS_TEST_TG_CHAT are not in the env file")
    with bench_lock.bot_on_unit(lambda form: b.api.post_form("/api/orch", form, 10.0), token, chat, "bench_rows"):
        return body()


def row_c05(b: Bench) -> Grade:
    """A real 6-photo album from the owner's phone: a clean answer, or a cut that recovers; never silence."""

    def body() -> Grade:
        before = b.snap("state-before")
        seen = b.mark()
        b.need_human("Send a 6-photo album of real meals to the bench bot chat from your phone. Answer yes once sent.")
        end, lines = time.time() + b.args.album_wait_s, []
        while time.time() < end:
            lines = b.new_lines(seen)
            if any("turn end:" in line for line in lines):
                break
            b.sleep(b.poll_s)
        b.ev.write("errlog.txt", "\n".join(lines))
        b.ev.write("reply.txt", b.api.get("/api/lastturn", 15.0).text)
        fails = _no_restart(before, b.snap("state-after"))
        ended = [line for line in lines if "turn end:" in line]
        if not ended:
            fails.append("no turn ended: the album was lost or the owner was left in silence")
        elif not any("turn end: ok" in line for line in ended):
            fails.append(f"the album's turn failed: {ended[-1]}")
        cut = any(" cut" in line for line in lines)
        if cut and not any("the round runs once more" in line for line in lines):
            fails.append("the album cut and the round did not run again")
        return _verdict(
            fails, "the album was answered, after a recovered cut" if cut else "the album was answered clean"
        )

    return _with_bot(b, body)


def _inject_member_photo(b: Bench, chat: int, update: int, name: str, size: int) -> None:
    import tg_archive_bench as tb

    b.con(
        "TGINJECT " + tb.page([tb.local_photo_update(update, chat, f"/mem/files/bench500/{name}", size)]),
        "injected",
        15.0,
    )


def _member_chat(b: Bench) -> int:
    chat = b.args.member_chat
    added = b.api.post_form("/api/telegram/add", {"id": str(chat), "name": "Bench member"})
    if added.code != 200:
        raise Blocked(f"could not approve the member chat: {added.code}")
    return chat


def row_c06(b: Bench) -> Grade:
    """A member sends the same photo twice: one stored file, one charge. Then a person fills the limit from a member
    chat and reads the refusal, because the reply must reach the sender and only a real chat shows that."""
    import tg_archive_bench as tb

    chat = _member_chat(b)
    data = tb.noise_jpeg(480, 360, 77)
    if b.api.upload("bench500", "dup.jpg", data).code != 200:
        raise Blocked("could not stage the photo")
    update = tb.first_update_id()
    seen = b.mark()
    for i in range(2):
        _inject_member_photo(b, chat, update + i * 100, "dup.jpg", len(data))
        b.sleep(b.poll_s)
    listed = b.api.get(f"/api/files/list?project=telegram-{chat}")
    b.ev.write("files-list.json", listed.text)
    b.ev.write("errlog.txt", "\n".join(b.new_lines(seen)))
    files = listed.json().get("files") or []
    if len(files) != 1:
        return ("FAIL", f"the same photo twice left {len(files)} stored files")
    b.need_human(
        "From an approved member chat, send photos until the limit refuses one. Did the refusal reach the chat as a reply?"
    )
    return ("PASS", "one stored file for the duplicate; the person saw the refusal reach the sender")


def row_v04(b: Bench) -> Grade:
    """A member asks for speech: text plus the plain sentence, no reason given, and nothing on the speaker."""
    chat = _member_chat(b)
    import tg_archive_bench as tb

    seen = b.mark()
    update = tb.first_update_id()
    b.con(
        "TGINJECT " + tb.page([tb.text_update(update, chat, "Please say hello out loud on the device.")]),
        "injected",
        15.0,
    )
    b.console.wait("turn end:", b.args.album_wait_s, since=time.time() - 1)
    lines = b.new_lines(seen)
    b.ev.write("errlog.txt", "\n".join(lines))
    rows = b.api.get(f"/api/mem/episodic?session={chat}&limit=10")
    b.ev.write("member-reply.txt", rows.text)
    fails = []
    if "Only the device's owner can speak aloud on the device." not in rows.text:
        fails.append("the member's reply does not carry the sentence")
    if any(re.search(r"\btts: (speak|play)", line) for line in lines):
        fails.append("the speaker played for a member")
    return _verdict(fails, "the member got the text and the sentence; nothing played")


# ---- D03: the lights action ------------------------------------------------------------------------------


def row_d03(b: Bench) -> Grade:
    """Whether the head returns a lights action is the model's choice, so a few wordings are tried, each logged."""
    before = b.snap("state-before")
    seen = b.mark()
    action = []
    for wording in LIGHTS_WORDINGS:
        result = b.turn(wording)
        b.ev.append("turns.txt", f"{wording} -> ok={result.ok} {result.reply[:120]}")
        action = [line for line in b.new_lines(seen) if "devsink: lights" in line]
        if action:
            break
    lines = b.new_lines(seen)
    b.ev.write("errlog.txt", "\n".join(lines))
    fails = _no_restart(before, b.snap("state-after"))
    if fails:
        return _verdict(fails, "")
    if not action:
        return ("BLOCKED", f"the head never returned a lights action in {len(LIGHTS_WORDINGS)} wordings")
    if any("dropped" in line for line in lines):
        return ("FAIL", "the durable log shows a dropped line near the lights action")
    return ("PASS", f"durable log: {action[0][:100]}; no restart")


# ---- E01: power cut during files.edit ------------------------------------------------------------------------


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def edit_csv(rows: int) -> Tuple[bytes, bytes]:
    original = "a,b,c\n" + "".join(f"{i},row{i},v{i}\n" for i in range(rows))
    return original.encode(), original.replace("row0,", "edited0,", 1).encode()


def _one_cut(b: Bench, n: int, original: bytes, edited: bytes) -> Dict[str, Any]:
    """One hand-timed cut with the check before and after. A cut that did not restart the unit is not counted."""
    if b.api.upload("bench500", "edit.csv", original).code != 200:
        raise Blocked("could not upload the file before a cut")
    before = b.state()
    delay = random.uniform(0.5, 3.0)
    b.need_human(
        f"Cut {n}: hold the USB-C cable. Answer yes, then pull it the moment PULL NOW prints ({delay:.1f} s later)."
    )
    worker = threading.Thread(
        target=lambda: b.tool(
            "files.edit", {"project": "bench500", "name": "edit.csv", "find": "row0,", "replace": "edited0,"}
        ),
        daemon=True,
    )
    worker.start()
    b.sleep(delay)
    b.say("PULL NOW")
    b.need_human(f"Cut {n}: pull done. Plug it back in, and answer yes when the screen is back.")
    after = b.wait_for_unit()
    log = b.errlog()
    b.ev.write(f"errlog-{n}.txt", log)
    got = b.api.download("bench500", "edit.csv")
    b.ev.write(f"after-{n}.csv", got.body)
    listed = b.api.get("/api/files/list?project=bench500")
    digest = sha(got.body)
    return {
        "n": n,
        "delayS": round(delay, 2),
        "restarted": boot_n(after) != boot_n(before),
        "http": got.code,
        "hash": digest,
        "isOld": digest == sha(original),
        "isNew": digest == sha(edited),
        "listed": "edit.csv" in listed.text,
        "fsMounted": after.get("fsMounted"),
        "started": "files.edit" in log,
    }


def grade_cuts(cuts: List[Dict[str, Any]], minimum_inside: int = 3) -> Grade:
    """Every counted cut must leave the old file or the new one, whole and listed. Too few cuts that restarted the
    unit mid-edit prove nothing, so that is BLOCKED."""
    counted = [c for c in cuts if c["restarted"]]
    fails = []
    for cut in counted:
        if not (cut["isOld"] or cut["isNew"]):
            fails.append(f"cut {cut['n']} left a file that is neither the old nor the new one (http {cut['http']})")
        if not cut["listed"]:
            fails.append(f"cut {cut['n']}: the file is missing from the listing")
        if cut["fsMounted"] is False:
            fails.append(f"cut {cut['n']}: the file area did not mount")
    if fails:
        return ("FAIL", "; ".join(fails))
    inside = [c for c in counted if c["started"]]
    if len(inside) < minimum_inside:
        return ("BLOCKED", f"only {len(inside)} of {len(cuts)} cuts landed inside an edit (need {minimum_inside})")
    return ("PASS", f"{len(inside)} cuts inside an edit, every file whole")


def row_e01(b: Bench) -> Grade:
    """Ten hand-timed cable pulls during files.edit of an 800 KB file. Old or new, whole; never missing or mixed."""
    original, edited = edit_csv(b.args.e01_rows)
    b.ev.write("edit.csv", original)
    b.ev.write("edit-new.csv", edited)
    b.ev.write("hashes.txt", f"old {sha(original)}\nnew {sha(edited)}\n")
    cuts = [_one_cut(b, n, original, edited) for n in range(1, b.args.cuts + 1)]
    b.ev.write("cuts.json", cuts)
    return grade_cuts(cuts)


# ---- the provider-refusal rows --------------------------------------------------------------------------------


def _head_on_cumulo(b: Bench) -> None:
    b.need_key("cumulo")
    b.api.post_form("/api/orch", {"orchHost": "cumulo"})


def _arm_park(b: Bench, body: str) -> str:
    """One turn into an armed 402, the fault lifted after it. Returns what PARK? said."""
    if "armed" not in b.con(f"PROVFAULT {CUMULO_HOST} 402 {body}", "armed", 8.0):
        raise Blocked("PROVFAULT did not arm")
    try:
        result = b.turn("Reply with exactly one word: ok", 120.0)
        b.ev.write("reply.txt", f"ok={result.ok} {result.reply}\n{result.error}")
        return b.con("PARK?", "PARK?", 6.0) + "\n" + "\n".join(b.console.since(time.time() - 2.0))
    finally:
        b.con("PROVFAULT off", "PROVFAULT", 6.0)


def row_s10(b: Bench) -> Grade:
    """The clamp 402: the turn answers on a capped resend and the provider is not parked. PROVFAULT answers at
    the transport's seat (TestAnswerTransport), so the resend runs against the scripted refusal; a served capped
    reply still needs a real low-wallet account."""
    _head_on_cumulo(b)
    seen = b.mark()
    parked = _arm_park(b, CLAMP_402)
    lines = b.new_lines(seen)
    b.ev.write("errlog.txt", "\n".join(lines))
    b.ev.write("park.txt", parked)
    fails = []
    if not any("resending once with the output capped" in line for line in lines):
        fails.append("no capped resend in the log")
    if re.search(r"PARK cumulo", parked):
        fails.append("the provider parked on a clamp")
    if fails:
        return ("FAIL", "; ".join(fails))
    return (
        "OPEN",
        "the capped resend ran against the scripted refusal; a served capped reply still needs a low-wallet account",
    )


def _wait_recheck(b: Bench, seen: set, wanted: Tuple[str, ...], label: str) -> List[str]:
    """Poll the durable log until the re-check's outcome shows or the interval is well past. The orch state is kept
    each time, because the park's Health row is part of the evidence."""
    end = time.time() + b.args.recheck_wait_s
    lines: List[str] = []
    n = 0
    while time.time() < end:
        n += 1
        b.ev.write(f"orch-{label}-{n}.json", b.api.get_json("/api/orch"))
        lines = b.new_lines(seen)
        if any(any(w in line for w in wanted) for line in lines):
            break
        b.sleep(b.args.recheck_poll_s)
    return lines


def row_s11(b: Bench) -> Grade:
    """An empty wallet parks the provider, the park says so, and the re-check opens it again by itself. Also row S12:
    the memory across that TLS re-check."""
    _head_on_cumulo(b)
    seen = b.mark()
    parked = _arm_park(b, EMPTY_402)
    orch = b.api.get_json("/api/orch")
    b.ev.write("orch-parked.json", orch)
    b.ev.write("park.txt", parked)
    if "kind=billing" not in parked:
        return ("FAIL", "PARK? does not show billing after an empty-wallet refusal")
    if "empty" not in json.dumps(orch.get("parks") or []).lower():
        return ("FAIL", "the park does not say the credit balance is empty")
    before = b.snap("state-before")
    lines = _wait_recheck(b, seen, ("re-check answered", "refused again"), "s11")
    b.ev.write("errlog.txt", "\n".join(lines))
    after = b.snap("state-after")
    fails = _no_restart(before, after)
    if not any("re-check answered - open again" in line for line in lines):
        fails.append("the park did not open by itself within the wait")
    if any("add credits" in line.lower() for line in lines):
        fails.append("an add-credits notice from a re-check")
    low = mem(after, "lowWater")
    if not isinstance(low, (int, float)) or low < MIN_LOW_WATER:
        fails.append(f"low water {low} B across the re-check")
    return _verdict(fails, f"opened by itself; low water {low} B across the re-check")


def _due_soon_turn(b: Bench, seen: set, label: str, send: Callable[[], Any]) -> List[str]:
    """Park, wait until the re-check is one minute off, do ``send``, and read the log until the re-check's outcome."""
    _head_on_cumulo(b)
    _arm_park(b, EMPTY_402)
    b.sleep(max(0.0, b.args.recheck_lead_s))
    send()
    return _wait_recheck(b, seen, ("re-check handed back", "re-check answered", "refused again"), label)


def row_s13(b: Bench) -> Grade:
    """A voice note and an image request while the poll holds the TLS slot, with a park due: the re-check hands back
    rather than holding the slot, and the next caller's request is the probe."""
    seen = b.mark()

    def send() -> None:
        b.con(f"AUDIOINJECT {b.args.clip}", "AUDIOINJECT", 10.0)
        b.turn("Make me an image of a small red boat.", 120.0)

    lines = _due_soon_turn(b, seen, "s13", send)
    b.ev.write("errlog.txt", "\n".join(lines))
    if any("re-check handed back - no request went out" in line for line in lines):
        return ("PASS", "the re-check handed back and no request went out")
    return ("BLOCKED", "the timing did not land: no hand-back in the log, so this run proves nothing either way")


def row_s14(b: Bench) -> Grade:
    """A web turn one minute before the re-check, then quiet: one asking line, and that message is answered first."""
    seen = b.mark()
    replies: List[bd.TurnResult] = []
    lines = _due_soon_turn(b, seen, "s14", lambda: replies.append(b.turn("Reply with exactly one word: ok", 120.0)))
    b.ev.write("errlog.txt", "\n".join(lines))
    asking = [line for line in lines if "park: asking" in line]
    fails = []
    if len(asking) != 1:
        fails.append(f"{len(asking)} 'park: asking' lines, want exactly one")
    if not (replies and replies[0].ok):
        fails.append("the message sent before the re-check was not answered")
    return _verdict(fails, "one asking line, and the message was answered first")


def row_b03(b: Bench) -> Grade:
    """A saved park across the update. --phase pre on the v4.6.4 start state before B01's flip; --phase post after it."""
    if b.args.phase == "pre":
        _head_on_cumulo(b)
        parked = _arm_park(b, EMPTY_402)
        b.ev.write("park-pre.txt", parked)
        if "cumulo" not in parked:
            return ("FAIL", "PARK? does not list cumulo on the start state")
        b.snap("state-pre")
        return ("OPEN", "parked on the start state; run B01, then this row with --phase post")
    if b.args.phase != "post":
        raise Blocked("pass --phase pre before the update and --phase post after it")
    seen: set = set()  # the whole log since the update counts
    before = b.snap("state-post")
    lines = _wait_recheck(b, seen, ("answered - open again", "refused again"), "b03")
    b.ev.write("errlog.txt", "\n".join(lines))
    fails = _no_restart(before, b.snap("state-after"))
    if not any("answered - open again" in line or "refused again" in line for line in lines):
        fails.append("the old park did not end with an outcome line within the wait")
    if any("add credits" in line.lower() for line in lines):
        fails.append("an add-credits notice from a clamp")
    return _verdict(fails, "the old park was read and ended by itself")


# ---- human rows and the one with no seam --------------------------------------------------------------------


def row_h23(b: Bench) -> Grade:
    """Two looks in a browser at http://<ip>/. The person types the device token into the page's gate."""
    b.need_human(
        "Open a Telegram photo's chat bubble on the web page. Does it read [photo]? Save a screenshot under the row's folder."
    )
    b.need_human("With the card held busy, reload the web page. Does a 503 still read 'SD card'? Save a screenshot.")
    return ("PASS", "the person saw both; screenshots are the evidence")


def row_v05(b: Bench) -> Grade:
    raise Blocked(
        "needs a real short write on a stalled socket, and http_multipart.cpp has no seam; only the wording is host-tested"
    )


ROWS: Dict[str, Tuple[Callable[[Bench], Grade], bool]] = {
    # row: (function, needs the console)
    "A07": (row_a07, True),
    "A08": (row_a08, False),
    "B03": (row_b03, True),
    "C05": (row_c05, False),
    "C06": (row_c06, True),
    "D03": (row_d03, False),
    "E01": (row_e01, False),
    "H23": (row_h23, False),
    "S10": (row_s10, True),
    "S11": (row_s11, True),
    "S13": (row_s13, True),
    "S14": (row_s14, True),
    "V04": (row_v04, True),
    "V05": (row_v05, False),
}


def run_row(b: Bench, row: str) -> Dict[str, Any]:
    """Run one row and write verdict.json. A row that raises anything but a refusal is BLOCKED with the reason."""
    fn, _ = ROWS[row]
    started = time.time()
    try:
        grade, reason = fn(b)
    except Blocked as exc:
        grade, reason = "BLOCKED", str(exc)
    verdict = {
        "row": row,
        "grade": grade,
        "reason": reason,
        "at": bd.utc_iso(started),
        "seconds": round(time.time() - started, 1),
    }
    b.ev.write("verdict.json", verdict)
    b.say(f"{row}: {grade}: {reason}")
    return verdict


# ---- the command line --------------------------------------------------------------------------------------


def ask_stdin(question: str) -> Optional[bool]:
    if not sys.stdin.isatty():
        return None
    return input(f"{question} [y/n] ").strip().lower().startswith("y")


def parse_args(argv=None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("row", help="a row id, or 'list'")
    ap.add_argument("--ip", default="")
    ap.add_argument("--token-file")
    ap.add_argument("--port")
    ap.add_argument("--mac", default="")
    ap.add_argument("--out", default=".", help="the evidence root; the row writes under <out>/<row>/")
    ap.add_argument("--env-file", default="", help="read by exact key name, never printed")
    ap.add_argument("--a02-state", default="", help="A08: A02's state file")
    ap.add_argument("--phase", default="", help="B03: pre or post")
    ap.add_argument("--member-chat", type=int, default=5151515151, help="C06, V04: the synthetic member chat")
    ap.add_argument("--clip", default="/mem/files/bench500/voice.wav", help="S13: the staged 16 kHz mono clip")
    ap.add_argument("--cuts", type=int, default=10)
    ap.add_argument("--e01-rows", type=int, default=40000)
    ap.add_argument("--album-wait-s", type=float, default=300.0)
    ap.add_argument("--recheck-wait-s", type=float, default=RECHECK_WAIT_S)
    ap.add_argument("--recheck-poll-s", type=float, default=RECHECK_POLL_S)
    ap.add_argument("--recheck-lead-s", type=float, default=29 * 60.0, help="S13, S14: how long after the park to act")
    return ap.parse_args(argv)


def _open_console(args, ev: bd.Evidence):
    port_guard.check_mac(args.mac, "bench_rows")
    port_guard.check_port(args.port, "bench_rows")
    if port_guard.port_for_mac(args.mac, "bench_rows") != args.port:
        raise port_guard.ForbiddenDevice(f"bench_rows: {args.port} is not the board {args.mac}")
    return bd.Console(args.port, ev.path("console.log"), "bench_rows")


def _prepare_run(args):
    """Validate the row's arguments and open what it needs. Returns (api, ev, console), or an exit code."""
    if args.row != "V05" and not (args.ip and args.token_file):
        print("--ip and --token-file are required", file=sys.stderr)
        return 2
    api = bd.Api(args.ip or "0.0.0.0", bd.read_token(args.token_file) if args.token_file else "unset")
    ev = bd.Evidence(os.path.join(args.out, args.row), api.scrub)
    try:
        if args.port:
            return api, ev, _open_console(args, ev)
        if ROWS[args.row][1]:
            print(f"{args.row} needs the test console: pass --port and --mac", file=sys.stderr)
            return 2
    except port_guard.ForbiddenDevice as exc:
        print(exc, file=sys.stderr)
        return port_guard.REFUSED_EXIT
    return api, ev, None


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.row == "list":
        for row, (fn, _) in sorted(ROWS.items()):
            print(f"{row}  {(fn.__doc__ or '').splitlines()[0]}")
        return 0
    if args.row not in ROWS:
        print(f"no row {args.row}; try: bench_rows.py list", file=sys.stderr)
        return 2
    prepared = _prepare_run(args)
    if isinstance(prepared, int):
        return prepared
    api, ev, console = prepared
    try:
        verdict = run_row(Bench(args=args, api=api, ev=ev, console=console, ask=ask_stdin), args.row)
    finally:
        if console:
            console.close()
    return 0 if verdict["grade"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
