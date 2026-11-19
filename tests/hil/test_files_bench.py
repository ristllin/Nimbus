"""Bench legs for v5.2.0's plain file tracking (lane RB-REC7; replaces the ledger bench RB-R1 planned).

Owner ruling 41 took the ledger out of the product: tracking is an ordinary CSV file and the plain file tools
(rebuild/DESIGN-GENERAL-RECORDS.md, the RB-R1 gate rows). The legs:

  1. a cut during files.append: the file is the old one, or the old one plus the whole text, or the old one plus a
     torn tail; a torn tail is named by the next read and fenced by the next append (a line end before the text);
  2. a cut during files.edit: the file is the old one or the new one, never a mix;
  3. a round trip with one row added: exactly one line more, every other byte as it was, the sha8 the tool states
     is the file's;
  4. the up/down/up leg: the candidate, then a released image (v5.1.1, v5.1.2), then the candidate again: the sha8
     of a file holding engine-2 rows is unchanged with no writes, and the old image's append leaves those rows
     byte-identical (ruling 42 item 7: v5.1.x's keyed append may merge rows of one date and key; that is recorded);
  5. a refused allocation: one append and one edit under a refusing allocator change nothing and say so.

The "cut" on this bench is a panic reset at a random moment (console PANIC): the bench has no switchable USB
power (uhubctl finds no hub that switches ports), and a cable pull needs a person. A panic stops the write at an
arbitrary byte like a power loss does; it does not take power from the card mid-sector, so the evidence says
"panic reset", never "power cut".

The pure checks below (classification of a file after a cut, sha8, rows rewritten) run on the host in CI; the legs
need the test image (`/api/test/astool`), the board and --allow-hardware:

    pytest tests/hil/test_files_bench.py -m hil --allow-hardware [--rollback-images DIR]
"""

from __future__ import annotations

import os
import random
import threading
import time
import urllib.parse

import pytest

import bench_driver as bd

PROJECT = "bench52"
CUTS = int(os.environ.get("NIMBUS_FILES_BENCH_CUTS", "5"))


# ---- the pure checks (host) ---------------------------------------------------------------------------------


def fnv64(data: bytes, h: int = 1469598103934665603) -> int:
    """FNV-1a 64 (lib/core blob_store.h's content hash)."""
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def sha8(data: bytes) -> str:
    """The sha8 the file tools state: 8 lowercase hex of FNV-1a 64's high 32 bits (file_aids.h sha8Hex)."""
    return f"{fnv64(data) >> 32:08x}"


def after_append(before: bytes, text: bytes, after: bytes) -> str:
    """What a cut during an append left: 'old', 'whole' (the old file, a fence when it had no line end, the text),
    'torn' (the old file plus a cut-short prefix of that), or 'mixed' (anything else: a lost or changed byte)."""
    whole = before + (b"\n" if before and not before.endswith(b"\n") else b"") + text
    if after == before:
        return "old"
    if after == whole:
        return "whole"
    if len(before) < len(after) < len(whole) and whole.startswith(after):
        return "torn"
    return "mixed"


def fenced(torn: bytes, text: bytes, after: bytes, result: str) -> bool:
    """The append after a torn tail wrote a line end before the text and said so."""
    return after == torn + b"\n" + text and "newline" in result


def after_edit(old: bytes, new: bytes, after: bytes) -> str:
    return "old" if after == old else "new" if after == new else "mixed"


def rows_rewritten(before: bytes, after: bytes) -> list:
    """Lines of `before` that are not byte-identical, in order, at the start of `after` (an append keeps them)."""
    old, now = before.split(b"\n"), after.split(b"\n")
    return [i + 1 for i, line in enumerate(old[:-1] if before.endswith(b"\n") else old) if i >= len(now) or now[i] != line]


def csv_rows(n: int, start: int = 0) -> bytes:
    """Rows in the shape of the validation families (an invented expense log)."""
    return "".join(f"2026-11-{1 + (i % 28):02d},item{i},note {i},{i % 97}.{i % 10}0,card\n" for i in range(start, start + n)).encode()


HEADER = b"date,category,note,amount,paid_with\n"


def test_after_append_names_every_outcome():
    before, text = HEADER + csv_rows(3), csv_rows(2, 3)
    assert after_append(before, text, before) == "old"
    assert after_append(before, text, before + text) == "whole"
    assert after_append(before, text, before + text[:7]) == "torn"
    assert after_append(before, text, before[:-1] + text) == "mixed"
    assert after_append(before, text, before + text + text) == "mixed"  # a doubled write is never "whole"
    assert after_append(before[:-1], text, before + text) == "whole"  # the fence counts as part of the append


def test_a_torn_tail_must_be_fenced_and_said():
    torn, text = HEADER + b"2026-11-19,item,no", b"2026-11-19,coffee,kiosk,2.40,card\n"
    assert fenced(torn, text, torn + b"\n" + text, "appended 1 row; added a newline before the text")
    assert not fenced(torn, text, torn + text, "appended 1 row")
    assert not fenced(torn, text, torn + b"\n" + text, "appended 1 row")


def test_after_edit_is_old_new_or_mixed():
    old = HEADER + csv_rows(4)
    new = old.replace(b"item0,", b"edited0,", 1)
    assert after_edit(old, new, old) == "old" and after_edit(old, new, new) == "new"
    assert after_edit(old, new, new[: len(new) // 2] + old[len(old) // 2 :]) == "mixed"


def test_rows_rewritten_sees_a_changed_row_and_lets_an_append_pass():
    before = HEADER + csv_rows(3)
    assert rows_rewritten(before, before + csv_rows(1, 3)) == []
    assert rows_rewritten(before, before.replace(b"item1", b"itemX")) == [3]
    assert rows_rewritten(before, HEADER + csv_rows(2)) == [4]


def test_sha8_is_the_tools_sha8():
    # The shared file aids state sha8=fff90131 for this 6-byte file after replacing line 3 (file_replay, edit case).
    assert sha8(b"a\nb\nz\n") == "fff90131"
    assert sha8(b"") != sha8(b"\n")


# ---- the legs (hardware) ------------------------------------------------------------------------------------


@pytest.fixture()
def api(device, net):
    if not net.ip or not net.token:
        pytest.skip("BLOCKED: the board's IP and device sign-in code are needed for the file API")
    a = bd.Api(net.ip, net.token)
    probe = a.tool("files.list", {"project": PROJECT})
    if probe.code == 404:
        pytest.skip("BLOCKED: this image has no /api/test/astool (flash the test env of the candidate)")
    return a


def _call(api, name: str, args: dict, timeout: float = 120.0) -> str:
    r = api.tool(name, {"project": PROJECT, **args}, timeout=timeout)
    return r.text


def _file(api, name: str) -> bytes:
    r = api.download(PROJECT, name)
    return r.body if r.code == 200 else b""


def _cut_during(device, api, call) -> None:
    """Run `call` in a thread, panic the unit at a random moment inside it, wait for it to come back."""
    worker = threading.Thread(target=call, daemon=True)
    worker.start()
    time.sleep(random.uniform(0.05, 1.5))
    device.send("PANIC abort")
    worker.join(timeout=30)
    device.reopen_after_reenumerate()
    device.wait_ready(timeout=60)
    deadline = time.time() + 90
    while time.time() < deadline and api.state().get("fsMounted") is None:
        time.sleep(2)


@pytest.mark.hil
def test_cut_during_append_leaves_old_whole_or_a_fenced_torn_tail(device, api):
    outcomes = []
    for n in range(CUTS):
        name = f"append-{n}.csv"
        before = HEADER + csv_rows(3000)
        assert api.upload(PROJECT, name, before).code == 200
        text = csv_rows(1500, 3000)
        _cut_during(device, api, lambda: _call(api, "files.append", {"name": name, "text": text.decode()}))
        after = _file(api, name)
        state = after_append(before, text, after)
        outcomes.append(state)
        assert state != "mixed", f"cut {n}: the file is neither old, whole nor a torn tail of the append"
        if state == "torn":
            seen = _call(api, "files.read", {"name": name, "tail": 2})
            assert "newline" in seen or "cut short" in seen, f"cut {n}: the read does not name the torn tail: {seen[:200]}"
            row = b"2026-11-19,after,the cut,1.00,card\n"
            result = _call(api, "files.append", {"name": name, "text": row.decode()})
            assert fenced(after, row, _file(api, name), result), f"cut {n}: the next append did not fence the tail"
    print(f"append cuts: {outcomes}")


@pytest.mark.hil
def test_cut_during_edit_leaves_old_or_new_never_mixed(device, api):
    outcomes = []
    for n in range(CUTS):
        name = f"edit-{n}.csv"
        old = HEADER + csv_rows(20000)
        new = old.replace(b"item0,", b"edited0,", 1)
        assert api.upload(PROJECT, name, old).code == 200
        _cut_during(device, api, lambda: _call(api, "files.edit", {"name": name, "find": "item0,", "replace": "edited0,"}))
        state = after_edit(old, new, _file(api, name))
        outcomes.append(state)
        assert state != "mixed", f"cut {n}: the file is a mix of the old and the new one"
    print(f"edit cuts: {outcomes}")


@pytest.mark.hil
def test_round_trip_with_one_row_added(api):
    name = "roundtrip.csv"
    before = HEADER + csv_rows(200)
    assert api.upload(PROJECT, name, before).code == 200
    row = b"2026-11-19,coffee,kiosk,2.40,card\n"
    result = _call(api, "files.append", {"name": name, "text": row.decode()})
    after = _file(api, name)
    assert after == before + row, "the round trip changed more than the one row"
    assert rows_rewritten(before, after) == []
    listed = api.get("/api/files/list?" + urllib.parse.urlencode({"project": PROJECT})).text
    assert name in listed
    if "sha8=" in result:
        assert f"sha8={sha8(after)}" in result


@pytest.mark.hil
def test_refused_allocation_changes_nothing_and_says_so(device, api):
    """Needs a test-console verb that arms the refusing allocator (setWorkingAllocators) for the next N blocks."""
    reply = device.cmd("ALLOCREFUSE 1", "ALLOCREFUSE", timeout=5.0) if hasattr(device, "cmd") else ""
    if "armed" not in reply:
        pytest.skip("BLOCKED: no ALLOCREFUSE console verb on this image (src/test_console.cpp, a hot file)")
    name = "refused.csv"
    before = HEADER + csv_rows(50)
    assert api.upload(PROJECT, name, before).code == 200
    result = _call(api, "files.append", {"name": name, "text": "2026-11-19,x,y,1.00,card\n"})
    assert _file(api, name) == before and "nothing changed" in result
    device.cmd("ALLOCREFUSE 1", "ALLOCREFUSE", timeout=5.0)
    result = _call(api, "files.edit", {"name": name, "find": "item0,", "replace": "edited0,"})
    assert _file(api, name) == before and "nothing changed" in result


@pytest.mark.hil
def test_up_down_up_keeps_engine_two_rows(request, device, api):
    images = request.config.getoption("--rollback-images", default=None) if hasattr(request.config, "getoption") else None
    if not images:
        pytest.skip("BLOCKED: pass --rollback-images DIR holding candidate.bin, v5.1.1.bin and v5.1.2.bin (released images)")
    pytest.skip("BLOCKED: the flash steps of the rollback leg are run by RB-R1's release bench, not in this session")
