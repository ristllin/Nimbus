#!/usr/bin/env python3
"""Release gate: the TLS/relay staging buffers must stay in PSRAM (N7 headroom).

Two staging wins are guarded here, by two mechanisms, because the two paths hold
their staging differently:

--- Telegram (CUM-24, ELF leg) ---------------------------------------------------
The Telegram poll task (`tg_poll`) is the single, fully-serialized consumer of its
TLS staging buffers, so those buffers live in PSRAM
(`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`) behind a file-scope pointer handle and
only that pointer stays in internal SRAM:

  g_inboundStage  the InboundMsg drain slot (~4.1 KB payload)
  g_apiResp       the shared API-response scratch (~1 KB payload)
  g_pollBody      the getUpdates poll-body arena

That is the ~6.7 KB internal-DRAM win merged as 2598499. Because these are named
file-scope statics, a revert (e.g. `static InboundMsg im;` = 4,160 B) is visible in
the linked ELF: this leg inspects it via `nm -S -C` (no board needed) and fails if a
named handle is absent (empty/half-built ELF, or renamed without updating the guard -
a deliberate stop, not a silent pass) or is not pointer-sized (the buffer moved back
inline into internal `.bss`, the win regressed). It deliberately checks only these
named handles, not a blanket sweep: several other `agent::telegram::` statics
(s_pending, g_attach, ...) are legitimately internal and not part of this win.

--- Relay / cloud-sync (CUM-387, source leg) -------------------------------------
The relay path stages differently: its big transient buffers - the tunneled response
body + the HTTP parser's unparsed staging, and the inbound WS frame staging +
fragment reassembly + decoded message payload - are NOT file-scope statics but
per-operation heap containers. They ride PSRAM by TYPE: `nimbus::cloud::PsVector<T>`,
a std::vector whose allocator calls `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` (with
an internal-heap fallback) on device, and a plain std::vector on host. Because they
are heap-allocated, not named `.bss` symbols, they are invisible to `nm` - the ELF
leg above cannot see them. So the relay staging sites are guarded at the SOURCE: this
leg reads the two headers that declare them and fails if any relay staging container
has been reverted from the PSRAM alias to a plain internal-SRAM `std::vector<uint8_t>`,
or if the compile-time `static_assert` that also guards it has been removed. Reverting
one relay buffer to the internal heap turns this leg RED (proven in test_sram_staging).
The firmware build's own `static_assert`s (http_replay.h, relay_ws.h) are the first
line; this gate is the second, and it runs with no board and no build.

Usage:
    pio run -e esp32s3            # build first (only the Telegram ELF leg needs it)
    python3 tools/release_gate/check_sram_staging.py [--elf <path>]
Exit 0 = both legs pass; exit 1 = a staging site regressed, a handle/guard is missing,
or (Telegram leg only) the ELF/nm is unavailable.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
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


# --- Relay / cloud-sync staging (CUM-387, source leg) -------------------------
# The relay staging containers, by the header that declares them. Each must stay
# the PSRAM-backed alias `nimbus::cloud::PsVector<uint8_t>` (never a plain
# `std::vector<uint8_t>`, which is the scarce internal heap on device), and each
# header must keep its compile-time PSRAM static_assert. A new relay staging
# buffer that must live in PSRAM belongs in the relevant `members`/`aliases` set.
RELAY_HEADERS = {
    os.path.join("lib", "core", "include", "nimbus", "cloud", "http_replay.h"): {
        # alias name -> what rides it (ResponseParser::buf_ and ::body_ are BodyBuf)
        "aliases": {"BodyBuf": "tunneled response body + HTTP parser unparsed staging"},
        "members": {},
        "guard": "response body/staging PSRAM static_assert",
    },
    os.path.join("lib", "core", "include", "nimbus", "cloud", "relay_ws.h"): {
        "aliases": {},
        # member name -> what it stages (all uint8_t byte buffers on the WS path)
        "members": {
            "payload": "inbound WS decoded message payload",
            "buf_": "inbound WS unparsed frame staging",
            "frag_": "inbound WS fragment reassembly",
        },
        "guard": "inbound WS payload PSRAM static_assert",
    },
}

# A relay staging container is PSRAM-backed iff its declared type is the PsVector
# alias over uint8_t; the same shape over std::vector is the internal-heap revert.
_PS_ALIAS = r"(?:nimbus::cloud::)?PsVector\s*<\s*uint8_t\s*>"
_STD_VEC = r"std::vector\s*<\s*uint8_t\s*>"


def _has_psram_static_assert(text: str) -> bool:
    """True iff the header keeps a `static_assert(... PsramAlloc ...)` guard."""
    return re.search(r"static_assert\s*\([^;]*PsramAlloc", text, re.S) is not None


def judge_relay(sources: "dict[str, str]") -> "tuple[bool, list[str]]":
    """(ok, messages). Pure: takes {header path: source text}, decides the verdict.

    Fails if any relay staging alias/member has been reverted to a plain internal
    `std::vector<uint8_t>`, is missing (renamed/removed - a deliberate stop), or its
    header dropped the PSRAM static_assert. Structured so a single reverted relay
    buffer turns the gate RED (test_sram_staging proves it non-tautological).
    """
    ok = True
    msgs: "list[str]" = []
    for rel, spec in RELAY_HEADERS.items():
        text = sources.get(rel)
        if text is None:
            ok = False
            msgs.append(f"missing relay staging header {rel} (renamed/removed - deliberate stop)")
            continue
        # Each alias: must be `using <name> = ...PsVector<uint8_t>`.
        for name, what in spec["aliases"].items():
            if re.search(r"using\s+" + name + r"\s*=\s*" + _PS_ALIAS, text):
                msgs.append(f"{name} is the PSRAM alias ({what}) - win intact")
            elif re.search(r"using\s+" + name + r"\s*=\s*" + _STD_VEC, text):
                ok = False
                msgs.append(
                    f"{name} reverted to std::vector<uint8_t> (internal SRAM): "
                    f"the {what} moved back onto the internal heap (CUM-387 regressed)"
                )
            else:
                ok = False
                msgs.append(f"missing relay staging alias {name} in {rel} (renamed/removed)")
        # Each member: must be declared `PsVector<uint8_t> <name>`.
        for name, what in spec["members"].items():
            if re.search(_PS_ALIAS + r"\s+" + name + r"\b", text):
                msgs.append(f"{name} is PSRAM-backed ({what}) - win intact")
            elif re.search(_STD_VEC + r"\s+" + name + r"\b", text):
                ok = False
                msgs.append(
                    f"{name} reverted to std::vector<uint8_t> (internal SRAM): "
                    f"the {what} moved back onto the internal heap (CUM-387 regressed)"
                )
            else:
                ok = False
                msgs.append(f"missing relay staging member {name} in {rel} (renamed/removed)")
        # The header must keep its compile-time PSRAM guard.
        if _has_psram_static_assert(text):
            msgs.append(f"{spec['guard']} present in {os.path.basename(rel)} - guard intact")
        else:
            ok = False
            msgs.append(
                f"missing {spec['guard']} in {os.path.basename(rel)}: the compile-time "
                f"PSRAM guard was removed (CUM-387 regressed)"
            )
    return ok, msgs


def read_relay_sources(repo: str = REPO) -> "dict[str, str]":
    """Read the real relay staging headers into {repo-relative path: text}."""
    out: "dict[str, str]" = {}
    for rel in RELAY_HEADERS:
        path = os.path.join(repo, rel)
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                out[rel] = f.read()
    return out


def run_telegram_leg(elf: str) -> bool:
    """The Telegram ELF leg (CUM-24). Prints its verdict; returns True on pass."""
    if not os.path.exists(elf):
        print(f"[gate:sram-staging] FAIL: ELF not found at {elf} - run `pio run -e esp32s3` first")
        return False
    nm = find_nm()
    if not nm:
        print(
            "[gate:sram-staging] FAIL: xtensa-esp32s3-elf-nm not found (build the firmware once to fetch the toolchain)"
        )
        return False
    ok, msgs = judge(parse_nm(read_symbols(nm, elf)))
    for m in msgs:
        print(f"[gate:sram-staging:tg] {'ok' if ok else '!!'} {m}")
    return ok


def run_relay_leg() -> bool:
    """The relay/cloud-sync source leg (CUM-387). Prints its verdict; returns pass."""
    ok, msgs = judge_relay(read_relay_sources())
    for m in msgs:
        print(f"[gate:sram-staging:relay] {'ok' if ok else '!!'} {m}")
    return ok


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Release gate: Telegram (CUM-24) + relay (CUM-387) PSRAM staging check")
    ap.add_argument("--elf", default=DEFAULT_ELF, help="path to the built esp32s3 firmware.elf")
    ap.add_argument(
        "--relay-only",
        action="store_true",
        help="run only the relay source leg (no ELF/build needed)",
    )
    args = ap.parse_args(argv)

    # The relay leg needs no board and no build, so it always runs.
    relay_ok = run_relay_leg()
    tg_ok = True if args.relay_only else run_telegram_leg(args.elf)

    all_ok = relay_ok and tg_ok
    print(f"[gate:sram-staging] {'PASS' if all_ok else 'FAIL'}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
