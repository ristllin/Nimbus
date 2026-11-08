"""L71 - no panic under combined memory starvation (the S2 L24 finding).

S2's L24 leg on the release candidate (24 Oct, unit A, twice) panicked the device: with
``FAULT sram_ballast`` leaving about 16 KB of internal free and a grown serial-chat
history, one live turn was admitted, its own setup spent the rest (772 B free, 20 B
largest at the provider request, measured 26 Oct), the Wi-Fi task's 32 B internal
allocation for the PHY's PLL timer failed and IDF aborted. The fix keeps the turn's small
heap blocks in PSRAM, re-checks the turn floor before any provider request and ends
the turn with one honest reply when it is not met (nimbus/harness/turn_memory.h).

Bench legs (``hil`` + ``net``, unit A, test build, the live head on Anthropic
``NIMBUS_LAB_MODEL``, default claude-haiku-5-5; a few cents):

1. The L24 recipe, three runs in a row: the ballast leaves 16 KB free, one serial turn
   asks the head to read the device log. Every run ends with a reply in the chat history
   (served, or the honest short-of-memory reply) and the device never restarts: the boot
   number and the stored crash fingerprint stay the same.
2. Under the turn floor from the start, three runs in a row: the ballast takes internal
   free down to 6.5 KB (the floor is 12 KB) before the turn is asked for. Every run ends
   with exactly the honest admission refusal ("One moment, I'm finishing some background
   work and low on working memory. Please resend in a few seconds.") as the chat's history
   row, no provider request, no restart. A squeeze from the start lands on the CUM-404
   admission gate (engine.cpp: the turn is never taken in), whose wording is deliberately
   its own; the "I'm short of memory right now." reply belongs to the DURING-turn gate
   (turn_memory.h), which leg 3 covers. Bench H08 (v5-28813029) failed this leg on the
   wording alone: the device refused honestly, at the right gate.
3. The squeeze arriving at some point during the turn, three runs in a row. Where it lands
   is a race with the setup, and the setup has grown faster, so the leg asserts the class:
   whatever the moment, the turn ends in one reply that is an answer or an honest memory
   ending (classify_end), never silence, a raw dump or a restart.

Why leg 2 no longer squeezes after the turn starts (the 28 Oct RED, ruled on 1 Nov): the
leg squeezed 0.2 s after the turn task took the turn and expected the refusal. The setup
now finishes before that lands: the request check read 17,756 B free and 7,156 B largest,
above the 12,000/5,000 floor, so admitting the turn was correct. The squeeze then hit the
request body, which ended honestly ("the device ran out of working memory", no panic, no
retry storm). The firmware did what it promises; the leg's timing was stale.

The unit's monthly spend count is seeded for the legs when its ceiling is reached and put
back after (SPENDSET, test builds); the head and the ballast are restored in a finally.

Run: NIMBUS_TEST_IP=<unit ip> python3 -m pytest tests/hil/test_l71_combined_starvation.py -m hil --allow-hardware -s
"""

from __future__ import annotations

import os
import re
import time

import pytest

from bench_driver import failure_notice

pytestmark = [pytest.mark.hil, pytest.mark.net]

MODEL = os.environ.get("NIMBUS_LAB_MODEL", "claude-haiku-5-5")
RUNS = int(os.environ.get("NIMBUS_L71_RUNS", "3"))
LEAVE_FREE = 16384  # the S2 L24 recipe: internal free left by the ballast
LEAVE_SLACK = 2048  # the ballast must land within this of the recipe level
SQUEEZE_FREE = 6500  # under the 12000 B turn floor, after the rechecks too
SQUEEZE_DELAY_S = 0.2  # after the turn task took the turn: inside the setup (recall runs ~1-2 s)
TURN_BUDGET_S = 180.0
SHORT_REPLY = "I'm short of memory right now. Please try again in a minute."
# The CUM-404 admission gate's refusal (engine.cpp handleMessage): a turn asked for
# while the heap is already under the floor is never taken in, and this is its
# wording. Distinct from SHORT_REPLY on purpose: the during-turn gate
# (turn_memory.h) refuses a turn already running.
ADMIT_REPLY = (
    "One moment, I'm finishing some background work and low on working memory. Please resend in a few seconds."
)
PROMPT = "CALL the device.log tool with scan_kb 64 and lines 60 now, then tell me what the oldest lines say."
BALLAST_RE = re.compile(r"FAULT sram_ballast=(\d+) held=(\d+) intFree=(\d+) intLargest=(\d+)")
OVERALL_RE = re.compile(
    r'"limitCents":(\d+),"spentCents":(\d+),"frozen":(true|false)(?:,"ownerSet":\w+,"active":(true|false))?'
)


# The honest endings of a turn that ran short of memory, by their fixed opening words
# (engine.cpp, orch_head_loop.cpp). A turn that outran the squeeze just answers.
MEMORY_ENDINGS = (
    "That didn't finish - the device ran out of working memory",
    "That didn't finish - the device could not finish sending its request",
)
# What must never reach a person, under any pressure.
NOT_A_REPLY = ('{"reply"', '"session_ops"', '"mem_write"', "placeholder")


def classify_end(reply: str) -> str:
    """How a squeezed turn ended: "refused" (before any provider request, at either
    memory gate), "memory" (an honest ending part way), "failed" (the device's failure
    notice for any other cause, such as a provider it could not reach), "served" (an
    answer), or "bad" (nothing a person can read). "failed" is lane SNI's: on bf40a3ed
    every reply here was "That didn't finish - anthropic could not be reached ..." and
    this leg read it as served."""
    text = (reply or "").strip()
    if not text or any(mark in text for mark in NOT_A_REPLY):
        return "bad"
    if text == SHORT_REPLY or text == ADMIT_REPLY:
        return "refused"
    if text.startswith(MEMORY_ENDINGS):
        return "memory"
    if failure_notice(text):
        return "failed"
    return "served"


def _state(net) -> dict:
    return net.get_json("/api/state", timeout=10.0)


def _identity(net) -> tuple:
    st = _state(net)
    boot = st.get("boot") if isinstance(st.get("boot"), dict) else {}
    crash = boot.get("crash") if isinstance(boot.get("crash"), dict) else {}
    return boot.get("n"), crash.get("fp"), int(st.get("uptimeMs") or 0)


# How long a history read keeps retrying a busy card before the leg fails on it.
HISTORY_BUSY_BUDGET_S = 30.0


def history_page(status: int, read_json) -> "dict | None":
    """The chat-history page of one /api/mem/episodic answer, or None when the answer
    is not one: since v4.6.1 (HFW) the route tries the card lock for 150 ms and answers
    503 busy while a turn holds it, and the admission check answers a text/plain busy
    under the ballast. A busy answer is "retry", never an empty history: read as {}, it
    lowered the leg's id floor to nothing (every earlier reply counted as new) or
    emptied the re-read of a reply that had landed."""
    if status != 200:
        return None
    try:
        page = read_json()
    except ValueError:  # a text/plain busy answer
        return None
    return page if isinstance(page, dict) and isinstance(page.get("messages"), list) else None


def assistant_rows(page: dict) -> dict:
    return {
        str(r.get("id")): str(r.get("text") or "")
        for r in page["messages"]
        if r.get("role") == "assistant" and r.get("kind", "message") == "message"
    }


def _replies(net, budget_s: float = HISTORY_BUSY_BUDGET_S) -> dict:
    """The serial chat's assistant replies, retried while the card is busy."""
    end = time.time() + budget_s
    while True:
        r = net.get("/api/mem/episodic?session=serial&kind=message&limit=20", timeout=10.0)
        page = history_page(r.status_code, r.json)
        if page is not None:
            return assistant_rows(page)
        if time.time() >= end:
            raise AssertionError(
                f"/api/mem/episodic stayed busy for {budget_s:.0f}s: HTTP {r.status_code} {r.text[:120]!r}"
            )
        time.sleep(1.0)


def _new_reply(net, before: dict, timeout: float) -> str:
    """The turn's one new reply row (ids above everything the snapshot held)."""
    floor = max(before) if before else ""
    end = time.time() + timeout
    while time.time() < end:
        try:
            got = {k: v for k, v in _replies(net).items() if k not in before and k > floor}
        except Exception:  # noqa: BLE001 - a starved device answers slowly: keep polling
            got = {}
        if got:
            time.sleep(6.0)  # a second reply from the same turn would land here
            got = {k: v for k, v in _replies(net).items() if k not in before and k > floor}
            assert len(got) == 1, f"one turn must leave exactly one reply row, got {list(got.values())!r}"
            return next(iter(got.values()))
        time.sleep(3.0)
    pytest.fail("the turn left no reply row in the chat history (silence is not an honest end)")


def _ballast(device, hold: int) -> tuple:
    out = device.cmd(f"FAULT sram_ballast {hold}", expect="FAULT sram_ballast", timeout=10.0)
    m = BALLAST_RE.search(out)
    if not m:
        pytest.skip(f"FAULT sram_ballast is not bound in this build ({out.strip()!r})")
    return int(m.group(3)), int(m.group(4))


def _seed_spend(device):
    """Room under the monthly spend ceiling: when it is active and (nearly) reached,
    seed this month's count to 0 and return the count to put back (None: untouched)."""
    m = OVERALL_RE.search(device.cmd("SPENDSET ?", expect="SPENDSET {", timeout=8.0))
    if not m or m.group(4) == "false" or not int(m.group(1)):
        return None
    limit, spent, frozen = int(m.group(1)), int(m.group(2)), m.group(3) == "true"
    if not frozen and limit - spent >= 300:
        return None
    device.cmd("SPENDSET 0", expect="SPENDSET {", timeout=8.0)
    return spent


def _restore_spend(device, seeded) -> None:
    """The seeded count back, plus what the leg itself spent."""
    if seeded is None:
        return
    now = OVERALL_RE.search(device.cmd("SPENDSET ?", expect="SPENDSET {", timeout=8.0))
    device.cmd(f"SPENDSET {seeded + (int(now.group(2)) if now else 0)}", expect="SPENDSET {", timeout=8.0)


@pytest.fixture
def starvation(device, net):
    """The live head on Anthropic MODEL with room under the monthly spend ceiling; the
    head, the spend count and the ballast put back however the leg ends."""
    orch = net.get_json("/api/orch", timeout=10.0)
    anthropic = (orch.get("providers") or {}).get("anthropic") or {}
    if not anthropic.get("hasKey"):
        pytest.skip("the unit has no Anthropic key for the live head")
    restore = {"orchHost": orch.get("orchHost") or "", "convReset": "1"}
    if anthropic.get("orchModel"):
        restore["orchM_anthropic"] = anthropic["orchModel"]
    seeded = _seed_spend(device)
    try:
        r = net.post("/api/orch", {"orchHost": "anthropic", "orchModel": MODEL, "convReset": "1"}, timeout=10.0)
        assert r.status_code == 200, f"head -> anthropic: HTTP {r.status_code}"
        yield
    finally:
        try:
            device.cmd("FAULT sram_ballast 0", expect="FAULT sram_ballast", timeout=10.0)
        finally:
            net.post("/api/orch", restore, timeout=10.0)
            _restore_spend(device, seeded)


def _episode(device, net, squeeze: bool, first: bool = False) -> str:
    """One run of the recipe; returns the turn's reply row. Fails on a restart. `squeeze`
    takes internal free under the turn floor during the turn, `first` before it starts."""
    boot0, fp0, up0 = _identity(net)
    free = int(_state(net).get("heap") or 0)
    assert free > 0, "the state carries no internal free figure"
    hold = max(0, free - LEAVE_FREE)
    free_after, _ = _ballast(device, hold)
    if abs(free_after - LEAVE_FREE) > LEAVE_SLACK:  # the heap moved between the read and the ballast
        hold = max(0, hold + free_after - LEAVE_FREE)
        free_after, _ = _ballast(device, hold)
    assert abs(free_after - LEAVE_FREE) <= LEAVE_SLACK, (
        f"the ballast left {free_after} B free, not the recipe's {LEAVE_FREE} B"
    )
    before = _replies(net)
    tighter = hold + max(0, free_after - SQUEEZE_FREE)
    try:
        if first:
            _ballast(device, tighter)
        device.cmd(f"TURN {PROMPT}", expect="TURN <-", timeout=6.0)
        if squeeze or first:
            if not first:
                try:  # the turn task took the turn
                    device.expect("local turn from serial", timeout=15.0)
                except Exception:  # noqa: BLE001 - a drained line: squeeze on the clock instead
                    pass
                time.sleep(SQUEEZE_DELAY_S)
                _ballast(device, tighter)
            try:  # the turn's end on the console (a drained line just means it already passed)
                device.expect_re(r"turn ended for memory|turn host=", timeout=60.0)
            except Exception:  # noqa: BLE001 - the reply row below is the graded evidence
                pass
            _ballast(device, hold)  # the squeeze starves the web server too: back to the recipe
        reply = _new_reply(net, before, TURN_BUDGET_S)
    finally:
        _ballast(device, 0)
    boot1, fp1, up1 = _identity(net)
    assert boot1 == boot0 and fp1 == fp0 and up1 > up0, (
        f"the device restarted under the starvation: boot {boot0}->{boot1}, crash {fp0}->{fp1}, uptime {up0}->{up1}"
    )
    return reply


def test_l71_l24_recipe_runs_three_times_without_a_panic(device, net, starvation):
    for i in range(RUNS):
        reply = _episode(device, net, squeeze=False)
        print(f"\n[l71] L24 recipe run {i + 1}/{RUNS}: {reply[:120]!r}")
        assert reply.strip(), "an empty reply row"
        assert classify_end(reply) != "failed", f"run {i + 1}: the turn failed: {reply[:160]!r}"


def test_l71_under_the_floor_from_the_start_is_refused_three_times(device, net, starvation):
    for i in range(RUNS):
        reply = _episode(device, net, squeeze=False, first=True)
        print(f"\n[l71] under the floor run {i + 1}/{RUNS}: {reply[:120]!r}")
        # The squeeze lands before the turn, so the CUM-404 admission gate refuses
        # it (engine.cpp) with its own exact wording; SHORT_REPLY is the during-turn
        # gate's, covered by the squeeze leg below. Still exact, still a refusal.
        assert reply == ADMIT_REPLY, f"run {i + 1}: not the honest admission refusal: {reply[:160]!r}"


def test_l71_a_squeeze_during_the_turn_ends_honestly_three_times(device, net, starvation):
    for i in range(RUNS):
        reply = _episode(device, net, squeeze=True)
        kind = classify_end(reply)
        print(f"\n[l71] squeeze run {i + 1}/{RUNS} ({kind}): {reply[:120]!r}")
        assert kind != "bad", f"run {i + 1}: nothing a person can read: {reply[:160]!r}"
        assert kind != "failed", f"run {i + 1}: the turn failed for a cause other than memory: {reply[:160]!r}"
