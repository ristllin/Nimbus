"""Host checks for the claim-check bench legs' readers (no device).

C1 once failed with the turn's reply delivered: the reply reader walked the
episodic page oldest first (the API answers newest first, across every chat) and
returned the oldest assistant row on the page, which a new reply does not change.
These pin the reader: a new reply is a new id wherever it sits on the page and
whatever its text, rows that slide off the page are not new, one turn gives
exactly one reply, and user and log rows are not replies. C2 reads the scratchpad
route's result from its 200 and from its 507 (a change applied but not saved).

The legs also run here end to end against FakeUnit, a model of the device rules
they check (a config save at a full NVS answers 507; the scratchpad saves to a
flash file, so only a failed flash write keeps it until restart; a write that does
not land under a "Saved it" reply gets the correction), so a leg that asks the
device for something it refuses fails on the host first.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

import test_claim_check_bench as leg
from mock_llm import CLAIM_TURNS
from test_claim_check_bench import (
    CLAIM_FACT,
    RAM_LINE,
    RAM_TOOL,
    SAVED_LINE,
    assistant_replies,
    fact_ids,
    new_replies,
    scratch_result,
    the_one_reply,
    QUEUED_LINE,
)
from test_l12_mock_llm import Backend

pytestmark = pytest.mark.host

REPLY = "Saved it to memory.\n\n(Correction: that has not been saved.)"


def _row(n: int, role: str, text: str, kind: str = "message") -> dict:
    return {"id": f"m{n:08x}", "session": "serial", "role": role, "kind": kind, "text": text}


def _page(*rows: dict) -> dict:
    return {"messages": list(rows)}


OLDER = (
    _row(0x12, "assistant", "the reply before"),
    _row(0x11, "user", "a question"),
    _row(0x10, "assistant", "the oldest reply on the page"),
)


def test_a_new_reply_at_the_top_of_a_newest_first_page_is_found():
    before = assistant_replies(_page(*OLDER))
    after = assistant_replies(_page(_row(0x14, "assistant", REPLY), _row(0x13, "user", "remember it"), *OLDER))
    assert the_one_reply(new_replies(before, after), "mock-claim-saved") == REPLY


def test_a_reply_repeating_an_earlier_text_is_still_new():
    before = assistant_replies(_page(_row(0x12, "assistant", REPLY), *OLDER[1:]))
    after = assistant_replies(_page(_row(0x14, "assistant", REPLY), _row(0x12, "assistant", REPLY), *OLDER[1:]))
    assert new_replies(before, after) == {"m00000014": REPLY}


def test_rows_sliding_off_a_full_page_are_not_new():
    # A limit-3 page: the new reply and its question push the two oldest rows off.
    before = assistant_replies(_page(*OLDER))
    after = assistant_replies(_page(_row(0x14, "assistant", REPLY), _row(0x13, "user", "remember it"), OLDER[0]))
    assert new_replies(before, after) == {"m00000014": REPLY}


def test_user_and_log_rows_are_not_replies():
    page = _page(
        _row(0x16, "system", "memory: embeddings parked", kind="log"),
        _row(0x15, "assistant", "thinking about it", kind="thinking"),
        _row(0x14, "user", "remember it"),
    )
    assert assistant_replies(page) == {}


def test_no_reply_fails_and_two_replies_fail():
    with pytest.raises(pytest.fail.Exception, match="no delivered reply recorded"):
        the_one_reply({}, "mock-claim-saved")
    with pytest.raises(AssertionError, match="exactly one reply"):
        the_one_reply({"m00000014": REPLY, "m00000015": "Done."}, "mock-claim-saved")


def test_the_scratchpad_result_is_read_from_a_save_and_from_a_failed_save():
    assert scratch_result(200, {"ok": True, "msg": "added to short"}) == (200, "added to short")
    failed = {
        "error": "storage_full",
        "message": "Couldn't save: device storage is full.",
        "msg": f"added to short; {RAM_TOOL}",
    }
    status, msg = scratch_result(507, failed)
    assert status == 507 and RAM_TOOL in msg
    # An older build's 507 carries no result: read as empty, so the leg fails loudly.
    assert scratch_result(507, {"error": "storage_full"}) == (507, "")
    with pytest.raises(AssertionError, match="scratchpad route -> 400"):
        scratch_result(400, {"ok": False, "msg": "could not add (tier full or empty item)"})


# ---- the legs against a model of the device ------------------------------------
NOTE = CLAIM_FACT  # "bench claim check note", the mock's one fact
SAVED_REPLY = "Saved it to memory."
DURABLE_REPLY = "Put it in my notes, so it will survive a restart."


class _Resp:
    def __init__(self, status: int, body: dict):
        self.status_code = status
        self._body = body

    def json(self) -> dict:
        return self._body


class FakeUnit:
    """The device as the claim-check legs see it: serial console, web routes and
    the mock head's recording, following the product rules."""

    def __init__(self):
        self.nvs_full = False
        self.embed_fail = False
        self.scratch_fail = False
        self.model = "mock-echo"
        self.short: list = []
        self.rows: list = []
        self.tailed = 0
        self.requests: list = []
        self.refused_saves: list = []
        self.stored: dict = {}  # vector store: id -> content (content-addressed ids)
        self.pending: list = []  # facts queued while embeddings are down

    # -- serial console
    def cmd(self, line: str, expect: str, timeout: float = 5.0) -> str:
        if line.startswith("NVSFILL "):
            self.nvs_full = line.split()[1] != "0"
            return "NVSFILL wrote=400 commit=1 freeEntries 400->0 full=1" if self.nvs_full else "NVSFILL cleared"
        if line.startswith("FAULT embed_fail "):
            self.embed_fail = line.endswith(" on")
            if not self.embed_fail:  # the pause ends and the queue drains into the store
                for fact in self.pending:
                    self.stored[f"v{abs(hash(fact)) % 10**8:08d}"] = fact
                self.pending = []
            return f"FAULT embed_fail={int(self.embed_fail)} pending={len(self.pending)}"
        if line.startswith("FAULT scratch_fail "):
            self.scratch_fail = line.endswith(" on")
            return "FAULT scratch_fail=1" if self.scratch_fail else "FAULT scratch_fail=0 saved=1"
        if line.startswith("TURN "):
            self._turn(line[5:])
            return f"TURN <- {line[5:]}"
        raise AssertionError(f"unexpected console line {line!r}")

    def _row(self, role: str, text: str) -> None:
        self.rows.append(
            {"id": f"m{len(self.rows) + 1:08x}", "session": "serial", "role": role, "kind": "message", "text": text}
        )

    def _turn(self, text: str) -> None:
        self.requests.append(self.model)
        reply = {"mock-claim-saved": SAVED_REPLY, "mock-claim-durable": DURABLE_REPLY}.get(self.model, "mock says hi")
        if self.model == "mock-claim-saved":
            fact = NOTE
            if fact in self.stored.values():  # a duplicate: already saved, the reply is true
                pass
            elif self.embed_fail:  # queued (or coalesced with a queued copy), not saved yet
                if fact not in self.pending:
                    self.pending.append(fact)
                reply += "\n\n" + QUEUED_LINE
                self.tailed += 1
            else:
                self.stored[f"v{abs(hash(fact)) % 10**8:08d}"] = fact
        if self.model == "mock-claim-durable":
            changed = self.short != [NOTE]
            self.short = [NOTE]
            if changed and self.scratch_fail:  # the flash file did not take it
                reply += "\n\n" + RAM_LINE
                self.tailed += 1
        self._row("user", text)
        self._row("assistant", reply)

    # -- web routes (form posts and token-gated GETs)
    def post(self, path: str, data: dict, ip=None, timeout: float = 5.0):
        if path == "/api/orch":  # a persisted config save
            if self.nvs_full:
                self.refused_saves.append(dict(data))
                return _Resp(507, {"error": "storage_full"})
            self.model = data.get("custModel", self.model)
            return _Resp(200, {"ok": True})
        if path == "/api/mem/scratchpad":
            return self._scratch(data)
        if path == "/api/mem/vector" and data.get("op") == "delete":
            return _Resp(200, {"ok": self.stored.pop(data["id"], None) is not None, "total": len(self.stored)})
        raise AssertionError(f"unexpected POST {path}")

    def _scratch(self, data: dict) -> _Resp:
        before = list(self.short)
        if data["action"] == "add":
            self.short.append(data["text"])
            msg = "added to short"
        else:
            self.short = []
            msg = "short cleared"
        if self.short != before and self.scratch_fail:  # the flash write failed; NVS plays no part
            return _Resp(507, {"error": "storage_full", "msg": f"{msg}; {RAM_TOOL}"})
        return _Resp(200, {"ok": True, "msg": msg})

    def get_json(self, path: str, ip=None, timeout: float = 5.0) -> dict:
        if path.startswith("/api/state"):
            return {"mode": 1, "claimsTailed": self.tailed}
        if path.startswith("/api/orch"):
            return {"orchHost": "custom", "cust": {"model": self.model}}
        if path.startswith("/api/mem/episodic?session=serial"):
            return {"messages": list(reversed(self.rows))[:20]}
        if path.startswith("/api/mem/vector?"):
            q = dict(kv.split("=") for kv in path.split("?", 1)[1].split("&"))
            off, lim = int(q.get("offset", 0)), int(q.get("limit", 10))
            items = [{"id": k, "content": v} for k, v in self.stored.items()]
            return {"entries": items[off : off + lim], "total": len(items), "mode": "browse"}
        if path.startswith("/api/mem/stats"):
            return {"pending": len(self.pending), "vectors": len(self.stored)}
        raise AssertionError(f"unexpected GET {path}")

    # -- the mock head's recording
    def reset(self) -> None:
        self.requests = []

    def wait_for_request(self, model: str, count: int = 1, timeout: float = 90.0) -> list:
        return [r for r in self.requests if r == model][:count]


@pytest.fixture
def unit(monkeypatch):
    monkeypatch.setattr(leg, "SETTLE_S", 0.0)
    monkeypatch.setattr(leg.time, "sleep", lambda _s: None)
    u = FakeUnit()
    u.server = u
    u.be = Backend(u, u, u, "192.0.2.1", "tok", "192.0.2.2")
    return u


def test_c1_runs_against_the_device_rules(unit):
    unit.rows = [{"id": "m000000ff", "session": "serial", "role": "assistant", "kind": "message", "text": "older"}]
    leg.test_c1_refused_memory_write_gets_the_correction_line(unit.be)
    assert unit.tailed == 1 and not unit.embed_fail
    assert unit.stored == {} and unit.pending == [], "the leg left its fact behind"


def test_c1_repeats_on_a_unit_that_already_holds_the_fact(unit):
    """Unit L, 22 Oct 2026: the 21 Oct runs drained the queued fact into the store, so
    the next run's write was a duplicate (saved, true, no line) and C1 failed. The leg
    forgets the fact first, and runs twice in a row on the same unit."""
    unit.stored = {"v00000001": NOTE, "v00000002": "the owner drinks flat white"}
    for _ in range(2):
        leg.test_c1_refused_memory_write_gets_the_correction_line(unit.be)
    assert unit.tailed == 2
    assert unit.stored == {"v00000002": "the owner drinks flat white"}, "only the leg's own fact may go"


def test_a_stored_fact_makes_the_saved_reply_true(unit):
    """The device rule the leg works around: a duplicate write landed, so "Saved it to
    memory." stands with no correction even while embeddings are down."""
    unit.stored = {"v00000001": NOTE}
    unit.embed_fail = True
    unit.model = "mock-claim-saved"
    unit.cmd("TURN remember the bench note", expect="TURN <-")
    assert unit.rows[-1]["text"] == SAVED_REPLY and unit.tailed == 0


def test_fact_ids_match_the_whole_content_only():
    page = {"entries": [{"id": "a", "content": NOTE}, {"id": "b", "content": NOTE + " (edited)"}, {"id": "c"}]}
    assert fact_ids(page, NOTE) == ["a"]


def test_c2_runs_against_the_device_rules(unit):
    leg.test_c2_scratchpad_note_follows_the_flash_file(unit.be)
    assert not unit.nvs_full and not unit.scratch_fail and unit.short == []
    assert unit.refused_saves == []


def test_c3_c4_run_against_the_device_rules(unit):
    leg.test_c3_c4_durability_claim_follows_the_flash_save(unit.be)
    assert unit.refused_saves == [], f"a config save was sent at a full NVS: {unit.refused_saves}"
    assert unit.tailed == 1  # only the turn whose flash save failed got the note
    assert not unit.nvs_full and not unit.scratch_fail and unit.short == []


def test_the_host_suites_check_the_turns_the_mock_sends():
    """test/support/mock_claim_turns.h carries the mock's claim turns byte for byte
    (the C++ host suites run them through the real parse, apply and engine) and the
    lines the legs expect."""
    hdr = (Path(__file__).resolve().parents[2] / "test/support/mock_claim_turns.h").read_text()
    raw = dict(re.findall(r'constexpr char (k\w+)\[\] = R"\((.*)\)";', hdr))
    assert raw["kMockClaimSavedTurn"] == json.dumps(CLAIM_TURNS["mock-claim-saved"])
    assert raw["kMockClaimDurableTurn"] == json.dumps(CLAIM_TURNS["mock-claim-durable"])
    lines = dict(re.findall(r'constexpr char (k\w+Line)\[\] =\s*"([^"]*)";', hdr))
    assert lines == {"kNotSavedLine": SAVED_LINE, "kRamOnlyLine": RAM_LINE}
