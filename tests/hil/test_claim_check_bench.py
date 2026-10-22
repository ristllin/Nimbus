"""Lane S bench legs: the claim check on a real device (the fill unit).

The head runs on the LAN mock (the custom endpoint, single-shot structured
turn), so every reply is fixed and the legs cost nothing. The device must hold
each reply's claim against what the turn really did:

  C1  a memory write that does not land (``FAULT embed_fail on``: the embedding
      outage queues it, so it is not saved yet) under a reply that says "Saved
      it to memory." -> the turn delivers exactly one reply, carrying
      "(Correction: that has not been saved.)", and ``claimsTailed`` goes up.
      The mock writes one fixed fact and the store is content-addressed: once a
      run's queued copy drains (``FAULT embed_fail off``), the same write is a
      duplicate, which IS saved, so the reply is true and gets no line (unit L,
      22 Oct 2026, after the 21 Oct runs). The leg forgets that fact before the
      turn and after the queue drains, so it repeats on the same unit;
  C2  the scratchpad route: the scratchpad lives in a flash file (v4.5.14), so a
      full NVS partition (``NVSFILL``) still saves and the result carries no
      note; a failed flash write (``FAULT scratch_fail on``) answers 507 with
      the result "... kept until restart only (saving failed)"; once the fault
      ends the notes land and the note is gone;
  C3  a durability claim ("so it will survive a restart"), the scenario set
      while NVS has room (a config save at a full NVS answers 507): at a full
      NVS the scratchpad still saves to flash, so no line and ``claimsTailed``
      unchanged; with the flash write failing too (``FAULT scratch_fail on``)
      the RAM-only note rides the reply and ``claimsTailed`` goes up; C4 the
      same turn with room -> no line and ``claimsTailed`` unchanged.

C1 needs lane P's ``FAULT embed_fail`` binding and C2/C3 ``FAULT scratch_fail``; on
a build without one the leg is skipped with that reason (never passed). Unit:
the fill unit (unit L).

Run: python3 -m pytest tests/hil/test_claim_check_bench.py -m "hil and net" --allow-hardware
"""

from __future__ import annotations

import os
import time

import pytest

from mock_llm import CLAIM_TURNS, mock_llm  # noqa: F401  (mock_llm is a fixture)
from test_l12_mock_llm import _opost, custom_backend  # noqa: F401  (fixture + helper)

pytestmark = [pytest.mark.hil, pytest.mark.net]

SAVED_LINE = "(Correction: that has not been saved.)"
RAM_LINE = "(Note: this is kept only until the next restart; saving it permanently failed.)"
RAM_TOOL = "kept until restart only (saving failed)"
FILL_CHUNK = int(os.environ.get("NIMBUS_S_FILL_CHUNK", "400"))
# The chat a serial TURN runs in (main.cpp: injectMessage("serial", ...)).
TURN_CHAT = "serial"
# After the first reply row appears, how long to wait for a second one from the
# same turn (a turn must deliver exactly one reply).
SETTLE_S = 6.0
# The one fact the mock's "mock-claim-saved" turn writes.
CLAIM_FACT = CLAIM_TURNS["mock-claim-saved"]["mem_write"][0]["content"]
# After FAULT embed_fail off the queue drains one step per 20 s (memory_subsystem.cpp
# kDrainEveryMs); a unit whose embedding provider is down keeps it queued.
DRAIN_WAIT_S = 60.0


def assistant_replies(page: dict) -> dict:
    """id -> text of the assistant replies on one /api/mem/episodic page. The API
    answers newest first; ids are unique, so a new reply is a new id, wherever it
    sits on the page and even when its text repeats an earlier reply."""
    return {
        str(r.get("id")): str(r.get("text") or "")
        for r in page.get("messages", [])
        if r.get("role") == "assistant" and r.get("kind", "message") == "message"
    }


def new_replies(before: dict, after: dict) -> dict:
    return {k: v for k, v in after.items() if k not in before}


def the_one_reply(got: dict, model: str) -> str:
    """The turn's reply: exactly one new reply row, never none and never two."""
    if not got:
        pytest.fail(f"no delivered reply recorded for {model} in the {TURN_CHAT} chat")
    assert len(got) == 1, f"one turn must deliver exactly one reply, got {len(got)}: {list(got.values())!r}"
    return next(iter(got.values()))


def _replies(be) -> dict:
    q = f"/api/mem/episodic?session={TURN_CHAT}&kind=message&limit=20&t={be.tok}"
    return assistant_replies(be.net.get_json(q, ip=be.ip, timeout=8.0))


def _tailed(be) -> int:
    return int(be.state().get("claimsTailed", -1))


def _turn(be, model: str, text: str, timeout: float = 90.0, switch: bool = True) -> str:
    """One head turn on the mock scenario `model`; returns the one reply it delivered.
    switch=False keeps the scenario already set: switching is a persisted config
    save, which answers 507 at a full NVS, so a full-NVS turn switches before the fill."""
    if switch:
        be.set_model(model)
    else:
        be.server.reset()
    before = _replies(be)
    try:
        be.device.cmd(f"TURN {text}", expect="TURN <-", timeout=6.0)
    except Exception:  # noqa: BLE001 - the echo is advisory; the turn still fires
        pass
    assert be.server.wait_for_request(model, count=1, timeout=timeout), f"the head never reached {model}"
    end = time.time() + timeout
    while time.time() < end and not new_replies(before, _replies(be)):
        time.sleep(2.0)
    time.sleep(SETTLE_S)  # a second reply from the same turn would land in this window
    return the_one_reply(new_replies(before, _replies(be)), model)


def fact_ids(page: dict, fact: str) -> list:
    """Ids of the stored copies of ``fact`` on one /api/mem/vector browse page."""
    return [str(e.get("id")) for e in page.get("entries", []) if e.get("content") == fact]


def _stored_fact_ids(be) -> list:
    ids, offset = [], 0
    while True:
        page = be.net.get_json(f"/api/mem/vector?limit=100&offset={offset}", ip=be.ip, timeout=10.0)
        ids += fact_ids(page, CLAIM_FACT)
        offset += 100
        if offset >= int(page.get("total", 0)):
            return ids


def _forget_fact(be) -> None:
    """Delete every stored copy of the mock's fact, and check none is left."""
    for fid in _stored_fact_ids(be):
        r = be.net.post("/api/mem/vector", {"op": "delete", "id": fid}, ip=be.ip, timeout=10.0)
        assert r.status_code == 200 and r.json().get("ok"), f"delete {fid} -> {r.status_code} {r.text[:120]!r}"
    left = _stored_fact_ids(be)
    assert not left, f"the leg's fact is still stored after the delete: {left}"


def _drain_then_forget(be, strict: bool) -> None:
    """After FAULT embed_fail off: let the queued fact drain into the store, then
    forget it, so the leg leaves no fact behind. A queue that does not drain in time
    (no working embedding provider) keeps the fact queued, where the next run's write
    coalesces with it and still gets the correction. ``strict`` False (the leg already
    failed) reports a cleanup failure instead of raising over the real one."""
    try:
        end = time.time() + DRAIN_WAIT_S
        while time.time() < end and int(be.net.get_json("/api/mem/stats", ip=be.ip, timeout=8.0).get("pending", 0)):
            time.sleep(4.0)
        _forget_fact(be)
    except Exception as exc:  # noqa: BLE001 - cleanup after a failed leg must not hide it
        if strict:
            raise
        print(f"[C1] cleanup after a failed leg: {exc}")


def _fill_nvs(device) -> None:
    for _ in range(40):
        out = device.cmd(f"NVSFILL {FILL_CHUNK}", expect="NVSFILL", timeout=20.0)
        if "full=1" in out:
            return
    pytest.fail("NVSFILL never reported a full partition")


def _free_nvs(device) -> None:
    device.cmd("NVSFILL 0", expect="NVSFILL", timeout=20.0)


def scratch_result(status: int, body: dict) -> tuple:
    """(status, the tool's result) of one scratchpad route answer: 200 carries the
    result as msg, and so does the 507 for a change whose save did not land (the
    change is applied until restart). Anything else is a refusal of the request."""
    assert status in (200, 507), f"scratchpad route -> {status} {body!r}"
    return status, str(body.get("msg", ""))


def _scratch(be, action: str, **fields) -> tuple:
    r = be.net.post("/api/mem/scratchpad", {"action": action, "t": be.tok, **fields}, ip=be.ip, timeout=8.0)
    return scratch_result(r.status_code, r.json())


def _scratch_fault(device, on: bool) -> str:
    """FAULT scratch_fail: the scratchpad's flash file write fails until off (off
    retries the save and reports saved=<0|1>)."""
    out = device.cmd(f"FAULT scratch_fail {'on' if on else 'off'}", expect="FAULT", timeout=6.0)
    if "scratch_fail=" not in out:
        pytest.skip(f"FAULT scratch_fail is not bound in this build ({out.strip()!r})")
    return out


@pytest.fixture
def mock_head(custom_backend):  # noqa: F811
    """The head on the mock (orchHost=custom), restored afterwards."""
    be = custom_backend
    prior = be.orch().get("orchHost", "")
    r = _opost(be.net, be.ip, be.tok, {"orchHost": "custom", "convReset": "1"})
    assert r.status_code == 200, f"orchHost=custom -> {r.status_code}"
    try:
        yield be
    finally:
        _opost(be.net, be.ip, be.tok, {"orchHost": prior, "convReset": "1"})


def test_c1_refused_memory_write_gets_the_correction_line(mock_head):
    be = mock_head
    out = be.device.cmd("FAULT embed_fail on", expect="FAULT", timeout=6.0)
    if "not wired" in out:
        pytest.skip("FAULT embed_fail is not bound in this build (lane P binds it)")
    passed = False
    try:
        _forget_fact(be)  # a copy an earlier run stored makes the write a duplicate: saved, no line
        t0 = _tailed(be)
        reply = _turn(be, "mock-claim-saved", "remember the bench note")
        assert reply.startswith("Saved it to memory."), reply
        assert SAVED_LINE in reply, f"no correction line on a refused write: {reply!r}"
        assert _tailed(be) == t0 + 1
        passed = True
    finally:
        be.device.cmd("FAULT embed_fail off", expect="FAULT", timeout=6.0)
        _drain_then_forget(be, strict=passed)


def test_c2_scratchpad_note_follows_the_flash_file(mock_head):
    be = mock_head
    _scratch_fault(be.device, False)  # skips on a build without the verb
    _scratch(be, "clear", tier="short")
    code, msg = _scratch(be, "add", tier="short", text="bench fresh room")
    assert (code, RAM_TOOL in msg) == (200, False), f"fresh room: the save lands ({code} {msg!r})"
    _fill_nvs(be.device)
    try:
        code, msg = _scratch(be, "add", tier="short", text="bench full nvs")
        assert (code, RAM_TOOL in msg) == (200, False), f"a full NVS must not touch the flash save ({code} {msg!r})"
    finally:
        _free_nvs(be.device)
    _scratch_fault(be.device, True)
    try:
        code, msg = _scratch(be, "add", tier="short", text="bench flash fail")
        assert code == 507, f"a change whose flash save failed must answer 507 ({code} {msg!r})"
        assert RAM_TOOL in msg, f"a failed flash save must say the note lasts only until restart ({msg!r})"
    finally:
        out = _scratch_fault(be.device, False)
    assert "saved=1" in out, f"the notes did not land once the fault ended: {out!r}"
    code, msg = _scratch(be, "add", tier="short", text="bench room again")
    assert (code, RAM_TOOL in msg) == (200, False), f"the save lands again ({code} {msg!r})"
    _scratch(be, "clear", tier="short")


def test_c3_c4_durability_claim_follows_the_flash_save(mock_head):
    be = mock_head
    _scratch_fault(be.device, False)  # skips on a build without the verb
    model, ask = "mock-claim-durable", "put the bench note in your notes"
    be.set_model(model)  # a config save: before the fill (at a full NVS it answers 507)
    _scratch(be, "clear", tier="short")
    _fill_nvs(be.device)
    try:
        # C3a: the scratchpad saves to flash, so at a full NVS the claim is true.
        t0 = _tailed(be)
        reply = _turn(be, model, ask, switch=False)
        assert RAM_LINE not in reply and SAVED_LINE not in reply, f"at a full NVS the flash save lands: {reply!r}"
        assert _tailed(be) == t0
        # C3b: the flash write fails too -> the RAM-only note rides the reply.
        code, _ = _scratch(be, "clear", tier="short")
        assert code == 200, f"the clear must save to flash at a full NVS ({code})"
        _scratch_fault(be.device, True)
        try:
            t1 = _tailed(be)
            reply = _turn(be, model, ask, switch=False)
            assert RAM_LINE in reply, f"no RAM-only note on a durability claim the flash did not take: {reply!r}"
            assert _tailed(be) == t1 + 1
        finally:
            _scratch_fault(be.device, False)
    finally:
        _free_nvs(be.device)
    # C4, the counter-test: room and a working flash, the same claim costs nothing.
    _scratch(be, "clear", tier="short")
    t2 = _tailed(be)
    reply = _turn(be, model, ask, switch=False)
    assert RAM_LINE not in reply and SAVED_LINE not in reply, reply
    assert _tailed(be) == t2
    _scratch(be, "clear", tier="short")
