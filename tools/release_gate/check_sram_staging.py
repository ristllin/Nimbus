#!/usr/bin/env python3
"""Release gate: the Telegram TLS staging buffers must stay in PSRAM (N7 win #2).

CUM-24 verification bar. The Telegram poll task (`tg_poll`) is the single,
fully-serialized consumer of its TLS staging buffers, so those buffers live in
PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`) and only a pointer handle
stays in internal SRAM:

  g_inboundStage  the InboundMsg drain slot (~4.1 KB payload)
  g_apiResp       the shared API-response scratch (~1 KB payload)
  g_pollBody      the getUpdates poll-body arena

That is the ~6.7 KB internal-DRAM win merged as 2598499. It shipped with a
one-time manual `xtensa-esp32s3-elf-size -A` before/after and no automated guard:
a later edit that reverts any of these to a file-scope inline static (e.g.
`static InboundMsg im;` = 4,160 B, or `static char resp[1024]`) would move
kilobytes back into internal `.bss` while every host test stayed green. This is
that guard - the companion to tools/check_tft_elf_no_eink.py (which guards the
B4 e-ink win the same way).

It inspects the linked ELF (no board needed) via `nm -S -C` and fails if any of
the named staging handles is:
  1. absent      - empty/half-built ELF, or the code was renamed without
                   updating this guard (a deliberate stop, not a silent pass), or
  2. not pointer-sized - the buffer moved back inline into internal SRAM and the
                   ~6.7 KB win regressed.

It deliberately checks only these named handles, not a blanket size sweep over
the translation unit: several other `agent::telegram::` statics (s_pending,
g_attach, g_media, ...) are legitimately internal and are not part of this win.

Usage:
    pio run -e esp32s3            # build first
    python3 tools/release_gate/check_sram_staging.py [--elf <path>]
Exit 0 = the staging handles are all PSRAM pointers; exit 1 = a handle regressed,
is missing, or the ELF/nm is unavailable.
"""

from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_ELF = os.path.join(REPO, ".pio", "build", "esp32s3", "firmware.elf")

# A defined data/bss symbol that is a bare pointer handle is at most this many
# bytes (4 on the 32-bit target; 8 leaves headroom for a host-built ELF). Any
# larger and the payload is inline in internal SRAM, not behind a PSRAM handle.
POINTER_MAX = 8

# The tg_poll TLS staging buffers introduced by 2598499. name -> what it stages.
# A new tg_poll staging buffer that must live in PSRAM belongs in this list.
STAGING = {
    "g_inboundStage": "InboundMsg drain slot (~4.1 KB)",
    "g_apiResp": "shared API-response scratch (~1 KB)",
    "g_pollBody": "getUpdates poll-body arena",
}

# nm data/bss type letters (uppercase = global, lowercase = local/static).
_DATA_BSS = set("bBdD")


def find_nm() -> "str | None":
    hits = glob.glob(os.path.expanduser("~/.platformio/packages/toolchain-*/bin/xtensa-esp32s3-elf-nm"))
    return hits[0] if hits else None


def read_symbols(nm: str, elf: str) -> str:
    return subprocess.run([nm, "-S", "-C", elf], capture_output=True, text=True).stdout


def parse_nm(text: str) -> "list[tuple[int, str, str]]":
    """Parse `nm -S -C` output into (size, type, name) rows.

    A sized row is `<addr> <size> <type> <name...>`; unsized rows (4 fields with
    no size, or undefined symbols) carry no internal-SRAM cost and are skipped.
    The demangled name can contain spaces, so the name is the joined remainder.
    """
    rows: "list[tuple[int, str, str]]" = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 4:
            continue
        size, typ = parts[1], parts[2]
        name = " ".join(parts[3:])
        try:
            size_i = int(size, 16)
        except ValueError:
            continue  # unsized (the 2nd field was the type, not a size)
        if len(typ) != 1:
            continue
        rows.append((size_i, typ, name))
    return rows


def _leaf(name: str) -> str:
    """The final `::`-delimited component of a (demangled) symbol name."""
    return name.rsplit("::", 1)[-1]


def judge(rows: "list[tuple[int, str, str]]") -> "tuple[bool, list[str]]":
    """(ok, messages). Pure: takes parsed nm rows, decides the gate verdict."""
    # Map each staging leaf name to the largest data/bss size seen for it.
    sizes: "dict[str, int]" = {}
    for size, typ, name in rows:
        if typ not in _DATA_BSS:
            continue
        if "agent::telegram::" not in name:
            continue
        leaf = _leaf(name)
        if leaf in STAGING:
            sizes[leaf] = max(sizes.get(leaf, 0), size)

    ok = True
    msgs: "list[str]" = []
    for key, what in STAGING.items():
        if key not in sizes:
            ok = False
            msgs.append(f"missing staging handle {key} ({what}) - ELF empty/half-built or code renamed")
        elif sizes[key] > POINTER_MAX:
            ok = False
            msgs.append(
                f"{key} is {sizes[key]} B of internal SRAM, not a PSRAM pointer "
                f"(<= {POINTER_MAX} B): the {what} moved back inline (win 2598499 regressed)"
            )
        else:
            msgs.append(f"{key} is a {sizes[key]} B PSRAM handle ({what}) - win intact")
    return ok, msgs


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Release gate: Telegram PSRAM staging check (CUM-24)")
    ap.add_argument("--elf", default=DEFAULT_ELF, help="path to the built esp32s3 firmware.elf")
    args = ap.parse_args(argv)

    if not os.path.exists(args.elf):
        print(f"[gate:sram-staging] FAIL: ELF not found at {args.elf} - run `pio run -e esp32s3` first")
        return 1
    nm = find_nm()
    if not nm:
        print(
            "[gate:sram-staging] FAIL: xtensa-esp32s3-elf-nm not found (build the firmware once to fetch the toolchain)"
        )
        return 1

    ok, msgs = judge(parse_nm(read_symbols(nm, args.elf)))
    for m in msgs:
        print(f"[gate:sram-staging] {'ok' if ok else '!!'} {m}")
    print(f"[gate:sram-staging] {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
