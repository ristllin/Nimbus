"""Lane S bench legs: the claim check on a real device (the fill unit).

The head runs on the LAN mock (the custom endpoint, single-shot structured
turn), so every reply is fixed and the legs cost nothing. The device must hold
each reply's claim against what the turn really did:

  C1  a memory write the store refuses (``FAULT embed_fail on``) under a reply
      that says "Saved it to memory." -> the delivered reply carries
      "(Correction: that has not been saved.)" and ``claimsTailed`` goes up;
  C2  the scratchpad route at a full NVS partition (``NVSFILL``) -> the change
      result says "kept until restart only (saving failed)"; after
      ``NVSFILL 0`` (fresh room) it does not;
  C3  a durability claim ("so it will survive a restart") at a full NVS ->
      the RAM-only note rides the reply; C4 the same turn with room -> no line
      and ``claimsTailed`` unchanged (the counter-test).

C1 needs lane P's ``FAULT embed_fail`` binding; on a build without it the leg
is skipped with that reason (never passed). Unit: the fill unit (unit L).

Run: python3 -m pytest tests/hil/test_claim_check_bench.py -m "hil and net" --allow-hardware
"""

from __future__ import annotations

import os
import time

import pytest

from mock_llm import mock_llm  # noqa: F401  (fixture)
from test_l12_mock_llm import _opost, custom_backend  # noqa: F401  (fixture + helper)

pytestmark = [pytest.mark.hil, pytest.mark.net]

SAVED_LINE = "(Correction: that has not been saved.)"
RAM_LINE = "(Note: this is kept only until the next restart; saving it permanently failed.)"
RAM_TOOL = "kept until restart only (saving failed)"
FILL_CHUNK = int(os.environ.get("NIMBUS_S_FILL_CHUNK", "400"))


def _episodic_last_assistant(be) -> str:
    rows = be.net.get_json(f"/api/mem/episodic?limit=20&t={be.tok}", ip=be.ip, timeout=8.0).get("messages", [])
    for r in reversed(rows):
        if r.get("role") == "assistant":
            return r.get("text", "")
    return ""


def _tailed(be) -> int:
    return int(be.state().get("claimsTailed", -1))


def _turn(be, model: str, text: str, timeout: float = 90.0) -> str:
    """One head turn on the mock scenario `model`; returns the delivered reply."""
    be.set_model(model)
    before = _episodic_last_assistant(be)
    try:
        be.device.cmd(f"TURN {text}", expect="TURN <-", timeout=6.0)
    except Exception:  # noqa: BLE001 - the echo is advisory; the turn still fires
        pass
    assert be.server.wait_for_request(model, count=1, timeout=timeout), f"the head never reached {model}"
    end = time.time() + timeout
    while time.time() < end:
        got = _episodic_last_assistant(be)
        if got and got != before:
            return got
        time.sleep(2.0)
    pytest.fail(f"no delivered reply recorded for {model}")


def _fill_nvs(device) -> None:
    for _ in range(40):
        out = device.cmd(f"NVSFILL {FILL_CHUNK}", expect="NVSFILL", timeout=20.0)
        if "full=1" in out:
            return
    pytest.fail("NVSFILL never reported a full partition")


def _free_nvs(device) -> None:
    device.cmd("NVSFILL 0", expect="NVSFILL", timeout=20.0)


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
    try:
        t0 = _tailed(be)
        reply = _turn(be, "mock-claim-saved", "remember the bench note")
        assert reply.startswith("Saved it to memory."), reply
        assert SAVED_LINE in reply, f"no correction line on a refused write: {reply!r}"
        assert _tailed(be) == t0 + 1
    finally:
        be.device.cmd("FAULT embed_fail off", expect="FAULT", timeout=6.0)


def test_c2_scratchpad_route_reports_a_ram_only_save(mock_head):
    be = mock_head

    def add(text: str) -> str:
        r = be.net.post(
            "/api/mem/scratchpad", {"action": "add", "tier": "short", "text": text, "t": be.tok}, ip=be.ip, timeout=8.0
        )
        assert r.status_code == 200, f"scratchpad add -> {r.status_code}"
        return r.json().get("msg", "")

    assert RAM_TOOL not in add("bench fresh room")  # fresh room: the save lands
    _fill_nvs(be.device)
    try:
        assert RAM_TOOL in add("bench full nvs"), "a failed save must say the note lasts only until restart"
    finally:
        _free_nvs(be.device)
    assert RAM_TOOL not in add("bench room again")
    be.net.post("/api/mem/scratchpad", {"action": "clear", "tier": "short", "t": be.tok}, ip=be.ip, timeout=8.0)


def test_c3_c4_durability_claim_full_nvs_then_room(mock_head):
    be = mock_head
    _fill_nvs(be.device)
    try:
        t0 = _tailed(be)
        reply = _turn(be, "mock-claim-durable", "put the bench note in your notes")
        assert RAM_LINE in reply, f"no RAM-only note on a durability claim at full NVS: {reply!r}"
        assert _tailed(be) == t0 + 1
    finally:
        _free_nvs(be.device)
    # C4, the counter-test: with room, the same claim is true and costs nothing.
    t1 = _tailed(be)
    be.net.post("/api/mem/scratchpad", {"action": "clear", "tier": "short", "t": be.tok}, ip=be.ip, timeout=8.0)
    reply = _turn(be, "mock-claim-durable", "put the bench note in your notes")
    assert RAM_LINE not in reply and SAVED_LINE not in reply, reply
    assert _tailed(be) == t1
    be.net.post("/api/mem/scratchpad", {"action": "clear", "tier": "short", "t": be.tok}, ip=be.ip, timeout=8.0)
