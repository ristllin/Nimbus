"""L44 - memory writes survive an embedding outage (program-2026-10-18, lane P).

On 13-14 Oct 2026 an OpenAI credit outage refused every embedding call, and the
device threw away 8 facts the owner had shared while the turns still said "ok".
The fix keeps such a write as text in a durable pending queue and stores it once
embeddings work again. This leg proves that on the board, with the test console's
``FAULT embed_fail on`` (only the embedding call fails, with the recorded
``HTTP 429 credit_balance_exhausted`` refusal; the reply model still answers):

1. a turn that asks to remember a fact queues it: ``/api/mem/stats`` shows
   ``pending`` up by 1 and the vector count unchanged;
2. it survives a restart (the boot line reads ``pending=<n>``; the fault is RAM
   only, so the restarted board has embeddings back);
3. the drain stores it: the vector count grows by exactly 1 and ``pending``
   returns to where it was, with the drain's stack headroom logged;
4. while parked, a turn makes no recall embed attempt (``recall skipped -
   embeddings parked``, never ``recall embed failed``);
5. a write failure mid-drain (``FAULT sd_io``, the power-cut stand-in) loses
   and duplicates nothing;
6. the duplicate rule: a fact already stored is answered "duplicate" during the
   outage and never queued, and a fact written twice while waiting waits once;
7. the drain waits while internal memory is under the admission gate
   (``FAULT sram_ballast``) and runs once it is released.

Every test writes its own fact, made unique by a per-run nonce and a distinct
topic: the vector store on the card persists across tests and runs, and a fact it
already holds is (correctly) a duplicate, not a new write. The module removes the
vectors it created when it ends, so a rerun starts from the same store.

Needs: the test build, Orchestrator mode, a keyed head, a configured embedder
whose key has credit (the drain legs check it first and fail at once, naming the
real refusal, when it does not), and the LAN (``NIMBUS_TEST_IP`` +
``NIMBUS_TEST_TOKEN``). Spend: about six short head turns.
"""

from __future__ import annotations

import json
import re
import time
import uuid

import pytest

from device import Device, ExpectTimeout
from webrig import ip_tok_or_skip, make_session, require_orchestrator

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

RUN = uuid.uuid4().hex[:6]  # this run's nonce: no earlier run stored these facts
# One topic per test, so two facts of the same run are never near-duplicates.
FACTS = {
    "restart": "my neighbour Priya waters the tomatoes on Tuesdays",
    "parked": "the spare bike pump hangs behind the garage door",
    "sd_io": "my favourite tea is smoked lapsang souchong",
    "stack": "the dishwasher salt lives in the cupboard under the sink",
    "gate": "the attic ladder squeaks unless it is oiled in spring",
}
# Words only this module's facts carry: teardown deletes a new vector only when it
# is one of ours (the nonce, or a topic word if the head left the nonce out).
MARKERS = ("tomatoes", "bike pump", "lapsang", "dishwasher salt", "attic ladder")
# The drain waits a minute after boot and then runs at most every 20 s.
DRAIN_WAIT_S = 180.0
STACK_FLOOR_B = 4096
GATE_FREE_B = 12000  # the drain's admission gate (internal free heap)
BALLAST_FREE_B = 7000  # what the gate leg leaves free: well under the gate, over the ballast's 4 KB floor
ALIVE_HEAP = re.compile(r"ORCH alive .*\bheap=(\d+)")  # the console's periodic internal free heap


def _fact_turn(topic: str) -> str:
    return f"Please remember this for later (bench note {RUN}): {FACTS[topic]}."


@pytest.fixture(scope="module")
def lan():
    ip, tok = ip_tok_or_skip("L44")
    s = make_session(tok)
    require_orchestrator(s, ip, "L44")
    before = set(_vectors((s, ip)))
    yield s, ip
    # Leave the store as found: remove the vectors this module wrote, and nothing
    # else (a memory someone else saved meanwhile stays).
    for vid, content in _vectors((s, ip)).items():
        if vid not in before and _ours(content):
            s.post(f"http://{ip}/api/mem/vector", data={"op": "delete", "id": vid}, timeout=10)


@pytest.fixture(scope="module")
def funded_embedder(device: Device, lan):
    """The legs that wait for a drain need the REAL embedder to work: the injector
    only fakes the outage, and recovery needs a real embedding. Checked once through
    /api/mem/embedverify, which calls the provider directly (FAULT embed_fail does
    not touch it), so a real refusal fails these legs at once and says so instead
    of timing out in every drain wait.

    Then the ground is cleared for this run: facts an interrupted earlier run left
    waiting are drained, and earlier runs' facts are removed. Either would collide
    with this run's facts, because a near-duplicate is (correctly) not stored twice."""
    s, ip = lan
    emb = _stats(lan).get("embed") or {}
    r = s.post(
        f"http://{ip}/api/mem/embedverify",
        data={"provider": emb.get("provider", ""), "model": emb.get("model", ""), "dims": emb.get("dims", 0)},
        timeout=30,
    )
    if r.status_code != 200:
        pytest.fail(
            f"the real embedding provider refuses ({r.text[:200]}): fund or fix the embedding key. "
            "This is not FAULT embed_fail (the check bypasses it); no drain can complete until it works."
        )
    _fault(device, False)  # ends any pause, so leftovers drain now
    left = _stats(lan)["pending"]
    if left:
        _wait_stats(
            lan, lambda st: st["pending"] == 0, 90.0 + 25.0 * left, f"{left} leftover queued fact(s) did not drain"
        )
    for vid, content in _vectors(lan).items():
        if _ours_any_run(content):
            s.post(f"http://{ip}/api/mem/vector", data={"op": "delete", "id": vid}, timeout=10)


def _ours_any_run(content: str) -> bool:
    low = content.lower()
    return (
        "bench note" in low
        or low.startswith(("l44 duplicate rule", "l44 coalesce rule"))
        or any(m in low for m in MARKERS)
    )


def _ours(content: str) -> bool:
    low = content.lower()
    return RUN in low or any(m in low for m in MARKERS)


def _vectors(lan) -> dict:
    """id -> content for every stored vector."""
    s, ip = lan
    out, offset = {}, 0
    while True:
        r = s.get(f"http://{ip}/api/mem/vector", params={"limit": 100, "offset": offset}, timeout=15)
        assert r.status_code == 200, f"/api/mem/vector -> {r.status_code}"
        rows = r.json().get("entries", [])
        out.update((e["id"], e.get("content", "")) for e in rows)
        if len(rows) < 100:
            return out
        offset += 100


def _stats(lan) -> dict:
    s, ip = lan
    r = s.get(f"http://{ip}/api/mem/stats", timeout=10)
    assert r.status_code == 200, f"/api/mem/stats -> {r.status_code}"
    return r.json()


def _fault(dev: Device, on: bool) -> str:
    line = dev.cmd(f"FAULT embed_fail {'on' if on else 'off'}", "FAULT embed_fail", timeout=8.0)
    assert "not wired" not in line, f"FAULT embed_fail is not wired in this build: {line}"
    assert f"embed_fail={1 if on else 0}" in line, line
    return line


def _write(lan, content: str) -> str:
    """memory.write as the owner (the web surface), with exact text: no head turn."""
    s, ip = lan
    body = json.dumps(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": "memory.write", "arguments": {"content": content}},
        }
    )
    r = s.post(f"http://{ip}/api/test/astool", data={"chat": "web", "body": body}, timeout=25)
    assert r.status_code == 200, r.text
    return json.dumps(r.json())


def _wait_stats(lan, pred, timeout: float, what: str) -> dict:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = _stats(lan)
        if pred(last):
            return last
        time.sleep(3.0)
    pytest.fail(
        f"{what} within {timeout:.0f} s (last stats: pending={last.get('pending')} vectors={last.get('vectors')})"
    )


def _lines_until(dev: Device, substr: str, timeout: float) -> list:
    """Every console line up to and including the first one containing ``substr``."""
    deadline = time.time() + timeout
    out = []
    while True:
        line = dev._readline(deadline)
        if line is None:
            raise ExpectTimeout(substr, dev._transcript)
        out.append(line)
        if substr in line:
            return out


def _lines_for(dev: Device, seconds: float) -> list:
    """Every console line seen in the next ``seconds``."""
    deadline = time.time() + seconds
    out = []
    while True:
        line = dev._readline(deadline)
        if line is None:
            return out
        out.append(line)


def _wait_idle(lan, timeout: float = 120.0) -> None:
    """No turn in flight and no sub-agent running on two polls in a row (GET
    /api/chat?watch=1 busy), then a short settle."""
    s, ip = lan
    deadline, quiet = time.time() + timeout, 0
    while time.time() < deadline:
        busy = int(s.get(f"http://{ip}/api/chat", params={"watch": "1"}, timeout=10).json().get("busy") or 0)
        quiet = quiet + 1 if busy == 0 else 0
        if quiet >= 2:
            time.sleep(3.0)
            return
        time.sleep(2.0)
    pytest.fail(f"the device did not go idle within {timeout:.0f}s")


def _queue_one(dev: Device, lan, base: dict, topic: str) -> dict:
    """Fault on, one turn that asks to remember this test's fact; returns the stats with it queued."""
    _fault(dev, True)
    dev.turn(_fact_turn(topic))
    try:
        dev.expect("ORCH REPLY", timeout=90.0)
    except ExpectTimeout:
        pytest.fail("the remember turn never replied (the head should still answer during an embedding outage)")
    st = _wait_stats(
        lan,
        lambda s: s["pending"] >= base["pending"] + 1,
        30.0,
        f"the '{topic}' fact was not queued (if the store already held a near-duplicate, the write is "
        "correctly a duplicate: check /api/mem/vector for a leftover from an interrupted run)",
    )
    assert st["vectors"] == base["vectors"], "a write during the outage must not reach the vector store"
    assert st["embedParked"], "embeddings should read as parked during the outage"
    return st


def test_queued_write_survives_restart_and_drains(device: Device, lan, funded_embedder):
    dev = device
    base = _stats(lan)
    assert "pending" in base, "/api/mem/stats has no pending field (old firmware?)"
    queued = _queue_one(dev, lan, base, "restart")

    # Restart while queued: the queue is durable; the fault is RAM only.
    dev.reboot_and_confirm(timeout=40.0)
    st = _stats(lan)
    assert st["pending"] == queued["pending"], f"the queue did not survive the restart: {st['pending']}"
    assert st["vectors"] == base["vectors"]

    # The drain stores it: exactly one more vector, the queue back to its base.
    done = _wait_stats(
        lan,
        lambda s: s["pending"] == base["pending"] and s["vectors"] >= base["vectors"] + 1,
        DRAIN_WAIT_S,
        "the drain did not store the queued fact",
    )
    assert done["vectors"] == base["vectors"] + 1, (
        f"the drain stored {done['vectors'] - base['vectors']} vectors, want 1"
    )
    assert not done["embedParked"], "the park should have ended with the successful drain"


def test_parked_turn_makes_no_recall_embed(device: Device, lan, funded_embedder):
    dev = device
    base = _stats(lan)
    _queue_one(dev, lan, base, "parked")  # the first refusal parks embeddings
    try:
        dev.turn("What is two plus two? Answer with one number.")
        lines = _lines_until(dev, "ORCH REPLY", timeout=90.0)
    except ExpectTimeout:
        pytest.fail("the parked turn never replied")
    turn_log = "\n".join(lines)
    assert "memory: recall skipped - embeddings parked" in turn_log, "a parked turn did not skip its recall embed"
    assert "recall embed failed" not in turn_log, "a parked turn still paid a recall embed"
    assert "embed failed" not in turn_log, "a parked turn still made an embedding call"
    _fault(dev, False)
    _wait_stats(
        lan, lambda s: s["pending"] == base["pending"], DRAIN_WAIT_S, "the queue did not drain after the fault cleared"
    )


def test_write_failure_mid_drain_loses_and_duplicates_nothing(device: Device, lan, funded_embedder):
    """FAULT sd_io makes the vector persist fail during the drain step (the power
    cut between the vector write and the queue write): the entry must stay queued,
    and once storage is back exactly one vector lands."""
    dev = device
    base = _stats(lan)
    if not base.get("sdPresent"):
        pytest.skip("needs an SD card (unit A)")
    _queue_one(dev, lan, base, "sd_io")
    dev.cmd("FAULT sd_io on", "FAULT", timeout=8.0)
    try:
        _fault(dev, False)  # embeddings back: the next drain step embeds, then cannot persist
        try:
            line = dev.expect_re(r"memory: pending drain (not-durable|stored)", timeout=DRAIN_WAIT_S)
        except ExpectTimeout:
            pytest.fail("no drain step ran with storage failing")
        assert "not-durable" in line.group(0), f"a drain claimed stored while storage failed: {line.group(0)}"
        st = _stats(lan)
        assert st["pending"] == base["pending"] + 1, "the entry left the queue before its vector was durable"
    finally:
        dev.cmd("FAULT sd_io off", "FAULT", timeout=8.0)
        # The power cut, and the way back from a demoted card even when an assertion
        # above failed: a restart (it also clears the RAM-only embed fault).
        dev.reboot_and_confirm(timeout=40.0)
    # The replay stores it once (or only pops it).
    done = _wait_stats(
        lan, lambda s: s["pending"] == base["pending"], DRAIN_WAIT_S, "the queue did not drain after the restart"
    )
    assert done["vectors"] == base["vectors"] + 1, (
        f"want exactly one stored vector, got {done['vectors'] - base['vectors']}"
    )


def test_drain_step_keeps_stack_headroom(device: Device, lan, funded_embedder):
    """Bench: a drain step on the Telegram task keeps at least 4 KB of stack free."""
    dev = device
    base = _stats(lan)
    _queue_one(dev, lan, base, "stack")
    _fault(dev, False)
    try:
        m = dev.expect_re(r"memory: pending drain stored \S+ \(left=\d+, stack free (\d+) B\)", timeout=DRAIN_WAIT_S)
    except ExpectTimeout:
        pytest.fail("no drain step was logged")
    free = int(m.group(1))
    assert free >= STACK_FLOOR_B, f"drain step left {free} B of stack, want >= {STACK_FLOOR_B}"


def test_duplicate_fact_is_neither_queued_nor_stored_twice(device: Device, lan, funded_embedder):
    """The duplicate rule, with exact text (no head): a fact already stored is
    answered "duplicate" during the outage and never queued; a fact written twice
    while it waits is queued once and stored once."""
    dev = device
    base = _stats(lan)
    stored = f"L44 duplicate rule {RUN}: the hallway clock runs five minutes fast"
    waiting = f"L44 coalesce rule {RUN}: the garden hose reel is in the shed"
    _fault(dev, False)
    out = _write(lan, stored)
    assert "stored memory" in out, f"the first write was not stored: {out}"
    _fault(dev, True)
    try:
        again = _write(lan, stored)
        assert "duplicate of an existing memory" in again, f"a stored fact was not a duplicate: {again}"
        first, second = _write(lan, waiting), _write(lan, waiting)
        assert "queued memory" in first and "queued memory" in second, (first, second)
        st = _stats(lan)
        assert st["pending"] == base["pending"] + 1, "the duplicate was queued, or the waiting fact twice"
        assert st["vectors"] == base["vectors"] + 1
    finally:
        _fault(dev, False)
    done = _wait_stats(
        lan, lambda s: s["pending"] == base["pending"], DRAIN_WAIT_S, "the queue did not drain after the fault cleared"
    )
    assert done["vectors"] == base["vectors"] + 2, f"want 2 new vectors, got {done['vectors'] - base['vectors']}"


def test_drain_waits_below_the_admission_gate(device: Device, lan, funded_embedder):
    """Bench: with internal free memory under the drain's admission gate (held by
    the multi-block FAULT sram_ballast), no drain step runs; released, it does.
    The ballast goes on BEFORE the outage ends, so no drain can slip in first, and
    nothing talks HTTP while it is held: the console is the only channel."""
    dev = device
    base = _stats(lan)
    _queue_one(dev, lan, base, "gate")  # queued and parked
    # Size the ballast only once the remember turn has fully ended: memory its tail
    # still held (the release run sized it on ORCH REPLY) came back inside the window,
    # lifted free memory over the gate and the drain was rightly admitted.
    _wait_idle(lan)
    free = int(dev.cmd_re("FAULT sram_ballast 0", r"intFree=(\d+)", timeout=8.0).group(1))
    held = dev.cmd_re(f"FAULT sram_ballast {max(free - BALLAST_FREE_B, 0)}", r"held=(\d+) intFree=(\d+)", timeout=8.0)
    try:
        if int(held.group(2)) >= GATE_FREE_B:
            pytest.fail(f"the ballast could not take free memory under {GATE_FREE_B} B: {held.group(0)}")
        _fault(dev, False)  # embeddings back and the park cleared: only the gate holds the drain now
        lines = _lines_for(dev, 60.0)
        heaps = [int(m.group(1)) for m in (ALIVE_HEAP.search(ln) for ln in lines) if m]
        assert heaps and max(heaps) < GATE_FREE_B, (
            f"the leg lost its precondition: free memory read {max(heaps) if heaps else 'nothing'} B in the "
            f"window, not under the {GATE_FREE_B} B gate (the ballast did not hold it down)"
        )
        assert not any("memory: pending drain" in ln for ln in lines), (
            "a drain step ran below the admission gate:\n" + "\n".join(ln for ln in lines if "pending drain" in ln)
        )
    finally:
        dev.cmd("FAULT sram_ballast 0", "FAULT sram_ballast", timeout=8.0)
        _fault(dev, False)
    done = _wait_stats(
        lan,
        lambda s: s["pending"] == base["pending"],
        DRAIN_WAIT_S,
        "the drain did not run once the ballast was released",
    )
    assert done["vectors"] == base["vectors"] + 1


def test_boot_line_names_the_queue(device: Device, lan):
    """The boot line names the queue the device reports (pending=<n>, before the
    drain's first-minute grace ends). The fresh-device case (no queue file) is
    covered by the host suite."""
    dev = device
    dev.reboot_and_confirm(timeout=40.0)
    reported = _stats(lan)["pending"]
    pat = re.compile(r"memory: ready \(.*pending=(\d+)\)")
    # The line usually prints before the boot is confirmed: look in what the reboot
    # already read (after this boot's reset line) before waiting for more.
    # (No reset line seen: an older boot's line could match, so only wait for a new one.)
    since = max((i for i, ln in enumerate(dev._transcript) if "rst:" in ln), default=None)
    fresh = dev._transcript[since + 1 :] if since is not None else []
    m = next((pat.search(ln) for ln in fresh if pat.search(ln)), None)
    if m is None:
        try:
            m = dev.expect_re(pat.pattern, timeout=40.0)
        except ExpectTimeout:
            pytest.skip("the boot line scrolled past before the console reopened (see the durable log)")
    assert int(m.group(1)) == reported, f"boot line says pending={m.group(1)}, /api/mem/stats says {reported}"
