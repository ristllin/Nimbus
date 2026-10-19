#!/usr/bin/env python3
"""Decode a device crash report against the ELF that built the crashing image.

After a panic or a watchdog reset the next boot writes three lines to the durable log
(GET /api/errlog, or /log/nimbus.log on the SD card):

    crash: fp=1a2b3c4d new=1 this-reset=1 task=tg_poll pc=0x4201abcd cause=28(LoadProhibited) ... elf=0badf00d1 fw-match=0
    crash-bt: depth=12 corrupt=0 0x4201abcd,0x4201b0c4,...
    crash-why: Stack canary watchpoint triggered (tg_poll)

This tool finds those lines in a log (a file or stdin), checks the ELF is the one the
crash came from (the ``elf=`` field is the first hex digits of the ELF's SHA-256, the
same hash the release gate checks against the image), and prints each program counter
as function + file:line with the toolchain's addr2line. The ELF for a release is the
``firmware-elf-<tag>`` CI artifact (docs/ota.md); an ELF that does not match is refused
unless --force, because a wrong ELF decodes to plausible-looking nonsense.

Usage:
    python3 tools/crash_decode.py --elf firmware-freenove-28.elf [log.txt]
    curl -s -H "X-Nimbus-Token: $TOKEN" http://<device>/api/errlog | python3 tools/crash_decode.py --elf <elf>
Exit 0 = decoded; 1 = no crash report, ELF mismatch, or addr2line missing.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import os
import re
import subprocess
import sys
from typing import List, Optional

CRASH_RE = re.compile(
    r"crash: fp=(?P<fp>[0-9a-f]{8}) .*?task=(?P<task>\S+) pc=0x(?P<pc>[0-9a-f]{8}) .*?elf=(?P<elf>\S+)"
)
BT_RE = re.compile(r"crash-bt: depth=(?P<depth>\d+) corrupt=(?P<corrupt>[01])(?: (?P<pcs>\S+))?")
WHY_RE = re.compile(r"crash-why: (?P<why>.+)$")


def last_report(text: str) -> Optional[dict]:
    """The newest full crash report in a log: the last ``crash:`` line that carries a
    task and PC, with the ``crash-bt:`` / ``crash-why:`` lines that follow it."""
    rep: Optional[dict] = None
    for line in text.splitlines():
        m = CRASH_RE.search(line)
        if m:
            rep = {**m.groupdict(), "pcs": [], "corrupt": None, "why": None}
            continue
        if rep is None:
            continue
        b = BT_RE.search(line)
        if b and not rep["pcs"]:
            rep["pcs"] = [p for p in (b.group("pcs") or "").split(",") if p]
            rep["corrupt"] = b.group("corrupt") == "1"
            continue
        w = WHY_RE.search(line)
        if w and rep["why"] is None:
            rep["why"] = w.group("why")
    return rep


def elf_matches(elf_bytes: bytes, elf_field: str) -> bool:
    """The crash line's ``elf=`` field is a prefix of sha256(ELF)."""
    field = elf_field.strip().lower()
    return bool(re.fullmatch(r"[0-9a-f]{6,64}", field)) and hashlib.sha256(elf_bytes).hexdigest().startswith(field)


def find_addr2line() -> Optional[str]:
    hits = glob.glob(os.path.expanduser("~/.platformio/packages/toolchain-*/bin/xtensa-esp32s3-elf-addr2line"))
    return hits[0] if hits else None


def decode(addr2line: str, elf: str, pcs: List[str]) -> List[str]:
    out = subprocess.run([addr2line, "-pfiaC", "-e", elf, *pcs], capture_output=True, text=True, check=False)
    return out.stdout.splitlines()


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Decode a Nimbus crash report against its ELF")
    ap.add_argument("--elf", required=True, help="the firmware.elf of the crashing release")
    ap.add_argument("--force", action="store_true", help="decode even if the ELF hash does not match")
    ap.add_argument("log", nargs="?", help="durable log text (default: stdin)")
    args = ap.parse_args(argv)

    if args.log:
        with open(args.log, encoding="utf-8", errors="replace") as f:
            text = f.read()
    else:
        text = sys.stdin.read()
    rep = last_report(text)
    if not rep:
        print("no crash report (crash: line with task and pc) in the log")
        return 1
    with open(args.elf, "rb") as f:
        elf_bytes = f.read()
    if not elf_matches(elf_bytes, rep["elf"]):
        print(
            f"ELF mismatch: the crash came from elf={rep['elf']}, this ELF is {hashlib.sha256(elf_bytes).hexdigest()[:9]}"
        )
        if not args.force:
            return 1
    tool = find_addr2line()
    if not tool:
        print("xtensa-esp32s3-elf-addr2line not found (build the firmware once to fetch the toolchain)")
        return 1
    print(f"crash fp={rep['fp']} task={rep['task']} why={rep['why'] or '-'}")
    pcs = rep["pcs"] or ["0x" + rep["pc"]]
    for line in decode(tool, args.elf, pcs):
        print("  " + line)
    if rep["corrupt"]:
        print("  (the backtrace walk hit a corrupted frame; frames past it are missing)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
