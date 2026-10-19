#!/usr/bin/env python3
"""Release gate: the shipped image's ELF - symbols and identity.

1. Symbols (CUM-167). The TFT firmware must link ZERO e-paper (GxEPD2) symbols: the
   ~14.5 KB internal-SRAM win from compiling the e-paper driver out must survive any
   display fix. It also asserts the color-TFT panel plumbing IS linked (a real defined
   symbol), so an empty/half-built ELF cannot pass silently.

2. Identity (crash forensics). A field core dump can only be decoded against the exact
   ELF that produced the image, so the release archives every variant's firmware.elf.
   With --bin, the gate proves the archived ELF IS that image: the build embeds the
   ELF's SHA-256 in the image's app descriptor (esp_app_desc_t.app_elf_sha256, image
   offset 0xB0), and the gate checks it equals sha256(firmware.elf). The device's core
   dump records the first hex digits of the same hash, so the archive is searchable by
   the "elf=" field of a crash line. --sha-out writes "<variant> <sha256>" for the
   release record.

Usage:
    pio run -e esp32s3            # build first
    python3 tools/release_gate/check_elf_symbols.py [--elf <path>]
    python3 tools/release_gate/check_elf_symbols.py --elf <elf> --bin <bin> [--identity-only]
        [--variant <name> --sha-out <file>]
Exit 0 = clean; exit 1 = e-paper linked, plumbing missing, ELF/bin mismatch, or a tool
or file is unavailable.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import os
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_ELF = os.path.join(REPO, ".pio", "build", "esp32s3", "firmware.elf")

# Symbols that must NOT appear (the e-paper footprint).
EPAPER_MARKERS = ("GxEPD2", "SSD1680")
# A symbol that MUST appear (proves the color-TFT driver is actually linked).
REQUIRED_TFT = "solide11display_tft5begin"  # mangled solide::display_tft::begin
# Test-build crash switches (the console's PANIC ... arm=turn): a shipped image that
# linked them could be made to crash on demand. Source guard: test_panic_guard.py.
TEST_ONLY_MARKERS = ("armTurnPanic", "testPanicPoint", "parsePanicArm", "panicArmFires")

# ESP-IDF app image layout (esp_image_format.h / esp_app_desc.h): a 24-byte image
# header, an 8-byte first-segment header, then esp_app_desc_t, whose app_elf_sha256
# field sits 0x90 into the descriptor.
IMAGE_MAGIC = 0xE9
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432
ELF_SHA_OFFSET = 0xB0
ELF_SHA_LEN = 32


def find_nm() -> "str | None":
    hits = glob.glob(os.path.expanduser("~/.platformio/packages/toolchain-*/bin/xtensa-esp32s3-elf-nm"))
    return hits[0] if hits else None


def read_symbols(nm: str, elf: str) -> str:
    out = subprocess.run([nm, elf], capture_output=True, text=True)
    return out.stdout


def check(symbols: str) -> "tuple[bool, list[str]]":
    """(ok, messages)."""
    msgs = []
    ok = True
    epaper = [m for m in EPAPER_MARKERS if m.lower() in symbols.lower()]
    if epaper:
        ok = False
        msgs.append(f"e-paper symbols present in the TFT image: {epaper} (the 14.5 KB win regressed)")
    else:
        msgs.append("no GxEPD2 / e-paper symbols (size win intact)")
    test_only = [m for m in TEST_ONLY_MARKERS if m in symbols]
    if test_only:
        ok = False
        msgs.append(f"test-build crash switches linked into a shipped image: {test_only}")
    else:
        msgs.append("no test-build crash switches")
    if REQUIRED_TFT not in symbols:
        ok = False
        msgs.append(f"missing color-TFT plumbing symbol ({REQUIRED_TFT}) - ELF may be empty/half-built")
    else:
        msgs.append("color-TFT panel plumbing is linked")
    return ok, msgs


def embedded_elf_sha(image: bytes) -> "str | None":
    """The app_elf_sha256 the build embedded in an app image, or None if it is not one."""
    if len(image) < ELF_SHA_OFFSET + ELF_SHA_LEN or image[0] != IMAGE_MAGIC:
        return None
    (magic,) = struct.unpack_from("<I", image, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        return None
    return image[ELF_SHA_OFFSET : ELF_SHA_OFFSET + ELF_SHA_LEN].hex()


def check_identity(elf_bytes: bytes, image: bytes) -> "tuple[bool, str, str]":
    """(ok, message, elf_sha256): the image's embedded ELF hash equals sha256(elf)."""
    elf_sha = hashlib.sha256(elf_bytes).hexdigest()
    embedded = embedded_elf_sha(image)
    if embedded is None:
        return False, "the --bin file is not an ESP-IDF app image (no app descriptor)", elf_sha
    if embedded != elf_sha:
        return False, f"ELF does not match the image: image says {embedded[:16]}.., ELF is {elf_sha[:16]}..", elf_sha
    return True, f"ELF matches the image (app_elf_sha256 {elf_sha[:16]}..)", elf_sha


def run_symbols(elf_path: str) -> bool:
    nm = find_nm()
    if not nm:
        print(
            "[gate:elf-symbols] FAIL: xtensa-esp32s3-elf-nm not found (build the firmware once to fetch the toolchain)"
        )
        return False
    ok, msgs = check(read_symbols(nm, elf_path))
    for m in msgs:
        print(f"[gate:elf-symbols] {'ok' if ok else '!!'} {m}")
    return ok


def run_identity(elf_path: str, bin_path: str, variant: str, sha_out: "str | None") -> bool:
    if not os.path.exists(bin_path):
        print(f"[gate:elf-symbols] FAIL: image not found at {bin_path}")
        return False
    with open(elf_path, "rb") as f:
        elf_bytes = f.read()
    with open(bin_path, "rb") as f:
        image = f.read()
    ok, msg, sha = check_identity(elf_bytes, image)
    print(f"[gate:elf-symbols] {'ok' if ok else '!!'} {msg}")
    if ok and sha_out:
        with open(sha_out, "a") as f:
            f.write(f"{variant or os.path.basename(bin_path)} {sha}\n")
    return ok


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Release gate: ELF symbols and image identity")
    ap.add_argument("--elf", default=DEFAULT_ELF, help="path to the built firmware.elf")
    ap.add_argument("--bin", help="the matching firmware.bin: assert its embedded ELF hash")
    ap.add_argument("--identity-only", action="store_true", help="skip the symbol check (non-TFT variants)")
    ap.add_argument("--variant", default="", help="variant name for --sha-out")
    ap.add_argument("--sha-out", help="append '<variant> <app_elf_sha256>' to this file")
    args = ap.parse_args(argv)
    if args.identity_only and not args.bin:
        ap.error("--identity-only checks nothing without --bin")  # a gate never passes vacuously

    if not os.path.exists(args.elf):
        print(f"[gate:elf-symbols] FAIL: ELF not found at {args.elf} - run `pio run -e esp32s3` first")
        return 1
    ok = args.identity_only or run_symbols(args.elf)
    if args.bin:
        ok = run_identity(args.elf, args.bin, args.variant, args.sha_out) and ok
    print(f"[gate:elf-symbols] {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
