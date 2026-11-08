#!/usr/bin/env python3
"""Release gate: the Telegram poll task (tg_poll) keeps its stack floor on every path.

tg_poll runs the whole turn: the long poll, head rounds (the cut retry and the claim
correction round included), every tool, STT, media downloads, the staged OTA check and
install, and the Telegram archive. On 8 Nov 2026 Lumi (v4.9.0) logged "tg_poll stack:
min free 936 B of 24576" after a voice note that created a routine, and the 29 Oct crash
2419bf4a aborted on the same task. This gate measures the worst case from the built
image instead of waiting for the field to find it.

How it measures (coverage/STACK-2026-11.md has the full method):
- Frames: the `entry a1, N` of every windowed function in the ELF, the number
  -fstack-usage writes to a .su file (checked equal: pollTask 4944 B in both), which also
  covers the precompiled ESP-IDF libraries no .su file describes. ROM functions come from
  the chip's ROM ELF.
- Calls: `call8`, the -mlongcalls form (`l32r aN, <lit>` then `callx8 aN`), and indirect
  calls resolved three ways, in order: a rule in tg_poll_stack.json (function pointers:
  the hooks main.cpp registers, provider seams, library tables); a std::function call
  (found from the inline chain addr2line -i reports) reaches every `_M_invoke` of that
  exact signature; a virtual call reaches every vtable entry at the same slot whose
  method name is called on the source line. Code that only ever runs on another task
  (the web server, Bluetooth, the relay task...) is listed in the config and never taken
  as an indirect target. An indirect site nothing resolves is charged
  `unresolved_indirect_bytes`.
- Decode: objdump decodes each function linearly and loses step after the alignment
  padding that may follow an unconditional jump. Every stretch whose real entry (a
  branch target of reachable code, or a landing pad from the exception tables) it did
  not decode as an instruction start is decoded again from that entry (resync), so no
  call is invented there and none is missed.
- Recursion: the graph is walked depth-first from the task entry and every back edge is
  cut. A cut cycle must be declared in the config: real recursion with its depth bound
  (the recursive function's frame is counted that many times), or a cycle the
  over-approximation made that cannot happen at run time, with why. An undeclared cycle
  fails the gate.
- A function that moves its stack pointer at run time (a VLA or alloca) needs a bound in
  the config, or the gate fails.
- Fixed costs the frames do not show: the coprocessor save area and TLS block at the top
  of every task stack, and one interrupt exception frame at the deepest point.

The verdict is the worst case from the task entry (vPortTaskWrapper -> pollTask) and, per
named path (turn start, head round, each tool, STT, OTA, archive...), the worst case of
any chain through that path. Each must leave at least `min_free_bytes` of `stack_bytes`.

Usage:
    python3 tools/release_gate/check_tg_poll_stack.py                 # .pio/build/esp32s3-cyd
    python3 tools/release_gate/check_tg_poll_stack.py --elf <firmware.elf> [--report out.md]
Exit 0 = every path leaves the floor; 1 = a path is under it, or the analysis is not
sound (an undeclared cycle or dynamic frame, a path that names nothing, a missing tool or
ELF). A config row the image does not use is reported, not failed: the four release
images share one config.
"""

from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONFIG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tg_poll_stack.json")
DEFAULT_ENVS = ("esp32s3-cyd",)
INCLUDE_ROOTS = ("lib/core/include", "lib/harness/include", "include", "src")
CTX_BASE = 1 << 40  # ids of receiver-bound copies of Print / Stream helpers
RECEIVER_HELPER_RE = re.compile(r"^(Print|Stream)::")

CALL_RE = re.compile(r"^call(0|4|8|12)$")
CALLX_RE = re.compile(r"^callx(0|4|8|12)$")
INSN_RE = re.compile(r"^\s*([0-9a-f]{8}):\s+(\S+)(?:\s+(.*))?$")
LIT_RE = re.compile(r"\(([0-9a-f]{1,8}) <")
STDFN_RE = re.compile(r"^std::function<(.*)>::operator\(\)\(")
STDFN_MANAGE_RE = re.compile(r"^std::function<(.*)>::(function|operator=|~function)\(")
HANDLER = "std::_Function_handler<"


def handler_signature(dname: str, member: str) -> "str | None":
    """The signature of `std::_Function_handler<SIG, Functor>::member(...)`, or None.

    SIG is the first template argument, cut at its top-level comma: the signature
    itself has commas inside brackets (`std::basic_string<char, ...>`)."""
    if not dname.startswith(HANDLER):
        return None
    depth = 0
    for i in range(len(HANDLER), len(dname)):
        c = dname[i]
        if c in "<(":
            depth += 1
        elif c in ">)":
            depth -= 1
        elif c == "," and depth == 0:
            sig = dname[len(HANDLER) : i]
            return sig if ">::" + member + "(" in dname else None
    return None


IDENT_CALL_RE = re.compile(r"(~?[A-Za-z_][A-Za-z0-9_]*)\s*\(")
MEMBER_CALL_RE = re.compile(r"(?:\.|->)\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(")


# ---- toolchain -----------------------------------------------------------------------


def find_tool(name: str) -> str:
    """xtensa-esp32s3-elf-<name> from $NIMBUS_XTENSA_BIN, PlatformIO's toolchain, or PATH."""
    exe = "xtensa-esp32s3-elf-" + name
    dirs = []
    if os.environ.get("NIMBUS_XTENSA_BIN"):
        dirs.append(os.environ["NIMBUS_XTENSA_BIN"])
    core = os.environ.get("PLATFORMIO_CORE_DIR", os.path.expanduser("~/.platformio"))
    dirs.append(os.path.join(core, "packages", "toolchain-xtensa-esp-elf", "bin"))
    for d in dirs:
        p = os.path.join(d, exe)
        if os.access(p, os.X_OK):
            return p
    p = shutil.which(exe)
    if p:
        return p
    raise FileNotFoundError(f"{exe} not found (set NIMBUS_XTENSA_BIN or install the PlatformIO toolchain)")


def find_rom_elf(cfg: dict) -> str:
    if os.environ.get("NIMBUS_ROM_ELF"):
        return os.environ["NIMBUS_ROM_ELF"]
    core = os.environ.get("PLATFORMIO_CORE_DIR", os.path.expanduser("~/.platformio"))
    return os.path.join(core, "packages", "tool-esp-rom-elfs", cfg["rom_elf"])


def run(cmd: list, stdin: "str | None" = None) -> str:
    r = subprocess.run(cmd, input=stdin, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"{os.path.basename(cmd[0])} failed ({r.returncode}): {r.stderr.strip()[:300]}")
    return r.stdout


# ---- the image ---------------------------------------------------------------------


class Fn:
    __slots__ = ("addr", "size", "name", "dname", "frame", "dynamic", "calls", "indirect", "rom", "probes")

    def __init__(self, addr: int, size: int, name: str, rom: bool = False):
        self.addr, self.size, self.name, self.rom = addr, size, name, rom
        self.dname = name
        self.frame = 0
        self.dynamic = False
        self.calls = []  # (site, target addr)
        self.indirect = []  # (site, load offset or None): calls the disassembly cannot resolve
        self.probes = []  # its first instruction addresses (where a lambda's source line is read)


def parse_nm(text: str, rom: bool = False) -> dict:
    """addr -> Fn for every code symbol (t/T/w/W) with a size.

    The ROM ELF leaves its jump-table entries (`__call_memset`: l32r + jx to the real
    memset) unsized; there a symbol runs to the next one.
    """
    funcs = {}
    unsized = []
    for line in text.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in ("t", "T", "w", "W"):
            addr, size = int(p[0], 16), int(p[1], 16)
            if size and addr not in funcs:
                funcs[addr] = Fn(addr, size, p[3], rom)
        elif rom and len(p) == 3 and p[1] in ("t", "T"):
            unsized.append((int(p[0], 16), p[2]))
    starts = sorted(set(funcs) | {a for a, _ in unsized})
    nxt = {a: b for a, b in zip(starts, starts[1:])}
    for addr, name in unsized:
        if addr not in funcs and addr in nxt:
            funcs[addr] = Fn(addr, min(nxt[addr] - addr, 4096), name, rom)
    return funcs


def _regs(args: str) -> list:
    return [a.strip() for a in args.split(",")]


def frame_of(op: str, args: str) -> "int | None":
    if op == "entry":
        parts = _regs(args)
        if len(parts) == 2 and parts[0] == "a1":
            return int(parts[1], 0)
    return None


def call0_adjust(op: str, args: str) -> int:
    """Bytes a call0-ABI function takes with `addi a1, a1, -N` (0 when not such a move)."""
    if op in ("addi", "addmi", "addi.n"):
        parts = _regs(args)
        if len(parts) == 3 and parts[0] == "a1" and parts[1] == "a1":
            v = int(parts[2], 0)
            return -v if v < 0 else 0
    return 0


NO_DEST_RE = re.compile(
    r"^(s8i|s16i|s32i|s32i\.n|s32c1i|s32e|s32ri|s32nb|ss\w*|b\w*|j|jx|call\w*|ret\w*|"
    r"entry|w\w*|memw|isync|dsync|esync|rsync|extw|nop\w*|loop\w*|mula\.\w*|mul\.\w*|"
    r"umul\.\w*|ee\.\w*|simcall|break\w*|ill\w*|rfe|rfi|rfwo|rfwu)$"
)
CLOBBER = {"0": range(0, 16), "4": range(4, 8), "8": range(8, 16), "12": range(12, 16)}


class Regs:
    """The last instruction that wrote each address register, along one linear sweep.

    Control-flow merges are not followed: a write on another branch is not seen, which is
    why only the two forms GCC emits right before an indirect call are read from it.
    """

    def __init__(self):
        self.last = {}
        self.junk = False  # the previous line was data objdump decoded (.byte) or a DSP op

    def note(self, op: str, args: str) -> None:
        self.junk = op.startswith(".") or op.startswith("ee.")
        m = CALL_RE.match(op) or CALLX_RE.match(op)
        if m:
            for r in CLOBBER[m.group(1)]:
                self.last.pop(f"a{r}", None)
            return
        if NO_DEST_RE.match(op) or not args:
            return
        dst = _regs(args)[0]
        if re.fullmatch(r"a\d+", dst):
            self.last[dst] = (op, args)


def _callx(cur: Fn, addr: int, reg: str, regs: Regs, starts: set) -> None:
    """One `callx`: a long call when the register was loaded from a literal, else indirect.

    `l32r aN, <lit>` is the -mlongcalls form: a direct call to the literal's value.
    `l32i aN, aM, K` loads from offset K: a vtable slot when the call is virtual.
    """
    op, args = regs.last.get(reg, ("", ""))
    if op == "l32r":
        lm = LIT_RE.search(args)
        if lm:
            target = int(lm.group(1), 16)
            if target in starts or target >= 0x40000000:
                cur.calls.append((addr, target))  # an address not in the image resolves later
                return
    slot = None
    if op in ("l32i", "l32i.n"):
        parts = _regs(args)
        if len(parts) == 3 and parts[1] != "a1":  # a load from the stack is a spill reload, not a slot
            slot = int(parts[2], 0)
    cur.indirect.append((addr, slot))


def _insn(cur: Fn, addr: int, op: str, args: str, regs: Regs, starts: set) -> None:
    if op == "movsp" and _regs(args)[0] == "a1":
        prev = regs.last.get(_regs(args)[1], ("", ""))
        if prev[0] == "sub":
            cur.dynamic = True
    elif CALL_RE.match(op):
        t = int(args.split()[0], 16)
        if t in starts:
            cur.calls.append((addr, t))
    elif CALLX_RE.match(op) and not regs.junk:
        _callx(cur, addr, args.split()[0] if args else "", regs, starts)
    elif op in ("j", "jx"):
        _tail(cur, addr, op, args, regs, starts)
    regs.note(op, args)


def _tail(cur: Fn, addr: int, op: str, args: str, regs: Regs, starts: set) -> None:
    """A jump into another function's entry (a ROM jump-table trampoline, a tail call) is a call."""
    target = None
    if op == "j":
        target = int(args.split()[0], 16)
    else:
        prev = regs.last.get(args.split()[0] if args else "", ("", ""))
        lm = LIT_RE.search(prev[1]) if prev[0] == "l32r" else None
        target = int(lm.group(1), 16) if lm else None
    if target is not None and target in starts and not (cur.addr <= target < cur.addr + cur.size):
        cur.calls.append((addr, target))


UNCOND_RE = re.compile(r"^(j|jx|ret|ret\.n|retw|retw\.n|ill|ill\.n)$")
TARGET_RE = re.compile(r"\b([0-9a-f]{8})\b")


def _function_body(cur: Fn, insns: list, starts: set, entries: "set | None" = None) -> None:
    """Calls of one function. Code after an unconditional jump or return is dead until a
    branch or a landing pad lands there: objdump decodes the padding and data in it as
    instructions, and a `callx` there that no long-call `l32r` of the same dead stretch
    loads is not a call."""
    targets = set(entries or ())
    for _addr, op, args in insns:
        if _is_branch(op):
            targets.update(int(t, 16) for t in TARGET_RE.findall(args))
    regs = Regs()
    dead = False
    for addr, op, args in insns:
        if addr in targets:
            dead = False
        if dead and (CALLX_RE.match(op) and regs.last.get(args.split()[0] if args else "", ("",))[0] != "l32r"):
            regs.note(op, args)
            continue
        _insn(cur, addr, op, args, regs, starts)
        if UNCOND_RE.match(op):
            dead = True
            regs = Regs()


def _is_branch(op: str) -> bool:
    return (op.startswith("b") and not op.startswith("break")) or op in ("j", "loop", "loopnez", "loopgtz")


def function_bodies(text: str, funcs: dict) -> dict:
    """addr -> [(insn addr, op, args)] of every Fn, from `objdump -d` text.

    Only instructions inside a symbol's [addr, addr+size) count, so literal pools and
    padding objdump decodes as code between functions never add a call.
    """
    bodies = {}
    cur, end = None, 0
    for line in text.splitlines():
        m = INSN_RE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        if addr in funcs:
            cur, end = addr, addr + funcs[addr].size
            bodies[cur] = []
        if cur is None or addr >= end:
            continue
        bodies[cur].append((addr, m.group(2), (m.group(3) or "").strip()))
    return bodies


def parse_disassembly(
    text: str, funcs: dict, known: "set | None" = None, decoder: "Decoder | None" = None, pads: "dict | None" = None
) -> None:
    """Fill frame, calls and indirect sites of every Fn from `objdump -d` text.

    With a decoder, the stretches objdump decoded out of step are decoded again from
    their real entries first (resync); `pads` are the landing pads of each function.
    """
    starts = known if known is not None else set(funcs)
    pads = pads or {}
    bodies = function_bodies(text, funcs)
    if decoder is not None:
        resync(bodies, funcs, pads, decoder)
    for a, insns in bodies.items():
        cur = funcs[a]
        if insns:
            f = frame_of(insns[0][1], insns[0][2])
            cur.frame = f if f is not None else sum(call0_adjust(op, x) for _y, op, x in insns)
        _function_body(cur, insns, starts, pads.get(a))
        cur.probes = [x for x, _op, _args in insns[:24]]


# ---- decode resync --------------------------------------------------------------------
#
# objdump decodes a function linearly. After an unconditional jump the assembler may pad
# to align the next instruction, and objdump then decodes out of step until it happens to
# land on an instruction boundary again: it misses the real instructions there and
# invents others, such as a `call8` into the middle of the image (7631b6a2: a landing pad
# in a runTurn lambda decoded as a call to runTurn, a cycle that is not there). Every real
# entry is known: the function start, a branch target of reachable code, and each landing
# pad the exception tables (.eh_frame -> LSDA) list. A stretch whose entry objdump did not
# decode as an instruction start is decoded again from that entry until it meets the
# linear decode at a shared boundary, and replaces the instructions it overlaps.


def insn_length(first_byte: int) -> int:
    """Bytes of the ESP32-S3 instruction starting with this byte: op0 0-7 is 24-bit, 8-13
    the 16-bit density forms, 14-15 the 32-bit TIE (ee.*) forms. Checked against every
    instruction objdump decodes in the image (test_tg_poll_stack)."""
    op0 = first_byte & 0xF
    return 3 if op0 < 8 else 2 if op0 < 14 else 4


class Decoder:
    """Reads an ELF's loaded bytes and decodes chosen stretches of it with objdump."""

    NOP3 = bytes((0xF0, 0x20, 0x00))  # nop
    NOP2 = bytes((0x3D, 0xF0))  # nop.n
    BLOB_VMA = 0x10000000

    def __init__(self, read, objdump: str):
        self.read, self.objdump = read, objdump

    def length(self, addr: int, op: str = "") -> int:
        if op.startswith("."):
            return 1  # `.byte`: objdump could not decode it
        b = self.read(addr, 1)
        return insn_length(b[0]) if b else 1

    def stretch_end(self, entry: int, starts: set, end: int) -> int:
        """Where a decode from `entry` meets an instruction start of the linear decode."""
        p = entry
        while p < end and (p == entry or p not in starts) and p - entry < 4096:
            p += self.length(p)
        return p

    def decode(self, stretches: list) -> dict:
        """entry -> [(addr, op, args)] for each (entry, stop), with one objdump run.

        The stretches go into one blob, each at the same address mod 4 as in the image
        (l32r and call targets depend on it), joined by nops so the decode stays in step;
        branch, call and literal operands are moved back to the stretch's real address.
        """
        blob, place = bytearray(), []
        for entry, stop in stretches:
            data = self.read(entry, stop - entry)
            if not data:
                continue
            gap = (entry - len(blob)) % 4
            gap = 5 if gap == 1 else gap
            blob += self.NOP3 + self.NOP2 * ((gap - 3) // 2) if gap % 2 else self.NOP2 * (gap // 2)
            place.append((len(blob), entry, len(data)))
            blob += data
        if not place:
            return {}
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "stretches.bin")
            with open(path, "wb") as f:
                f.write(bytes(blob))
            text = run(
                [
                    self.objdump,
                    "-D",
                    "-z",
                    "--no-show-raw-insn",
                    "-b",
                    "binary",
                    "-m",
                    "xtensa",
                    f"--adjust-vma={self.BLOB_VMA:#x}",
                    path,
                ]
            )
        return self._parse(text, place)

    def _parse(self, text: str, place: list) -> dict:
        out = {entry: [] for _o, entry, _n in place}
        i = 0
        for line in text.splitlines():
            m = BLOB_INSN_RE.match(line)
            if not m:
                continue
            off = int(m.group(1), 16) - self.BLOB_VMA
            while i < len(place) and off >= place[i][0] + place[i][2]:
                i += 1
            if i == len(place):
                break
            start, entry, _n = place[i]
            if off < start:
                continue  # the nops between stretches
            delta = entry - (self.BLOB_VMA + start)
            op, args = m.group(2), (m.group(3) or "").strip()
            out[entry].append((entry + off - start, op, self._operands(op, args, delta)))
        return out

    def _operands(self, op: str, args: str, delta: int) -> str:
        """The blob's pc-relative operands as the ELF decode prints them."""
        parts = _regs(args) if args else []
        if not parts or not parts[-1].startswith("0x"):
            return args
        real = (int(parts[-1].split()[0], 16) + delta) & 0xFFFFFFFF  # `0x... (0x<literal>)`
        if op == "l32r":
            raw = self.read(real, 4)
            value = struct.unpack("<I", raw)[0] if raw else 0
            return f"{parts[0]}, {real:08x} <lit> ({value:08x} <x>)"
        if CALL_RE.match(op) or _is_branch(op):
            return ", ".join(parts[:-1] + [f"{real:08x} <x>"])
        return args


BLOB_INSN_RE = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)(?:\s+(.*))?$")


def live_targets(insns: list, entries: set) -> set:
    """Branch targets of the code the entries reach by falling through or branching."""
    flow = [
        (addr, tuple(int(t, 16) for t in TARGET_RE.findall(args)) if _is_branch(op) else (), bool(UNCOND_RE.match(op)))
        for addr, op, args in insns
    ]
    targets = set()
    while True:
        revive = entries | targets
        found = set()
        dead = False
        for addr, goes, stops in flow:
            if addr in revive:
                dead = False
            if dead:
                continue
            found.update(goes)
            dead = stops
        if found <= targets:
            return targets
        targets |= found


def _misaligned(a: int, insns: list, end: int, pads: dict, tried: set) -> list:
    starts = {x for x, _op, _args in insns}
    entries = {a} | pads.get(a, set())
    return [
        e
        for e in sorted(entries | live_targets(insns, entries))
        if a < e < end and e not in starts and (a, e) not in tried
    ]


def _splice(insns: list, redone: list, decoder: Decoder) -> list:
    """insns with each (entry, stop, decoded) stretch put in place of what it overlaps."""
    addrs = [i[0] for i in insns]
    drop, add = set(), {}
    for entry, stop, decoded in redone:
        for k in range(bisect.bisect_left(addrs, entry - 3), bisect.bisect_left(addrs, stop)):
            a, op, _args = insns[k]
            if a >= entry or a + decoder.length(a, op) > entry:
                drop.add(k)
        add.update((i[0], i) for i in decoded if entry <= i[0] < stop)
    keep = [i for k, i in enumerate(insns) if k not in drop and i[0] not in add]
    return sorted(keep + list(add.values()), key=lambda i: i[0])


def resync(bodies: dict, funcs: dict, pads: dict, decoder: Decoder, rounds: int = 6) -> int:
    """Decode again every reachable stretch objdump decoded out of step; returns how many.

    A decoded stretch can hold branches to further misaligned entries, so this repeats
    on the functions it changed."""
    tried, done = set(), 0
    look = [a for a, insns in bodies.items() if insns]
    for _ in range(rounds):
        spans = defaultdict(list)
        for a in look:
            end = a + funcs[a].size
            starts = {x for x, _op, _args in bodies[a]}
            for e in _misaligned(a, bodies[a], end, pads, tried):
                spans[a].append((e, decoder.stretch_end(e, starts, end)))
                tried.add((a, e))
        if not spans:
            break
        decoded = decoder.decode([span for a in spans for span in spans[a]])
        for a, mine in spans.items():
            redone = [(e, stop, decoded[e]) for e, stop in mine if decoded.get(e)]
            bodies[a] = _splice(bodies[a], redone, decoder)
            done += len(redone)
        look = sorted(spans)
    return done


# ---- exception tables: landing pads ------------------------------------------------------


def _uleb(read, p: int) -> "tuple[int, int]":
    value = shift = 0
    while True:
        b = read(p, 1)[0]
        p += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return value, p


def _sleb(read, p: int) -> "tuple[int, int]":
    value, q = _uleb(read, p)
    bits = 7 * (q - p)
    return (value - (1 << bits) if value >> (bits - 1) & 1 else value), q


EH_FORMATS = {0x00: "<I", 0x02: "<H", 0x03: "<I", 0x0A: "<h", 0x0B: "<i"}


def _eh_pointer(read, p: int, enc: int) -> "tuple[int | None, int]":
    """One DW_EH_PE-encoded pointer at p: (value, the address after it)."""
    if enc == 0xFF:
        return None, p
    fmt, base = enc & 0x0F, p
    if fmt in (0x01, 0x09):
        value, p = (_uleb if fmt == 0x01 else _sleb)(read, p)
    elif fmt in EH_FORMATS:
        size = struct.calcsize(EH_FORMATS[fmt])
        value, p = struct.unpack(EH_FORMATS[fmt], read(p, size))[0], p + size
    else:
        raise ValueError(f"pointer encoding {enc:#x} not handled")
    if enc & 0x70 == 0x10:
        value = (value + base) & 0xFFFFFFFF
    elif enc & 0x70:
        raise ValueError(f"pointer encoding {enc:#x} not handled")
    if enc & 0x80:
        value = struct.unpack("<I", read(value, 4))[0]
    return value, p


def _cie(read, p: int) -> dict:
    """The augmentation of the CIE whose body (after its id) starts at p."""
    version = read(p, 1)[0]
    aug, p = b"", p + 1
    while read(p, 1) != b"\0":
        aug, p = aug + read(p, 1), p + 1
    _, p = _uleb(read, p + 1)
    _, p = _sleb(read, p)
    p = p + 1 if version == 1 else _uleb(read, p)[1]
    info = {"z": aug.startswith(b"z"), "fde": 0x00, "lsda": 0xFF}
    if info["z"]:
        _, p = _uleb(read, p)
        for c in aug[1:].decode():
            if c in "LR":
                info["lsda" if c == "L" else "fde"] = read(p, 1)[0]
                p += 1
            elif c == "P":
                _, p = _eh_pointer(read, p + 1, read(p, 1)[0])
    return info


def eh_frames(read, start: int):
    """(function start, LSDA address or None) of every FDE in the .eh_frame at start."""
    cies, p = {}, start
    while True:
        length = struct.unpack("<I", read(p, 4))[0]
        if length in (0, 0xFFFFFFFF):
            return
        body = p + 4
        cie_id = struct.unpack("<I", read(body, 4))[0]
        if cie_id == 0:
            cies[p] = _cie(read, body + 4)
        else:
            cie = cies[body - cie_id]
            pc, q = _eh_pointer(read, body + 4, cie["fde"])
            _, q = _eh_pointer(read, q, cie["fde"] & 0x0F)
            lsda = None
            if cie["z"]:
                _, q = _uleb(read, q)
                lsda, q = _eh_pointer(read, q, cie["lsda"])
            yield pc, lsda
        p = body + length


def landing_pads(read, func: int, lsda: int) -> set:
    """The landing pads one LSDA (.gcc_except_table) lists for the function at func."""
    lp_enc, p = read(lsda, 1)[0], lsda + 1
    lp_start = func
    if lp_enc != 0xFF:
        lp_start, p = _eh_pointer(read, p, lp_enc)
    tt_enc, p = read(p, 1)[0], p + 1
    if tt_enc != 0xFF:
        _, p = _uleb(read, p)
    cs_enc, p = read(p, 1)[0], p + 1
    length, p = _uleb(read, p)
    end, pads = p + length, set()
    while p < end:
        _, p = _eh_pointer(read, p, cs_enc)
        _, p = _eh_pointer(read, p, cs_enc)
        pad, p = _eh_pointer(read, p, cs_enc)
        _, p = _uleb(read, p)
        if pad:
            pads.add(lp_start + pad)
    return pads


def image_landing_pads(read, nm_text: str) -> dict:
    """function addr -> its landing pads, from the image's .eh_frame (`__eh_frame`)."""
    m = re.search(r"^([0-9a-f]{8}) (?:[0-9a-f]+ )?[AaDdRr] __eh_frame$", nm_text, re.M)
    if not m:
        return {}
    pads = {}
    for func, lsda in eh_frames(read, int(m.group(1), 16)):
        if lsda:
            pads[func] = landing_pads(read, func, lsda)
    return pads


def elf_memory(path: str):
    """A function: (addr, n) -> the n bytes the ELF loads at addr (or None)."""
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 1:
        raise ValueError(f"{path}: not a 32-bit little-endian ELF")
    shoff = struct.unpack_from("<I", blob, 0x20)[0]
    shentsize, shnum = struct.unpack_from("<HH", blob, 0x2E)
    secs = []
    for i in range(shnum):
        _n, typ, flags, addr, off, size = struct.unpack_from("<IIIIII", blob, shoff + i * shentsize)
        if flags & 0x2 and typ != 8 and size:  # SHF_ALLOC, not NOBITS
            secs.append((addr, size, off))

    hit = [secs[0] if secs else (0, 0, 0)]

    def read(a: int, n: int) -> "bytes | None":
        addr, size, off = hit[0]
        if not (addr <= a and a + n <= addr + size):
            sec = next((x for x in secs if x[0] <= a and a + n <= x[0] + x[1]), None)
            if sec is None:
                return None
            hit[0] = addr, size, off = sec
        return blob[off + a - addr : off + a - addr + n]

    return read


def elf_reader(path: str):
    """A function: addr -> the 4-byte little-endian word the ELF loads there (or None)."""
    read = elf_memory(path)

    def word(a: int) -> "int | None":
        raw = read(a, 4)
        return struct.unpack("<I", raw)[0] if raw else None

    return word


def _strip_params(dname: str) -> str:
    i = dname.rfind(")")
    depth = 0
    while i >= 0:
        c = dname[i]
        if c == ")":
            depth += 1
        elif c == "(":
            depth -= 1
            if depth == 0:
                break
        i -= 1
    return dname[:i] if i > 0 else dname


def split_name(dname: str) -> "tuple[str, str]":
    """(qualifier, unqualified name) of a demangled function: `ns::Cls::read(int)` -> (`ns::Cls`, `read`)."""
    for prefix in ("non-virtual thunk to ", "virtual thunk to "):
        if dname.startswith(prefix):
            dname = dname[len(prefix) :]
    head = _strip_params(dname)
    depth = 0
    cut = 0
    for j, c in enumerate(head):
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
        elif c == ":" and depth == 0 and head[j + 1 : j + 2] == ":":
            cut = j + 2
    return head[: max(cut - 2, 0)], head[cut:]


def plain(dname: str) -> str:
    """A name without the receiver binding the walk adds (` [this: HWCDC]`)."""
    cut = dname.find(" [this: ")
    return dname if cut < 0 else dname[:cut]


def last_name(qualified: str) -> str:
    """`ns::Cls` -> `Cls` (template arguments kept)."""
    depth = 0
    cut = 0
    for j, c in enumerate(qualified):
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
        elif c == ":" and depth == 0 and qualified[j + 1 : j + 2] == ":":
            cut = j + 2
    return qualified[cut:]


def method_name(dname: str) -> str:
    """The unqualified name of a demangled function: `ns::Cls::read(int)` -> `read`; any destructor -> `~`."""
    name = split_name(dname)[1]
    return "~" if name.startswith("~") else name


def vtable_slots(nm_text: str, word, funcs: dict) -> dict:
    """(method name, byte offset from an address point) -> sorted addrs, from every vtable.

    An address point is where an object's vptr points: two words past an offset-to-top
    (0, or a small negative number for a secondary base) and a typeinfo pointer (0 here:
    the firmware builds with -fno-rtti). A class with several bases has several.
    """
    typeinfo, tables = _vtable_symbols(nm_text)
    out = defaultdict(set)
    for base, size in tables:
        words = [word(a) for a in range(base, base + size, 4)]
        point = None
        for i, w in enumerate(words):
            if w not in funcs:
                continue
            if i >= 2 and _address_point(words[i - 2], words[i - 1], typeinfo):
                point = i
            if point is not None:
                out[(method_name(funcs[w].dname), (i - point) * 4)].add(w)
    return {k: sorted(v) for k, v in out.items()}


def _vtable_symbols(nm_text: str) -> "tuple[set, list]":
    """(typeinfo addresses, [(vtable addr, size)]) from `nm -S` text."""
    typeinfo = set()
    tables = []
    for line in nm_text.splitlines():
        p = line.split()
        if len(p) != 4:
            continue
        if p[3].startswith("_ZTI"):
            typeinfo.add(int(p[0], 16))
        elif p[3].startswith("_ZTV"):
            tables.append((int(p[0], 16), int(p[1], 16)))
    return typeinfo, tables


def _address_point(offset_to_top: "int | None", typeinfo_word: "int | None", typeinfo: set) -> bool:
    """The two words before an address point: offset-to-top (0, or small negative) and typeinfo."""
    top_ok = offset_to_top == 0 or (offset_to_top or 0) >= 0xFFFF0000
    return top_ok and (typeinfo_word == 0 or typeinfo_word in typeinfo)


STRING_RE = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


def code_only(line: str) -> str:
    """A source line without its string literals and comments."""
    line = STRING_RE.sub('""', line)
    line = re.sub(r"/\*.*?\*/", " ", line)
    return line.split("//", 1)[0]


class Sources:
    """Source lines by path, read once (a missing file reads as empty).

    The ELF names this checkout's files. `root`, a checkout of the commit the ELF was
    built from, is read in their place: a call resolved by its source line must read the
    line the compiler saw (an older image read against today's files resolves wrongly).
    """

    def __init__(self, root: "str | None" = None):
        self.cache = {}
        self.joined = {}
        self.root = os.path.abspath(root) if root else None

    def local(self, path: str) -> str:
        if self.root and (path == REPO or path.startswith(REPO + os.sep)):
            return self.root + path[len(REPO) :]
        return path

    def headers(self, path: str) -> list:
        """The sibling header, then the project headers the file #includes (one level)."""
        stem = os.path.splitext(path)[0]
        out = [stem + ext for ext in (".h", ".hpp") if stem + ext != path]
        self.line(path + ":1")
        roots = [os.path.dirname(path)] + [os.path.join(REPO, d) for d in INCLUDE_ROOTS]
        for line in self.cache.get(path, []):
            m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
            if not m:
                continue
            for root in roots:
                cand = os.path.normpath(os.path.join(root, m.group(1)))
                if os.path.exists(self.local(cand)):
                    out.append(cand)
                    break
        for h in out:
            self.line(h + ":1")
        return out

    def text(self, loc: str) -> str:
        path = loc.split(" (discriminator")[0].rpartition(":")[0]
        if path not in self.joined:
            self.line(loc)
            self.joined[path] = "\n".join(self.cache.get(path, []))
        return self.joined[path]

    def line(self, loc: str) -> str:
        loc = loc.split(" (discriminator")[0]
        path, _, num = loc.rpartition(":")
        if not path or not num.isdigit():
            return ""
        if path not in self.cache:
            try:
                with open(self.local(path), encoding="utf-8", errors="replace") as f:
                    self.cache[path] = f.read().splitlines()
            except OSError:
                self.cache[path] = []
        lines = self.cache[path]
        n = int(num)
        return lines[n - 1] if 0 < n <= len(lines) else ""


def demangle(funcs: dict, cxxfilt: str) -> None:
    """Demangled names. The Xtensa toolchain marks a GCC clone with `$isra$0` or
    `$constprop$0` where other targets write `.isra.0`; c++filt leaves such a name
    mangled, so the base is demangled alone and the clone tag kept as a suffix."""
    order = list(funcs.values())
    bases = [f.name.split("$", 1)[0] for f in order]
    out = run([cxxfilt], "\n".join(bases) + "\n").splitlines()
    for f, base, d in zip(order, bases, out):
        f.dname = d + (" [" + f.name[len(base) :].replace("$", " ").strip() + "]" if f.name != base else "")


def addr2line(elf: str, tool: str, addrs: list) -> dict:
    """addr -> [(function, file:line), ...] innermost inline frame first."""
    if not addrs:
        return {}
    text = run([tool, "-a", "-f", "-i", "-C", "-e", elf], "\n".join(f"{a:08x}" for a in addrs) + "\n")
    out = {}
    cur = None
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("0x"):
            cur = int(ln, 16)
            out[cur] = []
            i += 1
        elif cur is not None and i + 1 < len(lines):
            out[cur].append((ln, lines[i + 1]))
            i += 2
        else:
            i += 1
    return out


def lambda_parent(dname: str) -> "str | None":
    """The function a std::function handler's lambda is defined in, from the handler's name."""
    sig = handler_signature(dname, "_M_invoke")
    if sig is None:
        return None
    rest = dname[len(HANDLER) + len(sig) + 2 :]
    cut = rest.find("::{lambda")
    return rest[:cut] if cut > 0 else None


STDLIB_LOC_RE = re.compile(r"/include/c\+\+/|/bits/|^\?\?")
LAMBDA_OPEN_RE = re.compile(r"\[[^\]]*\]\s*(\(|\{|mutable)")
ASSIGNED_RE = re.compile(r"([A-Za-z_]\w*)\s*=\s*\[")


def lambda_names(sources: "Sources", loc: str) -> set:
    """Names a lambda is assigned to (`d.headHost = [..]` -> headHost): read from the
    nearest lambda introducer at or above a line of its body."""
    path, _, num = loc.split(" (discriminator")[0].rpartition(":")
    if not num.isdigit():
        return set()
    sources.line(loc)
    lines = sources.cache.get(path, [])
    for i in range(min(int(num), len(lines)) - 1, max(int(num) - 80, 0) - 1, -1):
        text = code_only(lines[i])
        if LAMBDA_OPEN_RE.search(text):
            return set(ASSIGNED_RE.findall(text))
    return set()


GDB_IN_RE = re.compile(r"^0x([0-9a-f]+) is in (.*) \((.+):(\d+)\)\.$")


def find_gdb() -> "str | None":
    core = os.environ.get("PLATFORMIO_CORE_DIR", os.path.expanduser("~/.platformio"))
    p = os.path.join(core, "packages", "tool-xtensa-esp-elf-gdb", "bin", "xtensa-esp-elf-gdb-no-python")
    return p if os.access(p, os.X_OK) else shutil.which("xtensa-esp-elf-gdb-no-python")


def gdb_lines(elf: str, gdb: str, addrs: list) -> dict:
    """addr -> [(innermost function, file:line)] from GDB, which reads DWARF without trusting
    .debug_aranges (see repair_inlines)."""
    if not addrs:
        return {}
    with tempfile.NamedTemporaryFile("w", suffix=".gdb", delete=False) as f:
        f.write("set listsize 1\n" + "".join(f"list *0x{a:x}\n" for a in addrs))
        cmd = f.name
    try:
        text = run([gdb, "-batch", "-nx", "-x", cmd, elf])
    finally:
        os.unlink(cmd)
    out = {}
    for line in text.splitlines():
        m = GDB_IN_RE.match(line.strip())
        if m:
            out[int(m.group(1), 16)] = [(m.group(2), f"{m.group(3)}:{m.group(4)}")]
    return out


def repair_inlines(elf: str, funcs: dict, inlines: dict) -> int:
    """Replace inline chains addr2line got wrong with GDB's innermost frame; returns how many
    (minus how many need it when GDB is not installed: the caller fails the analysis).

    Some links carry a .debug_aranges entry that points past .debug_info (GDB warns about
    it); BFD trusts the table and then maps whole functions to an unrelated template, with
    no line. A chain whose outermost function is not the function the site is in is that
    case. GDB's answer names only the innermost frame, so the chain gets the containing
    function as its outer frame and no call-site line for it.
    """
    owner = {}
    for f in funcs.values():
        if not f.rom:
            for s, _slot in f.indirect:
                owner[s] = f
    bad = [s for s, ch in inlines.items() if s in owner and _chain_wrong(ch, owner[s])]
    if not bad:
        return 0
    gdb = find_gdb()
    if gdb is None:  # the wrong chains would resolve these calls wrongly: fail closed
        return -len(bad)
    fixed = gdb_lines(elf, gdb, bad)
    for s, ch in fixed.items():
        inner = ch[0]
        caller = owner[s].dname
        inlines[s] = [inner] if plain_name(inner[0]) == plain_name(caller) else [inner, (caller, "??:0")]
    return len(fixed)


def plain_name(dname: str) -> str:
    return _strip_params(plain(dname).split(" [")[0])


def _chain_wrong(chain: list, caller: Fn) -> bool:
    if not chain or chain[0][1].startswith("??"):
        return True
    return plain_name(chain[-1][0]) != plain_name(caller.dname)


def short_loc(loc: str) -> str:
    loc = loc.split(" (discriminator")[0]
    for marker in (
        "/nimbus/",
        "/esp-idf/components/",
        "/packages/framework-arduinoespressif32/",
        "/libdeps/esp32s3-cyd/",
        "/libdeps/",
    ):
        if marker in loc:
            return loc.split(marker, 1)[1]
    return "/".join(loc.split("/")[-2:])


# ---- the model ---------------------------------------------------------------------


class Model:
    def __init__(
        self, funcs: dict, cfg: dict, inlines: dict, vslots: "dict | None" = None, sources: "Sources | None" = None
    ):
        self.funcs = funcs
        self.cfg = cfg
        self.inlines = inlines
        self.vslots = vslots or {}
        self.vnames = defaultdict(set)
        for (name, _slot), addrs in self.vslots.items():
            self.vnames[name].update(addrs)
        self.sources = sources or Sources()
        self.errors = []
        self.edges = defaultdict(list)  # caller -> [(site, callee)]
        self.unresolved = defaultdict(list)  # caller -> [site]
        self.hits = defaultdict(int)  # config row -> times it applied
        self.how = defaultdict(int)  # how indirect sites were resolved
        self.cache = {}
        self.elsewhere = [re.compile(r["function"]) for r in cfg.get("not_on_tg_poll", [])]
        self.dropped = 0
        self.mult = {}
        self.deferred = []  # (caller, site, candidate handlers) of generic std::function calls
        self.edge_set = set()
        self.fallen_back = set()
        self.callers = None
        self.callers_seen = []
        self.ctx_ids = {}  # (helper addr, receiver class) -> the bound copy's id
        self.ctx_class = {}  # bound copy id -> receiver class
        self.cur_ctx = None
        self.lambda_names = {}  # handler -> names its lambda is assigned to

    def match(self, pattern: str) -> list:
        if pattern not in self.cache:
            rx = re.compile(pattern)
            self.cache[pattern] = sorted(a for a, f in self.funcs.items() if rx.search(f.dname))
        return self.cache[pattern]

    def invokers(self, sig: str) -> list:
        key = ("invoke", sig)
        if key not in self.cache:
            hits = [a for a, f in self.funcs.items() if handler_signature(f.dname, "_M_invoke") == sig]
            self.cache[key] = sorted(hits)
        return self.cache[key]

    def on_tg_poll(self, targets) -> list:
        """Drop indirect targets that only ever run on another task (config not_on_tg_poll)."""
        out = []
        for t in sorted(targets):
            name = self.funcs[t].dname
            hit = next((i for i, rx in enumerate(self.elsewhere) if rx.search(name)), None)
            if hit is None:
                out.append(t)
            else:
                self.hits["elsewhere|" + self.cfg["not_on_tg_poll"][hit]["function"]] += 1
                self.dropped += 1
        return out

    def _by_rule(self, names: list, locs: list) -> "list | None":
        """The first indirect rule whose `in` names a function of the inline chain (and whose
        `at`, if any, a location of it). `{1}` in a target is the `in` match's first group."""
        for rule in self.cfg["indirect"]:
            m = next((m for m in (re.search(rule["in"], n) for n in names) if m), None)
            if m is None:
                continue
            if "at" in rule and not any(re.search(rule["at"], loc) for loc in locs):
                continue
            self.hits["indirect|" + rule["in"] + "|" + rule.get("at", "")] += 1
            self.how["config rule"] += 1
            return self.on_tg_poll(self._rule_targets(rule, m))
        return None

    def _rule_targets(self, rule: dict, m) -> set:
        out = set()
        for pat in rule["targets"]:
            out.update(self.match(pat.replace("{1}", m.group(1) or "") if "{1}" in pat else pat))
        sc = self.cfg.get("stdout_context")
        if self.cur_ctx == "stdout" and sc:  # the console stream: its device, never a file
            out = {t for t in out if not re.search(sc["files"], self.funcs[t].dname)}
        return out

    def managers(self, sig: "str | None") -> list:
        """_M_manager functions (copy, move, destroy a stored functor) of one signature, or of all."""
        key = ("manager", sig)
        if key not in self.cache:
            hits = []
            for a, f in self.funcs.items():
                if f.dname.startswith("std::_Function_base::_Base_manager<") and "::_M_manager(" in f.dname:
                    hits.append(a)
                    continue
                own = handler_signature(f.dname, "_M_manager")
                if own is not None and (sig is None or own == sig):
                    hits.append(a)
            self.cache[key] = sorted(hits)
        return self.cache[key]

    def _by_signature(self, names: list, chain: list, caller: Fn, site: int) -> "list | None":
        """A std::function call reaches the functors stored as that exact signature.

        Narrowed, in order: to the lambdas assigned to the field the call names
        (`d.headHost = [..]` for `d.headHost()`, read from each lambda's source); for a
        signature many functors share, to the lambdas created by code this task runs
        (decided after the walk, see `deferred`); otherwise every functor of the
        signature. A copy, assignment or destruction reaches that signature's managers.
        """
        for i, n in enumerate(names):
            m = STDFN_RE.match(n)
            if m:
                sig = m.group(1)
                cands = self.invokers(sig)
                bound = self.bind_by_field(cands, chain[i + 1][1] if i + 1 < len(chain) else "")
                if bound:
                    self.how["std::function, by the field it is called through"] += 1
                    return self.on_tg_poll(bound)
                if len(cands) > self.cfg.get("generic_signature_functors", 8):
                    self.deferred.append((caller.addr, site, cands))
                    self.how["std::function, by the functors this task creates"] += 1
                    return []
                self.how["std::function, by signature"] += 1
                return self.on_tg_poll(cands)
            m = STDFN_MANAGE_RE.match(n)
            if m or n.startswith("std::_Function_base::~_Function_base()"):
                self.how["std::function manager, by signature"] += 1
                return self.on_tg_poll(self.managers(m.group(1) if m else None))
        return None

    def bind_by_field(self, cands: list, loc: str) -> list:
        """The candidates whose lambda is assigned to a name the call site calls."""
        if not loc:
            return []
        # Only a member access binds (`d.headHost()`, `cfg->validate(...)`): a bare call
        # (`read(buf, n)`) is a parameter or a local, which any caller may have filled.
        called = set(MEMBER_CALL_RE.findall(code_only(self.sources.line(loc))))
        return [h for h in cands if called & self.lambda_names.get(h, set())]

    def creators_reached(self, cands: list) -> list:
        """Handlers whose lambda is created inside a function this task reaches."""
        out = []
        for h in cands:
            parent = lambda_parent(self.funcs[h].dname)
            if parent and parent in self.reach_names:
                out.append(h)
        return out

    def _by_vtable(self, locs: list, slot: "int | None", names: list) -> "list | None":
        """A virtual call loads its target from a vtable slot: the methods at that slot named on the line."""
        if not locs:
            return None
        text = code_only(self.sources.line(locs[0]))
        called = set(IDENT_CALL_RE.findall(text))
        if re.search(r"\bdelete\b", text):
            called.add("~")
        out = set()
        for name in called:
            cls = self.deleted_class(names) if name == "~" else self.receiver_class(locs[0], text, name, names)
            known = cls is not None and self.known_class(cls)
            if slot is None and not known:
                continue  # no slot and no receiver type: nothing ties this call to a vtable
            found = set(self.vslots.get((name, slot), ()) if slot is not None else self.vnames.get(name, ()))
            if found and known:
                found = {t for t in found if self.derives(last_name(split_name(self.funcs[t].dname)[0]), cls)}
            out.update(found)
        if not out:
            return None
        out = self.same_hierarchy(out, locs[0], names)
        self.how["virtual, by slot and source line"] += 1
        return self.on_tg_poll(out)

    def known_class(self, cls: str) -> bool:
        bases = self.cfg.get("bases", {})
        return cls in bases or any(cls in v for v in bases.values())

    def derives(self, cls: str, base: str) -> bool:
        """cls is base or derives from it, by the config's bases map (unqualified names)."""
        bases = self.cfg.get("bases", {})
        todo, seen = [cls], set()
        while todo:
            c = todo.pop()
            if c == base:
                return True
            if c not in seen:
                seen.add(c)
                todo.extend(bases.get(c, []))
        return False

    def receiver_class(self, loc: str, code: str, name: str, names: list) -> "str | None":
        """The static class a virtual call goes through, when the source says it.

        `name(...)` alone is a call on this: the enclosing method's class. `x.name(...)` or
        `x->name(...)` goes through x's declared type, the nearest declaration of x above
        the call in the same file (a smart pointer's pointee; a config alias for a typedef).
        """
        m = re.search(r"([A-Za-z_]\w*)\s*(?:\.|->)\s*" + re.escape(name) + r"\s*\(", code)
        if m is None:
            if re.search(r"(?<![\w.>:~])" + re.escape(name) + r"\s*\(", code) and names:
                return self.cur_ctx or last_name(split_name(names[0])[0]) or None
            return None
        return self.declared_type(loc, m.group(1))

    @staticmethod
    def deleted_class(names: list) -> "str | None":
        """The static type a `delete` goes through: the unique_ptr or default_delete it is inlined from."""
        for n in names:
            m = re.match(r"^std::(?:default_delete|unique_ptr)<([^,<>]+)", n)
            if m:
                return last_name(m.group(1).strip())
        return None

    def declared_type(self, loc: str, ident: str) -> "str | None":
        """ident's declared class: the nearest declaration above the call, else in the sibling header."""
        aliases = self.cfg.get("aliases", {})
        if ident in aliases:  # a macro or global the source never declares (Serial)
            return aliases[ident]
        path, _, num = loc.split(" (discriminator")[0].rpartition(":")
        if not num.isdigit():
            return None
        decl = re.compile(
            r"((?:[A-Za-z_]\w*::)*[A-Za-z_]\w*)\s*(<[^;(){}]*>)?\s*(?:const\s*)?[&*]*\s*(?:const\s*)?"
            r"\b" + re.escape(ident) + r"\b\s*(?:[;,)=({\[]|$)"
        )
        self.sources.line(loc)
        lines = self.sources.cache.get(path, [])
        found = self._scan_decl(decl, lines[: int(num)][::-1])
        for header in self.sources.headers(path) if found is None else ():
            found = self._scan_decl(decl, self.sources.cache.get(header, []))
            if found is not None:
                break
        return found

    def _scan_decl(self, decl, lines) -> "str | None":
        for line in lines:
            for m in decl.finditer(code_only(line)):
                typ, args = m.group(1), m.group(2) or ""
                if typ in ("return", "else", "case", "delete", "new", "sizeof", "const", "auto", "static", "if"):
                    continue
                if re.search(r"(unique_ptr|shared_ptr)$", typ) and args:
                    typ = re.findall(r"[A-Za-z_]\w*", args)[-1]
                return self.cfg.get("aliases", {}).get(last_name(typ), last_name(typ))
        return None

    def same_hierarchy(self, targets: set, loc: str, names: list) -> set:
        """Keep the candidates whose class hierarchy the call site names (config hierarchies).

        Slot and method name do not tell `fs::File::write` from `CardWriter::write`: both
        sit at the same slot of unrelated vtables. A call site names the type it calls
        through, in its source file or in the inline chain's template arguments, so a
        hierarchy no word of which appears there is not the one called. Candidates of no
        listed hierarchy are always kept; when the site names none, all are kept.
        """
        text = self.sources.text(loc) + "\n" + "\n".join(names)
        rows = self.cfg.get("hierarchies", [])
        tagged = {}
        for t in targets:
            cls = split_name(self.funcs[t].dname)[0]
            tagged[t] = next((i for i, r in enumerate(rows) if any(re.search(c, cls) for c in r["classes"])), None)
        named = set()
        for i in {v for v in tagged.values() if v is not None}:
            if any(re.search(r"\b" + re.escape(w) + r"\b", text) for w in rows[i]["names"]):
                named.add(i)
                self.hits["hierarchy|" + rows[i]["names"][0]] += 1
        if not named:
            return targets
        return {t for t, i in tagged.items() if i is None or i in named}

    def resolve_site(self, caller: Fn, site: int, slot: "int | None") -> "list | None":
        """Targets of one indirect call site, or None when nothing names them."""
        chain = self.inlines.get(site, [])
        names = [n for n, _ in chain] + [caller.dname]
        locs = [loc for _, loc in chain]
        found = self._by_rule(names, locs)
        if found is None:
            found = self._by_signature(names, chain, caller, site)
        if found is None:
            found = self._by_vtable(locs, slot, names)
        if found is None:
            self.how["unresolved"] += 1
        return found

    def build(self, root: int) -> None:
        """Edges reachable from root, with indirect sites resolved; a deferred std::function
        site gets its targets once the walk knows which lambdas this task creates, and
        the walk goes on from them until nothing new is reached."""
        self.reach = set()
        self.reach_names = set()
        todo = [root]
        while True:
            while todo:
                self._walk(todo)
                todo = self._resolve_deferred(fallback=False)
            todo = self._resolve_deferred(fallback=True)
            if not todo:
                break
        for a in self.edges:
            self.edges[a] = sorted(set(self.edges[a]))

    def ancestors(self, fn: int, levels: int) -> set:
        """Names of the functions up to `levels` calls above fn (direct and resolved edges)."""
        if self.callers is None or len(self.callers_seen) != len(self.edge_set) + len(self.reach):
            self.callers = defaultdict(set)
            for a in self.reach:
                for _site, t in self.funcs[a].calls:
                    self.callers[t].add(a)
            for a, es in self.edges.items():
                for _site, t in es:
                    self.callers[t].add(a)
            self.callers_seen = [0] * (len(self.edge_set) + len(self.reach))
        out, frontier = {fn}, {fn}
        for _ in range(levels):
            frontier = {c for f in frontier for c in self.callers.get(f, ())} - out
            out |= frontier
        return {self.funcs[a].dname for a in out}

    def _resolve_deferred(self, fallback: bool) -> list:
        """Targets of the deferred std::function sites, called through a parameter or a
        local: the lambdas created in the calling function or up to three calls above it
        (the lambda was passed down); with `fallback`, a site with none such is unresolved."""
        todo = []
        for caller, site, cands in self.deferred:
            above = self.ancestors(caller, 3)
            created = [h for h in cands if lambda_parent(self.funcs[h].dname) in above]
            if not created and fallback and site not in self.fallen_back:
                self.fallen_back.add(site)
                self.unresolved[caller].append(site)
            for t in self.on_tg_poll(created):
                if (site, t) not in self.edge_set:
                    self.edge_set.add((site, t))
                    self.edges[caller].append((site, t))
                    todo.append(t)
        return todo

    def with_receiver(self, a: int, f: Fn, site: int, t: int) -> int:
        """A call into a Print or Stream helper (print, println, readBytes...) gets a copy of
        that helper bound to the receiver's class, so the helper's own virtual write or
        read goes to that class only: Serial.println reaches HWCDC::write, never the TLS
        client a client.print in other code reaches. Inside a bound helper the binding
        follows its calls on this; a receiver the source does not show stays unbound."""
        callee = self.funcs[t]
        stream = self.stdout_binding(a, callee)
        if stream is not None:
            return self.bound_copy(t, stream)
        if not RECEIVER_HELPER_RE.match(callee.dname):
            return t
        cls = self.ctx_class.get(a) if RECEIVER_HELPER_RE.match(f.dname) else None
        if cls is None:
            chain = self.inlines.get(site, [])
            if chain:
                code = code_only(self.sources.line(chain[0][1]))
                m = re.search(r"([A-Za-z_]\w*)\s*(?:\.|->)\s*" + re.escape(method_name(callee.dname)) + r"\s*\(", code)
                cls = self.declared_type(chain[0][1], m.group(1)) if m else None
        if cls is None or not self.known_class(cls):
            return t
        return self.bound_copy(t, cls)

    def stdout_binding(self, a: int, callee: Fn) -> "str | None":
        """`stdout` when the call prints to the console stream (config stdout_context): a
        printf-family entry, or a stdio internal called from a copy already bound to it."""
        sc = self.cfg.get("stdout_context")
        if not sc:
            return None
        if re.search(sc["entries"], callee.dname):
            self.hits["stdout|entries"] += 1
            return "stdout"
        if self.ctx_class.get(a) == "stdout" and re.search(sc["family"], callee.dname):
            return "stdout"
        return None

    def bound_copy(self, t: int, cls: str) -> int:
        callee = self.funcs[t]
        key = (t, cls)
        if key not in self.ctx_ids:
            nid = CTX_BASE + len(self.ctx_ids)
            g = Fn(nid, callee.size, callee.name, callee.rom)
            g.dname = f"{callee.dname} [this: {cls}]"
            g.frame, g.dynamic, g.calls, g.indirect = callee.frame, callee.dynamic, callee.calls, callee.indirect
            self.funcs[nid] = g
            self.ctx_ids[key] = nid
            self.ctx_class[nid] = cls
        return self.ctx_ids[key]

    def impossible_call(self, caller: Fn, callee: int) -> bool:
        """A direct call the config shows cannot run (an error branch whose condition the
        only callers never meet), with why."""
        for r in self.cfg.get("impossible_calls", []):
            if re.search(r["caller"], plain(caller.dname)) and re.search(r["callee"], plain(self.funcs[callee].dname)):
                self.hits["impossible-call|" + r["caller"] + "|" + r["callee"]] += 1
                return True
        return False

    def _walk(self, todo: list) -> None:
        terminal = [re.compile(r["function"]) for r in self.cfg.get("terminal", [])]
        while todo:
            a = todo.pop()
            if a in self.reach or a not in self.funcs:
                continue
            self.reach.add(a)
            f = self.funcs[a]
            hit = next((i for i, rx in enumerate(terminal) if rx.search(plain(f.dname))), None)
            if hit is not None:  # never returns: the device restarts; what it calls is not followed
                self.hits["terminal|" + self.cfg["terminal"][hit]["function"]] += 1
                continue
            todo.extend(self._direct_calls(a, f))
            todo.extend(self._indirect_calls(a, f))

    def _direct_calls(self, a: int, f: Fn) -> list:
        out = []
        for site, t in f.calls:
            if t not in self.funcs:
                self.unresolved[a].append(site)
            elif not self.impossible_call(f, t):
                t = self.with_receiver(a, f, site, t)
                self.edges[a].append((site, t))
                out.append(t)
        return out

    def _indirect_calls(self, a: int, f: Fn) -> list:
        self.cur_ctx = self.ctx_class.get(a)
        out = []
        for site, slot in f.indirect:
            targets = self.resolve_site(f, site, slot)
            if targets is None:
                self.unresolved[a].append(site)
                continue
            for t in targets:
                t = self.bound_copy(t, "stdout") if self.stdout_binding(a, self.funcs[t]) else t
                self.edge_set.add((site, t))
                self.edges[a].append((site, t))
                out.append(t)
        return out

    def cut_cycles(self, root: int) -> None:
        """Depth-first from root; every back edge is cut. Leaves self.order (callers first)."""
        state = {root: 1}  # 1 on the walk, 2 done
        post = []
        self.back = []  # (caller, site, callee)
        self.cyclic = set()  # every function on a cut cycle
        self.dag = defaultdict(list)
        stack = [(root, 0)]
        while stack:
            v, i = stack[-1]
            succ = self.edges.get(v, [])
            if i == len(succ):
                state[v] = 2
                post.append(v)
                stack.pop()
                continue
            stack[-1] = (v, i + 1)
            site, w = succ[i]
            if state.get(w) == 1:
                self.back.append((v, site, w))
                on_walk = [n for n, _ in stack]
                self.cyclic.update(on_walk[on_walk.index(w) :])  # the cycle: w ... v
                continue
            self.dag[v].append((site, w))
            if w not in state:
                state[w] = 1
                stack.append((w, 0))
        self.order = post[::-1]

    def declare_cycles(self) -> None:
        """Every cut cycle is declared real (with a depth) or impossible (with why).

        A real one counts the frame of every reachable function its row names `depth`
        times, so a mutual recursion (parseVariant -> parseObject -> parseVariant) is paid
        for at every level, not only at the function the walk happened to cut at.
        """
        rec = self.cfg.get("recursion", [])
        fake = self.cfg.get("impossible_cycles", [])
        for a in self.cyclic:
            row = next((r for r in rec if re.search(r["function"], plain(self.funcs[a].dname))), None)
            if row is not None:
                self.mult[a] = max(self.mult.get(a, 1), row["depth"])
        for caller, _site, callee in self.back:
            names = (plain(self.funcs[caller].dname), plain(self.funcs[callee].dname))
            row = next((r for r in rec if any(re.search(r["function"], n) for n in names)), None)
            if row is not None:
                self.hits["recursion|" + row["function"]] += 1
                continue
            row = next((r for r in fake if re.search(r["caller"], names[0]) and re.search(r["callee"], names[1])), None)
            if row is not None:
                self.hits["impossible|" + row["caller"] + "|" + row["callee"]] += 1
                continue
            self.errors.append(f"undeclared cycle: {names[0][:140]} -> {names[1][:140]}")

    def _frames(self) -> None:
        dyn = self.cfg.get("dynamic_frames", [])
        self.frame = {}
        for a in self.reach:
            f = self.funcs[a]
            fr = f.frame
            if f.dynamic:
                row = next((r for r in dyn if re.search(r["function"], plain(f.dname))), None)
                if row is None:
                    self.errors.append(f"dynamic stack frame with no bound in the config: {f.dname}")
                else:
                    self.hits["dynamic|" + row["function"]] += 1
                    fr += row["extra_bytes"]
            self.frame[a] = fr * self.mult.get(a, 1)

    def _below(self) -> None:
        """below[a]: the worst bytes from entering a to the deepest point under it."""
        unknown = self.cfg["unresolved_indirect_bytes"]
        self.below = {}
        self.below_next = {}
        for a in reversed(self.order):
            best, nxt = 0, None
            for site, t in self.dag.get(a, ()):
                if self.below[t] > best:
                    best, nxt = self.below[t], (site, t)
            if self.unresolved.get(a) and unknown > best:
                best, nxt = unknown, (self.unresolved[a][0], None)
            self.below[a] = self.frame[a] + best
            self.below_next[a] = nxt

    def _entry(self) -> None:
        """entry[a]: the worst bytes already on the stack when a is entered."""
        self.entry = {self.root: 0}
        self.entry_prev = {self.root: None}
        for a in self.order:
            here = self.entry[a] + self.frame[a]
            for site, t in self.dag.get(a, ()):
                if here > self.entry.get(t, -1):
                    self.entry[t] = here
                    self.entry_prev[t] = (a, site)

    def analyse(self, root: int) -> None:
        self.root = root
        self.build(root)
        self.cut_cycles(root)
        self.declare_cycles()
        self._frames()
        self._below()
        self._entry()

    def chain_down(self, start: int) -> list:
        """[(addr or None, site)] from start down the worst way (site = the call into it)."""
        out = [(start, None)]
        a = start
        while a is not None:
            nxt = self.below_next.get(a)
            if nxt is None:
                break
            site, a = nxt
            out.append((a, site))
        return out

    def chain_up(self, target: int) -> list:
        out = []
        node = target
        while True:
            prev = self.entry_prev.get(node)
            out.append((node, prev[1] if prev else None))
            if prev is None:
                break
            node = prev[0]
        out.reverse()
        return out

    def through(self, addrs: list) -> "tuple | None":
        """(worst total, function) over the given functions reachable from the root."""
        best = None
        for a in addrs:
            if a in self.entry:
                total = self.entry[a] + self.below[a]
                if best is None or total > best[0]:
                    best = (total, a)
        return best


# ---- the verdict -------------------------------------------------------------------


def load_config(path: str = CONFIG) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def load_image(elf: str, cfg: dict, tools: dict) -> "tuple[dict, str]":
    nm_text = run([tools["nm"], "-S", "--defined-only", elf])
    funcs = parse_nm(nm_text)
    rom = find_rom_elf(cfg)
    if not os.path.exists(rom):
        raise FileNotFoundError(f"ROM ELF {rom} not found (set NIMBUS_ROM_ELF; PlatformIO installs tool-esp-rom-elfs)")
    for a, f in parse_nm(run([tools["nm"], "-S", "--defined-only", rom]), rom=True).items():
        funcs.setdefault(a, f)
    known = set(funcs)
    # The precompiled ESP-IDF libraries' Xtensa property tables (.xt.prop) mark their own
    # code as data, so objdump prints mbedTLS, lwIP and newlib as empty words. Without
    # them objdump decodes every function; resync then decodes again what it decoded out
    # of step, and the dead-code rule (_function_body) keeps the data it decodes inside
    # functions from adding calls.
    read = elf_memory(elf)
    with tempfile.TemporaryDirectory() as tmp:
        bare = os.path.join(tmp, "noprop.elf")
        run([tools["objcopy"], "--remove-section", ".xt.prop", "--remove-section", ".xt.lit", elf, bare])
        parse_disassembly(
            run([tools["objdump"], "-d", "--no-show-raw-insn", bare]),
            {a: f for a, f in funcs.items() if not f.rom},
            known,
            Decoder(read, tools["objdump"]),
            image_landing_pads(read, nm_text),
        )
    parse_disassembly(
        run([tools["objdump"], "-d", "--no-show-raw-insn", rom]),
        {a: f for a, f in funcs.items() if f.rom},
        known,
        Decoder(elf_memory(rom), tools["objdump"]),
    )
    demangle(funcs, tools["c++filt"])
    return funcs, nm_text


def task_root(funcs: dict, cfg: dict) -> int:
    """The task's entry: vPortTaskWrapper, which calls pollTask through a register."""
    entry = [a for a, f in funcs.items() if f.name == cfg["entry"]]
    if not entry:
        raise ValueError(f"entry {cfg['entry']} not in the image")
    wrapper = [a for a, f in funcs.items() if f.name == cfg["task_wrapper"]]
    if not wrapper:
        return entry[0]
    funcs[wrapper[0]].calls = [(wrapper[0], entry[0])]
    funcs[wrapper[0]].indirect = []
    return wrapper[0]


def inline_sites(funcs: dict) -> list:
    """Sites whose inline chain the analysis reads: every indirect call, and every call into
    a Print or Stream helper (its receiver is read from the source line)."""
    sites = {s for f in funcs.values() if not f.rom for s, _ in f.indirect}
    sites |= {
        s
        for f in funcs.values()
        if not f.rom
        for s, t in f.calls
        if t in funcs and RECEIVER_HELPER_RE.match(funcs[t].dname)
    }
    return sorted(sites)


def analyse_elf(elf: str, cfg: dict, sources: "str | None" = None) -> "tuple[Model, dict]":
    tools = {n: find_tool(n) for n in ("nm", "objdump", "objcopy", "addr2line", "c++filt")}
    funcs, nm_text = load_image(elf, cfg, tools)
    root = task_root(funcs, cfg)
    inlines = addr2line(elf, tools["addr2line"], inline_sites(funcs))
    repaired = repair_inlines(elf, funcs, inlines)
    vslots = vtable_slots(nm_text, elf_reader(elf), funcs)
    model = Model(funcs, cfg, inlines, vslots, Sources(sources))
    model.lambda_names = handler_lambda_names(elf, tools, funcs, model.sources)
    model.analyse(root)
    if repaired < 0:
        model.errors.append(
            f"{-repaired} indirect call sites have inline chains addr2line got wrong and GDB "
            "(tool-xtensa-esp-elf-gdb, xtensa-esp-elf-gdb-no-python) is not installed to repair them"
        )
    model.tools = tools
    model.elf = elf
    model.repaired = repaired
    return model, funcs


def handler_lambda_names(elf: str, tools: dict, funcs: dict, sources: Sources) -> dict:
    """handler addr -> the names its lambda is assigned to, for every std::function handler."""
    handlers = [f for f in funcs.values() if not f.rom and handler_signature(f.dname, "_M_invoke") is not None]
    probes = sorted({p for f in handlers for p in f.probes})
    chains = addr2line(elf, tools["addr2line"], probes)
    out = {}
    for f in handlers:
        for p in f.probes:
            loc = next((loc for _name, loc in chains.get(p, []) if not STDLIB_LOC_RE.search(loc)), None)
            if loc:
                out[f.addr] = lambda_names(sources, loc)
                break
    return out


def describe(model: Model, chain: list, site_locs: dict) -> list:
    rows = []
    total = 0
    for addr, site in chain:
        loc = short_loc(site_locs.get(site, [("", "")])[0][1]) if site is not None else ""
        if addr is None:
            b = model.cfg["unresolved_indirect_bytes"]
            total += b
            rows.append((b, total, "(unresolved indirect call: config unresolved_indirect_bytes)", loc))
            continue
        b = model.frame[addr]
        total += b
        name = model.funcs[addr].dname
        if model.mult.get(addr, 1) > 1:
            name += f"  [recursion x{model.mult[addr]}]"
        rows.append((b, total, name, loc))
    return rows


STALE_CHECKS = (  # config section, the hit key of a row, what an unused row means
    ("indirect", lambda r: "indirect|" + r["in"] + "|" + r.get("at", ""), "indirect rule (no call site on tg_poll)"),
    ("recursion", lambda r: "recursion|" + r["function"], "recursion row (no cycle on tg_poll)"),
    ("impossible_cycles", lambda r: "impossible|" + r["caller"] + "|" + r["callee"], "impossible-cycle row"),
    ("hierarchies", lambda r: "hierarchy|" + r["names"][0], "hierarchy row (no virtual call site names it)"),
    ("not_on_tg_poll", lambda r: "elsewhere|" + r["function"], "not-on-tg_poll row (never an indirect target)"),
    ("impossible_calls", lambda r: "impossible-call|" + r["caller"] + "|" + r["callee"], "impossible-call row"),
    ("terminal", lambda r: "terminal|" + r["function"], "terminal row (not reachable on tg_poll)"),
    ("dynamic_frames", lambda r: "dynamic|" + r["function"], "dynamic-frame row (no such function on tg_poll)"),
)


def stale_rows(model: Model) -> list:
    """Config rows this image never used."""
    out = []
    for section, key, what in STALE_CHECKS:
        for r in model.cfg.get(section, []):
            if not model.hits.get(key(r)):
                shown = {k: v for k, v in r.items() if k not in ("why", "targets", "classes", "names")}
                out.append(f"stale {what}: {shown or r.get('names', [''])[0]!r}")
    return out


def chain_locations(model: Model, chains: dict) -> dict:
    """site -> inline chain for every call on the reported chains, repaired like the analysis's."""
    owner = {}
    for ch in chains.values():
        for (prev, _s), (_a, site) in zip(ch, ch[1:]):
            if site is not None and prev is not None and prev in model.funcs:
                owner[site] = model.funcs[prev]
    locs = addr2line(model.elf, model.tools["addr2line"], sorted(owner))
    bad = [s for s, ch in locs.items() if s in owner and _chain_wrong(ch, owner[s])]
    gdb = find_gdb()
    if bad and gdb:
        locs.update(gdb_lines(model.elf, gdb, bad))
    return locs


def named_paths(model: Model) -> list:
    """[(name, (worst total, function))]: the whole task, then each configured path."""
    paths = [("worst case (any path)", model.through([model.root]))]
    for row in model.cfg["paths"]:
        addrs = [a for pat in row["through"] for a in model.match(pat)]
        hit = model.through(addrs) if addrs else None
        if hit is None:
            model.errors.append(f"path '{row['name']}': nothing matching {row['through']} is reachable")
        else:
            paths.append((row["name"], hit))
    return paths


def verdict(model: Model) -> dict:
    cfg = model.cfg
    stack = cfg["stack_bytes"]
    fixed = sum(v for k, v in cfg["fixed_overhead"].items() if not k.startswith("_"))
    floor = cfg["min_free_bytes"]
    paths = named_paths(model)
    chains = {name: model.chain_up(fn)[:-1] + model.chain_down(fn) for name, (_t, fn) in paths}
    locs = chain_locations(model, chains)
    rows = []
    for name, (total, _fn) in paths:
        used = total + fixed
        rows.append(
            {
                "name": name,
                "used": used,
                "free": stack - used,
                "ok": stack - used >= floor,
                "chain": describe(model, chains[name], locs),
            }
        )
    errors = list(model.errors)
    return {
        "stack": stack,
        "fixed": fixed,
        "floor": floor,
        "rows": rows,
        "reach": len(model.reach),
        "edges": sum(len(v) for v in model.edges.values()),
        "unresolved": sum(len(v) for v in model.unresolved.values()),
        "how": dict(model.how),
        "cut": len(model.back),
        "dropped": model.dropped,
        "errors": errors,
        "stale": stale_rows(model),
        "ok": all(r["ok"] for r in rows) and not errors,
    }


def render(v: dict, elf: str, chain_rows: int = 60) -> str:
    how = ", ".join(f"{k} {n}" for k, n in sorted(v["how"].items()))
    lines = [
        f"tg_poll stack budget for {elf}",
        "",
        f"Stack {v['stack']} B, floor {v['floor']} B free, fixed overhead {v['fixed']} B (coprocessor save "
        f"area, TLS block, one interrupt frame). {v['reach']} functions and {v['edges']} call edges reachable. "
        f"Indirect sites: {how}. {v['dropped']} indirect targets dropped as not on tg_poll; "
        f"{v['cut']} back edges cut.",
        "",
        "| path | worst used B | free B | verdict |",
        "|---|---:|---:|---|",
    ]
    for r in v["rows"]:
        lines.append(f"| {r['name']} | {r['used']} | {r['free']} | {'ok' if r['ok'] else 'UNDER FLOOR'} |")
    for r in v["rows"]:
        lines += [
            "",
            f"### {r['name']}: {r['used']} B used, {r['free']} B free",
            "",
            "| frame B | running B | function | called at |",
            "|---:|---:|---|---|",
        ]
        for b, tot, name, loc in r["chain"][:chain_rows]:
            lines.append(f"| {b} | {tot} | `{name[:150]}` | {loc} |")
        if len(r["chain"]) > chain_rows:
            lines.append(f"| | | ... {len(r['chain']) - chain_rows} more frames | |")
    if v["errors"]:
        lines += ["", "Analysis errors (the gate fails on each):"]
        lines += ["- " + e for e in v["errors"]]
    if v["stale"]:
        lines += ["", "Config rows this image does not use (another image may; prune a row no image uses):"]
        lines += ["- " + e for e in v["stale"]]
    return "\n".join(lines)


def check(elf: str, cfg: dict, report: "str | None" = None, quiet: bool = False, sources: "str | None" = None) -> bool:
    model, _ = analyse_elf(elf, cfg, sources)
    v = verdict(model)
    text = render(v, elf)
    if report:
        with open(report, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    if not quiet:
        print(text)
        print()
    for r in v["rows"]:
        print(
            ("PASS " if r["ok"] else "FAIL ") + f"tg_poll {r['name']}: {r['used']} B used, {r['free']} B free "
            f"(floor {v['floor']} B)"
        )
    if v["errors"]:
        print(f"FAIL tg_poll stack analysis: {len(v['errors'])} error(s) listed above")
    return v["ok"]


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="tg_poll worst-case stack from the built image")
    ap.add_argument("--elf", help="firmware.elf to check (default: every built env in the config)")
    ap.add_argument("--config", default=CONFIG, help=argparse.SUPPRESS)
    ap.add_argument("--report", help="also write the full report (markdown) here")
    ap.add_argument("--quiet", action="store_true", help="print only the verdict lines")
    ap.add_argument(
        "--sources",
        help="a checkout of the commit the ELF was built from, read for its source lines "
        "(default: this checkout; needed for an image older than the working tree)",
    )
    args = ap.parse_args(argv)
    try:
        cfg = load_config(args.config)
    except (OSError, ValueError) as e:
        print(f"FAIL tg_poll stack: cannot read {args.config}: {e}")
        return 1
    if args.elf:
        elfs = [args.elf]
    else:
        elfs = [os.path.join(REPO, ".pio", "build", env, "firmware.elf") for env in cfg.get("envs", DEFAULT_ENVS)]
        elfs = [e for e in elfs if os.path.exists(e)]
    if not elfs:
        print("FAIL tg_poll stack: no image is built (pio run -e esp32s3-cyd first)")
        return 1
    ok = True
    for elf in elfs:
        try:
            ok = check(elf, cfg, args.report, args.quiet, args.sources) and ok
        except (OSError, ValueError, RuntimeError) as e:
            print(f"FAIL tg_poll stack: {elf}: {e}")
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
