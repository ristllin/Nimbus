"""Host tests for tools/crash_decode.py (the bench and field decoder for crash reports).

Run: python3 -m pytest tools/release_gate
"""

from __future__ import annotations

import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import crash_decode as cd  # noqa: E402

LOG = "\n".join(
    [
        "0 812 boot boot: n=16 fw=v4.5.14 build=b reason=crash(panic)(0x0c) wake=none mode=orch t=0 prev=none",
        "5 1031 boot crash: fp=11111111 new=1 this-reset=1 task=loopTask pc=0x40370000 "
        "cause=29(StoreProhibited) vaddr=0x00000000 size=1000/65536 elf=aaaaaaaa1 fw-match=1",
        "6 1031 boot crash-bt: depth=1 corrupt=0 0x40370000",
        "7 1031 boot crash-why: older crash",
        "0 900 boot boot: n=17 fw=v4.5.14 build=b reason=crash(panic)(0x0c) wake=none mode=orch t=0 prev=none",
        "5 1100 boot crash: fp=1a2b3c4d new=1 this-reset=1 task=tg_poll pc=0x4201abcd "
        "cause=28(LoadProhibited) vaddr=0x00000000 size=23456/65536 elf=0badf00d1 fw-match=0",
        "6 1100 boot crash-bt: depth=3 corrupt=1 0x4201abcd,0x4201b0c4,0x4200beef",
        "7 1100 boot crash-why: Stack canary watchpoint triggered (tg_poll)",
        "8 1200 - [sd] mounted",
    ]
)


def test_last_report_takes_the_newest_crash():
    rep = cd.last_report(LOG)
    assert rep["fp"] == "1a2b3c4d" and rep["task"] == "tg_poll" and rep["pc"] == "4201abcd"
    assert rep["pcs"] == ["0x4201abcd", "0x4201b0c4", "0x4200beef"]
    assert rep["corrupt"] is True
    assert rep["why"] == "Stack canary watchpoint triggered (tg_poll)"
    assert rep["elf"] == "0badf00d1"


def test_stale_and_invalid_lines_are_not_reports():
    stale_only = "1 1 boot crash: image=bad-crc fp=deadbeef new=1 this-reset=1 size=65536/65536\n"
    assert cd.last_report(stale_only) is None
    assert cd.last_report("") is None


def test_elf_match_is_a_sha256_prefix():
    elf = b"\x7fELF bytes"
    sha = hashlib.sha256(elf).hexdigest()
    assert cd.elf_matches(elf, sha[:9])
    assert cd.elf_matches(elf, sha.upper()[:9])
    assert not cd.elf_matches(elf, "0" * 9)
    assert not cd.elf_matches(elf, "-")
    assert not cd.elf_matches(elf, sha[:3])  # too short to identify an ELF


def test_main_refuses_a_mismatched_elf(tmp_path):
    log = tmp_path / "log.txt"
    elf = tmp_path / "firmware.elf"
    log.write_text(LOG)
    elf.write_bytes(b"not the crashing ELF")
    assert cd.main(["--elf", str(elf), str(log)]) == 1
    empty = tmp_path / "empty.txt"
    empty.write_text("no crash here\n")
    assert cd.main(["--elf", str(elf), str(empty)]) == 1
