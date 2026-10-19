#!/usr/bin/env python3
"""Bench gate (test images): the armed PANIC faults survive the compiler.

``PANIC <kind> arm=turn`` crashes the device inside a turn (src/sys/crash_report.cpp).
Two of its faults were once undone by the optimizer, and nothing but the bench noticed:

* the null store was deleted (a store through a constant null pointer is undefined
  behavior), so ``PANIC nullptr arm=turn`` armed nothing;
* the stack recursion became a loop (tail-recursion elimination), so
  ``PANIC canary arm=turn`` spun the turn task forever and never reached the end of its
  stack.

This check reads the built test image and requires both faults to be real: the
null-store function holds a store instruction, and the recursion function calls itself.
Run it on the test build before the crash-forensics bench legs:

    pio run -e test            # or test-cyd
    python3 tools/release_gate/check_panic_actions.py --elf .pio/build/test/firmware.elf

Exit 0 = both faults present; 1 = a fault was optimized away, a symbol is missing, or the
toolchain is unavailable.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys

NULL_STORE = "faultNullStore"
RECURSION = "faultDeepen"

_HEADER = re.compile(r"^([0-9a-f]+) <(.+)>:\s*$")
_STORE = re.compile(r"\ts32i(?:\.n)?\s")
_CALL = re.compile(r"\tcall(?:0|4|8|12)\s+([0-9a-f]+)\b")


def _tool(name: str) -> "str | None":
    pats = (
        f"~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp-elf-{name}",
        f"~/.platformio/packages/toolchain-*/bin/xtensa-esp32s3-elf-{name}",
    )
    for pat in pats:
        hits = glob.glob(os.path.expanduser(pat))
        if hits:
            return hits[0]
    return None


def functions(disassembly: str) -> "dict[str, tuple[int, list[str]]]":
    """objdump -d text -> {symbol: (start address, instruction lines)}."""
    out: "dict[str, tuple[int, list[str]]]" = {}
    name = None
    for line in disassembly.splitlines():
        m = _HEADER.match(line)
        if m:
            name = m.group(2)
            out[name] = (int(m.group(1), 16), [])
        elif name is not None and line.strip():
            out[name][1].append(line)
    return out


def _one(funcs: "dict[str, tuple[int, list[str]]]", needle: str) -> "tuple[str, int, list[str]] | None":
    hits = [(n, a, body) for n, (a, body) in funcs.items() if needle in n]
    return hits[0] if len(hits) == 1 else None


def judge(funcs: "dict[str, tuple[int, list[str]]]") -> "list[str]":
    """Problems with the two faults (empty = both real)."""
    problems = []
    null = _one(funcs, NULL_STORE)
    if null is None:
        problems.append(f"no single {NULL_STORE} function in the image (inlined, renamed or deleted)")
    elif not any(_STORE.search(ln) for ln in null[2]):
        problems.append(f"{NULL_STORE} holds no store instruction: the null store was optimized away")
    rec = _one(funcs, RECURSION)
    if rec is None:
        problems.append(f"no single {RECURSION} function in the image (inlined, renamed or deleted)")
    else:
        name, start, body = rec
        targets = {int(m.group(1), 16) for ln in body for m in [_CALL.search(ln)] if m}
        if start not in targets:
            problems.append(f"{RECURSION} never calls itself: the recursion became a loop ({name})")
    return problems


def _disassemble(elf: str) -> "str | None":
    nm, objdump = _tool("nm"), _tool("objdump")
    if not nm or not objdump:
        return None
    table = subprocess.run([nm, "-S", elf], capture_output=True, text=True, check=False).stdout
    parts = []
    for line in table.splitlines():
        f = line.split()
        if len(f) == 4 and (NULL_STORE in f[3] or RECURSION in f[3]):
            start, size = int(f[0], 16), int(f[1], 16)
            r = subprocess.run(
                [objdump, "-d", "-C", f"--start-address={start:#x}", f"--stop-address={start + size:#x}", elf],
                capture_output=True,
                text=True,
                check=False,
            )
            parts.append(r.stdout)
    return "\n".join(parts)


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--elf", required=True, help="the TEST build's firmware.elf")
    args = ap.parse_args(argv)
    text = _disassemble(args.elf)
    if text is None:
        print("[gate:panic-actions] !! the xtensa nm/objdump were not found (build once with pio)")
        return 1
    problems = judge(functions(text))
    for p in problems:
        print(f"[gate:panic-actions] !! {p}")
    if problems:
        print("[gate:panic-actions] FAIL")
        return 1
    print("[gate:panic-actions] ok the null store and the stack recursion are real")
    print("[gate:panic-actions] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
