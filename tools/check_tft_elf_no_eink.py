#!/usr/bin/env python3
"""check_tft_elf_no_eink - binary-level regression guard for the colour-TFT build.

The e-paper driver is compile-gated behind SOLIDE_HAS_EPAPER (default off), so a
colour-TFT firmware (esp32s3 / esp32s3-cyd / test) must link NONE of the GxEPD2
e-ink stack: that is the ~14.5 KB internal-SRAM win, and it is also the safety
property behind CUM-167. The colour TFT path (solide::display_tft) is a separate,
ungated driver; if a change ever makes the TFT init or render path reach into the
gated solide::display (e-paper) code, that code is a no-op STUB on a TFT build and
the panel silently goes dark while every log says it is up.

This guard inspects the linked ELF (no board needed) and fails if:
  1. any GxEPD2 symbol is present  (the e-ink stack crept back / the 14.5 KB
     win regressed), or
  2. the colour TFT driver symbols are absent (the TFT path did not link at all).

The companion tools/check_no_eink.sh guards user-facing SOURCE text; this guards
the compiled BINARY. Run it after `pio run -e esp32s3` (and the other TFT envs).

Usage:
  tools/check_tft_elf_no_eink.py [ELF ...]
Defaults to .pio/build/esp32s3/firmware.elf when no path is given.
"""

import os
import shutil
import subprocess
import sys


def find_nm():
    """Prefer the xtensa toolchain nm; fall back to a demangling system nm."""
    for cand in ("xtensa-esp32s3-elf-nm",):
        p = shutil.which(cand)
        if p:
            return p
    home = os.path.expanduser("~")
    pkg = os.path.join(home, ".platformio", "packages")
    if os.path.isdir(pkg):
        for root, _dirs, files in os.walk(pkg):
            if "xtensa-esp32s3-elf-nm" in files:
                return os.path.join(root, "xtensa-esp32s3-elf-nm")
    return shutil.which("nm")


def symbols(nm, elf):
    out = subprocess.run([nm, "-C", elf], capture_output=True, text=True)
    if out.returncode != 0:
        print(f"  nm failed on {elf}:\n{out.stderr}", file=sys.stderr)
        sys.exit(2)
    return out.stdout


def check(nm, elf):
    if not os.path.isfile(elf):
        print(f"SKIP  {elf} (not built)")
        return True
    text = symbols(nm, elf)
    gxepd2 = [ln for ln in text.splitlines() if "GxEPD2" in ln]
    tft = [ln for ln in text.splitlines() if "solide::display_tft::" in ln]
    ok = True
    if gxepd2:
        ok = False
        print(
            f"FAIL  {elf}: {len(gxepd2)} GxEPD2 symbol(s) linked into a TFT build "
            "(the e-ink stack must stay compiled out; SOLIDE_HAS_EPAPER is off)."
        )
        for ln in gxepd2[:8]:
            print(f"        {ln.strip()}")
    if not tft:
        ok = False
        print(f"FAIL  {elf}: no solide::display_tft:: symbols - the colour TFT path did not link.")
    if ok:
        print(f"OK    {elf}: 0 GxEPD2 symbols, colour TFT path present ({len(tft)} display_tft symbols).")
    return ok


def main(argv):
    elfs = argv[1:] or [".pio/build/esp32s3/firmware.elf"]
    nm = find_nm()
    if not nm:
        print("error: no nm found (need xtensa-esp32s3-elf-nm or nm on PATH)", file=sys.stderr)
        return 2
    return 0 if all(check(nm, e) for e in elfs) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
