"""Bench legs for v5.2.0's plain file tracking (lane RB-REC7, ruling 87; replaces the ledger bench RB-R1 planned).

Owner ruling 41 took the ledger out of the product: tracking is an ordinary CSV file and the plain file tools
(rebuild/DESIGN-GENERAL-RECORDS.md, the RB-R1 gate rows). The legs, on the candidate's test image (test-cyd):

  1. a cut during files.append at chosen byte offsets of the appended bytes: the file is the old one plus exactly
     that prefix; a read names the torn tail and the next append fences it (a line end before its text);
  2. a cut during files.edit, in its part file and at each card operation of its swap: after the boot the file
     is the old one or the new one, byte for byte, never a mix and never gone;
  3. a round trip with one row added: exactly one line more, every other byte as it was;
  4. a refused allocation: one append and one edit under a refusing working allocator change nothing and say so;
  5. the up, down, up leg: the candidate, then released v5.1.1, then released v5.1.2, then the candidate again,
     each flashed app-only (never OTA): with no write the file is byte-identical across every flash (the sha8),
     and each old image's own append leaves the engine-2 rows byte-identical (ruling 42 item 7).

The cut is a console panic armed at the cut point (CUTARM append|part|swap <n>, NIMBUS_TEST builds only): the
bench cannot switch USB power. A panic stops the write at that byte, with the bytes before it closed on the card;
it does not take power from the card mid-sector. A physical power cut is NOT exercised here (a cable pull by the
owner is an optional RB-R1 row). test/test_file_cut tears the same writes at every byte offset on the host.

A leg that cannot run FAILS with "BLOCKED: ..." under --allow-hardware: a gate row never passes by a skip.

    NIMBUS_PORT=<port> NIMBUS_BENCH_MAC=<mac> NIMBUS_TEST_IP=<ip> NIMBUS_TEST_TOKEN=<token> \
        pytest tests/hil/test_files_bench.py -m hil --allow-hardware [--rollback-images DIR]
DIR holds candidate.bin (the candidate's test-cyd image), v5.1.1.bin and v5.1.2.bin (released esp32s3-cyd images).
The pure checks below run on the host in CI.
"""

from __future__ import annotations

import os
import time

import pytest

import bench_driver as bd

PROJECT = "bench52"


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
    return [
        i + 1 for i, line in enumerate(old[:-1] if before.endswith(b"\n") else old) if i >= len(now) or now[i] != line
    ]


def csv_rows(n: int, start: int = 0) -> bytes:
    """Rows in the shape of the validation families (an invented expense log)."""
    return "".join(
        f"2026-11-{1 + (i % 28):02d},item{i},note {i},{i % 97}.{i % 10}0,card\n" for i in range(start, start + n)
    ).encode()


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

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ESPTOOL = os.environ.get("NIMBUS_ESPTOOL", os.path.expanduser("~/.platformio/penv/bin/esptool"))
EVIDENCE = os.environ.get("NIMBUS_FILES_BENCH_EVIDENCE", "")


def record(name: str, text: str) -> None:
    """One line of evidence per step, beside the run (NIMBUS_FILES_BENCH_EVIDENCE), token-free by construction."""
    if EVIDENCE:
        os.makedirs(EVIDENCE, exist_ok=True)
        with open(os.path.join(EVIDENCE, name), "a", encoding="utf-8") as fh:
            fh.write(time.strftime("%H:%M:%SZ ", time.gmtime()) + text + "\n")


class Unit:
    """The bench unit: one long-lived console session (an open resets an S3 USB console, so it is opened once and
    again only after the unit itself went away) and the web API."""

    def __init__(self, port: str, mac: str, api: bd.Api):
        self.port, self.mac, self.api = port, mac, api
        self.dev = None

    def attach(self) -> None:
        from device import Device

        self.dev = Device(port=self.port).open()
        self.dev.wait_console_ready(40)

    def reattach(self) -> None:
        """After the unit died (a cut) or was flashed: the USB node re-enumerates."""
        if self.dev is None:
            return self.attach()
        self.dev.reopen_after_reenumerate()
        self.dev.wait_console_ready(40)

    def detach(self) -> None:
        if self.dev is not None:
            self.dev.close()
            self.dev = None

    def con(self, line: str, expect: str, timeout: float = 10.0) -> str:
        if self.dev is None:
            self.attach()
        try:
            return self.dev.cmd(line, expect, timeout)
        except Exception as e:  # noqa: BLE001 - an unanswered command is an empty reply; the leg asserts on it
            record("console.log", f"{line!r}: no {expect!r} ({type(e).__name__})")
            return ""

    def wait_api(self, timeout: float = 120.0) -> dict:
        end = time.time() + timeout
        while time.time() < end:
            st = self.api.state(5.0)
            if st.get("build"):
                return st
            time.sleep(3)
        raise AssertionError("the unit's web API did not come back")

    def rezero(self, label: str, console: bool = True) -> None:
        """Sound 0 and voice replies off after every flash or reset, read back and recorded (the bench rule)."""
        if console:
            record("rezero.log", f"{label} console SFXVOL 0 -> {self.con('SFXVOL 0', 'SFXVOL')}")
        for _ in range(10):  # right after a boot the settings route may answer empty for a moment
            r = self.api.post_form("/api/orch", {"ttsOn": "0", "sfxVol": "0", "sfxLvlO": "0", "sfxLvlN": "0"})
            back = self.api.get_json("/api/orch")
            if back.get("sfxVol") is not None:
                break
            time.sleep(3)
        state = self.api.state()
        record(
            "rezero.log",
            f"{label} api {r.code}; readback sfxVol={back.get('sfxVol')} ttsOn={back.get('ttsOn')} "
            f"sfxLvlO={back.get('sfxLvlO')} sfxLvlN={back.get('sfxLvlN')} build={state.get('build')} boot={(state.get('boot') or {}).get('n')}",
        )
        assert back.get("sfxVol") == 0 and back.get("ttsOn") is False, f"{label}: sound not zeroed: {back}"

    def flash(self, image: str, label: str) -> dict:
        """App-only flash (app0, otadata erased so app0 boots), MAC-verified first, never OTA."""
        import subprocess

        import port_guard

        port_guard.check_mac(self.mac, "test_files_bench flash")
        self.detach()  # esptool takes the port
        esp = [ESPTOOL, "--chip", "esp32s3", "--port", self.port]
        reset = [
            os.environ.get("NIMBUS_PY", "python3"),
            os.path.join(REPO, "tools", "usb_reset.py"),
            "--serial",
            self.mac,
        ]
        subprocess.run(reset, capture_output=True, timeout=60)
        time.sleep(2)
        mac = subprocess.run(
            esp + ["--before", "default-reset", "--after", "no-reset", "read-mac"],
            capture_output=True,
            text=True,
            timeout=60,
        ).stdout.lower()
        assert self.mac.lower() in mac, f"{label}: the port does not hold {self.mac}; nothing flashed"
        out = subprocess.run(
            esp + ["--before", "no-reset", "--after", "no-reset", "write-flash", "0x10000", image],
            capture_output=True,
            text=True,
            timeout=600,
        ).stdout
        assert "Hash of data verified" in out, f"{label}: the flash was not verified"
        subprocess.run(
            esp + ["--before", "no-reset", "--after", "hard-reset", "erase-region", "0xe000", "0x2000"],
            capture_output=True,
            timeout=120,
        )
        subprocess.run(reset, capture_output=True, timeout=60)
        st = self.wait_api(180)
        record("flash.log", f"{label}: {os.path.basename(image)} flashed, MAC verified, build={st.get('build')}")
        return st


@pytest.fixture()
def unit(allow_hardware):
    port, mac = os.environ.get("NIMBUS_PORT", ""), os.environ.get("NIMBUS_BENCH_MAC", "")
    ip, token = os.environ.get("NIMBUS_TEST_IP", ""), os.environ.get("NIMBUS_TEST_TOKEN", "")
    if not (port and mac and ip and token):
        pytest.fail("BLOCKED: set NIMBUS_PORT, NIMBUS_BENCH_MAC, NIMBUS_TEST_IP and NIMBUS_TEST_TOKEN")
    import port_guard

    port_guard.check_mac(mac, "test_files_bench")
    u = Unit(port, mac, bd.Api(ip, token))
    if u.api.tool("files.list", {"project": PROJECT}).code == 404:
        pytest.fail("BLOCKED: this image has no /api/test/astool (flash the candidate's test-cyd image)")
    yield u
    u.detach()


def _call(api, name: str, args: dict, timeout: float = 60.0) -> str:
    return api.tool(name, {"project": PROJECT, **args}, timeout=timeout).text


def _file(api, name: str) -> bytes:
    r = api.download(PROJECT, name)
    return r.body if r.code == 200 else b""


def _cut(unit: Unit, arm: str, call) -> None:
    """Arm the cut, make the call (the unit panics inside it), wait for the boot, re-zero."""
    assert "armed" in unit.con(f"CUTARM {arm}", "CUTARM"), f"BLOCKED: CUTARM {arm} did not arm"
    before = (unit.api.state().get("boot") or {}).get("n")
    try:
        call()
    except Exception:  # noqa: BLE001 - the unit dies inside the call: the request fails
        pass
    time.sleep(5)
    unit.reattach()
    st = unit.wait_api(180)
    after = (st.get("boot") or {}).get("n")
    assert after != before, f"CUTARM {arm}: the unit did not restart (boot {before} -> {after}); no cut happened"
    unit.rezero(f"after CUTARM {arm}")


@pytest.mark.hil
def test_cut_during_append_leaves_the_old_bytes_and_a_fenced_tail(unit):
    row = b"2026-11-19,after,the cut,1.00,card\n"
    text = csv_rows(40, 3000)
    for k in (0, 1, 17, len(text) // 2, len(text) - 1):
        name = f"append-cut-{k}.csv"
        before = HEADER + csv_rows(300)
        assert unit.api.upload(PROJECT, name, before).code == 200
        _cut(unit, f"append {k}", lambda: _call(unit.api, "files.append", {"name": name, "text": text.decode()}, 20))
        after = _file(unit.api, name)
        state = after_append(before, text, after)
        record("cuts.log", f"append cut at {k} of {len(text)} B: {state}, file {len(after)} B")
        assert after == before + text[:k], f"cut at {k}: the file is not the old bytes plus the {k} B that landed"
        if 0 < k < len(text):
            seen = _call(unit.api, "files.read", {"name": name, "tail": 2})
            named = any(w in seen for w in ("newline", "cut short", "incomplete", "cut mid-write"))
            record("cuts.log", f"append cut at {k}: the next read says {seen[-160:]!r}")
            assert named, f"cut at {k}: the read does not name the torn tail"
            result = _call(unit.api, "files.append", {"name": name, "text": row.decode()})
            assert fenced(after, row, _file(unit.api, name), result), f"cut at {k}: the next append did not fence it"


@pytest.mark.hil
def test_cut_during_edit_leaves_old_or_new_never_mixed(unit):
    old = HEADER + csv_rows(2000)
    new = old.replace(b"item0,", b"edited0,", 1)
    arms = [f"part {k}" for k in (0, 4096, len(new) // 2, len(new) - 1)] + [f"swap {i}" for i in range(6)]
    for arm in arms:
        name = "edit-cut-" + arm.replace(" ", "-") + ".csv"
        assert unit.api.upload(PROJECT, name, old).code == 200
        _cut(
            unit,
            arm,
            lambda: _call(unit.api, "files.edit", {"name": name, "find": "item0,", "replace": "edited0,"}, 30),
        )
        got = _file(unit.api, name)
        state = after_edit(old, new, got)
        record("cuts.log", f"edit cut {arm}: {state}, file {len(got)} B")
        assert got, f"edit cut {arm}: the file is gone"
        assert state != "mixed", f"edit cut {arm}: the file is a mix of the old and the new one"
        if arm.startswith("part"):
            assert state == "old", f"edit cut {arm}: a part cut must leave the old file"


@pytest.mark.hil
def test_round_trip_with_one_row_added(unit):
    name = "roundtrip.csv"
    before = HEADER + csv_rows(200)
    assert unit.api.upload(PROJECT, name, before).code == 200
    row = b"2026-11-19,coffee,kiosk,2.40,card\n"
    result = _call(unit.api, "files.append", {"name": name, "text": row.decode()})
    after = _file(unit.api, name)
    record(
        "roundtrip.log",
        f"append -> {result[:160]}; {len(before)} B -> {len(after)} B sha8 {sha8(before)} -> {sha8(after)}",
    )
    assert after == before + row, "the round trip changed more than the one row"
    assert rows_rewritten(before, after) == []


@pytest.mark.hil
def test_refused_allocation_changes_nothing_and_says_so(unit):
    """files.append's plan allocates on the heap (planAppend catches std::bad_alloc): ALLOCREFUSE heap refuses its
    first block of 4 KB or more. files.edit's streamed edit allocates through the working allocator (file_aids.h):
    ALLOCREFUSE 1 refuses its first block of 1 KB or more."""
    name = "refused.csv"
    before = HEADER + csv_rows(200)
    assert unit.api.upload(PROJECT, name, before).code == 200
    big = csv_rows(150, 5000).decode()  # over 4 KB, so the plan's copy is a refusable block
    assert len(big) > 4096
    for tool, args, arm, field in (
        ("files.append", {"name": name, "text": big}, "ALLOCREFUSE heap 4096", "heap="),
        ("files.edit", {"name": name, "find": "item0,", "replace": "edited0,"}, "ALLOCREFUSE 1 1024", "refused="),
    ):

        def count():
            line = unit.con("ALLOCREFUSE ?", "ALLOCREFUSE armed=")
            assert line, "BLOCKED: no ALLOCREFUSE console verb on this image"
            return int(line.split(field, 1)[1].split()[0])

        was = count()
        assert "armed" in unit.con(arm, "ALLOCREFUSE"), f"BLOCKED: {arm} did not arm"
        result = _call(unit.api, tool, args)
        now = count()
        record("refused.log", f"{tool} under {arm}: {result[:200]} | refused {was} -> {now}")
        assert now == was + 1, f"{tool}: the armed refusal did not land in the call ({was} -> {now})"
        assert _file(unit.api, name) == before, f"{tool}: the file changed under a refused allocation"
        assert "memory" in result.lower(), f"{tool}: the reply does not say why: {result[:200]}"
        assert "appended" not in result and "edited" not in result, f"{tool}: a refused call claims success"
    unit.con("ALLOCREFUSE off", "ALLOCREFUSE")


@pytest.mark.hil
def test_up_down_up_keeps_engine_two_rows(request, unit):
    images = request.config.getoption("--rollback-images", default=None) or os.environ.get("NIMBUS_ROLLBACK_IMAGES")
    if not images:
        pytest.fail("BLOCKED: no --rollback-images DIR (candidate.bin, v5.1.1.bin, v5.1.2.bin)")
    imgs = {k: os.path.join(images, f"{k}.bin") for k in ("candidate", "v5.1.1", "v5.1.2")}
    missing = [p for p in imgs.values() if not os.path.exists(p)]
    assert not missing, f"BLOCKED: missing images {missing}"
    name = "rollback.csv"
    rows = HEADER + csv_rows(12)
    assert unit.api.upload(PROJECT, name, rows).code == 200
    held = _file(unit.api, name)
    assert held == rows
    record("rollback.log", f"candidate seeded {name}: {len(held)} B sha8 {sha8(held)}")
    for old in ("v5.1.1", "v5.1.2"):
        unit.flash(imgs[old], old)
        unit.rezero(old, console=False)  # a released image has no test console; the API sets and reads back
        now = _file(unit.api, name)
        record("rollback.log", f"{old}: no write, {len(now)} B sha8 {sha8(now)} (was {sha8(held)})")
        assert now == held, f"{old}: the file changed with no write"
        line = f"2026-11-19,{old.replace('.', '')},rollback leg,1.{old[-1]}0,card"
        turn = bd.chat_turn(
            unit.api, f"Append exactly this one line to the file {PROJECT}/{name}, nothing else: {line}", 180
        )
        now = _file(unit.api, name)
        record(
            "rollback.log",
            f"{old}: append turn ok={turn.ok} -> {len(now)} B sha8 {sha8(now)}; rewritten lines "
            f"{rows_rewritten(held, now)}; reply {turn.reply[:160]!r}",
        )
        assert rows_rewritten(held, now) == [], f"{old} rewrote engine-2 rows: lines {rows_rewritten(held, now)}"
        assert line.encode() in now, f"{old}: its append did not land ({turn.error or turn.reply[:120]})"
        held = now
    unit.flash(imgs["candidate"], "candidate")
    unit.attach()
    unit.rezero("candidate again")
    now = _file(unit.api, name)
    record("rollback.log", f"candidate again: {len(now)} B sha8 {sha8(now)} (was {sha8(held)})")
    assert now == held, "the candidate changed the file with no write"
    row = b"2026-11-19,up again,round trip,2.00,card\n"
    _call(unit.api, "files.append", {"name": name, "text": row.decode()})
    assert _file(unit.api, name) == held + row
